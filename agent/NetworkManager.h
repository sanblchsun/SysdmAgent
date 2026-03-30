#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <functional>
#include <vector>
#include <thread>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

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

    bool Connect(const std::wstring& host, int port, const std::wstring& path);
    void Disconnect();

    bool SendText(const std::string& message);
    bool SendBinary(const uint8_t* data, size_t len);
    bool SendVideoFrame(const VideoFrame& frame);
    bool SendVideoFrameHTTP(const VideoFrame& frame);

    void SetMessageCallback(MessageCallback cb) { onMessage = std::move(cb); }
    void SetStateCallback(StateCallback cb) { onStateChange = std::move(cb); }
    void SetVideoCallback(VideoCallback cb) { onVideo = std::move(cb); }

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
    VideoCallback onVideo;

    std::wstring host;
    std::wstring wsPath;
    int port = 0;
};
