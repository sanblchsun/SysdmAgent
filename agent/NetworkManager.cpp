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

NetworkManager::NetworkManager() : running(false), state(WebSocketState::Disconnected) {
}

NetworkManager::~NetworkManager() {
    Disconnect();
}

bool NetworkManager::Connect(const std::string& host, int port, const std::string& path) {
    this->host = host;
    this->port = port;
    this->wsPath = path;

    if (state == WebSocketState::Connected || state == WebSocketState::Connecting) {
        Disconnect();
    }

    state = WebSocketState::Connecting;
    Log("[WS] Connecting to %s:%d%s...", host.c_str(), port, path.c_str());

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("[WS] WSAStartup failed");
        state = WebSocketState::Error;
        return false;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Log("[WS] socket failed");
        state = WebSocketState::Error;
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
        state = WebSocketState::Error;
        return false;
    }

    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        Log("[WS] connect failed: %d", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = WebSocketState::Error;
        return false;
    }
    freeaddrinfo(result);

    Log("[WS] TCP connected, performing handshake...");

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
        state = WebSocketState::Error;
        return false;
    }

    char response[1024];
    int received = recv(sock, response, sizeof(response) - 1, 0);
    if (received <= 0) {
        Log("[WS] handshake recv failed");
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = WebSocketState::Error;
        return false;
    }
    response[received] = '\0';

    std::string resp(response);
    if (resp.find("101") == std::string::npos) {
        Log("[WS] Expected 101, got: %.*s", received > 100 ? 100 : received, response);
        closesocket(sock);
        sock = INVALID_SOCKET;
        state = WebSocketState::Error;
        return false;
    }

    state = WebSocketState::Connected;
    running = true;
    receiveThread = std::thread(&NetworkManager::ReceiveLoop, this);

    Log("[WS] Connected!");
    if (onStateChange) onStateChange(state);
    return true;
}

void NetworkManager::ReceiveLoop() {
    Log("[WS] Receive thread started");

    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(0, &readfds, NULL, NULL, &tv);
        if (sel == 0) continue;
        if (sel < 0) { Log("[WS] select error"); break; }

        uint8_t header[14];
        int bytes = recv(sock, (char*)header, 2, MSG_PEEK);

        if (bytes <= 0) {
            if (!running) break;
            Log("[WS] recv peek failed: %d", WSAGetLastError());
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
        if (bytes != headerLen + (masked ? 4 : 0)) {
            Log("[WS] Failed to read full header");
            break;
        }

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
            idx += 4;
        }

        if (payloadLen > 1024 * 1024 * 10) {
            Log("[WS] Payload too large: %llu", payloadLen);
            break;
        }

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
            Log("[WS] Server closed connection");
            running = false;
            break;
        }

        if (opcode == 0x1 && onMessage) {
            std::string msg((char*)payload.data(), payload.size());
            Log("[WS] Received TEXT: %s", msg.c_str());
            onMessage(msg);
        } else if (opcode == 0x2) {
            Log("[WS] Received BINARY: %llu bytes", payloadLen);
        }
    }

    state = WebSocketState::Disconnected;
    if (onStateChange) onStateChange(state);
    Log("[WS] Receive thread ended");
}

void NetworkManager::Disconnect() {
    running = false;

    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (receiveThread.joinable()) receiveThread.join();

    WSACleanup();
    state = WebSocketState::Disconnected;
    if (onStateChange) onStateChange(state);
    Log("[WS] Disconnected");
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
        Log("[WS] SendFrame failed: %d", WSAGetLastError());
        return false;
    }

    return true;
}

bool NetworkManager::SendText(const std::string& message) {
    if (state != WebSocketState::Connected) return false;
    return SendFrame(0x1, (const uint8_t*)message.c_str(), message.length());
}

bool NetworkManager::SendBinary(const uint8_t* data, size_t len) {
    if (state != WebSocketState::Connected) return false;
    return SendFrame(0x2, data, len);
}

bool NetworkManager::SendVideoFrame(const VideoFrame& frame) {
    std::vector<uint8_t> buffer(16 + frame.data.size());

    *(int*)buffer.data() = frame.width;
    *(int*)(buffer.data() + 4) = frame.height;
    *(double*)(buffer.data() + 8) = frame.timestamp;
    memcpy(buffer.data() + 16, frame.data.data(), frame.data.size());

    bool result = SendBinary(buffer.data(), buffer.size());
    Log("[WS] SendVideoFrame: %dx%d, %d bytes, result=%d", frame.width, frame.height, buffer.size(), result);

    return result;
}
