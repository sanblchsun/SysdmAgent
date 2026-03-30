#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <gdiplus.h>
#include <objbase.h>

#include "NetworkManager.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")

#define SERVER_HOST L"192.168.88.127"
#define SERVER_PORT 8000
#define AGENT_ID L"agent1"

#define VIDEO_FPS 5
#define JPEG_QUALITY 70

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    wprintf(L"%s\n", buf);
}

class JpegEncoder {
public:
    JpegEncoder() : gdiplusToken(0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&gdiplusToken, &input, NULL);
    }
    
    ~JpegEncoder() {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
    
    bool CaptureScreen(int& width, int& height, std::vector<uint8_t>& jpegOut) {
        using namespace Gdiplus;
        
        HWND hwnd = GetDesktopWindow();
        HDC hdcScreen = GetDC(hwnd);
        if (!hdcScreen) return false;
        
        int screenW = GetDeviceCaps(hdcScreen, HORZRES);
        int screenH = GetDeviceCaps(hdcScreen, VERTRES);
        
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, screenW, screenH);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
        
        BitBlt(hdcMem, 0, 0, screenW, screenH, hdcScreen, 0, 0, SRCCOPY);
        
        Bitmap bitmap(hBitmap, NULL);
        
        SelectObject(hdcMem, hOldBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcScreen);
        DeleteObject(hBitmap);
        
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 0);
        if (!hMem) return false;
        
        IStream* stream = NULL;
        HRESULT hr = CreateStreamOnHGlobal(hMem, FALSE, &stream);
        if (FAILED(hr)) {
            GlobalFree(hMem);
            return false;
        }
        
        CLSID jpgClsid;
        GetEncoderClsid(L"image/jpeg", &jpgClsid);
        
        EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = EncoderQuality;
        params.Parameter[0].Type = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        ULONG quality = JPEG_QUALITY;
        params.Parameter[0].Value = &quality;
        
        Status st = bitmap.Save(stream, &jpgClsid, &params);
        
        if (st == Ok) {
            SIZE_T size = GlobalSize(hMem);
            void* data = GlobalLock(hMem);
            if (data) {
                jpegOut.assign((uint8_t*)data, (uint8_t*)data + size);
                GlobalUnlock(hMem);
            }
        }
        
        stream->Release();
        GlobalFree(hMem);
        
        width = screenW;
        height = screenH;
        
        return !jpegOut.empty();
    }
    
private:
    ULONG_PTR gdiplusToken;
    
    int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
        using namespace Gdiplus;
        UINT num = 0, size = 0;
        GetImageEncodersSize(&num, &size);
        if (size == 0) return -1;
        
        ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
        if (!pImageCodecInfo) return -1;
        
        GetImageEncoders(num, size, pImageCodecInfo);
        
        for (UINT i = 0; i < num; i++) {
            if (wcscmp(pImageCodecInfo[i].MimeType, format) == 0) {
                *pClsid = pImageCodecInfo[i].Clsid;
                free(pImageCodecInfo);
                return i;
            }
        }
        
        free(pImageCodecInfo);
        return -1;
    }
};

int main() {
    wprintf(L"=== SysdmAgent Starting (GDI Capture) ===\n");

    JpegEncoder encoder;

    NetworkManager network;
    std::wstring wsPath = std::wstring(L"/ws/agent/") + AGENT_ID;
    
    network.SetMessageCallback([&](const std::string& msg) {
        wprintf(L"[WS] Received: %S\n", msg.c_str());
    });
    
    network.SetStateCallback([&](WebSocketState state) {
        switch (state) {
            case WebSocketState::Connected:
                wprintf(L"[WS] Connected!\n");
                network.SendText("{\"type\":\"agent_ready\",\"width\":1920,\"height\":1080}");
                break;
            case WebSocketState::Disconnected:
                wprintf(L"[WS] Disconnected\n");
                break;
            case WebSocketState::Error:
                wprintf(L"[WS] Error\n");
                break;
            default:
                break;
        }
    });

    wprintf(L"Connecting to %s:%d%s...\n", SERVER_HOST, SERVER_PORT, wsPath.c_str());
    
    if (!network.Connect(SERVER_HOST, SERVER_PORT, wsPath)) {
        wprintf(L"Failed to connect\n");
        return 1;
    }

    int frameCount = 0;
    int64_t lastFrameTime = 0;
    int64_t frameInterval = 1000 / VIDEO_FPS;

    wprintf(L"Starting capture loop (%d FPS)...\n", VIDEO_FPS);

    while (network.GetState() == WebSocketState::Connected) {
        int64_t now = GetTickCount64();
        int64_t elapsed = (lastFrameTime == 0) ? 9999 : (now - lastFrameTime);
        
        if (elapsed >= frameInterval) {
            int width, height;
            std::vector<uint8_t> jpegData;
            
            if (encoder.CaptureScreen(width, height, jpegData)) {
                VideoFrame frame;
                frame.width = width;
                frame.height = height;
                frame.timestamp = (double)now / 1000.0;
                frame.data = jpegData;
                
                bool sent = network.SendVideoFrame(frame);
                
                if (frameCount <= 5 || frameCount % 50 == 0) {
                    wprintf(L"Frame #%d: %dx%d, JPEG=%zdb, sent=%d\n", 
                        frameCount, width, height, jpegData.size(), sent);
                }
                
                lastFrameTime = now;
            }
            frameCount++;
        } else {
            Sleep(10);
        }
    }

    network.Disconnect();
    wprintf(L"Agent stopped.\n");
    return 0;
}
