#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <gdiplus.h>

#include "NetworkManager.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
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
    
    bool Encode(const uint8_t* rgb, int width, int height, std::vector<uint8_t>& jpegOut) {
        using namespace Gdiplus;
        
        Bitmap bitmap(width, height, PixelFormat24bppRGB);
        
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 3;
                Color c(rgb[idx], rgb[idx + 1], rgb[idx + 2]);
                bitmap.SetPixel(x, y, c);
            }
        }
        
        IStream* stream = NULL;
        if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) {
            return false;
        }
        
        CLSID jpgClsid;
        GetEncoderClsid(L"image/jpeg", &jpgClsid);
        
        EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = EncoderQuality;
        params.Parameter[0].Type = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &JPEG_QUALITY;
        
        Status st = bitmap.Save(stream, &jpgClsid, &params);
        stream->Release();
        
        if (st != Ok) {
            return false;
        }
        
        HGLOBAL hGlobal = NULL;
        stream->GetHGlobal(&hGlobal);
        if (!hGlobal) return false;
        
        SIZE_T size = GlobalSize(hGlobal);
        void* data = GlobalLock(hGlobal);
        if (data) {
            jpegOut.assign((uint8_t*)data, (uint8_t*)data + size);
            GlobalUnlock(hGlobal);
        }
        
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
    wprintf(L"=== SysdmAgent Starting ===\n");

    JpegEncoder encoder;

    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hDesk) {
        wprintf(L"Failed to open input desktop\n");
        return 1;
    }
    SetThreadDesktop(hDesk);
    CloseDesktop(hDesk);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &level, &ctx);
    if (FAILED(hr)) {
        wprintf(L"Failed to create D3D device\n");
        return 1;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
    dxgiDevice->Release();

    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    adapter->Release();

    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();

    IDXGIOutputDuplication* dup = nullptr;
    hr = output1->DuplicateOutput(device, &dup);
    output1->Release();
    if (FAILED(hr)) {
        wprintf(L"Failed to create duplication: 0x%08X\n", hr);
        device->Release();
        return 1;
    }

    ID3D11Texture2D* stagingTex = nullptr;

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
        dup->Release();
        ctx->Release();
        device->Release();
        return 1;
    }

    int frameCount = 0;
    int64_t lastFrameTime = 0;
    int64_t frameInterval = 1000 / VIDEO_FPS;
    int captureWidth = 0;
    int captureHeight = 0;

    wprintf(L"Starting capture loop (%d FPS)...\n", VIDEO_FPS);

    while (network.GetState() == WebSocketState::Connected) {
        int64_t now = GetTickCount64();
        
        DXGI_OUTDUPL_FRAME_INFO info = {};
        IDXGIResource* res = nullptr;

        hr = dup->AcquireNextFrame(1000, &info, &res);
        
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            wprintf(L"Access lost, reinitializing...\n");
            dup->Release();
            
            device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
            dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
            dxgiDevice->Release();
            adapter->EnumOutputs(0, &output);
            adapter->Release();
            output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
            output->Release();
            output1->DuplicateOutput(device, &dup);
            output1->Release();
            
            if (stagingTex) {
                stagingTex->Release();
                stagingTex = nullptr;
            }
            captureWidth = 0;
            captureHeight = 0;
            continue;
        }
        
        if (FAILED(hr)) {
            wprintf(L"AcquireNextFrame failed: 0x%08X\n", hr);
            break;
        }

        frameCount++;
        
        if (info.LastPresentTime.QuadPart != 0) {
            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
            res->Release();

            if (tex) {
                D3D11_TEXTURE2D_DESC desc;
                tex->GetDesc(&desc);
                
                if (captureWidth != desc.Width || captureHeight != desc.Height) {
                    wprintf(L"New size: %dx%d\n", desc.Width, desc.Height);
                    captureWidth = desc.Width;
                    captureHeight = desc.Height;
                    
                    if (stagingTex) {
                        stagingTex->Release();
                    }
                    
                    D3D11_TEXTURE2D_DESC stagingDesc = {};
                    stagingDesc.Width = desc.Width;
                    stagingDesc.Height = desc.Height;
                    stagingDesc.MipLevels = 1;
                    stagingDesc.ArraySize = 1;
                    stagingDesc.Format = desc.Format;
                    stagingDesc.SampleDesc = { 1, 0 };
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.BindFlags = 0;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    
                    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
                    if (FAILED(hr)) {
                        wprintf(L"Failed to create staging texture: 0x%08X\n", hr);
                        tex->Release();
                        continue;
                    }
                }
                
                ctx->CopyResource(stagingTex, tex);
                
                int64_t elapsed = (lastFrameTime == 0) ? 9999 : (now - lastFrameTime);
                bool shouldSend = elapsed >= frameInterval;
                
                if (frameCount <= 3) {
                    wprintf(L"Frame #%d: %dx%d, elapsed=%lld, send=%d\n", 
                        frameCount, desc.Width, desc.Height, elapsed, shouldSend);
                }

                if (shouldSend) {
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    hr = ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                    
                    if (FAILED(hr)) {
                        wprintf(L"  Map FAILED: 0x%08X\n", hr);
                    } else {
                        std::vector<uint8_t> rgbData(desc.Width * desc.Height * 3);
                        
                        for (int y = 0; y < (int)desc.Height; y++) {
                            uint8_t* src = (uint8_t*)mapped.pData + y * mapped.RowPitch;
                            uint8_t* dst = rgbData.data() + y * desc.Width * 3;
                            
                            if (mapped.RowPitch >= desc.Width * 4) {
                                for (int x = 0; x < (int)desc.Width; x++) {
                                    uint8_t* s = src + x * 4;
                                    dst[x * 3] = s[2];
                                    dst[x * 3 + 1] = s[1];
                                    dst[x * 3 + 2] = s[0];
                                }
                            }
                        }
                        
                        ctx->Unmap(stagingTex, 0);
                        
                        std::vector<uint8_t> jpegData;
                        if (encoder.Encode(rgbData.data(), desc.Width, desc.Height, jpegData)) {
                            VideoFrame frame;
                            frame.width = desc.Width;
                            frame.height = desc.Height;
                            frame.timestamp = (double)now / 1000.0;
                            frame.data = jpegData;
                            
                            bool sent = network.SendVideoFrame(frame);
                            if (frameCount <= 3) {
                                wprintf(L"  JPEG: %d bytes, sent=%d\n", jpegData.size(), sent);
                            }
                        }
                        
                        lastFrameTime = now;
                    }
                }
                
                tex->Release();
            }
        }
        
        dup->ReleaseFrame();
    }

    network.Disconnect();
    if (stagingTex) stagingTex->Release();
    dup->Release();
    ctx->Release();
    device->Release();

    wprintf(L"Agent stopped.\n");
    return 0;
}
