// ==WindhawkMod==
// @id              start-menu-media-widget
// @name            Start Menu Media Widget
// @description     Feeds the "Music" tile (built with Start Menu Styler) with the current media session and makes its controls work
// @version         0.2.0
// @author          local
// @include         StartMenuExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -DWINRT_NO_MODULE_LOCK -lole32 -loleaut32 -lruntimeobject -ladvapi32
// ==/WindhawkMod==

#include <windows.h>
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Input.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;

using IInsp = winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::ColorHelper;

static constexpr int kExpected = 14;  // number of named elements the widget must have

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

struct MediaState {
    bool hasSession = false;
    bool playing = false;
    bool light = false;  // system (Start) theme is light
    std::wstring app;
    std::wstring title;
    std::wstring artist;
    double progress = 0;  // 0..100
    int64_t startTicks = 0;
    int64_t endTicks = 0;
    std::wstring artKey;
    InMemoryRandomAccessStream art{nullptr};
};

struct ArtCache {
    std::wstring key;
    InMemoryRandomAccessStream stream{nullptr};
    int retries = 0;
};

struct Widget {
    TextBlock app{nullptr}, title{nullptr}, artist{nullptr}, glyph{nullptr};
    Border art{nullptr}, bg{nullptr}, tint{nullptr}, seekTrack{nullptr}, seekFill{nullptr};
    Grid seekHost{nullptr};
    FrameworkElement seekThumb{nullptr};
    Button prev{nullptr}, play{nullptr}, next{nullptr};
    winrt::event_token tokens[10]{};
    bool hooked = false;
    bool valid = false;
    DWORD ownerThread = 0;
};

static HANDLE g_stopEvent = nullptr;
static HANDLE g_thread = nullptr;

static std::mutex g_sessionLock;
static GlobalSystemMediaTransportControlsSession g_session{nullptr};

static std::mutex g_widgetLock;
static Widget g_w;
static std::wstring g_appliedArtKey;
static int g_appliedHasArt = -1;
static int g_appliedLight = -1;
static ULONGLONG g_lastScan = 0;

// seekbar interaction (UI thread only)
static bool g_dragging = false;
static bool g_hover = false;
static double g_holdFrac = 0;
static ULONGLONG g_holdUntil = 0;
static int64_t g_startTicks = 0;
static int64_t g_endTicks = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <typename Async>
static auto WaitFor(Async const& op, int seconds = 3) {
    if (op.wait_for(std::chrono::seconds(seconds)) !=
        winrt::Windows::Foundation::AsyncStatus::Completed) {
        op.Cancel();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return op.GetResults();
}

static bool SystemUsesLightTheme() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) {
        return v != 0;
    }
    return false;
}

static std::wstring FriendlyAppName(winrt::hstring const& idh) {
    std::wstring id{std::wstring_view(idh)};
    std::wstring low = id;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    auto has = [&](const wchar_t* s) { return low.find(s) != std::wstring::npos; };
    if (has(L"spotify")) return L"Spotify";
    if (has(L"msedge")) return L"Edge";
    if (has(L"chrome")) return L"Chrome";
    if (has(L"firefox")) return L"Firefox";
    if (has(L"zunemusic") || has(L"mediaplayer")) return L"Media Player";
    if (has(L"vlc")) return L"VLC";
    auto bang = id.find(L'!');
    if (bang != std::wstring::npos) id = id.substr(bang + 1);
    if (id.size() > 4 && _wcsicmp(id.c_str() + id.size() - 4, L".exe") == 0)
        id.resize(id.size() - 4);
    return id.empty() ? L"Music" : id;
}

static InMemoryRandomAccessStream LoadArt(
    GlobalSystemMediaTransportControlsSessionMediaProperties const& props) {
    auto ref = props.Thumbnail();
    if (!ref) return InMemoryRandomAccessStream{nullptr};

    auto src = WaitFor(ref.OpenReadAsync());
    uint64_t size = src.Size();
    if (size == 0 || size > 8 * 1024 * 1024) return InMemoryRandomAccessStream{nullptr};

    DataReader reader(src.GetInputStreamAt(0));
    WaitFor(reader.LoadAsync((uint32_t)size));
    std::vector<uint8_t> bytes((size_t)size);
    reader.ReadBytes(bytes);

    InMemoryRandomAccessStream mem;
    DataWriter writer(mem.GetOutputStreamAt(0));
    writer.WriteBytes(bytes);
    WaitFor(writer.StoreAsync());
    WaitFor(writer.FlushAsync());
    writer.DetachStream();
    mem.Seek(0);
    return mem;
}

