#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <functional>
#include <vector>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

enum class WebSocketState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

struct VideoFrame {
    int width;
    int height;
    std::vector<uint8_t> data;
    double timestamp;
};

class NetworkManager {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StateCallback = std::function<void(WebSocketState state)>;
    using VideoCallback = std::function<void(const VideoFrame& frame)>;

    NetworkManager();
    ~NetworkManager();

    bool Connect(const std::string& host, int port, const std::string& path);
    void Disconnect();

    bool SendText(const std::string& message);
    bool SendBinary(const uint8_t* data, size_t len);
    bool SendVideoFrame(const VideoFrame& frame);

    void SetMessageCallback(MessageCallback cb) { onMessage = std::move(cb); }
    void SetStateCallback(StateCallback cb) { onStateChange = std::move(cb); }
    void SetVideoCallback(VideoCallback cb) { onVideo = std::move(cb); }

    WebSocketState GetState() const { return state; }

private:
    void ReceiveLoop();
    bool SendFrame(uint8_t opcode, const uint8_t* data, size_t len);

    SOCKET sock = INVALID_SOCKET;
    bool running = false;
    std::thread receiveThread;

    WebSocketState state = WebSocketState::Disconnected;

    MessageCallback onMessage;
    StateCallback onStateChange;
    VideoCallback onVideo;

    std::string host;
    std::string wsPath;
    int port = 0;
};
