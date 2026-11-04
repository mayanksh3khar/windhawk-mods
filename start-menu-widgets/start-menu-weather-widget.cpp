// ==WindhawkMod==
// @id              start-menu-weather-widget
// @name            Start Menu Weather Widget
// @description     Fills the weather tile of the custom Start menu (built with Start Menu Styler) using Open-Meteo, ipwho.is, Fluent Emoji and Unsplash. No API keys.
// @version         0.1.0
// @author          local
// @include         StartMenuExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -DWINRT_NO_MODULE_LOCK -lole32 -loleaut32 -lruntimeobject -lwinhttp
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- city: ""
  $name: City
  $description: Leave empty to detect your approximate location from your IP address
- units: celsius
  $name: Temperature unit
  $options:
  - celsius: Celsius
  - fahrenheit: Fahrenheit
- aqiStandard: us
  $name: Air quality scale
  $options:
  - us: US AQI
  - european: European AQI
- refreshMinutes: 15
  $name: Refresh interval (minutes)
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <winhttp.h>
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Security.Cryptography.Certificates.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Filters;
using namespace winrt::Windows::Security::Cryptography::Certificates;

using IInsp = winrt::Windows::Foundation::IInspectable;

static constexpr int kExpected = 6;  // named elements the weather tile must contain

// ---------------------------------------------------------------------------
// Types and globals
// ---------------------------------------------------------------------------

struct Config {
    std::wstring city;
    bool fahrenheit = false;
    bool europeanAqi = false;
    int refreshMinutes = 15;
};

struct Location {
    double lat = 0;
    double lon = 0;
    std::wstring city;
};

struct WeatherState {
    int version = 0;
    std::wstring temp;
    std::wstring condition;
    std::wstring aqi;
    std::wstring city;
    std::wstring iconUrl;
    std::wstring bgUrl;
};

struct Widget {
    TextBlock temp{nullptr}, condition{nullptr}, aqi{nullptr}, city{nullptr};
    Image icon{nullptr};
    Border bg{nullptr};
    winrt::event_token iconFailed{};
    bool valid = false;
    bool hooked = false;
    DWORD ownerThread = 0;
};

static HANDLE g_stopEvent = nullptr;
static HANDLE g_refreshEvent = nullptr;
static HANDLE g_thread = nullptr;

static std::mutex g_cfgLock;
static Config g_cfg;
static std::atomic<int> g_cfgVersion{0};

