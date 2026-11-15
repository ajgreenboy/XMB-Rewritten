// A harmless stand-in for a Steam game, used only by the Quit Game tests (audit/STEAM-QUIT.md). It is plain Win32:
// one visible window, optional helper children, and a self-exit timer so a stray copy cannot outlive a test.
//
//   --ignore-close        WM_CLOSE (and SC_CLOSE) are ignored: a stubborn title, only terminate or kill ends it
//   --children N          start N windowless helper copies of this program (launcher / anti-cheat style children)
//   --children-follow     the helpers exit by themselves when the parent is gone (otherwise they become orphans)
//   --max-seconds N       leave on its own after N seconds (default 120)
//   --title TEXT          window title
// Internal: --helper <parent pid> [--follow]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

bool g_ignoreClose = false;

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        if (g_ignoreClose) return 0;
        DestroyWindow(window);
        return 0;
    case WM_SYSCOMMAND:
        if (g_ignoreClose && (wParam & 0xFFF0) == SC_CLOSE) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool hasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) if (a == flag) return true;
    return false;
}
const wchar_t* valueOf(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == flag) return args[i + 1].c_str();
    return nullptr;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    int maxSeconds = 120;
    if (const wchar_t* v = valueOf(args, L"--max-seconds")) maxSeconds = _wtoi(v);
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(maxSeconds) * 1000;

    if (const wchar_t* parentText = valueOf(args, L"--helper")) {
        // A helper: no window, no message loop, ignores everything until the deadline (or the parent's end with --follow).
        HANDLE parent = nullptr;
        if (hasFlag(args, L"--follow")) parent = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(parentText)));
        while (GetTickCount64() < deadline) {
            if (parent && WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) break;
            Sleep(100);
        }
        return 0;
    }

    g_ignoreClose = hasFlag(args, L"--ignore-close");
    const int children = valueOf(args, L"--children") ? _wtoi(valueOf(args, L"--children")) : 0;
    if (children > 0) {
        wchar_t self[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, self, DWORD(sizeof(self) / sizeof(self[0])));
        for (int i = 0; i < children && i < 8; ++i) {
            std::wstring line = L"\"" + std::wstring(self) + L"\" --helper " + std::to_wstring(GetCurrentProcessId()) +
                                L" --max-seconds " + std::to_wstring(maxSeconds) + (hasFlag(args, L"--children-follow") ? L" --follow" : L"");
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"XmbSteamQuitStandIn";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    const wchar_t* title = valueOf(args, L"--title");
    HWND window = CreateWindowExW(0, wc.lpszClassName, title ? title : L"Quit test stand-in", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  40, 40, 320, 200, nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= deadline) return 0;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
}
