// ==WindhawkMod==
// @id              start-menu-system-stats
// @name            Start Menu System Stats
// @description     Live data for the NET, RAM, CPU and GPU tiles of the custom Start menu. A collector inside explorer.exe measures and the Start menu draws.
// @version         0.1.0
// @author          local
// @include         StartMenuExperienceHost.exe
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -DWINRT_NO_MODULE_LOCK -lole32 -loleaut32 -lruntimeobject -lwinhttp -lpdh -ldxgi -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- networkAdapter: ""
  $name: Network adapter filter
  $description: Leave empty to add up all physical adapters. Otherwise enter part of an adapter name, for example I211 or Wi-Fi.
- gpuName: ""
  $name: GPU name filter
  $description: Leave empty to use the GPU with the most dedicated memory. Otherwise enter part of its name, for example 6800.
- lhmUrl: ""
  $name: LibreHardwareMonitor data URL (optional)
  $description: Only used for the CPU temperature. Turn on Remote Web Server in LibreHardwareMonitor and enter http://localhost:8085/data.json here. Leave empty to hide the CPU temperature.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#undef GetCurrentTime

#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <sddl.h>
#include <winhttp.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

using XPolygon = winrt::Windows::UI::Xaml::Shapes::Polygon;
using XPolyline = winrt::Windows::UI::Xaml::Shapes::Polyline;

static constexpr int kSamples = 31;     // graph points (about 30 seconds)
static constexpr double kW = 152.0;     // tile width and height in the styler XAML
static constexpr double kH = 152.0;

static bool g_isUi = false;
static HANDLE g_stopEvent = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_collectorMutex = nullptr;

// ---------------------------------------------------------------------------
// Small helpers and settings
// ---------------------------------------------------------------------------

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

static std::wstring Trim(std::wstring s) {
    auto notSpace = [](wchar_t c) { return !iswspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

struct Config {
    std::wstring networkAdapter;
    std::wstring gpuName;
    std::wstring lhmUrl;
};

static std::mutex g_cfgLock;
static Config g_cfg;
static std::atomic<int> g_cfgVersion{0};

static std::wstring ReadStringSetting(PCWSTR name) {
    PCWSTR v = Wh_GetStringSetting(name);
    std::wstring s = Trim(v ? v : L"");
    Wh_FreeStringSetting(v);
    return s;
}

static void LoadSettings() {
    Config c;
    c.networkAdapter = ReadStringSetting(L"networkAdapter");
    c.gpuName = ReadStringSetting(L"gpuName");
    c.lhmUrl = ReadStringSetting(L"lhmUrl");
    std::lock_guard<std::mutex> l(g_cfgLock);
    g_cfg = c;
    g_cfgVersion++;
}

static Config GetConfig() {
    std::lock_guard<std::mutex> l(g_cfgLock);
    return g_cfg;
}

// ---------------------------------------------------------------------------
// Shared data: what the collector measures
// ---------------------------------------------------------------------------

struct Stats {
    long long netUp = -1, netDown = -1;           // bytes per second
    long long ramUsed = -1, ramTotal = -1;        // bytes
    int ramPct10 = -1;                            // tenths of a percent
    int cpuPct10 = -1, cpuMHz = -1, cpuTemp10 = -1;
    int gpuPct10 = -1, gpuTemp10 = -1;
    long long gpuVramUsed = -1, gpuVramTotal = -1;
};

static std::string Serialize(Stats const& s) {
    char buf[640];
    snprintf(buf, sizeof(buf),
             "v=1\nt=%llu\nnetUp=%lld\nnetDown=%lld\nramUsed=%lld\nramTotal=%lld\nramPct10=%d\n"
             "cpuPct10=%d\ncpuMHz=%d\ncpuTemp10=%d\ngpuPct10=%d\ngpuVramUsed=%lld\ngpuVramTotal=%lld\ngpuTemp10=%d\n",
             (unsigned long long)GetTickCount64(), s.netUp, s.netDown, s.ramUsed, s.ramTotal, s.ramPct10,
             s.cpuPct10, s.cpuMHz, s.cpuTemp10, s.gpuPct10, s.gpuVramUsed, s.gpuVramTotal, s.gpuTemp10);
    return std::string(buf);
}

static bool ParseStats(std::string const& text, Stats& s, ULONGLONG& stamp) {
    bool any = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        long long v = strtoll(line.c_str() + eq + 1, nullptr, 10);
        if (key == "t") { stamp = (ULONGLONG)v; any = true; }
        else if (key == "netUp") s.netUp = v;
        else if (key == "netDown") s.netDown = v;
        else if (key == "ramUsed") s.ramUsed = v;
        else if (key == "ramTotal") s.ramTotal = v;
        else if (key == "ramPct10") s.ramPct10 = (int)v;
        else if (key == "cpuPct10") s.cpuPct10 = (int)v;
        else if (key == "cpuMHz") s.cpuMHz = (int)v;
        else if (key == "cpuTemp10") s.cpuTemp10 = (int)v;
        else if (key == "gpuPct10") s.gpuPct10 = (int)v;
        else if (key == "gpuVramUsed") s.gpuVramUsed = v;
        else if (key == "gpuVramTotal") s.gpuVramTotal = v;
        else if (key == "gpuTemp10") s.gpuTemp10 = (int)v;
    }
    return any;
}

// ---------------------------------------------------------------------------
// The stats file (written by the collector, read inside the Start menu sandbox)
// ---------------------------------------------------------------------------

static std::wstring StatsDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
    return base + L"\\StartMenuWidgetStats";
}

