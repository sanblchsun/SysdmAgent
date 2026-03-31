#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
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

    NetworkManager();
    ~NetworkManager();

    bool Connect(const std::string& host, int port, const std::string& path);
    void Disconnect();

    bool SendText(const std::string& message);
    bool SendBinary(const uint8_t* data, size_t len);

    void SetMessageCallback(MessageCallback cb);
    void SetStateCallback(StateCallback cb);

    ConnectionState GetState() const;
    bool IsConnected() const;

private:
    void ReceiveLoop();
    bool SendFrame(uint8_t opcode, const uint8_t* data, size_t len);

    SOCKET sock = INVALID_SOCKET;
    bool running = false;
    std::thread receiveThread;

    ConnectionState state = ConnectionState::Disconnected;

    MessageCallback onMessage;
    StateCallback onStateChange;

    std::string host;
    std::string path;
    int port = 0;
};
