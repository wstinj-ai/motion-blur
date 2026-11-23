// MotionBlurRT - real-time motion blur overlay for games (Windows 10 2004+ / 11)
//
// How it works: captures the screen with the DXGI Desktop Duplication API,
// blends frames over time on the GPU (frame-rate independent accumulation blur),
// and shows the result in a click-through, always-on-top overlay window that is
// excluded from capture (so it doesn't blur itself).
//
// Hotkeys:  Ctrl+Alt+Up / Down = more / less blur
//           Ctrl+Alt+B         = toggle on/off
//           Ctrl+Alt+Q         = quit
//
// Run your game in BORDERLESS WINDOWED mode (exclusive fullscreen hides the overlay).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;

static float g_strength = 0.5f;   // 0..1  (shutter length: 0 = off, 1 = ~60 ms)
static bool  g_enabled  = true;
static HWND  g_hwnd     = nullptr;

enum { HK_UP = 1, HK_DOWN, HK_TOGGLE, HK_QUIT };

static const char* kShader = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState smp : register(s0);
cbuffer CB : register(b0) { float alpha; float3 pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 PS_Blend(VSOut i) : SV_Target {
    float3 c = tex0.Sample(smp, i.uv).rgb;   // current captured frame
    float3 h = tex1.Sample(smp, i.uv).rgb;   // accumulated history
    return float4(lerp(h, c, alpha), 1);
}
float4 PS_Show(VSOut i) : SV_Target {
    return float4(tex0.Sample(smp, i.uv).rgb, 1);
}
)";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HOTKEY:
        if (w == HK_UP)   { g_strength = std::min(1.0f, g_strength + 0.05f); printf("Blur strength: %.2f\n", g_strength); }
        if (w == HK_DOWN) { g_strength = std::max(0.0f, g_strength - 0.05f); printf("Blur strength: %.2f\n", g_strength); }
        if (w == HK_TOGGLE) {
            g_enabled = !g_enabled;
            ShowWindow(h, g_enabled ? SW_SHOWNA : SW_HIDE);
            printf("Motion blur: %s\n", g_enabled ? "ON" : "OFF");
        }
        if (w == HK_QUIT) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) printf("Shader error: %s\n", (const char*)err->GetBufferPointer());
        return nullptr;
    }
    return blob;
}

