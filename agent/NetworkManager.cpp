#include "NetworkManager.h"
#include <cstdio>

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    printf("%s\n", buf);
}

NetworkManager::NetworkManager() {
}

NetworkManager::~NetworkManager() {
    Disconnect();
}

void NetworkManager::SetReconnectParams(int maxRetries, int retryDelayMs) {
    this->maxRetries = maxRetries;
    this->retryDelayMs = retryDelayMs;
}

void NetworkManager::SetReconnectCallback(ReconnectCallback cb) {
    onReconnect = std::move(cb);
}

bool NetworkManager::Connect(const std::string& host, int port, const std::string& path) {
    if (state == ConnectionState::Connected || state == ConnectionState::Connecting) {
        Disconnect();
    }

    this->host = host;
    this->port = port;
    this->path = path;

    return DoConnect();
}

bool NetworkManager::DoConnect() {
    state = ConnectionState::Connecting;
    if (onStateChange) onStateChange(state);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("[WS] WSAStartup failed");
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Log("[WS] socket failed");
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }

    struct addrinfo hints, *result = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[16];
    sprintf_s(portStr, "%d", port);

    if (getaddrinfo(host.c_str(), portStr, &hints, &result) != 0) {
        Log("[WS] getaddrinfo failed");
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }

    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        Log("[WS] connect failed: %d", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }
    freeaddrinfo(result);

    std::string handshake = "GET ";
    handshake += path;
    handshake += " HTTP/1.1\r\n";
    handshake += "Host: ";
    handshake += host;
    handshake += ":";
    handshake += std::to_string(port);
    handshake += "\r\n";
    handshake += "Upgrade: websocket\r\n";
    handshake += "Connection: Upgrade\r\n";
    handshake += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    handshake += "Sec-WebSocket-Version: 13\r\n";
    handshake += "\r\n";

    if (send(sock, handshake.c_str(), (int)handshake.length(), 0) == SOCKET_ERROR) {
        Log("[WS] handshake send failed: %d", WSAGetLastError());
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }

    char response[1024];
    int received = recv(sock, response, sizeof(response) - 1, 0);
    if (received <= 0) {
        Log("[WS] handshake recv failed");
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }
    response[received] = '\0';

    std::string resp(response);
    if (resp.find("101") == std::string::npos) {
        Log("[WS] Expected 101, got: %.*s", received > 100 ? 100 : received, response);
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = ConnectionState::Error;
        if (onStateChange) onStateChange(state);
        return false;
    }

    running = true;
    state = ConnectionState::Connected;
    receiveThread = std::thread(&NetworkManager::ReceiveLoop, this);

    Log("[WS] Connected!");
    if (onStateChange) onStateChange(state);
    return true;
}

void NetworkManager::ReceiveLoop() {
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(0, &readfds, NULL, NULL, &tv);
        if (sel == 0) continue;
        if (sel < 0) break;

        uint8_t header[14];
        int bytes = recv(sock, (char*)header, 2, MSG_PEEK);

        if (bytes <= 0) {
            if (!running) break;
            break;
        }

        uint8_t opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t payloadLen = header[1] & 0x7F;

        int headerLen = 2;
        if (payloadLen == 126) headerLen += 2;
        else if (payloadLen == 127) headerLen += 8;

        std::vector<uint8_t> fullHeader(headerLen + (masked ? 4 : 0));
        bytes = recv(sock, (char*)fullHeader.data(), headerLen + (masked ? 4 : 0), 0);
        if (bytes != headerLen + (masked ? 4 : 0)) break;

        size_t idx = 2;
        if (payloadLen == 126) {
            payloadLen = (fullHeader[idx] << 8) | fullHeader[idx + 1];
            idx += 2;
        } else if (payloadLen == 127) {
            payloadLen = 0;
            for (int i = 0; i < 8; i++) {
                payloadLen = (payloadLen << 8) | fullHeader[idx + i];
            }
            idx += 8;
        }

        uint8_t maskKey[4] = {0};
        if (masked) {
            memcpy(maskKey, &fullHeader[idx], 4);
        }

        if (payloadLen > 1024 * 1024 * 10) break;

        std::vector<uint8_t> payload(payloadLen);
        size_t totalRead = 0;
        while (totalRead < payloadLen) {
            bytes = recv(sock, (char*)&payload[totalRead], (int)(payloadLen - totalRead), 0);
            if (bytes <= 0) break;
            totalRead += bytes;
        }

        for (size_t i = 0; i < payloadLen; i++) {
            payload[i] ^= maskKey[i % 4];
        }

        if (opcode == 0x8) {
            running = false;
            break;
        }

        if (opcode == 0x9) {
            SendFrame(0xA, payload.data(), payload.size());
        } else if (opcode == 0x1 && onMessage) {
            std::string msg((char*)payload.data(), payload.size());
            onMessage(msg);
        }
    }

    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    WSACleanup();

    state = ConnectionState::Disconnected;
    if (onStateChange) onStateChange(state);

    if (running && autoReconnect) {
        reconnectThread = std::thread(&NetworkManager::ReconnectLoop, this);
    }
}