static std::mutex g_widgetLock;
static Widget g_w;
static int g_appliedVersion = -1;
static std::wstring g_appliedIcon;
static std::wstring g_appliedBg;
static ULONGLONG g_lastScan = 0;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static std::wstring Trim(std::wstring s) {
    auto notSpace = [](wchar_t c) { return !iswspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static void LoadSettings() {
    Config c;

    PCWSTR city = Wh_GetStringSetting(L"city");
    c.city = Trim(city ? city : L"");
    Wh_FreeStringSetting(city);

    PCWSTR units = Wh_GetStringSetting(L"units");
    c.fahrenheit = units && _wcsicmp(units, L"fahrenheit") == 0;
    Wh_FreeStringSetting(units);

    PCWSTR aqi = Wh_GetStringSetting(L"aqiStandard");
    c.europeanAqi = aqi && _wcsicmp(aqi, L"european") == 0;
    Wh_FreeStringSetting(aqi);

    c.refreshMinutes = std::clamp(Wh_GetIntSetting(L"refreshMinutes"), 5, 180);

    std::lock_guard<std::mutex> l(g_cfgLock);
    g_cfg = c;
    g_cfgVersion++;
}

static Config GetConfig() {
    std::lock_guard<std::mutex> l(g_cfgLock);
    return g_cfg;
}

// ---------------------------------------------------------------------------
// Weather description -> text, icon and background
// ---------------------------------------------------------------------------

static const wchar_t* kIconBase = L"https://cdn.jsdelivr.net/gh/microsoft/fluentui-emoji@latest/assets/";

static const wchar_t* kIconSun = L"Sun/3D/sun_3d.png";
static const wchar_t* kIconSunSmallCloud = L"Sun%20behind%20small%20cloud/3D/sun_behind_small_cloud_3d.png";
static const wchar_t* kIconSunCloud = L"Sun%20behind%20cloud/3D/sun_behind_cloud_3d.png";
static const wchar_t* kIconSunRain = L"Sun%20behind%20rain%20cloud/3D/sun_behind_rain_cloud_3d.png";
static const wchar_t* kIconMoon = L"Crescent%20moon/3D/crescent_moon_3d.png";
static const wchar_t* kIconCloud = L"Cloud/3D/cloud_3d.png";
static const wchar_t* kIconFog = L"Fog/3D/fog_3d.png";
static const wchar_t* kIconRain = L"Cloud%20with%20rain/3D/cloud_with_rain_3d.png";
static const wchar_t* kIconSnow = L"Cloud%20with%20snow/3D/cloud_with_snow_3d.png";
static const wchar_t* kIconStorm = L"Cloud%20with%20lightning%20and%20rain/3D/cloud_with_lightning_and_rain_3d.png";

enum Bg { BgClear, BgPartly, BgOvercast, BgRain, BgStorm, BgSnow, BgFog, BgNight };

// Unsplash photos (free to use under the Unsplash License)
static const wchar_t* kBgIds[] = {
    L"photo-1625759910025-16c05c4a7ec2",  // clear day
    L"photo-1754764979352-ed7ce14dc545",  // partly cloudy
    L"photo-1560928863-e140ee0fc733",     // overcast
    L"photo-1560091241-e9794e7409f5",     // rain
    L"photo-1562070268-b473c84732f1",     // storm
    L"photo-1768215241564-cee8a532c772",  // snow
    L"photo-1634391325323-9ae90e52dc3b",  // fog
    L"photo-1435224668334-0f82ec57b605",  // clear night
};

struct Cond {
    const wchar_t* text;
    const wchar_t* icon;
    Bg bg;
};

// WMO weather interpretation codes as used by Open-Meteo
static Cond Describe(int code, bool day) {
    switch (code) {
        case 0: return day ? Cond{L"Clear sky", kIconSun, BgClear} : Cond{L"Clear night", kIconMoon, BgNight};
        case 1: return day ? Cond{L"Mainly clear", kIconSunSmallCloud, BgClear} : Cond{L"Mainly clear", kIconMoon, BgNight};
        case 2: return day ? Cond{L"Partly cloudy", kIconSunCloud, BgPartly} : Cond{L"Partly cloudy", kIconCloud, BgNight};
        case 3: return {L"Overcast", kIconCloud, BgOvercast};
        case 45: case 48: return {L"Fog", kIconFog, BgFog};
        case 51: return {L"Light drizzle", kIconRain, BgRain};
        case 53: return {L"Drizzle", kIconRain, BgRain};
        case 55: return {L"Dense drizzle", kIconRain, BgRain};
        case 56: case 57: return {L"Freezing drizzle", kIconRain, BgRain};
        case 61: return {L"Light rain", kIconRain, BgRain};
        case 63: return {L"Rain", kIconRain, BgRain};
        case 65: return {L"Heavy rain", kIconRain, BgRain};
        case 66: case 67: return {L"Freezing rain", kIconRain, BgRain};
        case 71: return {L"Light snow", kIconSnow, BgSnow};
        case 73: return {L"Snow", kIconSnow, BgSnow};
        case 75: return {L"Heavy snow", kIconSnow, BgSnow};
        case 77: return {L"Snow grains", kIconSnow, BgSnow};
        case 80: return {L"Light showers", day ? kIconSunRain : kIconRain, BgRain};
        case 81: return {L"Showers", day ? kIconSunRain : kIconRain, BgRain};
        case 82: return {L"Heavy showers", day ? kIconSunRain : kIconRain, BgRain};
        case 85: case 86: return {L"Snow showers", kIconSnow, BgSnow};
        case 95: return {L"Thunderstorm", kIconStorm, BgStorm};
        case 96: case 99: return {L"Hailstorm", kIconStorm, BgStorm};
        default: return {L"Unknown", kIconCloud, BgOvercast};
    }
}

static std::wstring IconUrl(const wchar_t* path) {
    return std::wstring(kIconBase) + path;
}

static std::wstring BgUrl(Bg bg) {
    return std::wstring(L"https://images.unsplash.com/") + kBgIds[bg] +
           L"?fm=jpg&q=60&w=400&auto=format&fit=crop";
}

static std::wstring AqiLabel(double v, bool european) {
    if (european) {
        if (v <= 20) return L"Good";
        if (v <= 40) return L"Fair";
        if (v <= 60) return L"Moderate";
        if (v <= 80) return L"Poor";
        if (v <= 100) return L"Very poor";
        return L"Extreme";
    }
    if (v <= 50) return L"Good";
    if (v <= 100) return L"Moderate";
    if (v <= 150) return L"Poor";  // unhealthy for sensitive groups
    if (v <= 200) return L"Unhealthy";
    if (v <= 300) return L"Severe";
    return L"Hazardous";
}

// ---------------------------------------------------------------------------
// Networking (worker thread)
// ---------------------------------------------------------------------------

template <typename Async>
static auto WaitFor(Async const& op, int seconds = 15) {
    auto status = op.wait_for(std::chrono::seconds(seconds));
    if (status == AsyncStatus::Started) {  // genuinely still running: timed out
        op.Cancel();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Request timed out");
    }
    return op.GetResults();  // throws the real error if the request failed or was canceled
}

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

[[noreturn]] static void ThrowWinHttp(const wchar_t* step) {
    DWORD err = GetLastError();
    throw winrt::hresult_error(
        HRESULT_FROM_WIN32(err),
        winrt::hstring(std::wstring(step) + L" failed (error " + std::to_wstring(err) + L")"));
}

static std::wstring HttpGetString(std::wstring const& url) {
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
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) ThrowWinHttp(L"WinHttpCrackUrl");

    WinHttpHandle session(WinHttpOpen(L"StartMenuWeatherWidget/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) ThrowWinHttp(L"WinHttpOpen");
    WinHttpSetTimeouts(session.h, 10000, 10000, 10000, 10000);

    WinHttpHandle connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect.h) ThrowWinHttp(L"WinHttpConnect");

    std::wstring target = std::wstring(path) + extra;
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle req(WinHttpOpenRequest(connect.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req.h) ThrowWinHttp(L"WinHttpOpenRequest");

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        ThrowWinHttp(L"WinHttpSendRequest");
    if (!WinHttpReceiveResponse(req.h, nullptr)) ThrowWinHttp(L"WinHttpReceiveResponse");

    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX))
        ThrowWinHttp(L"WinHttpQueryHeaders");
    if (status != 200) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"HTTP status " + std::to_wstring(status)));
    }

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) ThrowWinHttp(L"WinHttpQueryDataAvailable");
        if (avail == 0) break;
        size_t old = body.size();
        body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, &body[old], avail, &read)) ThrowWinHttp(L"WinHttpReadData");
        body.resize(old + read);
        if (body.size() > 4 * 1024 * 1024) throw winrt::hresult_error(E_FAIL, L"Response too large");
    }

    if (body.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), out.data(), n);
    return out;
}

