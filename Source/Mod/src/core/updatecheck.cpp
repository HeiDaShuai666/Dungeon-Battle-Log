// core/updatecheck.cpp -- THE UPDATE CHECK

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include "../internal.h"

// ---- UPDATE CHECK ----

static const wchar_t* kUcHost   = L"api.github.com";
static const wchar_t* kUcPath   = L"/repos/Vicorin/Blindest-Dungeon/releases/latest";
static const wchar_t* kUcUrlEnv = L"BLINDEST_DUNGEON_RELEASES_URL"; // full-URL override (testing)
static const wchar_t* kUcManifestRes = L"DDACCESS_MANIFEST";       // RCDATA stamped by package.ps1

enum { UC_IDLE = 0, UC_FOUND = 1, UC_NOTHING = 2, UC_SPOKEN = 3 };
static volatile LONG g_ucState = UC_IDLE;
static char  g_ucRemote[64];
static char  g_ucLocal[64];
static DWORD g_ucFoundTick = 0;

static std::vector<int> ucParseVersion(const std::string& s) {
    std::vector<int> out;
    size_t i = 0;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) i = 1;
    int cur = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); }
        else if (c == '.')        { out.push_back(cur); cur = 0; }
        else break;
    }
    out.push_back(cur);
    return out;
}

// True if remote is strictly newer than local (missing components read as 0).
static bool ucIsNewer(const std::string& remote, const std::string& local) {
    std::vector<int> r = ucParseVersion(remote), l = ucParseVersion(local);
    size_t n = (r.size() > l.size()) ? r.size() : l.size();
    for (size_t i = 0; i < n; ++i) {
        int rv = (i < r.size()) ? r[i] : 0;
        int lv = (i < l.size()) ? l[i] : 0;
        if (rv != lv) return rv > lv;
    }
    return false;
}

static std::string ucJsonString(const std::string& body, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' ||
                               body[p] == '\r' || body[p] == '\n')) ++p;
    if (p >= body.size() || body[p] != '"') return ""; // null or non-string
    ++p;
    std::string out;
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) ++p; // naive unescape (tags are plain)
        out += body[p++];
    }
    return out;
}

static bool ucHttpsGet(const std::wstring& host, const std::wstring& path,
                       INTERNET_PORT port, std::string& body) {
    body.clear();
    // User-Agent is REQUIRED by the GitHub API -- without it the response is 403.
    HINTERNET hSession = WinHttpOpen(L"BlindestDungeonMod",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (hConnect) {
        DWORD flags = (port == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hRequest) {
            WinHttpAddRequestHeaders(hRequest,
                L"Accept: application/vnd.github+json\r\n",
                (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0;
                    do {
                        avail = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
                        if (!avail) break;
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(hRequest, &chunk[0], avail, &read)) break;
                        body.append(chunk.data(), read);
                        if (body.size() > 512 * 1024) break; // sanity cap; the tag is near the top
                    } while (avail > 0);
                    ok = true;
                } else {
                    logLine("updatecheck: GitHub answered HTTP %lu", (unsigned long)status);
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

static std::string ucInstalledVersion() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ucInstalledVersion), &self) || !self)
        return "";
    HRSRC r = FindResourceW(self, kUcManifestRes, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return "";
    HGLOBAL g = LoadResource(self, r);
    if (!g) return "";
    const char* p = static_cast<const char*>(LockResource(g));
    DWORD n = SizeofResource(self, r);
    if (!p || !n) return "";
    return ucJsonString(std::string(p, n), "version");
}

static void ucSpokenVersion(const std::string& v, char* out, size_t outsz) {
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    strncpy(out, v.c_str() + i, outsz - 1);
    out[outsz - 1] = 0;
}

static DWORD WINAPI ucWorker(LPVOID) {
    if (!axCheckUpdates()) {
        logLine("updatecheck: off (F10 row), no request");
        InterlockedExchange(&g_ucState, UC_NOTHING);
        return 0;
    }
    std::string local = ucInstalledVersion();
    if (local.empty()) {
        logLine("updatecheck: no DDACCESS_MANIFEST in this build (dev build) -- not checking");
        InterlockedExchange(&g_ucState, UC_NOTHING);
        return 0;
    }

    std::wstring host = kUcHost, path = kUcPath;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    // Optional full-URL override for testing (the launcher had the same one).
    wchar_t envBuf[1024];
    DWORD n = GetEnvironmentVariableW(kUcUrlEnv, envBuf, 1024);
    if (n > 0 && n < 1024) {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t hostBuf[256], pathBuf[1024];
        uc.lpszHostName = hostBuf; uc.dwHostNameLength = 256;
        uc.lpszUrlPath  = pathBuf; uc.dwUrlPathLength  = 1024;
        if (WinHttpCrackUrl(envBuf, n, 0, &uc)) {
            host = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
            path = (uc.dwUrlPathLength > 0)
                 ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
            port = uc.nPort;
        }
    }

    std::string body;
    if (!ucHttpsGet(host, path, port, body)) {
        logLine("updatecheck: no answer from GitHub (offline?), staying silent");
        InterlockedExchange(&g_ucState, UC_NOTHING);
        return 0;
    }
    std::string tag = ucJsonString(body, "tag_name");
    if (tag.empty()) {
        logLine("updatecheck: release JSON carries no tag_name, staying silent");
        InterlockedExchange(&g_ucState, UC_NOTHING);
        return 0;
    }
    if (!ucIsNewer(tag, local)) {
        logLine("updatecheck: installed %s, latest %s -- up to date", local.c_str(), tag.c_str());
        InterlockedExchange(&g_ucState, UC_NOTHING);
        return 0;
    }
    logLine("updatecheck: installed %s, latest %s -- UPDATE AVAILABLE, handing to the main thread",
            local.c_str(), tag.c_str());
    ucSpokenVersion(tag,   g_ucRemote, sizeof g_ucRemote);
    ucSpokenVersion(local, g_ucLocal,  sizeof g_ucLocal);
    g_ucFoundTick = GetTickCount();
    InterlockedExchange(&g_ucState, UC_FOUND);
    return 0;
}

void axUpdateCheckStart() {
    HANDLE h = CreateThread(nullptr, 0, ucWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else logLine("updatecheck: CreateThread failed (%lu), no check this session",
                 (unsigned long)GetLastError());
}

void axUpdateCheckService() {
    if (g_ucState != UC_FOUND) return;
    if (!axLangLatched() && (GetTickCount() - g_ucFoundTick) < 30000) return;
    char line[512];
    _snprintf(line, sizeof line, axs(AXS_UPDATE_AVAILABLE_FMT), g_ucRemote, g_ucLocal);
    line[sizeof line - 1] = 0;
    postSpeech(line, false, SPK_EVENT);
    logLine("updatecheck: spoken (lang latched=%d)", axLangLatched() ? 1 : 0);
    InterlockedExchange(&g_ucState, UC_SPOKEN);
}
