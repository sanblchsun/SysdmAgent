#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>

#include "NetworkManager.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winhttp.lib")

#define SERVER_HOST L"192.168.88.127"
#define SERVER_PORT 8000
#define AGENT_ID L"agent1"

#define VIDEO_FPS 5

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    wprintf(L"%s\n", buf);
}

int main() {
    wprintf(L"=== SysdmAgent Starting ===\n");

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
    bool firstFrame = true;

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
                
                bool timeToSend = (now - lastFrameTime >= frameInterval) || firstFrame;
                
                if (firstFrame || frameCount % 60 == 0) {
                    wprintf(L"Frame #%d: %dx%d, time=%lld, elapsed=%lld, interval=%lld, send=%d\n", 
                        frameCount, desc.Width, desc.Height, 
                        (long long)info.LastPresentTime.QuadPart,
                        (long long)(now - lastFrameTime),
                        (long long)frameInterval,
                        timeToSend ? 1 : 0);
                }

                if (timeToSend) {
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(ctx->Map(tex, 0, D3D11_MAP_READ, 0, &mapped))) {
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
                        
                        ctx->Unmap(tex, 0);
                        
                        VideoFrame frame;
                        frame.width = desc.Width;
                        frame.height = desc.Height;
                        frame.timestamp = (double)now / 1000.0;
                        frame.data = rgbData;
                        
                        bool sent = network.SendVideoFrame(frame);
                        if (sent) {
                            wprintf(L"  -> Sent! (%d bytes)\n", frame.data.size());
                        } else {
                            wprintf(L"  -> Send FAILED!\n");
                        }
                        
                        lastFrameTime = now;
                        firstFrame = false;
                    }
                }
                
                tex->Release();
            }
        }
        
        dup->ReleaseFrame();
    }

    network.Disconnect();
    dup->Release();
    ctx->Release();
    device->Release();

    wprintf(L"Agent stopped.\n");
    return 0;
}