// Creates the folder with an ACL that lets sandboxed apps (ALL APPLICATION PACKAGES) read it.
static bool EnsureStatsDir() {
    std::wstring dir = StatsDir();
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;

    std::wstring userSid;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        if (len > 0) {
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
                LPWSTR str = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) {
                    userSid = str;
                    LocalFree(str);
                }
            }
        }
        CloseHandle(tok);
    }

    std::wstring sddl = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    if (!userSid.empty()) sddl += L"(A;OICI;FA;;;" + userSid + L")";
    sddl += L"(A;OICI;GRGX;;;S-1-15-2-1)";

    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &psd, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = psd;
    sa.bInheritHandle = FALSE;
    BOOL ok = CreateDirectoryW(dir.c_str(), &sa);
    DWORD err = GetLastError();
    LocalFree(psd);
    return ok || err == ERROR_ALREADY_EXISTS;
}

static bool WriteStatsFile(std::string const& content) {
    std::wstring dir = StatsDir();
    std::wstring tmp = dir + L"\\stats.tmp";
    std::wstring fin = dir + L"\\stats.txt";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

static bool ReadStatsFile(std::string& out, DWORD& err) {
    std::wstring fin = StatsDir() + L"\\stats.txt";
    HANDLE h = CreateFileW(fin.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    char buf[2048];
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) return false;
    out.assign(buf, read);
    return true;
}

// ---------------------------------------------------------------------------
// Collector (runs inside explorer.exe)
// ---------------------------------------------------------------------------

struct NamedVal {
    std::wstring name;
    double v;
};

static PDH_HQUERY g_q = nullptr;
static PDH_HCOUNTER g_cNetIn = nullptr, g_cNetOut = nullptr, g_cCpuUtil = nullptr, g_cCpuPerf = nullptr;
static PDH_HCOUNTER g_cGpuEng = nullptr, g_cGpuMem = nullptr;
static double g_baseMHz = 0;
static ULONGLONG g_installedBytes = 0;
static std::wstring g_gpuLuidStr;
static ULONGLONG g_gpuVramTotal = 0;
static int g_cpuTemp10 = -1;

// minimal D3DKMT declarations (same layout as the Windows driver kit headers)
struct KmtOpenAdapterFromLuid { LUID AdapterLuid; UINT hAdapter; };
struct KmtQueryAdapterInfo { UINT hAdapter; UINT Type; PVOID pPrivateDriverData; UINT PrivateDriverDataSize; };
struct KmtCloseAdapter { UINT hAdapter; };
struct KmtAdapterPerfData {
    UINT PhysicalAdapterIndex;
    ULONGLONG MemoryFrequency;
    ULONGLONG MaxMemoryFrequency;
    ULONGLONG MaxMemoryFrequencyOC;
    ULONGLONG MemoryBandwidth;
    ULONGLONG PCIEBandwidth;
    ULONG FanRPM;
    ULONG Power;
    ULONG Temperature;
    UCHAR PowerStateOverride;
};
typedef LONG(WINAPI* PFN_KmtOpen)(KmtOpenAdapterFromLuid*);
typedef LONG(WINAPI* PFN_KmtQuery)(KmtQueryAdapterInfo*);
typedef LONG(WINAPI* PFN_KmtClose)(KmtCloseAdapter*);
static PFN_KmtOpen g_kmtOpen = nullptr;
static PFN_KmtQuery g_kmtQuery = nullptr;
static PFN_KmtClose g_kmtClose = nullptr;
static UINT g_kmtAdapter = 0;

static const GUID kIidFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

static bool GetScalar(PDH_HCOUNTER h, double& out) {
    PDH_FMT_COUNTERVALUE val = {};
    if (PdhGetFormattedCounterValue(h, PDH_FMT_DOUBLE, nullptr, &val) != ERROR_SUCCESS) return false;
    if (val.CStatus != PDH_CSTATUS_VALID_DATA && val.CStatus != PDH_CSTATUS_NEW_DATA) return false;
    out = val.doubleValue;
    return true;
}

static bool GetArray(PDH_HCOUNTER h, std::vector<NamedVal>& out) {
    DWORD bytes = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (st != PDH_MORE_DATA || bytes == 0) return false;
    std::vector<BYTE> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    st = PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &bytes, &count, items);
    if (st != ERROR_SUCCESS) return false;
    out.reserve(count);
    for (DWORD i = 0; i < count; i++) {
        DWORD cs = items[i].FmtValue.CStatus;
        if (cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA) continue;
        NamedVal nv;
        nv.name = items[i].szName ? items[i].szName : L"";
        nv.v = items[i].FmtValue.doubleValue;
        out.push_back(nv);
    }
    return true;
}

