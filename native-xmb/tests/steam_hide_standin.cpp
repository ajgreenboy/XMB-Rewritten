// A harmless stand-in for steam.exe, used only by the Steam background-mode tests (audit/STEAM-HIDDEN.md). It never
// touches the real Steam: it refuses to run unless XMB_STEAM_REGISTRY_KEY names a key under
// HKEY_CURRENT_USER\Software\XmbTest\ and it writes only below that key.
//
// Environment (all optional):
//   XMB_STANDIN_RECORD       file that gets one line per start: the command line, then "pid=<pid>"
//   XMB_STANDIN_READY_MS     delay before ActiveProcess\pid is written (default 300)
//   XMB_STANDIN_USER         ActiveProcess\ActiveUser to write (default 0 = nobody signed in)
//   XMB_STANDIN_SIGNIN_MS    when set, ActiveUser becomes XMB_STANDIN_USER only this long after the pid (a user who
//                            signs in late); without it the user is written together with the pid
//   XMB_STANDIN_WINDOWS      visible top-level window titles to open, separated by '|'
//   XMB_STANDIN_MAX_SECONDS  leaves on its own after this long (default 60)
// A second copy started with "-applaunch <id>" while a client is registered acts like Steam's forwarding: it sets
// Apps\<id>\Running = 1 under the test key for XMB_STANDIN_GAME_MS (default 2500), then back to 0, and exits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

const wchar_t kAllowed[] = L"HKEY_CURRENT_USER\\Software\\XmbTest\\";

std::wstring env(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    return n > 0 && n < sizeof(buf) / sizeof(buf[0]) ? std::wstring(buf, n) : std::wstring();
}

// Key path below HKEY_CURRENT_USER, or empty when the seam is missing or not acceptable.
std::wstring testKey() {
    const std::wstring key = env(L"XMB_STEAM_REGISTRY_KEY");
    const std::wstring allowed = kAllowed;
    if (key.size() <= allowed.size() || _wcsnicmp(key.c_str(), allowed.c_str(), allowed.size()) != 0) return {};
    if (key.find(L"..") != std::wstring::npos) return {};
    return key.substr(wcslen(L"HKEY_CURRENT_USER\\"));
}

void setDword(const std::wstring& subKey, const wchar_t* name, DWORD value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(k);
}
void setText(const std::wstring& subKey, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}
DWORD getDword(const std::wstring& subKey, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v), type = 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) != ERROR_SUCCESS || type != REG_DWORD) v = 0;
    RegCloseKey(k);
    return v;
}

void record(const std::wstring& line) {
    const std::wstring path = env(L"XMB_STANDIN_RECORD");
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::wstring text = line + L"\r\n";
    // UTF-16 on disk would be awkward to read back; the command lines here are ASCII, so narrow them.
    std::string narrow;
    for (wchar_t c : text) narrow.push_back(c < 128 ? char(c) : '?');
    DWORD written = 0;
    WriteFile(f, narrow.data(), DWORD(narrow.size()), &written, nullptr);
    CloseHandle(f);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::vector<std::wstring> split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(separator, start);
        out.push_back(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const std::wstring key = testKey();
    if (key.empty()) return 3;   // never run against anything but a test key
    record(std::wstring(GetCommandLineW()) + L" pid=" + std::to_wstring(GetCurrentProcessId()));

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring launchId;
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"-applaunch") launchId = argv[i + 1];
    if (argv) LocalFree(argv);

    const std::wstring active = key + L"\\ActiveProcess";
    const DWORD registered = getDword(active, L"pid");
    if (!launchId.empty() && registered) {
        const std::wstring app = key + L"\\Apps\\" + launchId;
        const std::wstring gameMs = env(L"XMB_STANDIN_GAME_MS");
        setDword(app, L"Running", 1);
        setText(key, L"RunningAppID", launchId);
        Sleep(gameMs.empty() ? 2500 : DWORD(_wtoi(gameMs.c_str())));
        setDword(app, L"Running", 0);
        setText(key, L"RunningAppID", L"0");
        return 0;
    }

    const std::wstring readyText = env(L"XMB_STANDIN_READY_MS");
    Sleep(readyText.empty() ? 300 : DWORD(_wtoi(readyText.c_str())));
    const DWORD user = DWORD(_wtoi(env(L"XMB_STANDIN_USER").c_str()));
    const std::wstring signInText = env(L"XMB_STANDIN_SIGNIN_MS");
    setDword(active, L"ActiveUser", signInText.empty() ? user : 0);
    setDword(active, L"pid", GetCurrentProcessId());

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"XmbSteamHideStandIn";
    RegisterClassW(&wc);
    for (const auto& title : split(env(L"XMB_STANDIN_WINDOWS"), L'|'))
        if (!title.empty())
            CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 300, 160,
                            nullptr, nullptr, instance, nullptr);

    const std::wstring maxText = env(L"XMB_STANDIN_MAX_SECONDS");
    const ULONGLONG begin = GetTickCount64();
    const ULONGLONG deadline = begin + ULONGLONG(maxText.empty() ? 60 : _wtoi(maxText.c_str())) * 1000;
    bool signedLater = signInText.empty();
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!signedLater && GetTickCount64() - begin >= ULONGLONG(_wtoi(signInText.c_str()))) {
            setDword(active, L"ActiveUser", user);
            signedLater = true;
        }
        if (GetTickCount64() >= deadline) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
done:
    setDword(active, L"pid", 0);
    setDword(active, L"ActiveUser", 0);
    return 0;
}