static JsonObject GetJson(HttpClient const& client, std::wstring const& url) {
    (void)client;  // networking now goes through WinHTTP
    Wh_Log(L"GET %s", url.c_str());
    std::wstring text = HttpGetString(url);
    Wh_Log(L"Response (%d chars): %s", (int)text.size(), text.substr(0, 150).c_str());
    return JsonObject::Parse(winrt::hstring(text));
}

static bool TryNumber(JsonObject const& o, const wchar_t* key, double& out) {
    if (!o.HasKey(key)) return false;
    auto v = o.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Number) return false;
    out = v.GetNumber();
    return true;
}

static Location ResolveLocation(HttpClient const& client, Config const& cfg) {
    Location loc;
    if (!cfg.city.empty()) {
        std::wstring url =
            L"https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" +
            std::wstring(Uri::EscapeComponent(winrt::hstring(cfg.city)));
        auto json = GetJson(client, url);
        if (!json.HasKey(L"results")) {
            throw winrt::hresult_error(E_FAIL, L"City not found");
        }
        auto r = json.GetNamedArray(L"results").GetObjectAt(0);
        loc.lat = r.GetNamedNumber(L"latitude");
        loc.lon = r.GetNamedNumber(L"longitude");
        loc.city = std::wstring(std::wstring_view(r.GetNamedString(L"name", L"")));
    } else {
        auto json = GetJson(client, L"https://ipwho.is/?fields=success,city,latitude,longitude");
        if (!json.GetNamedBoolean(L"success", false)) {
            throw winrt::hresult_error(E_FAIL, L"IP lookup failed");
        }
        loc.lat = json.GetNamedNumber(L"latitude");
        loc.lon = json.GetNamedNumber(L"longitude");
        loc.city = std::wstring(std::wstring_view(json.GetNamedString(L"city", L"")));
    }
    if (loc.city.empty()) loc.city = L"Local";
    Wh_Log(L"Location resolved: %s", loc.city.c_str());
    return loc;
}