static std::shared_ptr<MediaState> BuildState(
    GlobalSystemMediaTransportControlsSessionManager const& mgr, ArtCache& cache) {
    auto st = std::make_shared<MediaState>();
    st->light = SystemUsesLightTheme();

    auto session = mgr.GetCurrentSession();
    {
        std::lock_guard<std::mutex> l(g_sessionLock);
        g_session = session;
    }
    if (!session) return st;

    st->hasSession = true;
    st->app = FriendlyAppName(session.SourceAppUserModelId());
    st->playing = session.GetPlaybackInfo().PlaybackStatus() ==
                  GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

    auto props = WaitFor(session.TryGetMediaPropertiesAsync());
    st->title = std::wstring(std::wstring_view(props.Title()));
    st->artist = std::wstring(std::wstring_view(props.Artist()));

    auto tl = session.GetTimelineProperties();
    auto start = tl.StartTime();
    auto end = tl.EndTime();
    auto pos = tl.Position();
    if (st->playing) pos += (winrt::clock::now() - tl.LastUpdatedTime());
    double range = (double)(end - start).count();
    if (range > 0) {
        st->startTicks = (int64_t)start.count();
        st->endTicks = (int64_t)end.count();
        st->progress = std::clamp((double)(pos - start).count() / range * 100.0, 0.0, 100.0);
    }

    std::wstring key = st->app + L"|" + st->title + L"|" + st->artist + L"|" +
                       std::wstring(std::wstring_view(props.AlbumTitle()));
    bool reload = false;
    if (key != cache.key) {
        cache.key = key;
        cache.stream = InMemoryRandomAccessStream{nullptr};
        cache.retries = 0;
        reload = true;
    } else if (!cache.stream && cache.retries < 10) {
        cache.retries++;
        reload = true;
    }
    if (reload) {
        try {
            cache.stream = LoadArt(props);
        } catch (...) {
            cache.stream = InMemoryRandomAccessStream{nullptr};
        }
    }
    st->art = cache.stream;
    st->artKey = key;
    return st;
}

static void DoControl(int cmd) {
    GlobalSystemMediaTransportControlsSession s{nullptr};
    {
        std::lock_guard<std::mutex> l(g_sessionLock);
        s = g_session;
    }
    if (!s) return;
    try {
        if (cmd == 0) (void)s.TrySkipPreviousAsync();
        else if (cmd == 1) (void)s.TryTogglePlayPauseAsync();
        else (void)s.TrySkipNextAsync();
    } catch (...) {
    }
}

static void DoSeek(double frac) {
    if (g_endTicks <= g_startTicks) return;
    GlobalSystemMediaTransportControlsSession s{nullptr};
    {
        std::lock_guard<std::mutex> l(g_sessionLock);
        s = g_session;
    }
    if (!s) return;
    int64_t target = g_startTicks + (int64_t)(frac * (double)(g_endTicks - g_startTicks));
    try {
        (void)s.TryChangePlaybackPositionAsync(target);
    } catch (...) {
    }
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
            if (name == L"MusicAppName") Take(w.app, fe, n);
            else if (name == L"MusicTitle") Take(w.title, fe, n);
            else if (name == L"MusicArtist") Take(w.artist, fe, n);
            else if (name == L"MusicArtGlyph") Take(w.glyph, fe, n);
            else if (name == L"MusicArt") Take(w.art, fe, n);
            else if (name == L"MusicBg") Take(w.bg, fe, n);
            else if (name == L"MusicBgTint") Take(w.tint, fe, n);
            else if (name == L"MusicSeekTrack") Take(w.seekTrack, fe, n);
            else if (name == L"MusicSeekFill") Take(w.seekFill, fe, n);
            else if (name == L"MusicSeekHost") Take(w.seekHost, fe, n);
            else if (name == L"MusicSeekThumb") Take(w.seekThumb, fe, n);
            else if (name == L"MusicPrev") Take(w.prev, fe, n);
            else if (name == L"MusicPlay") Take(w.play, fe, n);
            else if (name == L"MusicNext") Take(w.next, fe, n);
        }
    }
    int count = VisualTreeHelper::GetChildrenCount(o);
    for (int i = 0; i < count && n < kExpected; i++) {
        Scan(VisualTreeHelper::GetChild(o, i), w, n);
    }
}