static bool NetIncluded(std::wstring const& nameLower, std::wstring const& filterLower) {
    if (!filterLower.empty()) return nameLower.find(filterLower) != std::wstring::npos;
    static const wchar_t* bad[] = {L"loopback", L"isatap", L"teredo", L"pseudo", L"virtual", L"vmware",
                                   L"virtualbox", L"hyper-v", L"vethernet", L"wan miniport", L"kernel debug",
                                   L"wi-fi direct", L"6to4", L"tunnel", L"bluetooth", L"vpn", L"npcap"};
    for (auto b : bad) {
        if (nameLower.find(b) != std::wstring::npos) return false;
    }
    return true;
}

static int CpuPct10FromTimes() {
    static ULONGLONG pi = 0, pk = 0, pu = 0;
    static bool have = false;
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return -1;
    auto q = [](FILETIME f) {
        ULARGE_INTEGER x;
        x.LowPart = f.dwLowDateTime;
        x.HighPart = f.dwHighDateTime;
        return (ULONGLONG)x.QuadPart;
    };
    ULONGLONG i = q(fi), k = q(fk), u = q(fu);
    int result = -1;
    if (have) {
        ULONGLONG di = i - pi, dk = k - pk, du = u - pu;
        ULONGLONG total = dk + du;  // kernel time already includes idle time
        if (total > 0) result = (int)(((total - di) * 1000ULL) / total);
    }
    pi = i; pk = k; pu = u;
    have = true;
    return result;
}

typedef BOOL(WINAPI* PFN_GetInstalledMem)(PULONGLONG);

static void ReadStaticInfo() {
    DWORD mhz = 0, sz = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz",
                     RRF_RT_REG_DWORD, nullptr, &mhz, &sz) == ERROR_SUCCESS) {
        g_baseMHz = (double)mhz;
    }
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    auto fn = k ? reinterpret_cast<PFN_GetInstalledMem>(GetProcAddress(k, "GetPhysicallyInstalledSystemMemory"))
                : nullptr;
    ULONGLONG kb = 0;
    if (fn && fn(&kb) && kb > 0) g_installedBytes = kb * 1024ULL;
    Wh_Log(L"Base CPU speed %.0f MHz, installed memory %llu bytes", g_baseMHz, (unsigned long long)g_installedBytes);
}

static void SelectGpu(Config const& cfg) {
    if (g_kmtAdapter && g_kmtClose) {
        KmtCloseAdapter c = {g_kmtAdapter};
        g_kmtClose(&c);
    }
    g_kmtAdapter = 0;
    g_gpuLuidStr.clear();
    g_gpuVramTotal = 0;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(kIidFactory1, reinterpret_cast<void**>(&factory))) || !factory) {
        Wh_Log(L"DXGI factory unavailable");
        return;
    }
    std::wstring filter = Lower(cfg.gpuName);
    DXGI_ADAPTER_DESC1 best = {};
    bool found = false;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        a->Release();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!filter.empty() && Lower(d.Description).find(filter) == std::wstring::npos) continue;
        if (!found || d.DedicatedVideoMemory > best.DedicatedVideoMemory) {
            best = d;
            found = true;
        }
    }
    factory->Release();
    if (!found) {
        Wh_Log(L"No matching GPU found");
        return;
    }

    wchar_t luid[64];
    swprintf(luid, 64, L"luid_0x%08x_0x%08x", (unsigned)best.AdapterLuid.HighPart, (unsigned)best.AdapterLuid.LowPart);
    g_gpuLuidStr = Lower(luid);
    g_gpuVramTotal = (ULONGLONG)best.DedicatedVideoMemory;
    Wh_Log(L"GPU selected: %s (%s)", best.Description, g_gpuLuidStr.c_str());

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    if (gdi) {
        g_kmtOpen = reinterpret_cast<PFN_KmtOpen>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
        g_kmtQuery = reinterpret_cast<PFN_KmtQuery>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
        g_kmtClose = reinterpret_cast<PFN_KmtClose>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        if (g_kmtOpen && g_kmtQuery && g_kmtClose) {
            KmtOpenAdapterFromLuid o = {};
            o.AdapterLuid = best.AdapterLuid;
            LONG st = g_kmtOpen(&o);
            if (st == 0) g_kmtAdapter = o.hAdapter;
            else Wh_Log(L"D3DKMTOpenAdapterFromLuid failed: 0x%08X", (unsigned)st);
        }
    }
}

