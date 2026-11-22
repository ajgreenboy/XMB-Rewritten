// Stand-in "game" for the in-game live overlay (audit/INGAME-OVERLAY-DESIGN.md section 7). Never shipped.
//
//   xmb-overlay-standin [--borderless | --windowed] [--seconds N] [--log FILE] [--size WxH]
//                       [--blur none|minimize|pause|black] [--frame-log FILE] [--at X,Y]
//
// --borderless (default): a monitor-sized WS_POPUP with no caption, drawn through a flip-model D3D11 swap chain, which is what a
//                        borderless-windowed D3D game looks like to GameWindowMode classification.
// --windowed:            a WS_OVERLAPPEDWINDOW of --size (default 1280x720).
// A colour field cycles every frame and a bar sweeps across it, so a desktop capture over the stand-in shows whether the game
// behind an overlay keeps running. Once a second it appends "t=<s> frames=<n> fps=<f> foreground=<0|1>" to --log, which is how
// "focus returned to the game" is measured. Exits after --seconds (default 60) or on WM_CLOSE.
//
// --blur is what the title does when it loses focus (audit/INGAME-BG-CAPTURE.md): none = keeps animating (a normal title);
// minimize = SW_MINIMIZE and stops drawing (SDL's default for fullscreen titles); pause = stops presenting, the window
// stays where it is; black = keeps presenting but only black frames. --frame-log appends "<epoch ms> r g b barX" for every
// frame presented, so a screen sampler can tell which game frame a pixel behind the XMB came from.
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
bool g_pause=false,g_black=false;
int g_blur=0;   // 1 = minimize, 2 = pause, 3 = black on focus loss
long long g_inactiveAt=0;   // epoch ms of the last focus loss (WM_ACTIVATE inactive), logged beside the frames
long long epochMs() {
    FILETIME ft;GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER v;v.LowPart=ft.dwLowDateTime;v.HighPart=ft.dwHighDateTime;
    return (long long)(v.QuadPart/10000ULL)-11644473600000LL;
}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CLOSE||message==WM_DESTROY){g_closed=true;PostQuitMessage(0);return 0;}
    if(message==WM_ACTIVATE) {
        if(LOWORD(w)==WA_INACTIVE) {
            g_inactiveAt=epochMs();
            if(g_blur==1)ShowWindow(window,SW_MINIMIZE);
            if(g_blur==2)g_pause=true;
            if(g_blur==3)g_black=true;
        } else {g_pause=false;g_black=false;}
    }
    return DefWindowProcW(window,message,w,l);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    POINT atPoint{0,0};   // --at X,Y: the monitor holding this virtual-desktop point (default: the primary one)
    bool borderless=true;int seconds=60,width=1280,height=720;std::wstring logPath,frameLogPath;
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i<argc;++i) {
        const std::wstring a=argv[i];
        if(a==L"--windowed")borderless=false;
        else if(a==L"--borderless")borderless=true;
        else if(a==L"--seconds"&&i+1<argc)seconds=_wtoi(argv[++i]);
        else if(a==L"--log"&&i+1<argc)logPath=argv[++i];
        else if(a==L"--size"&&i+1<argc)swscanf_s(argv[++i],L"%dx%d",&width,&height);
        else if(a==L"--frame-log"&&i+1<argc)frameLogPath=argv[++i];
        else if(a==L"--at"&&i+1<argc)swscanf_s(argv[++i],L"%ld,%ld",&atPoint.x,&atPoint.y);
        else if(a==L"--blur"&&i+1<argc) {
            const std::wstring b=argv[++i];
            g_blur=b==L"minimize"?1:b==L"pause"?2:b==L"black"?3:0;
        }
    }
    SetProcessDPIAware();
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=instance;wc.lpszClassName=L"XmbOverlayStandin";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    RegisterClassW(&wc);
    HWND window=nullptr;
    if(borderless) {
        const HMONITOR monitor=MonitorFromPoint(atPoint,MONITOR_DEFAULTTONEAREST);
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
    FILE* frameLog=nullptr;if(!frameLogPath.empty())frameLog=_wfsopen(frameLogPath.c_str(),L"a",_SH_DENYNO);
    SetForegroundWindow(window);
    if(log){std::fprintf(log,"started pid=%lu\n",GetCurrentProcessId());std::fflush(log);}
    const ULONGLONG start=GetTickCount64();ULONGLONG lastLog=start;unsigned frames=0,logFrames=0;
    MSG msg{};
    while(!g_closed) {
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)g_closed=true;TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(g_closed)break;
        const ULONGLONG now=GetTickCount64();
        if(now-start>ULONGLONG(seconds)*1000)break;
        if(frameLog && g_inactiveAt) {std::fprintf(frameLog,"# inactive %lld\n",g_inactiveAt);std::fflush(frameLog);g_inactiveAt=0;}
        if(g_pause||IsIconic(window)){Sleep(16);continue;}   // a title that stops drawing on focus loss
        ComPtr<ID3D11Texture2D> buffer;ComPtr<ID3D11RenderTargetView> view;
        chain->GetBuffer(0,IID_PPV_ARGS(&buffer));device->CreateRenderTargetView(buffer.Get(),nullptr,&view);
        const float t=float(now-start)/1000.f;
        const float field[4]={.5f+.3f*float(sin(t*5.1)),.5f+.3f*float(sin(t*4.3+1)),.5f+.3f*float(sin(t*3.7+2)),1.f};   // fast, large swings: a probe 400 ms apart always sees a change, even under an 80% dim
        context->ClearRenderTargetView(view.Get(),field);
        const float bar[4]={1.f,.85f,.1f,1.f};
        const int x=int((now-start)/4%ULONGLONG(width));
        const D3D11_RECT rect{LONG(x),0,LONG(std::min(width,x+60)),LONG(height)};
        if(context1)context1->ClearView(view.Get(),bar,&rect,1);
        if(g_black){const float black[4]={0,0,0,1};context->ClearRenderTargetView(view.Get(),black);}
        chain->Present(1,0);
        if(frameLog) {
            if(g_black)std::fprintf(frameLog,"%lld 0 0 0 %d\n",epochMs(),x);
            else std::fprintf(frameLog,"%lld %d %d %d %d\n",epochMs(),int(field[0]*255.f+.5f),int(field[1]*255.f+.5f),int(field[2]*255.f+.5f),x);
        }
        ++frames;++logFrames;
        if(now-lastLog>=1000) {
            if(log){
                std::fprintf(log,"t=%.1f frames=%u fps=%.1f foreground=%d size=%dx%d\n",t,frames,logFrames*1000.0/double(now-lastLog),GetForegroundWindow()==window?1:0,width,height);
                std::fflush(log);
            }
            lastLog=now;logFrames=0;
            if(frameLog)std::fflush(frameLog);
        }
    }
    if(log)std::fclose(log);
    if(frameLog)std::fclose(frameLog);
    return 0;
}