static std::shared_ptr<WeatherState> FetchWeather(HttpClient const& client, Location const& loc,
                                                  Config const& cfg, int version) {
    auto st = std::make_shared<WeatherState>();
    st->version = version;
    st->city = loc.city;

    std::wstring coords = L"&latitude=" + std::to_wstring(loc.lat) + L"&longitude=" + std::to_wstring(loc.lon);

    // Weather
    std::wstring wurl =
        L"https://api.open-meteo.com/v1/forecast?current=temperature_2m,weather_code,is_day&temperature_unit=" +
        std::wstring(cfg.fahrenheit ? L"fahrenheit" : L"celsius") + coords;
    auto w = GetJson(client, wurl).GetNamedObject(L"current");
    double temp = 0, code = 3, isDay = 1;
    if (!TryNumber(w, L"temperature_2m", temp)) throw winrt::hresult_error(E_FAIL, L"No temperature in response");
    TryNumber(w, L"weather_code", code);
    TryNumber(w, L"is_day", isDay);

    st->temp = std::to_wstring(std::lround(temp)) + L"\u00B0" + (cfg.fahrenheit ? L"F" : L"C");
    Cond c = Describe((int)code, isDay != 0);
    st->condition = c.text;
    st->iconUrl = IconUrl(c.icon);
    st->bgUrl = BgUrl(c.bg);

    // Air quality (optional, may be unavailable for some places)
    st->aqi = L"AQI --";
    try {
        const wchar_t* field = cfg.europeanAqi ? L"european_aqi" : L"us_aqi";
        std::wstring aurl = std::wstring(L"https://air-quality-api.open-meteo.com/v1/air-quality?current=") + field + coords;
        auto a = GetJson(client, aurl).GetNamedObject(L"current");
        double aqi = 0;
        if (TryNumber(a, field, aqi)) {
            st->aqi = L"AQI " + std::to_wstring(std::lround(aqi)) + L" (" + AqiLabel(aqi, cfg.europeanAqi) + L")";
        }
    } catch (...) {
        Wh_Log(L"Air quality request failed");
    }
    return st;
}

static std::shared_ptr<WeatherState> ErrorState(Config const& cfg, int version) {
    auto st = std::make_shared<WeatherState>();
    Cond c = Describe(3, true);
    st->version = version;
    st->temp = L"--";
    st->condition = L"Weather unavailable";
    st->aqi = L"AQI --";
    st->city = cfg.city.empty() ? L"Local" : cfg.city;
    st->iconUrl = IconUrl(c.icon);
    st->bgUrl = BgUrl(c.bg);
    return st;
}

// ---------------------------------------------------------------------------
// UI-thread code
// ---------------------------------------------------------------------------

template <typename T>
static void Take(T& slot, FrameworkElement const& fe, int& n) {
    if (!slot) {
        if (auto x = fe.try_as<T>()) {
            slot = x;
            n++;
        }
    }
}

static void Scan(DependencyObject const& o, Widget& w, int& n) {
    if (!o || n >= kExpected) return;
    if (auto fe = o.try_as<FrameworkElement>()) {
        auto name = fe.Name();
        if (!name.empty()) {
            if (name == L"WeatherTemp") Take(w.temp, fe, n);
            else if (name == L"WeatherCondition") Take(w.condition, fe, n);
            else if (name == L"WeatherAqi") Take(w.aqi, fe, n);
            else if (name == L"WeatherCity") Take(w.city, fe, n);
            else if (name == L"WeatherIcon") Take(w.icon, fe, n);
            else if (name == L"WeatherBg") Take(w.bg, fe, n);
        }
    }
    int count = VisualTreeHelper::GetChildrenCount(o);
    for (int i = 0; i < count && n < kExpected; i++) {
        Scan(VisualTreeHelper::GetChild(o, i), w, n);
    }
}

static void ApplyOnUiThread(std::shared_ptr<WeatherState> s) {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        DWORD me = GetCurrentThreadId();

        if (g_w.valid && g_w.ownerThread != me) return;  // widget lives on another view

        if (!g_w.valid || !g_w.temp.IsLoaded()) {
            ULONGLONG now = GetTickCount64();
            if (now - g_lastScan < 3000) return;
            g_lastScan = now;

            g_w = Widget{};
            auto win = Window::Current();
            if (!win) return;
            auto root = win.Content();
            if (!root) return;

            Widget found;
            int n = 0;
            Scan(root, found, n);
            if (n < kExpected) return;

            g_w = found;
            g_w.valid = true;
            g_w.ownerThread = me;
            g_appliedVersion = -1;
            g_appliedIcon.clear();
            g_appliedBg.clear();
            g_w.iconFailed = g_w.icon.ImageFailed([](IInsp const&, ExceptionRoutedEventArgs const&) {
                Wh_Log(L"Weather icon failed to load: %s", g_appliedIcon.c_str());
            });
            g_w.hooked = true;
            Wh_Log(L"Weather widget found");
        }

        if (g_appliedVersion == s->version) return;

        g_w.temp.Text(winrt::hstring(s->temp));
        g_w.condition.Text(winrt::hstring(s->condition));
        g_w.aqi.Text(winrt::hstring(s->aqi));
        g_w.city.Text(winrt::hstring(s->city));

        if (g_appliedIcon != s->iconUrl) {
            g_w.icon.Source(BitmapImage(Uri(winrt::hstring(s->iconUrl))));
            g_appliedIcon = s->iconUrl;
        }
        if (g_appliedBg != s->bgUrl) {
            ImageBrush brush;
            brush.ImageSource(BitmapImage(Uri(winrt::hstring(s->bgUrl))));
            brush.Stretch(Stretch::UniformToFill);
            g_w.bg.Background(brush);
            g_appliedBg = s->bgUrl;
        }
        g_appliedVersion = s->version;
    } catch (...) {
    }
}