static int ReadGpuTemp10() {
    static bool logged = false;
    if (!g_kmtQuery || !g_kmtAdapter) return -1;
    KmtAdapterPerfData pd = {};
    KmtQueryAdapterInfo q = {};
    q.hAdapter = g_kmtAdapter;
    q.Type = 62;  // adapter performance data
    q.pPrivateDriverData = &pd;
    q.PrivateDriverDataSize = sizeof(pd);
    LONG st = g_kmtQuery(&q);
    if (st != 0) {
        if (!logged) {
            logged = true;
            Wh_Log(L"GPU temperature query failed: 0x%08X", (unsigned)st);
        }
        return -1;
    }
    ULONG t = pd.Temperature;
    if (t == 0) return -1;
    return t >= 200 ? (int)t : (int)t * 10;  // the driver may report tenths of a degree
}

// ---- optional CPU temperature from LibreHardwareMonitor's web server ------

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    WinHttpHandle(WinHttpHandle const&) = delete;
    WinHttpHandle& operator=(WinHttpHandle const&) = delete;
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
};

static bool HttpGetLocal(std::wstring const& url, std::wstring& out) {
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    wchar_t extra[2048] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;

    WinHttpHandle session(WinHttpOpen(L"StartMenuSystemStats/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 2000, 2000, 2000, 2000);
    WinHttpHandle connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect.h) return false;
    std::wstring target = std::wstring(path) + extra;
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle req(WinHttpOpenRequest(connect.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req.h) return false;
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(req.h, nullptr)) return false;

    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        return false;
    }
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return false;
        if (avail == 0) break;
        size_t old = body.size();
        body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, &body[old], avail, &read)) return false;
        body.resize(old + read);
        if (body.size() > 4 * 1024 * 1024) return false;
    }
    if (body.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
    if (n <= 0) return false;
    out.assign((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), out.data(), n);
    return true;
}

static bool HasAny(std::wstring const& lowerName, std::initializer_list<const wchar_t*> keys) {
    for (auto k : keys) {
        if (lowerName.find(k) != std::wstring::npos) return true;
    }
    return false;
}

struct LhmResult {
    double cpu = -1;
    int cpuScore = 0;
};

// hw: 0 = unknown hardware, 1 = CPU, 2 = GPU
static void LhmWalk(JsonObject const& node, int hw, LhmResult& r) {
    std::wstring text{std::wstring_view(node.GetNamedString(L"Text", L""))};
    std::wstring low = Lower(text);
    if (HasAny(low, {L"ryzen", L"intel core", L"core i", L"xeon", L"threadripper", L"athlon", L"pentium"})) hw = 1;
    else if (HasAny(low, {L"radeon", L"geforce", L"nvidia", L"rtx", L"gtx", L"intel arc"})) hw = 2;

    std::wstring value{std::wstring_view(node.GetNamedString(L"Value", L""))};
    if (hw == 1 && value.find(L"\u00B0C") != std::wstring::npos) {
        double t = wcstod(value.c_str(), nullptr);
        int score = 1;
        if (HasAny(low, {L"tctl", L"cpu package"})) score = 4;
        else if (HasAny(low, {L"core max", L"tdie"})) score = 3;
        else if (HasAny(low, {L"package"})) score = 2;
        if (score > r.cpuScore) {
            r.cpuScore = score;
            r.cpu = t;
        }
    }
    if (node.HasKey(L"Children")) {
        auto arr = node.GetNamedArray(L"Children");
        for (uint32_t i = 0; i < arr.Size(); i++) {
            LhmWalk(arr.GetObjectAt(i), hw, r);
        }
    }
}

static void UpdateCpuTempFromLhm(std::wstring const& url) {
    static bool logged = false;
    g_cpuTemp10 = -1;
    if (url.empty()) return;
    try {
        std::wstring text;
        if (!HttpGetLocal(url, text)) {
            if (!logged) {
                logged = true;
                Wh_Log(L"Could not reach LibreHardwareMonitor at %s", url.c_str());
            }
            return;
        }
        LhmResult r;
        LhmWalk(JsonObject::Parse(winrt::hstring(text)), 0, r);
        if (r.cpuScore > 0) g_cpuTemp10 = (int)std::lround(r.cpu * 10.0);
    } catch (...) {
        if (!logged) {
            logged = true;
            Wh_Log(L"Could not read the LibreHardwareMonitor data");
        }
    }
}

// ---- collector setup and sampling -----------------------------------------

