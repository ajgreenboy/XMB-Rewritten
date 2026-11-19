// A fake steamcmd.exe for tests/steamcmd_tests.cpp. It is NOT Valve's program and never talks to Steam: it replays the
// console behaviour the XMB has to cope with, chosen by the user name on its command line and by the app id:
//
//   prompts written WITHOUT a trailing newline, in two writes with a pause (so a reader must match a partial buffer),
//   progress redrawn with bare carriage returns, an email Steam Guard prompt, an app code prompt, the "confirm in the
//   phone app" wait, a wrong password, a wrong code that is asked again, a rate limit, a download with progress, a
//   failure, a download that never ends (for cancel), and a child that echoes the password back (to test scrubbing).
//
// The texts it prints are the ones the audit (audit/STEAMCMD.md) lists as sourced or assumed. The accepted
// credentials are compile-time constants below, so the secret never has to travel through this program's
// arguments or environment: the test scans the arguments and the environment this program records.
//
// XMB_STEAMCMD_STANDIN_CAPTURE=<file>: appends the argument list, every environment variable, and what arrived on stdin
// (length and verdict only, never the value) so the test can assert on them.
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* const kPassword = "Tr0ub4dor&3-standin";
static const char* const kEmailCode = "AB12C";
static const char* const kAppCode = "7K3PQ";

static FILE* g_capture = nullptr;

static void out(const char* text) { std::fwrite(text, 1, std::strlen(text), stdout); std::fflush(stdout); }
static void outSlow(const char* a, const char* b) { out(a); Sleep(120); out(b); }
// Writes without flushing: because stdout is a pipe (not a console) the C runtime fully buffers it, so this
// text stays in the process's own buffer until it fills or the process exits. Reproduces the gap where a real
// SteamCMD prompt written without a trailing newline and without a flush never reaches the reader's pipe.
static void outNoFlush(const char* text) { std::fwrite(text, 1, std::strlen(text), stdout); }
static void note(const std::string& s) { if (g_capture) { std::fwrite(s.data(), 1, s.size(), g_capture); std::fputc('\n', g_capture); std::fflush(g_capture); } }

static std::string readLine() {
    std::string s;
    int c;
    while ((c = std::fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (c != '\r') s.push_back(char(c));
    }
    return s;
}

static std::string wideToUtf8(const wchar_t* w, size_t n) {
    if (!n) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(n), s.data(), len, nullptr, nullptr);
    return s;
}

static void writeManifest(const std::string& dir, const std::string& appId) {
    std::string base = dir;
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    const size_t cut = base.find_last_of("\\/");
    const std::string installdir = cut == std::string::npos ? base : base.substr(cut + 1);
    const std::string steamapps = base + "\\steamapps";
    // CreateDirectory is not recursive; the test makes <base> itself under a scratch library.
    CreateDirectoryA(base.c_str(), nullptr);
    CreateDirectoryA(steamapps.c_str(), nullptr);
    const std::string file = steamapps + "\\appmanifest_" + appId + ".acf";
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
        std::fprintf(f, "\"AppState\"\n{\n\t\"appid\"\t\t\"%s\"\n\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"%s\"\n}\n", appId.c_str(), installdir.c_str());
        std::fclose(f);
    }
    std::string payload = base + "\\payload.bin";
    if (FILE* f = std::fopen(payload.c_str(), "wb")) { std::fputs("standin", f); std::fclose(f); }
}

