#include "NetworkManager.h"
#include <new>

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    wprintf(L"%s\n", buf);
}

NetworkManager::NetworkManager() {
    InitializeCriticalSection(&cs);
}

NetworkManager::~NetworkManager() {
    Disconnect();
    DeleteCriticalSection(&cs);
}

bool NetworkManager::Connect(const std::wstring& host, int port, const std::wstring& path) {
    this->host = host;
    this->port = port;
    this->path = path;

    if (state == WebSocketState::Connected || state == WebSocketState::Connecting) {
        Disconnect();
    }

    state = WebSocketState::Connecting;
    Log(L"[WS] Connecting to %s:%d%s...", host.c_str(), port, path.c_str());

    hSession = WinHttpOpen(L"SysdmAgent/1.0", 
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    
    if (!hSession) {
        Log(L"[WS] WinHttpOpen failed: %d", GetLastError());
        state = WebSocketState::Error;
        return false;
    }

    DWORD timeout = 10000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));

    hConnect = WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        Log(L"[WS] WinHttpConnect failed: %d", GetLastError());
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        state = WebSocketState::Error;
        return false;
    }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    
    if (!hRequest) {
        Log(L"[WS] WinHttpOpenRequest failed: %d", GetLastError());
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        state = WebSocketState::Error;
        return false;
    }

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        Log(L"[WS] Upgrade failed: %d", GetLastError());
        state = WebSocketState::Error;
        return false;
    }

    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) {
        Log(L"[WS] SendRequest failed: %d", GetLastError());
        state = WebSocketState::Error;
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        Log(L"[WS] ReceiveResponse failed: %d", GetLastError());
        state = WebSocketState::Error;
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

    Log(L"[WS] HTTP status: %d", statusCode);

    if (statusCode != 101) {
        Log(L"[WS] WebSocket handshake failed");
        state = WebSocketState::Error;
        return false;
    }

    isWebSocket = true;
    state = WebSocketState::Connected;
    running = true;
    
    receiveThread = new (std::nothrow) std::thread(&NetworkManager::ReceiveLoop, this);
    if (!receiveThread) {
        Log(L"[WS] Failed to create thread");
        Disconnect();
        return false;
    }

    Log(L"[WS] Connected!");
    if (onStateChange) onStateChange(state);
    return true;
}

void NetworkManager::ReceiveLoop() {
    Log(L"[WS] Receive thread started");
    
    while (running) {
        uint8_t buffer[16384];
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE msgType = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        
        HRESULT hr = WinHttpWebSocketReceive(hRequest, buffer, sizeof(buffer), &bytesRead, &msgType);
        
        if (!running) break;

        if (FAILED(hr)) {
            Log(L"[WS] Receive error: 0x%08X", hr);
            EnterCriticalSection(&cs);
            state = WebSocketState::Error;
            LeaveCriticalSection(&cs);
            if (onStateChange) onStateChange(state);
            break;
        }

        if (bytesRead == 0) {
            if (msgType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                Log(L"[WS] Server closed");
                break;
            }
            continue;
        }

        if (msgType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            EnterCriticalSection(&cs);
            MessageCallback cb = onMessage;
            LeaveCriticalSection(&cs);
            
            if (cb) {
                std::string msg((char*)buffer, bytesRead);
                cb(msg);
            }
        }
    }
    
    Log(L"[WS] Receive thread ended");
}

void NetworkManager::Disconnect() {
    EnterCriticalSection(&cs);
    running = false;
    
    if (hRequest) {
        if (isWebSocket) {
            WinHttpWebSocketShutdown(hRequest, 1000, NULL, 0);
        }
        WinHttpCloseHandle(hRequest);
        hRequest = nullptr;
    }
    if (hConnect) {
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
    }
    if (hSession) {
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
    }

    isWebSocket = false;
    state = WebSocketState::Disconnected;
    LeaveCriticalSection(&cs);

    if (receiveThread) {
        if (receiveThread->joinable()) {
            receiveThread->join();
        }
        delete receiveThread;
        receiveThread = nullptr;
    }

    if (onStateChange) onStateChange(state);
}

bool NetworkManager::SendText(const std::string& message) {
    EnterCriticalSection(&cs);
    bool canSend = (state == WebSocketState::Connected && hRequest != nullptr);
    LeaveCriticalSection(&cs);
    
    if (!canSend) return false;

    HRESULT hr = WinHttpWebSocketSend(hRequest, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, 
        (PVOID)message.c_str(), (DWORD)message.length());
    
    if (FAILED(hr)) {
        Log(L"[WS] Send failed: 0x%08X", hr);
        return false;
    }
    return true;
}

void NetworkManager::ProcessEvents() {
}