static void CollectorInit() {
    if (PdhOpenQueryW(nullptr, 0, &g_q) != ERROR_SUCCESS) {
        g_q = nullptr;
        Wh_Log(L"PdhOpenQuery failed");
        return;
    }
    auto add = [](const wchar_t* path, PDH_HCOUNTER& h) {
        if (PdhAddEnglishCounterW(g_q, path, 0, &h) != ERROR_SUCCESS) {
            h = nullptr;
            Wh_Log(L"Counter not available: %s", path);
        }
    };
    add(L"\\Network Interface(*)\\Bytes Received/sec", g_cNetIn);
    add(L"\\Network Interface(*)\\Bytes Sent/sec", g_cNetOut);
    add(L"\\Processor Information(_Total)\\% Processor Utility", g_cCpuUtil);
    if (!g_cCpuUtil) add(L"\\Processor(_Total)\\% Processor Time", g_cCpuUtil);
    add(L"\\Processor Information(_Total)\\% Processor Performance", g_cCpuPerf);
    add(L"\\GPU Engine(*)\\Utilization Percentage", g_cGpuEng);
    add(L"\\GPU Adapter Memory(*)\\Dedicated Usage", g_cGpuMem);
    PdhCollectQueryData(g_q);
    ReadStaticInfo();
}

static void CollectorShutdown() {
    if (g_kmtAdapter && g_kmtClose) {
        KmtCloseAdapter c = {g_kmtAdapter};
        g_kmtClose(&c);
        g_kmtAdapter = 0;
    }
    if (g_q) {
        PdhCloseQuery(g_q);
        g_q = nullptr;
    }
}

static Stats CollectorSample(Config const& cfg) {
    Stats s;
    if (g_q) PdhCollectQueryData(g_q);

    // network
    {
        std::vector<NamedVal> in, out;
        std::wstring filter = Lower(cfg.networkAdapter);
        bool okIn = g_cNetIn && GetArray(g_cNetIn, in);
        bool okOut = g_cNetOut && GetArray(g_cNetOut, out);
        if (okIn && okOut) {
            double sumIn = 0, sumOut = 0;
            for (auto const& e : in) {
                if (NetIncluded(Lower(e.name), filter)) sumIn += e.v;
            }
            for (auto const& e : out) {
                if (NetIncluded(Lower(e.name), filter)) sumOut += e.v;
            }
            s.netDown = (long long)std::llround(sumIn);
            s.netUp = (long long)std::llround(sumOut);
        }
    }

    // memory
    {
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
            ULONGLONG used = ms.ullTotalPhys - ms.ullAvailPhys;
            s.ramUsed = (long long)used;
            s.ramPct10 = (int)((used * 1000ULL + ms.ullTotalPhys / 2) / ms.ullTotalPhys);
            s.ramTotal = (long long)(g_installedBytes ? g_installedBytes : ms.ullTotalPhys);
        }
    }

    // processor
    {
        double v = 0;
        if (g_cCpuUtil && GetScalar(g_cCpuUtil, v)) {
            s.cpuPct10 = (int)std::lround(std::clamp(v, 0.0, 100.0) * 10.0);
        } else {
            int p = CpuPct10FromTimes();
            if (p >= 0) s.cpuPct10 = p;
        }
        if (g_baseMHz > 0) {
            double perf = 0;
            if (g_cCpuPerf && GetScalar(g_cCpuPerf, perf) && perf > 0) {
                s.cpuMHz = (int)std::lround(g_baseMHz * perf / 100.0);
            } else {
                s.cpuMHz = (int)g_baseMHz;
            }
        }
        s.cpuTemp10 = g_cpuTemp10;
    }

    // graphics card
    {
        const std::wstring luid = g_gpuLuidStr;
        if (g_cGpuEng) {
            std::vector<NamedVal> v;
            if (GetArray(g_cGpuEng, v)) {
                std::map<std::wstring, double> engines;
                for (auto const& e : v) {
                    std::wstring n = Lower(e.name);
                    if (!luid.empty() && n.find(luid) == std::wstring::npos) continue;
                    size_t p = n.find(L"luid_");
                    engines[p == std::wstring::npos ? n : n.substr(p)] += e.v;
                }
                double best = 0;
                for (auto const& kv : engines) best = std::max(best, kv.second);
                s.gpuPct10 = (int)std::lround(std::clamp(best, 0.0, 100.0) * 10.0);
            }
        }
        if (g_cGpuMem) {
            std::vector<NamedVal> v;
            if (GetArray(g_cGpuMem, v)) {
                double used = -1;
                for (auto const& e : v) {
                    std::wstring n = Lower(e.name);
                    if (!luid.empty() && n.find(luid) == std::wstring::npos) continue;
                    used = std::max(used, e.v);
                }
                if (used >= 0) s.gpuVramUsed = (long long)used;
            }
        }
        if (g_gpuVramTotal > 0) s.gpuVramTotal = (long long)g_gpuVramTotal;
        s.gpuTemp10 = ReadGpuTemp10();
    }
    return s;
}

