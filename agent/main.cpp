#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>
#include <gdiplus.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

#define SERVER_HOST "192.168.88.127"
#define SERVER_PORT 8000
#define AGENT_ID "agent1"
#define TARGET_FPS 5
#define JPEG_QUALITY 70

using namespace Gdiplus;

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    printf("%s\n", buf);
}

class ScreenCapturer {
public:
    ScreenCapturer() : hMemDC(NULL), hMemBitmap(NULL), hOldBitmap(NULL), gdiplusToken(0) {
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
        
        HDC hDC = GetDC(NULL);
        screenWidth = GetDeviceCaps(hDC, HORZRES);
        screenHeight = GetDeviceCaps(hDC, VERTRES);
        ReleaseDC(NULL, hDC);
        
        hMemDC = CreateCompatibleDC(NULL);
        hMemBitmap = CreateCompatibleBitmap(GetDC(NULL), screenWidth, screenHeight);
        hOldBitmap = SelectObject(hMemDC, hMemBitmap);
    }
    
    ~ScreenCapturer() {
        if (hOldBitmap) SelectObject(hMemDC, hOldBitmap);
        if (hMemBitmap) DeleteObject(hMemBitmap);
        if (hMemDC) DeleteDC(hMemDC);
        if (gdiplusToken) GdiplusShutdown(gdiplusToken);
    }
    
    std::vector<uint8_t> CaptureFrame() {
        HDC hDC = GetDC(NULL);
        BitBlt(hMemDC, 0, 0, screenWidth, screenHeight, hDC, 0, 0, SRCCOPY);
        ReleaseDC(NULL, hDC);
        
        Bitmap bitmap(hMemBitmap, NULL);
        CLSID jpgClsid;
        GetEncoderClsid(L"image/jpeg", &jpgClsid);
        
        IStream* pStream = NULL;
        CreateStreamOnHGlobal(NULL, TRUE, &pStream);
        
        EncoderParameters encoderParams;
        encoderParams.Count = 1;
        encoderParams.Parameter[0].Guid = EncoderQuality;
        encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
        encoderParams.Parameter[0].NumberOfValues = 1;
        encoderParams.Parameter[0].Value = &JPEG_QUALITY;
        
        bitmap.Save(pStream, &jpgClsid, &encoderParams);
        
        SIZE_T size;
        GetStreamSize(pStream, &size);
        
        HGLOBAL hMem;
        GetHGlobalFromStream(pStream, &hMem);
        LPVOID pData = GlobalLock(hMem);
        
        std::vector<uint8_t> jpegData((uint8_t*)pData, (uint8_t*)pData + size);
        
        GlobalUnlock(hMem);
        pStream->Release();
        
        return jpegData;
    }
    
    int GetWidth() const { return screenWidth; }
    int GetHeight() const { return screenHeight; }
    
private:
    int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
        UINT num = 0, size = 0;
        Gdiplus::ImageCodecInfo* pImageCodecInfo = NULL;
        GetImageEncodersSize(&num, &size);
        if (size == 0) return -1;
        
        pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
        if (!pImageCodecInfo) return -1;
        
        GetImageEncoders(num, size, pImageCodecInfo);
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(pImageCodecInfo[i].MimeType, format) == 0) {
                *pClsid = pImageCodecInfo[i].Clsid;
                free(pImageCodecInfo);
                return i;
            }
        }
        free(pImageCodecInfo);
        return -1;
    }
    
    HDC hMemDC;
    HBITMAP hMemBitmap;
    HGDIOBJ hOldBitmap;
    int screenWidth;
    int screenHeight;
    ULONG_PTR gdiplusToken;
};

class WSClient {
public:
    WSClient() : sock(INVALID_SOCKET), running(false) {}
    
    ~WSClient() { Disconnect(); }
    
    bool Connect(const char* host, int port, const char* path) {
        if (sock != INVALID_SOCKET) Disconnect();
        
        this->host = host;
        this->path = path;
        this->port = port;
        
        Log("[WS] Connecting to %s:%d%s...", host, port, path);
        
        WSADATA wsaData;
        int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (ret != 0) { Log("[WS] WSAStartup failed: %d", ret); return false; }
        
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) { Log("[WS] socket failed"); return false; }
        
        struct addrinfo hints, *result = NULL;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        
        char portStr[16];
        sprintf_s(portStr, "%d", port);
        