int main() {
    SetProcessDPIAware();

    // ---- find primary output ----
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { printf("DXGI factory failed\n"); return 1; }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    RECT mon = {};
    for (UINT a = 0; ; a++) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0; ; o++) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od; out->GetDesc(&od);
            bool primary = od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0;
            if (!output || primary) { adapter = ad; output = out; mon = od.DesktopCoordinates; }
            if (primary) break;
        }
    }
    if (!output) { printf("No display output found\n"); return 1; }
    const int W = mon.right - mon.left, H = mon.bottom - mon.top;

    // ---- device ----
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("D3D11 device failed\n"); return 1;
    }

    // ---- overlay window ----
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MotionBlurRTOverlay";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                             wc.lpszClassName, L"MotionBlurRT", WS_POPUP,
                             mon.left, mon.top, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE))
        printf("Warning: could not exclude overlay from capture (needs Windows 10 2004+). Expect feedback.\n");
    ShowWindow(g_hwnd, SW_SHOWNA);

    RegisterHotKey(g_hwnd, HK_UP,     MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_UP);
    RegisterHotKey(g_hwnd, HK_DOWN,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DOWN);
    RegisterHotKey(g_hwnd, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B');
    RegisterHotKey(g_hwnd, HK_QUIT,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    // ---- swap chain ----
    ComPtr<IDXGIFactory2> factory2;
    factory.As(&factory2);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W; sd.Height = H;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    if (FAILED(factory2->CreateSwapChainForHwnd(dev.Get(), g_hwnd, &sd, nullptr, nullptr, &swap))) {
        printf("Swap chain failed\n"); return 1;
    }

    // ---- shaders / state ----
    ComPtr<ID3DBlob> vsb = Compile("VS", "vs_5_0"), pb = Compile("PS_Blend", "ps_5_0"), ps = Compile("PS_Show", "ps_5_0");
    if (!vsb || !pb || !ps) return 1;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psBlend, psShow;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &psBlend);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &psShow);

    D3D11_SAMPLER_DESC smd = {};
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;
    dev->CreateSamplerState(&smd, &sampler);

    struct CB { float alpha; float pad[3]; };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cbuf;
    dev->CreateBuffer(&bd, nullptr, &cbuf);

    // ---- textures ----
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> curTex;
    dev->CreateTexture2D(&td, nullptr, &curTex);
    ComPtr<ID3D11ShaderResourceView> curSRV;
    dev->CreateShaderResourceView(curTex.Get(), nullptr, &curSRV);

    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // 16-bit history avoids stuck/ghost pixels
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> histTex[2];
    ComPtr<ID3D11ShaderResourceView> histSRV[2];
    ComPtr<ID3D11RenderTargetView> histRTV[2];
    for (int i = 0; i < 2; i++) {
        dev->CreateTexture2D(&td, nullptr, &histTex[i]);
        dev->CreateShaderResourceView(histTex[i].Get(), nullptr, &histSRV[i]);
        dev->CreateRenderTargetView(histTex[i].Get(), nullptr, &histRTV[i]);
    }

    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };

    // ---- desktop duplication ----
    ComPtr<IDXGIOutputDuplication> dupl;
    auto makeDupl = [&]() -> bool {
        dupl.Reset();
        ComPtr<IDXGIOutput1> out1;
        if (FAILED(output.As(&out1))) return false;
        return SUCCEEDED(out1->DuplicateOutput(dev.Get(), &dupl));
    };
    if (!makeDupl()) { printf("Desktop duplication failed (try running on the GPU that drives your monitor)\n"); return 1; }

    printf("MotionBlurRT running.\n"
           "  Ctrl+Alt+Up/Down = blur strength   Ctrl+Alt+B = toggle   Ctrl+Alt+Q = quit\n"
           "  Use borderless-windowed mode in your game.\n"
           "Blur strength: %.2f\n", g_strength);

    int  cur = 0;                 // history index we read from
    bool first = true;
    auto last = std::chrono::steady_clock::now();

    for (;;) {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit) break;
        if (!g_enabled) { Sleep(50); first = true; continue; }

        DXGI_OUTDUPL_FRAME_INFO fi = {};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl->AcquireNextFrame(50, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {                      // access lost (resolution change, fullscreen switch, UAC...)
            Sleep(250);
            if (!makeDupl()) Sleep(500);
            first = true;
            continue;
        }
        if (fi.LastPresentTime.QuadPart == 0) { dupl->ReleaseFrame(); continue; } // cursor-only update

        ComPtr<ID3D11Texture2D> frame;
        res.As(&frame);
        ctx->CopyResource(curTex.Get(), frame.Get());
        dupl->ReleaseFrame();

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::min(dt, 0.1f);

        // frame-rate independent exponential blend: tau = shutter length
        float tau = g_strength * 0.06f + 1e-4f;
        CB cb = {};
        cb.alpha = first ? 1.0f : 1.0f - std::exp(-dt / tau);
        first = false;
        ctx->UpdateSubresource(cbuf.Get(), 0, nullptr, &cb, 0, 0);

        int w = 1 - cur;   // history we write into

        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cbuf.GetAddressOf());

        // pass 1: blend current frame into history
        ctx->OMSetRenderTargets(1, histRTV[w].GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* srvs1[2] = { curSRV.Get(), histSRV[cur].Get() };
        ctx->PSSetShaderResources(0, 2, srvs1);
        ctx->PSSetShader(psBlend.Get(), nullptr, 0);
        ctx->Draw(3, 0);

        // pass 2: show result in the overlay
        ComPtr<ID3D11Texture2D> back;
        swap->GetBuffer(0, IID_PPV_ARGS(&back));
        ComPtr<ID3D11RenderTargetView> backRTV;
        dev->CreateRenderTargetView(back.Get(), nullptr, &backRTV);
        ctx->OMSetRenderTargets(1, backRTV.GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* srvs2[2] = { histSRV[w].Get(), nullptr };
        ctx->PSSetShaderResources(0, 2, srvs2);
        ctx->PSSetShader(psShow.Get(), nullptr, 0);
        ctx->Draw(3, 0);

        ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(0, 2, none);
        ID3D11RenderTargetView* noRT = nullptr;
        ctx->OMSetRenderTargets(1, &noRT, nullptr);

        swap->Present(0, 0);
        cur = w;
    }

    UnregisterHotKey(g_hwnd, HK_UP);
    UnregisterHotKey(g_hwnd, HK_DOWN);
    UnregisterHotKey(g_hwnd, HK_TOGGLE);
    UnregisterHotKey(g_hwnd, HK_QUIT);
    return 0;
}