// The shared prompt exchange. Returns true when the account is signed in.
static bool signIn(const std::string& user) {
    char banner[256];
    std::snprintf(banner, sizeof banner, "Logging in user '%s' to Steam Public...", user.c_str());
    out(banner);
    Sleep(60);
    if (user == "ratelimit_user") { out("FAILED (Rate Limit Exceeded)\r\n"); return false; }
    if (user == "buffered_user") {
        outNoFlush("\r\npassword: ");   // held in the full-buffered pipe; never reaches the reader
        readLine();                     // blocks forever; the reader's prompt timeout kills this process
        return false;
    }
    if (user == "buffered_guard_user") {
        outSlow("\r\npass", "word: ");  // the password prompt flushes, so the reader sees and answers it
        const std::string pw = readLine();
        note(std::string("stdin: password length=") + std::to_string(pw.size()) + (pw == kPassword ? " match=1" : " match=0"));
        if (pw != kPassword) { out("FAILED (Invalid Password)\r\n"); return false; }
        out("\r\nThis computer has not been authenticated for your account using Steam Guard.\r\n"
            "Please check your email for the message from Steam, and enter the Steam Guard code from that message.\r\n");
        outNoFlush("Steam Guard code:"); // the guard prompt stays in the full-buffered pipe; never reaches the reader
        readLine();                      // blocks forever; the re-armed prompt timeout kills this process
        return false;
    }
    outSlow("\r\npass", "word: ");
    const std::string pw = readLine();
    note(std::string("stdin: password length=") + std::to_string(pw.size()) + (pw == kPassword ? " match=1" : " match=0"));
    if (user == "echo_user") { out("You typed: "); out(pw.c_str()); out("\r\n"); }
    if (pw != kPassword) { out("FAILED (Invalid Password)\r\n"); return false; }
    if (user == "guard_user" || user == "guard_retry_user") {
        out("\r\nThis computer has not been authenticated for your account using Steam Guard.\r\n"
            "Please check your email for the message from Steam, and enter the Steam Guard code from that message.\r\n");
        for (int attempt = 0; attempt < 2; ++attempt) {
            outSlow("Steam Gu", "ard code:");
            const std::string code = readLine();
            note(std::string("stdin: code length=") + std::to_string(code.size()) + (code == kEmailCode ? " match=1" : " match=0"));
            if (code == kEmailCode) break;
            if (user == "guard_user" || attempt == 1) { out("FAILED (Invalid Login Auth Code)\r\n"); return false; }
            out("\r\nInvalid Login Auth Code\r\n");
        }
    } else if (user == "app_user") {
        outSlow("\r\nTwo-fact", "or code:");
        const std::string code = readLine();
        note(std::string("stdin: code length=") + std::to_string(code.size()) + (code == kAppCode ? " match=1" : " match=0"));
        if (code != kAppCode) { out("FAILED (Two-factor code mismatch)\r\n"); return false; }
    } else if (user == "mobile_user" || user == "mobile_timeout_user") {
        out("\r\nThis account is protected by a Steam Guard mobile authenticator.\r\n"
            "Please confirm the login in the Steam Mobile app on your phone.\r\n");
        for (int i = 0; i < 5; ++i) { out("Waiting for confirmation...\r"); Sleep(100); }
        if (user == "mobile_timeout_user") {
            out("Waiting for confirmation...Retrying...\r\nWait for confirmation timed out.\r\nTimed out waiting for confirmation.\r\nERROR (Timeout)\r\n");
            return false;
        }
    }
    out("OK\r\nWaiting for client config...OK\r\nWaiting for user info...OK\r\n");
    return true;
}

int main(int argc, char** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    if (const char* cap = std::getenv("XMB_STEAMCMD_STANDIN_CAPTURE")) g_capture = std::fopen(cap, "ab");
    std::string user, appId, forceDir;
    bool validate = false, noPrompt = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        note("arg: " + a);
        if (a == "+login" && i + 1 < argc) user = argv[++i];
        else if (a == "+app_update" && i + 1 < argc) appId = argv[++i];
        else if (a == "+force_install_dir" && i + 1 < argc) forceDir = argv[++i];
        else if (a == "validate") validate = true;
        else if (a == "+@NoPromptForPassword" && i + 1 < argc) noPrompt = std::string(argv[i + 1]) == "1";
    }
    if (g_capture) {
        wchar_t* env = GetEnvironmentStringsW();
        for (wchar_t* p = env; p && *p; p += wcslen(p) + 1) note("env: " + wideToUtf8(p, wcslen(p)));
        FreeEnvironmentStringsW(env);
    }
    if (user.empty()) { out("No login given.\r\n"); return 2; }

    if (appId.empty()) return signIn(user) ? 0 : 5;

    // ---- a job on the cached login ----
    if (user == "stale_user" && noPrompt) { out("Logging in user 'stale_user' to Steam Public...FAILED (No cached credentials and @NoPromptForPassword is set)\r\n"); return 5; }
    if (user == "prompt_user") { out("Logging in user 'prompt_user' to Steam Public...\r\npassword:"); Sleep(5000); return 5; }
    if (user == "ratelimit_user") { out("Logging in user 'ratelimit_user' to Steam Public...FAILED (Rate Limit Exceeded)\r\n"); return 5; }
    char banner[256];
    std::snprintf(banner, sizeof banner, "Logging in user '%s' to Steam Public...OK\r\nWaiting for client config...OK\r\nWaiting for user info...OK\r\n", user.c_str());
    out(banner);
    if (appId == "481") { out(" Update state (0x3) reconfiguring, progress: 0.00 (0 / 0)\rERROR! Failed to install app '481' (No subscription)\r\n"); return 8; }
    if (appId == "483") { out("Error! App '483' state is 0x202 after update job.\r\n"); return 8; }
    out(validate ? " Update state (0x5) validating, progress: 10.00 (100 / 1000)\r" : " Update state (0x3) reconfiguring, progress: 0.00 (0 / 0)\r");
    Sleep(60);
    if (appId == "482") {   // never finishes
        for (int i = 0; i < 6000; ++i) { out(" Update state (0x61) downloading, progress: 1.11 (11 / 1000)\r"); Sleep(100); }
        return 0;
    }
    char line[200];
    for (int pct = 25; pct <= 100; pct += 25) {
        std::snprintf(line, sizeof line, " Update state (0x61) downloading, progress: %d.00 (%d / 1000)\r", pct, pct * 10);
        out(line);
        Sleep(60);
    }
    out(" Update state (0x101) committing, progress: 100.00 (1000 / 1000)\r");
    if (!forceDir.empty()) writeManifest(forceDir, appId);
    std::snprintf(line, sizeof line, "\r\nSuccess! App '%s' fully installed.\r\n", appId.c_str());
    out(line);
    return 0;
}
