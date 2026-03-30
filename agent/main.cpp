#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <objbase.h>
#include <gdiplus.h>

#include "NetworkManager.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")

#define SERVER_HOST L"192.168.88.127"
#define SERVER_PORT 8000
#define AGENT_ID L"agent1"

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
    wprintf(L"=== Test: Sending 100 small messages ===\n");

    NetworkManager network;
    std::wstring wsPath = std::wstring(L"/ws/agent/") + AGENT_ID;
    
    network.SetMessageCallback([&](const std::string& msg) {
        wprintf(L"[WS] Received: %S\n", msg.c_str());
    });
    
    network.SetStateCallback([&](WebSocketState state) {
        switch (state) {
            case WebSocketState::Connected:
                wprintf(L"[WS] Connected!\n");
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

    wprintf(L"Connected! Sending 100 text messages...\n");
    
    for (int i = 0; i < 100 && network.GetState() == WebSocketState::Connected; i++) {
        char msg[64];
        sprintf_s(msg, "{\"msg\":%d}", i);
        
        bool sent = network.SendText(msg);
        wprintf(L"Sent #%d: %s (result=%d)\n", i, sent ? L"OK" : L"FAIL", sent);
        
        Sleep(100);
        network.ProcessEvents();
    }

    wprintf(L"Done. Disconnecting...\n");
    network.Disconnect();
    
    wprintf(L"Test complete.\n");
    return 0;
}
