#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

class NetworkManager {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StateCallback = std::function<void(ConnectionState state)>;
    using ReconnectCallback = std::function<void(int attempt)>;

    NetworkManager();
    ~NetworkManager();

    bool Connect(const std::string& host, int port, const std::string& path);
    void Disconnect();

    bool SendText(const std::string& message);
    bool SendBinary(const uint8_t* data, size_t len);
    bool SendPing();

    void SetMessageCallback(MessageCallback cb);
    void SetStateCallback(StateCallback cb);
    void SetReconnectCallback(ReconnectCallback cb);

    void SetReconnectParams(int maxRetries, int retryDelayMs);

    ConnectionState GetState() const;
    bool IsConnected() const;

private:
    bool DoConnect();
    void ReceiveLoop();
    bool SendFrame(uint8_t opcode, const uint8_t* data, size_t len);
    void ReconnectLoop();

    bool running = false;
    std::thread receiveThread;
    std::thread reconnectThread;

    SOCKET sock = INVALID_SOCKET;

    ConnectionState state = ConnectionState::Disconnected;

    MessageCallback onMessage;
    StateCallback onStateChange;
    ReconnectCallback onReconnect;

    std::string host;
    std::string path;
    int port = 0;

    std::atomic<int> reconnectAttempt{0};
    int maxRetries = 0;
    int retryDelayMs = 5000;
    bool autoReconnect = true;
};