static DWORD WINAPI CollectorWorker(LPVOID) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    {
        if (!EnsureStatsDir()) Wh_Log(L"Could not create the stats folder (error %lu)", GetLastError());
        CollectorInit();

        int cfgVer = -1;
        int tick = 0;
        bool loggedWrite = false;
        for (;;) {
            Config cfg = GetConfig();
            int v = g_cfgVersion.load();
            if (v != cfgVer) {
                cfgVer = v;
                SelectGpu(cfg);
                tick = 0;
            }
            if (tick % 2 == 0) UpdateCpuTempFromLhm(cfg.lhmUrl);
            tick++;

            Stats s = CollectorSample(cfg);
            if (WriteStatsFile(Serialize(s))) {
                if (!loggedWrite) {
                    loggedWrite = true;
                    Wh_Log(L"Collector running, writing stats every second");
                }
            } else {
                static bool failLogged = false;
                if (!failLogged) {
                    failLogged = true;
                    Wh_Log(L"Could not write the stats file (error %lu)", GetLastError());
                }
            }
            if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
        }
        CollectorShutdown();
    }
    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Renderer (runs inside the Start menu process)
// ---------------------------------------------------------------------------

struct Series {
    std::wstring fill;  // polygon name, empty for a plain line
    std::wstring line;  // polyline name
    std::vector<float> v;
    float maxh;
};

struct UiFrame {
    int version = 0;
    std::map<std::wstring, std::wstring> text;
    std::map<std::wstring, bool> visible;
    std::vector<Series> graphs;
    double gridOffset = 0;
};

struct Widget {
    std::map<std::wstring, FrameworkElement> el;
    std::map<std::wstring, TranslateTransform> tt;
    FrameworkElement anchor{nullptr};
    bool valid = false;
    DWORD owner = 0;
};

static std::mutex g_widgetLock;
static Widget g_w;
static int g_appliedVersion = -1;
static ULONGLONG g_lastScan = 0;

static const std::set<std::wstring>& KnownNames() {
    static const std::set<std::wstring> s = {
        L"NetGridV", L"NetFill", L"NetLine", L"NetSendLine", L"NetUpValue", L"NetDownValue",
        L"RamGridV", L"RamFill", L"RamLine", L"RamPercent", L"RamUsage",
        L"CpuGridV", L"CpuFill", L"CpuLine", L"CpuPercent", L"CpuTemp", L"CpuFreq",
        L"GpuGridV", L"GpuFill", L"GpuLine", L"GpuPercent", L"GpuTemp", L"GpuVram"};
    return s;
}

static void Scan(DependencyObject const& o, std::map<std::wstring, FrameworkElement>& out) {
    size_t want = KnownNames().size();
    if (!o || out.size() >= want) return;
    if (auto fe = o.try_as<FrameworkElement>()) {
        auto name = fe.Name();
        if (!name.empty()) {
            std::wstring n{std::wstring_view(name)};
            if (KnownNames().count(n) && !out.count(n)) out.emplace(n, fe);
        }
    }
    int count = VisualTreeHelper::GetChildrenCount(o);
    for (int i = 0; i < count && out.size() < want; i++) {
        Scan(VisualTreeHelper::GetChild(o, i), out);
    }
}

template <typename T>
static T El(const wchar_t* name) {
    auto it = g_w.el.find(name);
    if (it == g_w.el.end()) return T{nullptr};
    return it->second.try_as<T>();
}

static PointCollection MakePoints(std::vector<float> const& v, float maxh, bool closed) {
    PointCollection pc;
    int n = (int)v.size();
    if (closed) pc.Append(Point{0.0f, (float)kH});
    for (int i = 0; i < n; i++) {
        float x = (float)(i * kW / (n - 1));
        float y = (float)(kH - 1.0 - (double)v[i] * (double)maxh);
        pc.Append(Point{x, y});
    }
    if (closed) pc.Append(Point{(float)kW, (float)kH});
    return pc;
}

static void ApplyOnUiThread(std::shared_ptr<UiFrame> f) {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        DWORD me = GetCurrentThreadId();
        if (g_w.valid && g_w.owner != me) return;

        if (!g_w.valid || !g_w.anchor.IsLoaded()) {
            ULONGLONG now = GetTickCount64();
            if (now - g_lastScan < 3000) return;
            g_lastScan = now;

            g_w = Widget{};
            auto win = Window::Current();
            if (!win) return;
            auto root = win.Content();
            if (!root) return;

            std::map<std::wstring, FrameworkElement> found;
            Scan(root, found);
            if (found.size() < KnownNames().size()) return;

            g_w.el = found;
            g_w.anchor = found.at(L"NetLine");
            g_w.valid = true;
            g_w.owner = me;
            g_appliedVersion = -1;
            Wh_Log(L"Stats tiles found and hooked");
        }

        if (g_appliedVersion == f->version) return;

        for (auto const& kv : f->text) {
            auto tb = El<TextBlock>(kv.first.c_str());
            if (tb) tb.Text(winrt::hstring(kv.second));
        }
        for (auto const& kv : f->visible) {
            auto fe = El<FrameworkElement>(kv.first.c_str());
            if (fe) fe.Visibility(kv.second ? Visibility::Visible : Visibility::Collapsed);
        }
        for (auto const& g : f->graphs) {
            if (!g.fill.empty()) {
                auto pg = El<XPolygon>(g.fill.c_str());
                if (pg) pg.Points(MakePoints(g.v, g.maxh, true));
            }
            auto pl = El<XPolyline>(g.line.c_str());
            if (pl) pl.Points(MakePoints(g.v, g.maxh, false));
        }
        for (auto nm : {L"NetGridV", L"RamGridV", L"CpuGridV", L"GpuGridV"}) {
            auto cv = El<Canvas>(nm);
            if (!cv) continue;
            auto it = g_w.tt.find(nm);
            if (it == g_w.tt.end()) {
                TranslateTransform t;
                cv.RenderTransform(t);
                it = g_w.tt.emplace(nm, t).first;
            }
            it->second.X(-f->gridOffset);
        }
        g_appliedVersion = f->version;
    } catch (...) {
    }
}

