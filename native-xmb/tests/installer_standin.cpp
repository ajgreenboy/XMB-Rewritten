// A harmless, synthetic stand-in for an Inno Setup based repack installer, used only by the Installing panel tests
// (xmb-installer-panel-tests, work/installer-panel-e2e.ts). It is NOT a real installer and never downloads or runs
// anything: it opens one visible window, writes an Inno-style /LOG file and some junk files into /DIR=, and leaves.
//
// Arguments it understands (the same switches the XMB passes to a real installer): /DIR="<folder>" /LOG="<file>"
// /LANG=<x> /NORESTART /SP-; the rest is ignored. Behaviour comes from the environment so the exact command line the
// XMB builds stays untouched:
//   XMB_STANDIN_MODE      success (default) | fail | nofiles | stubborn
//        success   writes files, exits 0; WM_CLOSE cancels it politely (log "Rolling back changes.", exit code 5)
//        fail      writes some files, logs an exception, exits 2
//        nofiles   exits 0 without writing anything (a success exit code with nothing to show for it)
//        stubborn  ignores WM_CLOSE; starts one windowless helper child; only termination ends it
//   XMB_STANDIN_SECONDS   run time (default 10)
//   XMB_STANDIN_PERCENT   0 = do not write "Extracting ... NN%" log lines (default 1)
// Internal: --helper <seconds>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <share.h>
#include <cwchar>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::wstring g_dir, g_log, g_mode = L"success";
int g_totalSteps = 40, g_step = 0;
bool g_percent = true, g_stubborn = false, g_done = false;
HWND g_window = nullptr;
FILE* g_logFile = nullptr;

std::wstring env(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buffer[512];
    const DWORD n = GetEnvironmentVariableW(name, buffer, DWORD(sizeof(buffer) / sizeof(buffer[0])));
    return (n > 0 && n < sizeof(buffer) / sizeof(buffer[0])) ? std::wstring(buffer, n) : std::wstring(fallback);
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void logLine(const std::string& text) {
    if (!g_logFile) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d   %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds, text.c_str());
    std::fflush(g_logFile);
}

void ensureDir(const std::wstring& dir) {
    // Create every missing level; a failure to create is left to the file writes to expose.
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
}

void writeFile(const std::wstring& path, size_t bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<char> block(bytes, 'x');
    DWORD written = 0;
    WriteFile(h, block.data(), DWORD(block.size()), &written, nullptr);
    CloseHandle(h);
}

void finish(int code) {
    g_done = true;
    if (g_logFile) { logLine("Log closed."); std::fclose(g_logFile); g_logFile = nullptr; }
    ExitProcess(UINT(code));
}

void cancelPolitely() {
    logLine("Rolling back changes.");
    logLine("Setup was cancelled by the user (fixture).");
    finish(5);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        if (g_stubborn) return 0;
        cancelPolitely();
        return 0;
    case WM_SYSCOMMAND:
        if (g_stubborn && (wParam & 0xFFF0) == SC_CLOSE) return 0;
        break;
    case WM_TIMER: {
        if (g_done) return 0;
        if (g_mode == L"nofiles") { logLine("Nothing was installed (fixture)."); finish(0); }
        ++g_step;
        if (g_step == 1) logLine("-- Language entry --");
        const int file = (g_step - 1) / 4;
        if ((g_step - 1) % 4 == 0) {
            const std::wstring name = g_dir + L"\\data\\pack_" + std::to_wstring(file) + L".bin";
            logLine("-- File entry --");
            logLine("Dest filename: " + utf8(name));
        }
        writeFile(g_dir + L"\\data\\pack_" + std::to_wstring(file) + L".bin", size_t(g_step) * 16384);
        if (g_percent) logLine("Extracting data: " + std::to_string(g_step * 100 / g_totalSteps) + "%");
        if (g_mode == L"fail" && g_step >= g_totalSteps / 2) {
            logLine("Exception message: Synthetic failure (fixture)");
            logLine("Rolling back changes.");
            finish(2);
        }
        if (g_step >= g_totalSteps) {
            writeFile(g_dir + L"\\SyntheticGame.exe", 64);
            logLine("-- Icon entry --");
            logLine("Installation process succeeded.");
            finish(0);
        }
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT r;
        GetClientRect(window, &r);
        SetBkMode(dc, TRANSPARENT);
        const std::wstring head = L"Synthetic installer (XMB test fixture)";
        TextOutW(dc, 16, 14, head.c_str(), int(head.size()));
        const std::wstring text = L"Step " + std::to_wstring(g_step) + L" of " + std::to_wstring(g_totalSteps) + L"   (" + g_mode + L")";
        TextOutW(dc, 16, 40, text.c_str(), int(text.size()));
        RECT bar{16, 70, r.right - 16, 94};
        FrameRect(dc, &bar, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        RECT fill{17, 71, 17 + (r.right - 34) * g_step / (g_totalSteps ? g_totalSteps : 1), 93};
        FillRect(dc, &fill, reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::wstring argValue(const std::wstring& arg, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    if (arg.size() < n || _wcsnicmp(arg.c_str(), prefix, n) != 0) return {};
    std::wstring v = arg.substr(n);
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
    return v;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    if (args.size() >= 2 && args[0] == L"--helper") {
        const ULONGLONG until = GetTickCount64() + ULONGLONG(_wtoi(args[1].c_str())) * 1000;
        while (GetTickCount64() < until) Sleep(100);
        return 0;
    }

    for (const auto& a : args) {
        if (auto v = argValue(a, L"/DIR="); !v.empty()) g_dir = v;
        else if (auto l = argValue(a, L"/LOG="); !l.empty()) g_log = l;
    }
    g_mode = env(L"XMB_STANDIN_MODE", L"success");
    g_stubborn = g_mode == L"stubborn";
    g_percent = env(L"XMB_STANDIN_PERCENT", L"1") != L"0";
    const int seconds = std::max(1, _wtoi(env(L"XMB_STANDIN_SECONDS", L"10").c_str()));
    g_totalSteps = seconds * 4;
    if (g_dir.empty()) return 3;   // like Inno with no /DIR: the fixture refuses to guess a place to write

    if (!g_log.empty()) {
        g_logFile = _wfsopen(g_log.c_str(), L"wb", _SH_DENYWR);   // readers allowed: the panel tails this file while it is written
        if (g_logFile) {
            logLine("Log opened. (Time zone: UTC+00:00)");
            logLine("Setup version: Inno Setup version 6.2.2 (synthetic fixture)");
            logLine("Command line: /DIR=\"" + utf8(g_dir) + "\"");
            logLine("Starting the installation process.");
            logLine("Creating directory: " + utf8(g_dir));
        }
    }
    if (g_mode != L"nofiles") ensureDir(g_dir + L"\\data");

    if (g_stubborn) {
        wchar_t self[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, self, DWORD(sizeof(self) / sizeof(self[0])));
        std::wstring line = L"\"" + std::wstring(self) + L"\" --helper " + std::to_wstring(seconds + 5);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"XmbInstallerStandIn";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Synthetic Installer - Setup (XMB test fixture)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               80, 80, 480, 170, nullptr, nullptr, instance, nullptr);
    if (!g_window) return 2;
    SetTimer(g_window, 1, 250, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