static void TeardownOnUiThread() {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        if (g_w.valid && g_w.ownerThread == GetCurrentThreadId()) {
            if (g_w.hooked) g_w.icon.ImageFailed(g_w.iconFailed);
            g_w = Widget{};
        }
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void DispatchToViews(std::shared_ptr<WeatherState> st) {
    try {
        for (auto const& view : CoreApplication::Views()) {
            try {
                view.Dispatcher().RunAsync(CoreDispatcherPriority::Low, [st]() { ApplyOnUiThread(st); });
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

static DWORD WINAPI Worker(LPVOID) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    {
        HttpBaseProtocolFilter filter;
        filter.ServerCustomValidationRequested(
            [](HttpBaseProtocolFilter const&, HttpServerCustomValidationRequestedEventArgs const& e) {
                try {
                    auto errors = e.ServerCertificateErrors();
                    for (auto const& err : errors) {
                        Wh_Log(L"TLS certificate error code: %d", (int)err);
                    }
                    auto cert = e.ServerCertificate();
                    Wh_Log(L"TLS cert subject: %s | issuer: %s", cert.Subject().c_str(), cert.Issuer().c_str());
                    if (errors.Size() > 0) e.Reject();  // diagnostics only
                } catch (...) {
                }
            });
        HttpClient client(filter);
        try {
            client.DefaultRequestHeaders().UserAgent().ParseAdd(L"StartMenuWeatherWidget/0.1");
        } catch (...) {
        }

        Location loc;
        bool haveLoc = false;
        int locCfgVersion = -1;
        ULONGLONG locTime = 0;
        ULONGLONG nextRefresh = 0;
        int version = 0;
        std::shared_ptr<WeatherState> last;
        HANDLE waits[2] = {g_stopEvent, g_refreshEvent};

        for (;;) {
            ULONGLONG now = GetTickCount64();
            Config cfg = GetConfig();
            int cfgVer = g_cfgVersion.load();

            if (now >= nextRefresh) {
                try {
                    if (!haveLoc || cfgVer != locCfgVersion || now - locTime > 6ULL * 3600 * 1000) {
                        loc = ResolveLocation(client, cfg);
                        haveLoc = true;
                        locCfgVersion = cfgVer;
                        locTime = now;
                    }
                    last = FetchWeather(client, loc, cfg, ++version);
                    nextRefresh = now + (ULONGLONG)cfg.refreshMinutes * 60 * 1000;
                    Wh_Log(L"Weather updated: %s, %s", last->temp.c_str(), last->condition.c_str());
                } catch (winrt::hresult_error const& e) {
                    Wh_Log(L"Weather refresh failed: 0x%08X %s", (unsigned)e.code().value, e.message().c_str());
                    nextRefresh = now + 60 * 1000;
                    if (!last) last = ErrorState(cfg, ++version);
                } catch (...) {
                    Wh_Log(L"Weather refresh failed");
                    nextRefresh = now + 60 * 1000;
                    if (!last) last = ErrorState(cfg, ++version);
                }
            }

            if (last) DispatchToViews(last);

            DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5000);
            if (w == WAIT_OBJECT_0) break;                 // stop
            if (w == WAIT_OBJECT_0 + 1) nextRefresh = 0;   // settings changed: refresh now
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

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    LoadSettings();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_refreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_stopEvent && g_refreshEvent && g_thread;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    if (g_refreshEvent) SetEvent(g_refreshEvent);
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 15000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_refreshEvent) {
        CloseHandle(g_refreshEvent);
        g_refreshEvent = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}