static SolidColorBrush MakeBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return SolidColorBrush(ColorHelper::FromArgb(a, r, g, b));
}

static void SetSeekVisual(double frac) {
    double w = g_w.seekHost.ActualWidth();
    if (w <= 0) return;
    frac = std::clamp(frac, 0.0, 1.0);
    g_w.seekFill.Width(frac * w);
    double x = std::clamp(frac * w - 7.0, 0.0, std::max(0.0, w - 14.0));
    g_w.seekThumb.Margin(Thickness{x, 0, 0, 0});
}

static double FracFromEvent(PointerRoutedEventArgs const& e) {
    double w = g_w.seekHost.ActualWidth();
    if (w <= 0) return 0;
    double x = e.GetCurrentPoint(g_w.seekHost).Position().X;
    return std::clamp(x / w, 0.0, 1.0);
}

static void HookControls() {
    g_w.tokens[0] = g_w.prev.Click([](auto&&, auto&&) { DoControl(0); });
    g_w.tokens[1] = g_w.play.Click([](auto&&, auto&&) { DoControl(1); });
    g_w.tokens[2] = g_w.next.Click([](auto&&, auto&&) { DoControl(2); });

    g_w.tokens[3] = g_w.seekHost.PointerEntered([](IInsp const&, PointerRoutedEventArgs const&) {
        g_hover = true;
        g_w.seekThumb.Visibility(Visibility::Visible);
    });
    g_w.tokens[4] = g_w.seekHost.PointerExited([](IInsp const&, PointerRoutedEventArgs const&) {
        g_hover = false;
        if (!g_dragging) g_w.seekThumb.Visibility(Visibility::Collapsed);
    });
    g_w.tokens[5] = g_w.seekHost.PointerPressed([](IInsp const&, PointerRoutedEventArgs const& e) {
        if (g_endTicks <= g_startTicks) return;
        g_w.seekHost.CapturePointer(e.Pointer());
        g_dragging = true;
        g_w.seekThumb.Visibility(Visibility::Visible);
        SetSeekVisual(FracFromEvent(e));
        e.Handled(true);
    });
    g_w.tokens[6] = g_w.seekHost.PointerMoved([](IInsp const&, PointerRoutedEventArgs const& e) {
        if (g_dragging) SetSeekVisual(FracFromEvent(e));
    });
    g_w.tokens[7] = g_w.seekHost.PointerReleased([](IInsp const&, PointerRoutedEventArgs const& e) {
        if (!g_dragging) return;
        double frac = FracFromEvent(e);
        g_dragging = false;
        g_w.seekHost.ReleasePointerCapture(e.Pointer());
        SetSeekVisual(frac);
        g_holdFrac = frac;
        g_holdUntil = GetTickCount64() + 1500;
        DoSeek(frac);
        if (!g_hover) g_w.seekThumb.Visibility(Visibility::Collapsed);
        e.Handled(true);
    });
    g_w.tokens[8] = g_w.seekHost.PointerCaptureLost([](IInsp const&, PointerRoutedEventArgs const&) {
        if (!g_dragging) return;
        g_dragging = false;
        if (!g_hover) g_w.seekThumb.Visibility(Visibility::Collapsed);
    });
    g_w.tokens[9] = g_w.seekHost.PointerCanceled([](IInsp const&, PointerRoutedEventArgs const&) {
        if (!g_dragging) return;
        g_dragging = false;
        if (!g_hover) g_w.seekThumb.Visibility(Visibility::Collapsed);
    });
    g_w.hooked = true;
}

static void RefreshColors(bool light, bool hasArt) {
    auto text = light ? MakeBrush(255, 0x1A, 0x1A, 0x1A) : MakeBrush(255, 255, 255, 255);
    g_w.app.Foreground(text);
    g_w.title.Foreground(text);
    g_w.artist.Foreground(text);
    g_w.glyph.Foreground(text);
    g_w.prev.Foreground(text);
    g_w.next.Foreground(text);
    g_w.seekTrack.Background(light ? MakeBrush(0x33, 0, 0, 0) : MakeBrush(0x33, 255, 255, 255));

    if (hasArt) {
        g_w.tint.Background(light ? MakeBrush(0xB0, 255, 255, 255) : MakeBrush(0x99, 0, 0, 0));
    } else {
        g_w.tint.Background(MakeBrush(0, 0, 0, 0));
        g_w.bg.Background(light ? MakeBrush(255, 0xF3, 0xF3, 0xF3) : MakeBrush(255, 0x2B, 0x2B, 0x2B));
    }
}

