// Stand-in "game" for the in-game live overlay (audit/INGAME-OVERLAY-DESIGN.md section 7). Never shipped.
//
//   xmb-overlay-standin [--borderless | --windowed] [--seconds N] [--log FILE] [--size WxH]
//
// --borderless (default): a monitor-sized WS_POPUP with no caption, drawn through a flip-model D3D11 swap chain, which is what a
//                        borderless-windowed D3D game looks like to GameWindowMode classification.
// --windowed:            a WS_OVERLAPPEDWINDOW of --size (default 1280x720).
// A colour field cycles every frame and a bar sweeps across it, so a desktop capture over the stand-in shows whether the game
// behind an overlay keeps running. Once a second it appends "t=<s> frames=<n> fps=<f> foreground=<0|1>" to --log, which is how
// "focus returned to the game" is measured. Exits after --seconds (default 60) or on WM_CLOSE.
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <share.h>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
bool g_closed=false;
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CLOSE||message==WM_DESTROY){g_closed=true;PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    bool borderless=true;int seconds=60,width=1280,height=720;std::wstring logPath;
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i<argc;++i) {
        const std::wstring a=argv[i];
        if(a==L"--windowed")borderless=false;
        else if(a==L"--borderless")borderless=true;
        else if(a==L"--seconds"&&i+1<argc)seconds=_wtoi(argv[++i]);
        else if(a==L"--log"&&i+1<argc)logPath=argv[++i];
        else if(a==L"--size"&&i+1<argc)swscanf_s(argv[++i],L"%dx%d",&width,&height);
    }
    SetProcessDPIAware();
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=instance;wc.lpszClassName=L"XmbOverlayStandin";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    RegisterClassW(&wc);
    HWND window=nullptr;
    if(borderless) {
        const HMONITOR monitor=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
        width=mi.rcMonitor.right-mi.rcMonitor.left;height=mi.rcMonitor.bottom-mi.rcMonitor.top;
        window=CreateWindowExW(0,wc.lpszClassName,L"XMB Overlay Standin",WS_POPUP|WS_VISIBLE,mi.rcMonitor.left,mi.rcMonitor.top,width,height,nullptr,nullptr,instance,nullptr);
    } else {
        window=CreateWindowExW(0,wc.lpszClassName,L"XMB Overlay Standin",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,width,height,nullptr,nullptr,instance,nullptr);
    }
    if(!window)return 2;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)))return 3;
    ComPtr<IDXGIDevice> dxgiDevice;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    device.As(&dxgiDevice);dxgiDevice->GetAdapter(&adapter);adapter->GetParent(IID_PPV_ARGS(&factory));
    RECT client{};GetClientRect(window,&client);width=client.right-client.left;height=client.bottom-client.top;
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=UINT(width);desc.Height=UINT(height);desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    if(FAILED(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&chain)))return 4;
    factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
    ComPtr<ID3D11DeviceContext1> context1;context.As(&context1);
    FILE* log=nullptr;if(!logPath.empty())log=_wfsopen(logPath.c_str(),L"a",_SH_DENYNO);
    SetForegroundWindow(window);
    if(log){std::fprintf(log,"started pid=%lu\n",GetCurrentProcessId());std::fflush(log);}
    const ULONGLONG start=GetTickCount64();ULONGLONG lastLog=start;unsigned frames=0,logFrames=0;
    MSG msg{};
    while(!g_closed) {
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)g_closed=true;TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(g_closed)break;
        const ULONGLONG now=GetTickCount64();
        if(now-start>ULONGLONG(seconds)*1000)break;
        ComPtr<ID3D11Texture2D> buffer;ComPtr<ID3D11RenderTargetView> view;
        chain->GetBuffer(0,IID_PPV_ARGS(&buffer));device->CreateRenderTargetView(buffer.Get(),nullptr,&view);
        const float t=float(now-start)/1000.f;
        const float field[4]={.5f+.3f*float(sin(t*5.1)),.5f+.3f*float(sin(t*4.3+1)),.5f+.3f*float(sin(t*3.7+2)),1.f};   // fast, large swings: a probe 400 ms apart always sees a change, even under an 80% dim
        context->ClearRenderTargetView(view.Get(),field);
        const float bar[4]={1.f,.85f,.1f,1.f};
        const int x=int((now-start)/4%ULONGLONG(width));
        const D3D11_RECT rect{LONG(x),0,LONG(std::min(width,x+60)),LONG(height)};
        if(context1)context1->ClearView(view.Get(),bar,&rect,1);
        chain->Present(1,0);
        ++frames;++logFrames;
        if(now-lastLog>=1000) {
            if(log){
                std::fprintf(log,"t=%.1f frames=%u fps=%.1f foreground=%d size=%dx%d\n",t,frames,logFrames*1000.0/double(now-lastLog),GetForegroundWindow()==window?1:0,width,height);
                std::fflush(log);
            }
            lastLog=now;logFrames=0;
        }
    }
    if(log)std::fclose(log);
    return 0;
}