        ret = getaddrinfo(host, portStr, &hints, &result);
        if (ret != 0) { Log("[WS] getaddrinfo failed: %d", ret); closesocket(sock); sock = INVALID_SOCKET; return false; }
        
        if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            Log("[WS] connect failed: %d", WSAGetLastError());
            freeaddrinfo(result);
            closesocket(sock);
            sock = INVALID_SOCKET;
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
            Log("[WS] send failed: %d", WSAGetLastError());
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }
        
        char response[1024];
        int received = recv(sock, response, sizeof(response) - 1, 0);
        if (received <= 0) { Log("[WS] recv failed"); closesocket(sock); sock = INVALID_SOCKET; return false; }
        response[received] = '\0';
        
        Log("[WS] Handshake response:\n%.*s", received, response);
        
        std::string resp(response);
        if (resp.find("101") == std::string::npos) {
            Log("[WS] Expected 101 Switching Protocols, got something else");
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }
        
        running = true;
        receiveThread = std::thread([this]() { ReceiveLoop(); });
        
        Log("[WS] Connected!");
        return true;
    }
    
    void Disconnect() {
        running = false;
        
        if (sock != INVALID_SOCKET) {
            shutdown(sock, SD_BOTH);
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        
        if (receiveThread.joinable()) receiveThread.join();
        
        WSACleanup();
        Log("[WS] Disconnected");
    }
    
    bool SendText(const char* msg) {
        if (sock == INVALID_SOCKET) return false;
        return SendFrame(0x1, (const uint8_t*)msg, strlen(msg));
    }
    
    bool SendBinary(const uint8_t* data, size_t len) {
        if (sock == INVALID_SOCKET) return false;
        return SendFrame(0x2, data, len);
    }
    
    bool SendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
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
    
    void SetMessageCallback(std::function<void(const std::string&)> cb) { onMessage = cb; }

private:
    void ReceiveLoop() {
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
        
        Log("[WS] Receive thread ended");
    }
    
    SOCKET sock;
    bool running;
    std::thread receiveThread;
    
    std::string host, path;
    int port;
    
    std::function<void(const std::string&)> onMessage;
};

int main() {
    printf("=== Screen Capture WebSocket Agent ===\n");
    
    WSClient ws;
    ws.SetMessageCallback([](const std::string& msg) { printf("[MSG] %s\n", msg.c_str()); });
    
    char wsPath[256];
    sprintf_s(wsPath, "/ws/agent/%s", AGENT_ID);
    
    if (!ws.Connect(SERVER_HOST, SERVER_PORT, wsPath)) {
        printf("Failed to connect\n");
        return 1;
    }
    
    printf("Connected! Starting screen capture at %d FPS...\n", TARGET_FPS);
    
    ScreenCapturer capturer;
    printf("Screen: %dx%d\n", capturer.GetWidth(), capturer.GetHeight());
    
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    
    int frameCount = 0;
    int totalBytes = 0;
    auto startTime = std::chrono::steady_clock::now();
    
    while (ws.SendText("{\"type\":\"start\"}")) {
        auto frameStart = std::chrono::steady_clock::now();
        
        std::vector<uint8_t> jpegData = capturer.CaptureFrame();
        
        if (!jpegData.empty()) {
            bool sent = ws.SendBinary(jpegData.data(), jpegData.size());
            if (sent) {
                frameCount++;
                totalBytes += (int)jpegData.size();
                printf("[Frame #%d] Sent %d bytes\n", frameCount, (int)jpegData.size());
            } else {
                printf("Failed to send frame\n");
                break;
            }
        }
        
        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart).count();
        int targetDelay = 1000 / TARGET_FPS;
        int delay = targetDelay - (int)elapsed;
        if (delay > 0) Sleep(delay);
        
        auto now = std::chrono::steady_clock::now();
        auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
        if (totalElapsed >= 30) break;
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto totalSec = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
    
    printf("\n=== Summary ===\n");
    printf("Frames sent: %d\n", frameCount);
    printf("Total bytes: %d (%.2f MB)\n", totalBytes, totalBytes / 1024.0 / 1024.0);
    printf("Duration: %ld seconds\n", totalSec);
    printf("Avg FPS: %.1f\n", (float)frameCount / totalSec);
    printf("Avg frame size: %d bytes\n", frameCount > 0 ? totalBytes / frameCount : 0);
    
    ws.Disconnect();
    return 0;
}
