#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <functional>
#include <thread>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

enum class WebSocketState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

class NetworkManager {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StateCallback = std::function<void(WebSocketState state)>;

    NetworkManager();
    ~NetworkManager();

    bool Connect(const std::wstring& host, int port, const std::wstring& path);
    void Disconnect();

    bool SendText(const std::string& message);

    void SetMessageCallback(MessageCallback cb) { onMessage = std::move(cb); }
    void SetStateCallback(StateCallback cb) { onStateChange = std::move(cb); }

    void ProcessEvents();
    WebSocketState GetState() const { return state; }

private:
    void ReceiveLoop();

    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    bool isWebSocket = false;
    bool running = false;

    WebSocketState state = WebSocketState::Disconnected;
    
    CRITICAL_SECTION cs;
    std::thread* receiveThread = nullptr;

    MessageCallback onMessage;
    StateCallback onStateChange;

    std::wstring host;
    std::wstring path;
    int port = 0;
};