void NetworkManager::ReconnectLoop() {
    reconnectAttempt = 0;

    while (running && autoReconnect) {
        if (maxRetries > 0 && reconnectAttempt >= maxRetries) {
            Log("[WS] Max retries (%d) reached, giving up", maxRetries);
            state = ConnectionState::Error;
            if (onStateChange) onStateChange(state);
            return;
        }

        reconnectAttempt++;
        int attempt = reconnectAttempt.load();
        Log("[WS] Reconnecting in %d ms... (attempt %d/%d)",
            retryDelayMs, attempt, maxRetries > 0 ? maxRetries : -1);

        if (onReconnect) onReconnect(attempt);

        Sleep(retryDelayMs);

        if (!running || !autoReconnect) break;

        if (DoConnect()) {
            Log("[WS] Reconnected successfully on attempt %d", attempt);
            reconnectAttempt = 0;
            return;
        }

        Log("[WS] Reconnect attempt %d failed", attempt);
    }
}

void NetworkManager::Disconnect() {
    autoReconnect = false;
    running = false;

    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (receiveThread.joinable()) receiveThread.join();
    if (reconnectThread.joinable()) reconnectThread.join();

    WSACleanup();
    state = ConnectionState::Disconnected;
    reconnectAttempt = 0;
    if (onStateChange) onStateChange(state);
}

bool NetworkManager::SendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
    if (sock == INVALID_SOCKET) return false;

    std::vector<uint8_t> frame;
    frame.push_back(0x80 | opcode);

    if (len < 126) {
        frame.push_back(0x80 | (uint8_t)len);
    } else if (len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((len >> (i * 8)) & 0xFF);
        }
    }

    uint8_t mask[4];
    mask[0] = rand() % 256;
    mask[1] = rand() % 256;
    mask[2] = rand() % 256;
    mask[3] = rand() % 256;
    frame.insert(frame.end(), mask, mask + 4);

    for (size_t i = 0; i < len; i++) {
        frame.push_back(data[i] ^ mask[i % 4]);
    }

    int sent = send(sock, (const char*)frame.data(), (int)frame.size(), 0);
    if (sent == SOCKET_ERROR) {
        return false;
    }

    return true;
}

bool NetworkManager::SendText(const std::string& message) {
    if (state != ConnectionState::Connected) return false;
    return SendFrame(0x1, (const uint8_t*)message.c_str(), message.length());
}

bool NetworkManager::SendBinary(const uint8_t* data, size_t len) {
    if (state != ConnectionState::Connected) return false;
    return SendFrame(0x2, data, len);
}

bool NetworkManager::SendPing() {
    if (state != ConnectionState::Connected) return false;
    return SendFrame(0x9, NULL, 0);
}

ConnectionState NetworkManager::GetState() const {
    return state;
}

bool NetworkManager::IsConnected() const {
    return state == ConnectionState::Connected;
}

void NetworkManager::SetMessageCallback(MessageCallback cb) {
    onMessage = std::move(cb);
}

void NetworkManager::SetStateCallback(StateCallback cb) {
    onStateChange = std::move(cb);
}