static void TeardownOnUiThread() {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        if (g_w.valid && g_w.owner == GetCurrentThreadId()) {
            g_w = Widget{};
        }
    } catch (...) {
    }
}

static void DispatchToViews(std::shared_ptr<UiFrame> f) {
    try {
        for (auto const& view : CoreApplication::Views()) {
            try {
                view.Dispatcher().RunAsync(CoreDispatcherPriority::Low, [f]() { ApplyOnUiThread(f); });
            } catch (...) {
            }
        }
    } catch (...) {
    }
}

static void TeardownAllViews() {
    try {
        for (auto const& view : CoreApplication::Views()) {
            try {
                auto op = view.Dispatcher().RunAsync(CoreDispatcherPriority::Low, []() { TeardownOnUiThread(); });
                op.wait_for(std::chrono::seconds(2));
            } catch (...) {
            }
        }
    } catch (...) {
    }
}

// ---- turning numbers into text and graph shapes ---------------------------

static std::wstring Fmt(double v, int decimals) {
    long long scale = decimals == 1 ? 10 : 100;
    long long n = (long long)std::llround(v * (double)scale);
    long long ip = n / scale;
    long long fp = n % scale;
    if (fp < 0) fp = -fp;
    std::wstring f = std::to_wstring(fp);
    while ((int)f.size() < decimals) f = L"0" + f;
    return std::to_wstring(ip) + L"." + f;
}

static std::wstring RateText(long long bytesPerSec) {
    double b = (double)bytesPerSec;
    if (b >= 1048576.0) return Fmt(b / 1048576.0, 1) + L" MB/s";
    if (b >= 1024.0) return std::to_wstring((long long)std::llround(b / 1024.0)) + L" KB/s";
    return L"0 KB/s";
}

static std::wstring PctText(int pct10) {
    return std::to_wstring((long long)std::llround(pct10 / 10.0)) + L"%";
}

static std::wstring TempText(int temp10) {
    return L"(" + std::to_wstring((long long)std::llround(temp10 / 10.0)) + L"\u00B0C)";
}

static std::wstring GibText(long long bytes, int decimals) {
    return Fmt((double)bytes / 1073741824.0, decimals);
}

struct History {
    std::deque<double> netIn, netOut, ram, cpu, gpu;
    History()
        : netIn(kSamples, 0.0), netOut(kSamples, 0.0), ram(kSamples, 0.0), cpu(kSamples, 0.0), gpu(kSamples, 0.0) {}
};

static void Push(std::deque<double>& d, double v) {
    d.push_back(v);
    while ((int)d.size() > kSamples) d.pop_front();
}

static std::vector<float> Normalize(std::deque<double> const& d, double scale) {
    std::vector<float> out;
    out.reserve(d.size());
    for (double v : d) out.push_back((float)std::clamp(v / scale, 0.0, 1.0));
    return out;
}

