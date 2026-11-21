// A harmless stand-in game for the launch cover end-to-end run (work/launch-fade-e2e.ts, audit/GAME-LAUNCH-FADE.md).
// Plain Win32 and GDI, never shipped. It records what is on top of the screen from its very first instant, so the run can
// tell whether the XMB's black cover was already up before this process existed and when it went away.
//
//   --show-after-ms N        create and show the window N ms after start (default 0)
//   --windowed WxH           a framed window of that client size (default 1280x720)
//   --borderless             a monitor-sized popup instead
//   --fullscreen-after-ms N  N ms after it is shown, switch to a monitor-sized popup (a title that goes fullscreen late)
//   --never-show             never show a window (a launcher that hands over to nothing visible)
//   --exit-after-ms N        exit with code 3 N ms after start (a title that fails early)
//   --max-seconds N          leave on its own after N seconds (default 60)
//   --log PATH               append the lines below
//
// Log lines (at = milliseconds since the Unix epoch, comparable with the XMB's "launch-cover:" lines):
//   started pid=<pid> at=<ms>
//   probe at=<ms> top="<title>" toppid=<pid> pixel=<r>,<g>,<b> foreground=<0|1>   every 50 ms, the topmost visible window
//                                                                                 over the monitor centre and its pixel
//   shown at=<ms> | fullscreen at=<ms> | exit at=<ms> code=<n>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr COLORREF kFill = RGB(0, 200, 255);   // a colour the XMB never puts in the middle of the screen
std::wstring g_log;
HWND g_window = nullptr;

long long epochMs() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((v.QuadPart - 116444736000000000ULL) / 10000ULL);
}

void logLine(const std::string& line) {
    if (g_log.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_log.c_str(), L"ab") != 0 || !f) return;
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
}

RECT primaryMonitor() {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

struct TopSearch { POINT at; HWND found; };

std::string narrow(const wchar_t* text) {
    std::string out;
    for (const wchar_t* p = text; *p; ++p) out.push_back(*p < 128 && *p != L'"' ? char(*p) : '?');
    return out;
}

// EnumWindows walks top-level windows in z-order, topmost first: the first visible, uncloaked one containing the point
// is what the user sees there (layered, click-through windows included, which WindowFromPoint would skip).
void probe() {
    const RECT m = primaryMonitor();
    TopSearch search{POINT{(m.left + m.right) / 2, (m.top + m.bottom) / 2}, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<TopSearch*>(lp);
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        RECT r{};
        if (!GetWindowRect(h, &r) || !PtInRect(&r, s->at)) return TRUE;
        s->found = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    wchar_t title[128] = L"";
    DWORD pid = 0;
    if (search.found) {
        GetWindowTextW(search.found, title, 127);
        GetWindowThreadProcessId(search.found, &pid);
    }
    COLORREF pixel = CLR_INVALID;
    if (HDC screen = GetDC(nullptr)) {
        pixel = GetPixel(screen, search.at.x, search.at.y);
        ReleaseDC(nullptr, screen);
    }
    char line[400];
    std::snprintf(line, sizeof(line), "probe at=%lld top=\"%s\" toppid=%lu pixel=%d,%d,%d foreground=%d", epochMs(),
                  narrow(title).c_str(), pid, pixel == CLR_INVALID ? -1 : GetRValue(pixel), pixel == CLR_INVALID ? -1 : GetGValue(pixel),
                  pixel == CLR_INVALID ? -1 : GetBValue(pixel), g_window && GetForegroundWindow() == g_window ? 1 : 0);
    logLine(line);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        HBRUSH brush = CreateSolidBrush(kFill);
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

const wchar_t* valueOf(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == flag) return args[i + 1].c_str();
    return nullptr;
}
bool hasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) if (a == flag) return true;
    return false;
}
long long number(const std::vector<std::wstring>& args, const wchar_t* flag, long long fallback) {
    const wchar_t* v = valueOf(args, flag);
    return v ? _wtoi64(v) : fallback;
}

void goFullscreen(HWND window) {
    const RECT m = primaryMonitor();
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(window, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    if (const wchar_t* log = valueOf(args, L"--log")) g_log = log;

    const long long start = epochMs();
    {
        char line[96];
        std::snprintf(line, sizeof(line), "started pid=%lu at=%lld", GetCurrentProcessId(), start);
        logLine(line);
    }
    probe();   // the very first look, before this process has any window

    const long long showAfter = number(args, L"--show-after-ms", 0);
    const long long fullscreenAfter = number(args, L"--fullscreen-after-ms", -1);
    const long long exitAfter = number(args, L"--exit-after-ms", -1);
    const long long maxMs = number(args, L"--max-seconds", 60) * 1000;
    const bool never = hasFlag(args, L"--never-show");
    const bool borderless = hasFlag(args, L"--borderless");
    int width = 1280, height = 720;
    if (const wchar_t* size = valueOf(args, L"--windowed")) swscanf_s(size, L"%dx%d", &width, &height);

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"XmbLaunchFadeStandin";
    RegisterClassW(&wc);

    long long shownAt = -1;
    bool fullscreen = false;
    long long nextProbe = 0;
    int code = 0;
    for (;;) {
        const long long now = epochMs() - start;
        if (now >= maxMs) break;
        if (exitAfter >= 0 && now >= exitAfter) { code = 3; break; }
        if (!never && !g_window && now >= showAfter) {
            const RECT m = primaryMonitor();
            if (borderless) {
                g_window = CreateWindowExW(0, wc.lpszClassName, L"Launch Fade Standin", WS_POPUP, m.left, m.top, m.right - m.left,
                                           m.bottom - m.top, nullptr, nullptr, instance, nullptr);
            } else {
                RECT r{0, 0, width, height};
                AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                g_window = CreateWindowExW(0, wc.lpszClassName, L"Launch Fade Standin", WS_OVERLAPPEDWINDOW,
                                           m.left + 80, m.top + 80, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
            }
            ShowWindow(g_window, SW_SHOW);
            SetForegroundWindow(g_window);
            UpdateWindow(g_window);
            shownAt = now;
            char line[64];
            std::snprintf(line, sizeof(line), "shown at=%lld", epochMs());
            logLine(line);
        }
        if (g_window && !fullscreen && fullscreenAfter >= 0 && shownAt >= 0 && now >= shownAt + fullscreenAfter) {
            goFullscreen(g_window);
            fullscreen = true;
            char line[64];
            std::snprintf(line, sizeof(line), "fullscreen at=%lld", epochMs());
            logLine(line);
        }
        if (now >= nextProbe) { probe(); nextProbe = now + 50; }
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    char line[80];
    std::snprintf(line, sizeof(line), "exit at=%lld code=%d", epochMs(), code);
    logLine(line);
    return code;
}
