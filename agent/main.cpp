#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main() {
    // 1. Открываем рабочий стол
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hDesk) return 1;
    SetThreadDesktop(hDesk);
    CloseDesktop(hDesk);

    // 2. Создаём D3D устройство
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &level, &ctx);
    if (FAILED(hr)) return 1;

    // 3. Получаем DXGI adapter
    IDXGIDevice* dxgiDevice = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
    dxgiDevice->Release();

    // 4. Получаем первый вывод (монитор)
    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    adapter->Release();

    // 5. Получаем IDXGIOutput1
    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();

    // 6. Создаём дубликат экрана
    IDXGIOutputDuplication* dup = nullptr;
    hr = output1->DuplicateOutput(device, &dup);
    output1->Release();
    if (FAILED(hr)) return 1;

    // 7. Цикл захвата
    while (true) {
        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource* res = nullptr;

        hr = dup->AcquireNextFrame(16, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;

        if (info.LastPresentTime.QuadPart != 0) {
            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
            res->Release();

            if (tex) {
                D3D11_TEXTURE2D_DESC desc;
                tex->GetDesc(&desc);
                printf("Frame: %dx%d\n", desc.Width, desc.Height);
                tex->Release();
            }
        }
        dup->ReleaseFrame();
        Sleep(10);
    }

    dup->Release();
    ctx->Release();
    device->Release();
    return 0;
}