static std::shared_ptr<UiFrame> BuildFrame(bool ok, Stats const& s, History& h, double& gridOffset, int version) {
    auto f = std::make_shared<UiFrame>();
    f->version = version;

    Push(h.netIn, (ok && s.netDown >= 0) ? (double)s.netDown : 0.0);
    Push(h.netOut, (ok && s.netUp >= 0) ? (double)s.netUp : 0.0);
    Push(h.ram, (ok && s.ramPct10 >= 0) ? s.ramPct10 / 10.0 : 0.0);
    Push(h.cpu, (ok && s.cpuPct10 >= 0) ? s.cpuPct10 / 10.0 : 0.0);
    Push(h.gpu, (ok && s.gpuPct10 >= 0) ? s.gpuPct10 / 10.0 : 0.0);

    // network graph scales to the biggest value currently on screen
    double mx = 32.0 * 1024.0;
    for (double v : h.netIn) mx = std::max(mx, v);
    for (double v : h.netOut) mx = std::max(mx, v);
    f->graphs.push_back({L"NetFill", L"NetLine", Normalize(h.netIn, mx), 96.0f});
    f->graphs.push_back({L"", L"NetSendLine", Normalize(h.netOut, mx), 96.0f});
    f->graphs.push_back({L"RamFill", L"RamLine", Normalize(h.ram, 100.0), (float)(kH - 1.0)});
    f->graphs.push_back({L"CpuFill", L"CpuLine", Normalize(h.cpu, 100.0), (float)(kH - 1.0)});
    f->graphs.push_back({L"GpuFill", L"GpuLine", Normalize(h.gpu, 100.0), (float)(kH - 1.0)});

    auto& t = f->text;
    t[L"NetUpValue"] = (ok && s.netUp >= 0) ? L": " + RateText(s.netUp) : L": --";
    t[L"NetDownValue"] = (ok && s.netDown >= 0) ? L": " + RateText(s.netDown) : L": --";

    t[L"RamPercent"] = (ok && s.ramPct10 >= 0) ? PctText(s.ramPct10) : L"--";
    if (ok && s.ramUsed >= 0 && s.ramTotal > 0) {
        t[L"RamUsage"] = GibText(s.ramUsed, 1) + L" / " + GibText(s.ramTotal, 0).substr(0, std::wstring::npos) + L" GB";
    } else {
        t[L"RamUsage"] = L"-- / -- GB";
    }

    t[L"CpuPercent"] = (ok && s.cpuPct10 >= 0) ? PctText(s.cpuPct10) : L"--";
    t[L"CpuFreq"] = (ok && s.cpuMHz > 0) ? Fmt(s.cpuMHz / 1000.0, 2) + L" GHz" : L"-- GHz";
    bool cpuTemp = ok && s.cpuTemp10 >= 0;
    if (cpuTemp) t[L"CpuTemp"] = TempText(s.cpuTemp10);
    f->visible[L"CpuTemp"] = cpuTemp;

    t[L"GpuPercent"] = (ok && s.gpuPct10 >= 0) ? PctText(s.gpuPct10) : L"--";
    if (ok && s.gpuVramUsed >= 0 && s.gpuVramTotal > 0) {
        t[L"GpuVram"] = GibText(s.gpuVramUsed, 1) + L" / " + GibText(s.gpuVramTotal, 0) + L" GB";
    } else {
        t[L"GpuVram"] = L"-- / -- GB";
    }
    bool gpuTemp = ok && s.gpuTemp10 >= 0;
    if (gpuTemp) t[L"GpuTemp"] = TempText(s.gpuTemp10);
    f->visible[L"GpuTemp"] = gpuTemp;

    // the grid drifts left by one graph step per second
    gridOffset = std::fmod(gridOffset + kW / (kSamples - 1), 19.0);
    f->gridOffset = gridOffset;
    return f;
}

static bool ReadFreshStats(Stats& s) {
    static ULONGLONG lastLog = 0;
    std::string text;
    DWORD err = 0;
    if (!ReadStatsFile(text, err)) {
        ULONGLONG now = GetTickCount64();
        if (now - lastLog > 30000 || lastLog == 0) {
            lastLog = now;
            Wh_Log(L"Cannot read the stats file (error %lu). Is the collector running?", err);
        }
        return false;
    }
    ULONGLONG stamp = 0;
    if (!ParseStats(text, s, stamp)) return false;
    ULONGLONG now = GetTickCount64();
    return now < stamp || (now - stamp) < 5000;
}

static DWORD WINAPI UiWorker(LPVOID) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    {
        History hist;
        double gridOffset = 0;
        int version = 0;
        bool lastOk = false;
        bool first = true;
        for (;;) {
            Stats s;
            bool ok = ReadFreshStats(s);
            if (first || ok != lastOk) {
                first = false;
                lastOk = ok;
                if (ok) {
                    Wh_Log(L"Receiving stats from the collector");
                } else {
                    Wh_Log(L"No fresh stats from the collector");
                }
            }
            DispatchToViews(BuildFrame(ok, s, hist, gridOffset, ++version));
            if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
        }
        TeardownAllViews();
    }
    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

static bool IsStartHostProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"StartMenuExperienceHost.exe") == 0;
}

static bool AcquireCollectorRole() {
    g_collectorMutex = CreateMutexW(nullptr, FALSE, L"Local\\StartMenuSystemStatsCollector");
    if (!g_collectorMutex) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_collectorMutex);
        g_collectorMutex = nullptr;
        return false;
    }
    return true;
}

BOOL Wh_ModInit() {
    g_isUi = IsStartHostProcess();
    if (g_isUi) {
        Wh_Log(L"Init in the Start menu process");
    } else {
        Wh_Log(L"Init in explorer.exe");
    }
    LoadSettings();
    if (!g_isUi && !AcquireCollectorRole()) {
        Wh_Log(L"Another collector is already running");
        return TRUE;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, g_isUi ? UiWorker : CollectorWorker, nullptr, 0, nullptr);
    return g_stopEvent && g_thread;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 15000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_collectorMutex) {
        CloseHandle(g_collectorMutex);
        g_collectorMutex = nullptr;
    }
}