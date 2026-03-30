#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "NetworkManager.h"

#pragma comment(lib, "winhttp.lib")

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
    wprintf(L"=== Test: Sending 10 messages with flush ===\n");

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

    wprintf(L"Connecting...\n");
    
    if (!network.Connect(SERVER_HOST, SERVER_PORT, wsPath)) {
        wprintf(L"Failed to connect\n");
        return 1;
    }

    wprintf(L"Connected! Sending 10 messages...\n");
    
    for (int i = 0; i < 10 && network.GetState() == WebSocketState::Connected; i++) {
        char msg[64];
        sprintf_s(msg, "{\"msg\":%d}", i);
        
        bool sent = network.SendText(msg);
        wprintf(L"Sent #%d: %s\n", i, sent ? L"OK" : L"FAIL");
        
        Sleep(200);
        network.ProcessEvents();
    }

    wprintf(L"Done.\n");
    network.Disconnect();
    return 0;
}
