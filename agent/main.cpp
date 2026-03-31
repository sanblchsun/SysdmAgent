#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <thread>
#include <vector>
#include <objbase.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include "NetworkManager.h"

#define SERVER_HOST "192.168.88.127"
#define SERVER_PORT 8000
#define AGENT_ID "agent1"
#define TARGET_FPS 5
#define JPEG_QUALITY 70

using namespace Gdiplus;

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
        
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 0);
        IStream* pStream = NULL;
        CreateStreamOnHGlobal(hMem, TRUE, &pStream);
        
        EncoderParameters encoderParams;
        encoderParams.Count = 1;
        encoderParams.Parameter[0].Guid = EncoderQuality;
        encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
        encoderParams.Parameter[0].NumberOfValues = 1;
        ULONG quality = JPEG_QUALITY;
        encoderParams.Parameter[0].Value = &quality;
        
        bitmap.Save(pStream, &jpgClsid, &encoderParams);
        
        STATSTG stat;
        pStream->Stat(&stat, STATFLAG_DEFAULT);
        SIZE_T size = stat.cbSize.QuadPart;
        
        LARGE_INTEGER pos;
        pos.QuadPart = 0;
        pStream->Seek(pos, STREAM_SEEK_SET, NULL);
        
        std::vector<uint8_t> jpegData((size_t)size);
        pStream->Read(jpegData.data(), (ULONG)size, NULL);
        
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

int main() {
    printf("=== Screen Capture WebSocket Agent ===\n");
    
    NetworkManager net;
    net.SetMessageCallback([](const std::string& msg) { 
        printf("[MSG] %s\n", msg.c_str()); 
    });
    net.SetStateCallback([](ConnectionState state) {
        const char* stateStr = "Unknown";
        switch (state) {
            case ConnectionState::Disconnected: stateStr = "Disconnected"; break;
            case ConnectionState::Connecting: stateStr = "Connecting"; break;
            case ConnectionState::Connected: stateStr = "Connected"; break;
            case ConnectionState::Error: stateStr = "Error"; break;
        }
        printf("[STATE] %s\n", stateStr);
    });
    
    char wsPath[256];
    sprintf_s(wsPath, "/ws/agent/%s", AGENT_ID);
    
    if (!net.Connect(SERVER_HOST, SERVER_PORT, wsPath)) {
        printf("Failed to connect\n");
        return 1;
    }
    
    printf("Screen: %dx%d at %d FPS\n", 
           ScreenCapturer().GetWidth(), ScreenCapturer().GetHeight(), TARGET_FPS);
    
    ScreenCapturer capturer;
    int frameCount = 0;
    int totalBytes = 0;
    auto startTime = std::chrono::steady_clock::now();
    
    while (net.IsConnected()) {
        std::vector<uint8_t> jpegData = capturer.CaptureFrame();
        
        if (!jpegData.empty() && net.SendBinary(jpegData.data(), jpegData.size())) {
            frameCount++;
            totalBytes += (int)jpegData.size();
            printf("[Frame #%d] Sent %d bytes\n", frameCount, (int)jpegData.size());
        } else {
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / TARGET_FPS));
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
        if (elapsed >= 30) break;
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto totalSec = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
    
    printf("\n=== Summary ===\n");
    printf("Frames: %d, Bytes: %.2f MB, Duration: %lld sec, Avg FPS: %.1f\n",
           frameCount, totalBytes / 1024.0 / 1024.0, (long long)totalSec,
           totalSec > 0 ? (float)frameCount / totalSec : 0);
    
    net.Disconnect();
    return 0;
}