static void ApplyOnUiThread(std::shared_ptr<MediaState> s) {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        DWORD me = GetCurrentThreadId();

        if (g_w.valid && g_w.ownerThread != me) return;  // widget lives on another view

        if (!g_w.valid || !g_w.play.IsLoaded()) {
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
            g_appliedArtKey.clear();
            g_appliedHasArt = -1;
            g_appliedLight = -1;
            g_dragging = false;
            g_hover = false;
            HookControls();
            Wh_Log(L"Media widget found and hooked");
        }

        g_w.app.Text(s->hasSession ? winrt::hstring(s->app) : winrt::hstring(L"Music"));
        g_w.title.Text(s->title.empty() ? winrt::hstring(L"Nothing playing")
                                        : winrt::hstring(s->title));
        g_w.artist.Text(winrt::hstring(s->artist));
        g_w.play.Content(winrt::box_value(s->playing ? L"\uE769" : L"\uE768"));

        // seekbar (not while the user is dragging; briefly keep the chosen spot after a seek)
        g_startTicks = s->startTicks;
        g_endTicks = s->endTicks;
        if (!g_dragging) {
            double frac = s->progress / 100.0;
            if (GetTickCount64() < g_holdUntil) frac = g_holdFrac;
            SetSeekVisual(frac);
        }

        // album art + blurred background
        int hasArt = s->art ? 1 : 0;
        if (hasArt) {
            if (g_appliedArtKey != s->artKey) {
                BitmapImage big;
                big.SetSourceAsync(s->art.CloneStream());
                ImageBrush artBrush;
                artBrush.ImageSource(big);
                artBrush.Stretch(Stretch::UniformToFill);
                g_w.art.Background(artBrush);
                g_w.glyph.Visibility(Visibility::Collapsed);

                // tiny decode, stretched across the tile = soft blur
                BitmapImage small;
                small.DecodePixelType(DecodePixelType::Logical);
                small.DecodePixelWidth(24);
                small.SetSourceAsync(s->art.CloneStream());
                ImageBrush bgBrush;
                bgBrush.ImageSource(small);
                bgBrush.Stretch(Stretch::UniformToFill);
                g_w.bg.Background(bgBrush);

                g_appliedArtKey = s->artKey;
            }
        } else if (g_appliedHasArt != 0) {
            g_w.art.Background(MakeBrush(255, 0x4C, 0x4A, 0x48));
            g_w.glyph.Visibility(Visibility::Visible);
            g_appliedArtKey.clear();
        }

        int light = s->light ? 1 : 0;
        if (hasArt != g_appliedHasArt || light != g_appliedLight) {
            RefreshColors(s->light, hasArt != 0);
            g_appliedHasArt = hasArt;
            g_appliedLight = light;
        }
    } catch (...) {
    }
}

static void TeardownOnUiThread() {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        if (g_w.valid && g_w.ownerThread == GetCurrentThreadId()) {
            if (g_w.hooked) {
                g_w.prev.Click(g_w.tokens[0]);
                g_w.play.Click(g_w.tokens[1]);
                g_w.next.Click(g_w.tokens[2]);
                g_w.seekHost.PointerEntered(g_w.tokens[3]);
                g_w.seekHost.PointerExited(g_w.tokens[4]);
                g_w.seekHost.PointerPressed(g_w.tokens[5]);
                g_w.seekHost.PointerMoved(g_w.tokens[6]);
                g_w.seekHost.PointerReleased(g_w.tokens[7]);
                g_w.seekHost.PointerCaptureLost(g_w.tokens[8]);
                g_w.seekHost.PointerCanceled(g_w.tokens[9]);
            }
            g_dragging = false;
            g_hover = false;
            g_w = Widget{};
        }
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void DispatchToViews(std::shared_ptr<MediaState> st) {
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
        GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        ArtCache cache;
        while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
            try {
                if (!mgr) mgr = WaitFor(GlobalSystemMediaTransportControlsSessionManager::RequestAsync(), 5);
                DispatchToViews(BuildState(mgr, cache));
            } catch (...) {
            }
            if (WaitForSingleObject(g_stopEvent, 500) == WAIT_OBJECT_0) break;
        }
        TeardownAllViews();
        {
            std::lock_guard<std::mutex> l(g_sessionLock);
            g_session = nullptr;
        }
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
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_stopEvent && g_thread;
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
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
}