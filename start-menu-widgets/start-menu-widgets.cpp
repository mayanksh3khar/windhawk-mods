// ==WindhawkMod==
// @id              start-menu-widgets
// @name            Start Menu Widgets
// @description     One mod for the whole custom Start menu: profile, music, weather, system stats (NET/RAM/CPU/GPU), clock, stopwatch, timer and quick launch tiles
// @version         0.3
// @author          you
// @include         StartMenuExperienceHost.exe
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -DWINRT_NO_MAKE_DETECTION -DWINRT_NO_MODULE_LOCK -lole32 -loleaut32 -lruntimeobject -lwinmm -lshell32 -lgdi32 -ldwmapi -lwinhttp -lpdh -ldxgi -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Start Menu Widgets

Replaces the Recent section of the Start menu with a grid of widgets.
This single mod replaces the old "Start Menu Widgets", "Start Menu Media Widget",
"Start Menu Weather Widget" and "Start Menu System Stats" mods - disable those.

The mod loads into two processes:
- **StartMenuExperienceHost.exe** draws every widget.
- **explorer.exe** only runs the system-stats collector (performance counters, GPU
  and optional CPU temperature are not reachable from the sandboxed Start menu).
  It hands each sample to the Start menu over a named pipe. No files are written.
*/
// ==/WindhawkModReadme==

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
- city: ""
  $name: Weather city
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
  $name: Weather refresh interval (minutes)
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
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <mmsystem.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>

#ifdef small
#undef small   // rpcndr.h macro; the media code has a variable with that name
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::ApplicationModel::Core;

// ---------- Layout ----------
static const double kTile = 152, kGap = 20;
static const int kCols = 4, kRows = 3;
static const wchar_t* kRootName = L"WH_WidgetsRoot";
static const wchar_t* kNs = L" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"";

// ---------- Helpers ----------
static std::wstring Placeholder(const wchar_t* text, const wchar_t* color) {
    std::wstring s = LR"X(<Border CornerRadius="8" Background=")X";
    s += color;
    s += LR"X("><TextBlock Text=")X";
    s += text;
    s += LR"X(" Foreground="White" FontSize="13" FontWeight="SemiBold" VerticalAlignment="Bottom" Margin="10,0,10,8" /></Border>)X";
    return s;
}

static std::wstring Tb(const wchar_t* name, const wchar_t* text, bool temp) {
    std::wstring s = LR"X(<TextBlock Name=")X";
    s += name;
    s += LR"X(" Text=")X";
    s += text;
    s += LR"X(" FontSize="10" Foreground="White" Opacity="0.75" VerticalAlignment="Center")X";
    if (temp) s += LR"X( Margin="3,0,0,0")X";
    s += L" />";
    return s;
}

static std::wstring Info(const wchar_t* icon, const wchar_t* name, const wchar_t* text, bool second,
                         const wchar_t* tempName = nullptr, const wchar_t* temp = nullptr) {
    std::wstring s = second ? LR"X(<StackPanel Orientation="Horizontal" Margin="0,1,0,0">)X"
                            : LR"X(<StackPanel Orientation="Horizontal">)X";
    s += LR"X(<TextBlock Text=")X";
    s += icon;
    s += LR"X(" FontFamily="Segoe Fluent Icons" FontSize="10" Foreground="White" Opacity="0.75" VerticalAlignment="Center" Margin="0,0,3,0" />)X";
    s += Tb(name, text, false);
    if (tempName) s += Tb(tempName, temp, true);
    s += L"</StackPanel>";
    return s;
}

// Shared template for NET / RAM / CPU / GPU tiles.
static std::wstring StatTile(const std::wstring& p, const wchar_t* title, const wchar_t* fill,
                             const wchar_t* line, bool send, const std::wstring& line1,
                             const std::wstring& line2) {
    std::wstring s = L"<Grid>";
    s += LR"X(<Border Name=")X"; s += p;
    s += LR"X(Bg" CornerRadius="8" Background="#191919" />)X";
    s += LR"X(<Canvas Width="152" Height="152" HorizontalAlignment="Left" VerticalAlignment="Top" IsHitTestVisible="False"><Path Stroke="#2C2C2C" StrokeThickness="1" Data="M0,19.5 H152 M0,38.5 H152 M0,57.5 H152 M0,76.5 H152 M0,95.5 H152 M0,114.5 H152 M0,133.5 H152" /></Canvas>)X";
    s += LR"X(<Grid Width="152" Height="152" HorizontalAlignment="Left" VerticalAlignment="Top" IsHitTestVisible="False"><Grid.Clip><RectangleGeometry Rect="0,0,152,152" /></Grid.Clip><Canvas Name=")X"; s += p;
    s += LR"X(GridV" Width="190" Height="152" HorizontalAlignment="Left"><Path Stroke="#2C2C2C" StrokeThickness="1" Data="M19.5,0 V152 M38.5,0 V152 M57.5,0 V152 M76.5,0 V152 M95.5,0 V152 M114.5,0 V152 M133.5,0 V152 M152.5,0 V152 M171.5,0 V152 M190.5,0 V152" /></Canvas></Grid>)X";
    s += LR"X(<Canvas Name=")X"; s += p;
    s += LR"X(Graph" Width="152" Height="152" HorizontalAlignment="Left" VerticalAlignment="Top" IsHitTestVisible="False"><Polygon Name=")X"; s += p;
    s += LR"X(Fill" Fill=")X"; s += fill;
    s += LR"X(" Points="0,152 0,150 152,150 152,152" /><Polyline Name=")X"; s += p;
    s += LR"X(Line" Stroke=")X"; s += line;
    s += LR"X(" StrokeThickness="1.5" StrokeLineJoin="Round" Points="0,150 152,150" />)X";
    if (send) {
        s += LR"X(<Polyline Name=")X"; s += p;
        s += LR"X(SendLine" Stroke=")X"; s += line;
        s += LR"X(" StrokeThickness="1.5" StrokeDashArray="2,2" StrokeLineJoin="Round" Points="0,151 152,151" />)X";
    }
    s += LR"X(</Canvas><Grid VerticalAlignment="Top" Margin="8,6,8,0" IsHitTestVisible="False"><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><StackPanel Grid.Column="0" HorizontalAlignment="Left" VerticalAlignment="Top">)X";
    s += line1;
    s += line2;
    s += LR"X(</StackPanel><TextBlock Grid.Column="1" Name=")X"; s += p;
    s += LR"X(Title" Text=")X"; s += title;
    s += LR"X(" FontSize="22" FontWeight="Normal" Foreground="White" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="8,0,0,0" /></Grid></Grid>)X";
    return s;
}

// ---------- Shared helpers for Stopwatch / Timer ----------
template <class T>
static T Get(FrameworkElement const& r, const wchar_t* n) { return r.FindName(n).try_as<T>(); }

static void Show(UIElement const& e, bool on) {
    e.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
}

static std::wstring Btn(const wchar_t* name, const wchar_t* text, const wchar_t* bg, int col = -1) {
    std::wstring s = L"<Button Name=\""; s += name;
    s += L"\" Content=\""; s += text;
    s += L"\" Background=\""; s += bg;
    s += L"\" MinWidth=\"0\" MinHeight=\"0\" Height=\"28\" Padding=\"0\" CornerRadius=\"4\" BorderThickness=\"0\" FontSize=\"12\" HorizontalAlignment=\"Stretch\" VerticalAlignment=\"Bottom\"";
    if (col >= 0) s += L" Grid.Column=\"" + std::to_wstring(col) + L"\"";
    s += L" />";
    return s;
}

static std::wstring BtnPair(const wchar_t* name, const std::wstring& a, const std::wstring& b) {
    std::wstring s = L"<Grid Name=\""; s += name;
    s += LR"X(" VerticalAlignment="Bottom" ColumnSpacing="6" Visibility="Collapsed"><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>)X";
    s += a; s += b; s += L"</Grid>";
    return s;
}

static std::wstring Header(const wchar_t* title) {
    std::wstring s = LR"X(<TextBlock Text=")X";
    s += title;
    s += LR"X(" FontSize="13" FontWeight="SemiBold" VerticalAlignment="Top" />)X";
    return s;
}

static const wchar_t* kTileOpen = LR"X(<Border CornerRadius="8" Background="#20888888" BorderBrush="#10000000" BorderThickness="1" Padding="10"><Grid>)X";
static const wchar_t* kNeutral = L"#33FFFFFF";

// ---------- Stopwatch ----------
static std::wstring StopwatchWidget() {
    std::wstring s = kTileOpen;
    s += Header(L"Stopwatch"); 
    s += LR"X(<TextBlock Name="SwTime" Text="00:00.00" FontSize="30" FontWeight="SemiBold" HorizontalAlignment="Center" VerticalAlignment="Center" Margin="0,-6,0,0" />)X";
    s += LR"X(<TextBlock Name="SwLapText" Visibility="Collapsed" FontSize="11" Opacity="0.7" HorizontalAlignment="Center" VerticalAlignment="Bottom" Margin="0,0,0,40" />)X";
    s += Btn(L"SwStart", L"START", kNeutral);
    s += BtnPair(L"SwRun",    Btn(L"SwLap",   L"Lap",    kNeutral, 0),  Btn(L"SwPause",  L"Pause",  kNeutral, 1));
    s += BtnPair(L"SwPaused", Btn(L"SwReset", L"Reset",  L"#B3A53838", 0), Btn(L"SwResume", L"Resume", L"#B3358A3A", 1));
    s += L"</Grid></Border>";
    return s;
}

struct SwState { int mode = 0; ULONGLONG t0 = 0, acc = 0, lapMark = 0; int laps = 0; };  // mode: 0 idle, 1 running, 2 paused

static std::wstring FmtSw(ULONGLONG ms) {
    wchar_t b[32];
    unsigned long long cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    if (h) swprintf_s(b, L"%llu:%02llu:%02llu", h, m, s);
    else   swprintf_s(b, L"%02llu:%02llu.%02llu", m, s, cs);
    return b;
}

static void WireStopwatch(FrameworkElement const& r) {
    auto time = Get<TextBlock>(r, L"SwTime");
    auto lapTxt = Get<TextBlock>(r, L"SwLapText");
    auto bStart = Get<Button>(r, L"SwStart");
    auto gRun = Get<UIElement>(r, L"SwRun");
    auto gPaused = Get<UIElement>(r, L"SwPaused");
    auto bLap = Get<Button>(r, L"SwLap");
    auto bPause = Get<Button>(r, L"SwPause");
    auto bReset = Get<Button>(r, L"SwReset");
    auto bResume = Get<Button>(r, L"SwResume");
    if (!time || !lapTxt || !bStart || !gRun || !gPaused || !bLap || !bPause || !bReset || !bResume) return;

    auto st = std::make_shared<SwState>();
    DispatcherTimer tick;
    tick.Interval(std::chrono::milliseconds(33));

    auto elapsed = [st] { return st->acc + (st->mode == 1 ? GetTickCount64() - st->t0 : 0); };
    auto layout = [=] {
        Show(bStart, st->mode == 0);
        Show(gRun, st->mode == 1);
        Show(gPaused, st->mode == 2);
    };
    tick.Tick([=](auto&&, auto&&) { time.Text(FmtSw(elapsed())); });

    bStart.Click([=](auto&&, auto&&) {
        st->mode = 1; st->t0 = GetTickCount64(); st->acc = 0;
        tick.Start(); layout();
    });
    bPause.Click([=](auto&&, auto&&) {
        st->acc = elapsed(); st->mode = 2;
        tick.Stop(); time.Text(FmtSw(st->acc)); layout();
    });
    bResume.Click([=](auto&&, auto&&) {
        st->mode = 1; st->t0 = GetTickCount64();
        tick.Start(); layout();
    });
    bLap.Click([=](auto&&, auto&&) {
        ULONGLONG e = elapsed();
        st->laps++;
        std::wstring t = L"Lap " + std::to_wstring(st->laps) + L"  " + FmtSw(e - st->lapMark);
        st->lapMark = e;
        lapTxt.Text(t);
        Show(lapTxt, true);
    });
    bReset.Click([=](auto&&, auto&&) {
        *st = SwState{};
        tick.Stop();
        time.Text(L"00:00.00");
        lapTxt.Text(L"");
        Show(lapTxt, false);
        layout();
    });
    layout();
}

// ---------- Timer ----------
static std::wstring TimerWidget() {
    std::wstring s = kTileOpen;
    s += Header(L"Timer"); 
    // setup page
    s += LR"X(<StackPanel Name="TmSetup" VerticalAlignment="Center" Margin="0,22,0,34" Spacing="6">)X";
    s += LR"X(<Grid Height="30"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>)X";
    s += LR"X(<TextBox Name="TmMinutes" Text="15" Width="60" Height="30" MinHeight="0" Padding="4,3,4,3" FontSize="15" TextAlignment="Center" MaxLength="3" InputScope="Number" />)X";
    s += LR"X(<TextBlock Grid.Column="1" Text="min" FontSize="13" Opacity="0.8" VerticalAlignment="Center" Margin="8,0,0,0" />)X";
    s += LR"X(<StackPanel Grid.Column="2" VerticalAlignment="Center"><Button Name="TmUp" Width="26" Height="14" MinWidth="0" MinHeight="0" Padding="0" BorderThickness="0" Background="Transparent" FontFamily="Segoe Fluent Icons" FontSize="8" Content="&#xE70E;" /><Button Name="TmDown" Width="26" Height="14" MinWidth="0" MinHeight="0" Padding="0" BorderThickness="0" Background="Transparent" FontFamily="Segoe Fluent Icons" FontSize="8" Content="&#xE70D;" /></StackPanel></Grid>)X";
    s += LR"X(<StackPanel Orientation="Horizontal" Spacing="6" HorizontalAlignment="Center"><Button Name="TmP5" Content="+5m" Width="54" Height="24" MinWidth="0" MinHeight="0" Padding="0" FontSize="11" CornerRadius="12" BorderThickness="0" Background="#33FFFFFF" /><Button Name="TmP10" Content="+10m" Width="54" Height="24" MinWidth="0" MinHeight="0" Padding="0" FontSize="11" CornerRadius="12" BorderThickness="0" Background="#33FFFFFF" /></StackPanel></StackPanel>)X";
    // running page (same size/position as the clock's time)
    s += LR"X(<TextBlock Name="TmTime" Text="15:00" Visibility="Collapsed" FontSize="40" FontWeight="SemiBold" HorizontalAlignment="Center" VerticalAlignment="Center" />)X";
    s += LR"X(<TextBlock Name="TmNote" Text="Time's up" Visibility="Collapsed" FontSize="12" Opacity="0.7" HorizontalAlignment="Center" VerticalAlignment="Bottom" Margin="0,0,0,34" />)X";
    s += Btn(L"TmStart", L"START", kNeutral);
    s += BtnPair(L"TmRun", Btn(L"TmStop", L"Stop", L"#B3A53838", 0), Btn(L"TmPlus", L"+1 min", kNeutral, 1));
    s += L"</Grid></Border>";
    return s;
}

struct TmState { int mode = 0; ULONGLONG end = 0; int flashN = 0; };   // mode: 0 setup, 1 running, 2 finished

static int ReadMins(TextBox const& tb) {
    int v = _wtoi(tb.Text().c_str());
    return v < 1 ? 1 : (v > 999 ? 999 : v);
}
static void SetMins(TextBox const& tb, int v) {
    tb.Text(std::to_wstring(v < 1 ? 1 : (v > 999 ? 999 : v)));
}

static void PlayAlarm() {
    wchar_t path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\Media\\Alarm01.wav");
    // PlaySound goes to the default audio device chosen in Windows
    if (!PlaySoundW(path, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
        MessageBeep(MB_ICONEXCLAMATION);
}
static void StopAlarm() { PlaySoundW(nullptr, nullptr, 0); }

static void NotifyTimerDone() {
    try {
        using namespace winrt::Windows::UI::Notifications;
        using namespace winrt::Windows::Data::Xml::Dom;
        XmlDocument doc;
        doc.LoadXml(L"<toast><visual><binding template=\"ToastGeneric\"><text>Timer</text><text>Time's up</text></binding></visual><audio silent=\"true\"/></toast>");
        ToastNotification toast(doc);
        auto n = ToastNotificationManager::CreateToastNotifier();
        if (n.Setting() != NotificationSetting::Enabled) {
            // Start menu host has no notification entry: borrow one that can show toasts
            n = ToastNotificationManager::CreateToastNotifier(
                L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe");
        }
        n.Show(toast);
    } catch (...) {}
}

static void WireTimer(FrameworkElement const& r) {
    auto border = r.try_as<Border>();
    auto mins = Get<TextBox>(r, L"TmMinutes");
    auto setup = Get<UIElement>(r, L"TmSetup");
    auto time = Get<TextBlock>(r, L"TmTime");
    auto note = Get<UIElement>(r, L"TmNote");
    auto bStart = Get<Button>(r, L"TmStart");
    auto gRun = Get<UIElement>(r, L"TmRun");
    auto bStop = Get<Button>(r, L"TmStop");
    auto bPlus = Get<Button>(r, L"TmPlus");
    auto bUp = Get<Button>(r, L"TmUp");
    auto bDown = Get<Button>(r, L"TmDown");
    auto bP5 = Get<Button>(r, L"TmP5");
    auto bP10 = Get<Button>(r, L"TmP10");
    if (!border || !mins || !setup || !time || !note || !bStart || !gRun || !bStop || !bPlus || !bUp || !bDown || !bP5 || !bP10) return;

    auto st = std::make_shared<TmState>();
    DispatcherTimer tick;
    tick.Interval(std::chrono::milliseconds(200));

    // background flash (orange <-> normal), stops after ~15 s or when dismissed
    DispatcherTimer flash;
    flash.Interval(std::chrono::milliseconds(450));
    auto origBrush = border.Background();
    Media::SolidColorBrush flashBrush(winrt::Windows::UI::Color{0xB0, 0xE0, 0x5A, 0x1E});
    flash.Tick([=](auto&&, auto&&) {
        st->flashN++;
        if (st->flashN > 33) { flash.Stop(); border.Background(origBrush); return; }
        if (st->flashN % 2) border.Background(flashBrush);
        else border.Background(origBrush);
    });
    auto stopFlash = [=] {
        flash.Stop();
        st->flashN = 0;
        border.Background(origBrush);
        StopAlarm();
    };

    auto layout = [=] {
        Show(setup, st->mode == 0);
        Show(bStart, st->mode == 0);
        Show(time, st->mode != 0);
        Show(gRun, st->mode != 0);
        Show(note, st->mode == 2);
    };
    auto render = [=] {
        ULONGLONG now = GetTickCount64();
        ULONGLONG rem = st->end > now ? st->end - now : 0;
        ULONGLONG sec = (rem + 999) / 1000;
        wchar_t b[32];
        if (sec >= 3600) {
            swprintf_s(b, L"%llu:%02llu:%02llu", sec / 3600, (sec / 60) % 60, sec % 60);
            time.FontSize(28);
        } else {
            swprintf_s(b, L"%02llu:%02llu", sec / 60, sec % 60);
            time.FontSize(40);
        }
        time.Text(b);
        if (rem == 0 && st->mode == 1) {
            st->mode = 2;
            tick.Stop();
            PlayAlarm();
            NotifyTimerDone();
            st->flashN = 0;
            flash.Start();
            layout();
        }
    };
    tick.Tick([=](auto&&, auto&&) { render(); });

    bUp.Click([=](auto&&, auto&&) { SetMins(mins, ReadMins(mins) + 1); });
    bDown.Click([=](auto&&, auto&&) { SetMins(mins, ReadMins(mins) - 1); });
    bP5.Click([=](auto&&, auto&&) { SetMins(mins, ReadMins(mins) + 5); });
    bP10.Click([=](auto&&, auto&&) { SetMins(mins, ReadMins(mins) + 10); });

    bStart.Click([=](auto&&, auto&&) {
        int m = ReadMins(mins);
        SetMins(mins, m);
        st->end = GetTickCount64() + (ULONGLONG)m * 60000;
        st->mode = 1;
        render(); layout(); tick.Start();
    });
    bPlus.Click([=](auto&&, auto&&) {
        stopFlash();
        ULONGLONG now = GetTickCount64();
        if (st->mode == 2) st->end = now + 60000;
        else st->end += 60000;
        st->mode = 1;
        render(); layout(); tick.Start();
    });
    bStop.Click([=](auto&&, auto&&) {
        stopFlash();
        st->mode = 0;
        tick.Stop();
        layout();
    });
    layout();
}

// ---------- Widgets ----------
static std::wstring ProfileWidget() {
    return LR"X(<HyperlinkButton NavigateUri="ms-settings:" Padding="0" CornerRadius="8" HorizontalAlignment="Stretch" VerticalAlignment="Stretch" HorizontalContentAlignment="Stretch" VerticalContentAlignment="Stretch"><Border Name="ProfBorder" CornerRadius="8"><Border.Background><ImageBrush Stretch="UniformToFill" /></Border.Background><Grid VerticalAlignment="Bottom" Height="44" CornerRadius="0,0,8,8"><Grid.Background><LinearGradientBrush StartPoint="0,0" EndPoint="0,1"><GradientStop Color="#00000000" Offset="0"/><GradientStop Color="#B3000000" Offset="1"/></LinearGradientBrush></Grid.Background><TextBlock Name="ProfName" Text="" Foreground="White" FontSize="13" FontWeight="SemiBold" TextTrimming="CharacterEllipsis" HorizontalAlignment="Left" VerticalAlignment="Bottom" Margin="10,0,10,8" /></Grid></Border></HyperlinkButton>)X";
}

static std::wstring MusicWidget() {
    return LR"X(<Grid><Border Name="MusicBg" CornerRadius="8" Background="#2B2B2B" /><Border CornerRadius="8" Opacity="0.6" IsHitTestVisible="False"><Border.Background><AcrylicBrush BackgroundSource="Backdrop" TintColor="#00000000" TintOpacity="0" TintLuminosityOpacity="0" FallbackColor="#00000000" /></Border.Background></Border><Border Name="MusicBgTint" CornerRadius="8" Background="Transparent" IsHitTestVisible="False" /><Grid><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions><Border Name="MusicArt" Width="120" Height="120" Margin="16" CornerRadius="6" Background="#4C4A48" VerticalAlignment="Center"><TextBlock Name="MusicArtGlyph" Text="&#xE8D6;" FontFamily="Segoe Fluent Icons" FontSize="40" Foreground="White" Opacity="0.6" HorizontalAlignment="Center" VerticalAlignment="Center" /></Border><StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="0,0,16,0"><TextBlock Name="MusicAppName" Text="Music" Foreground="White" Opacity="0.7" FontSize="11" /><TextBlock Name="MusicTitle" Text="Nothing playing" Foreground="White" FontSize="15" FontWeight="SemiBold" TextTrimming="CharacterEllipsis" /><TextBlock Name="MusicArtist" Text="" Foreground="White" Opacity="0.7" FontSize="12" TextTrimming="CharacterEllipsis" /><Grid Name="MusicSeekHost" Height="16" Margin="0,4,0,4" Background="Transparent"><Border Name="MusicSeekTrack" Height="4" CornerRadius="2" Background="#33FFFFFF" VerticalAlignment="Center" /><Border Name="MusicSeekFill" Height="4" Width="0" CornerRadius="2" HorizontalAlignment="Left" VerticalAlignment="Center" Background="{ThemeResource SystemControlHighlightAccentBrush}" /><Ellipse Name="MusicSeekThumb" Width="14" Height="14" HorizontalAlignment="Left" VerticalAlignment="Center" Visibility="Collapsed" IsHitTestVisible="False" Fill="{ThemeResource SystemControlHighlightAccentBrush}" Stroke="White" StrokeThickness="2" /></Grid><StackPanel Orientation="Horizontal" Spacing="8"><Button Name="MusicPrev" Width="32" Height="32" Padding="0" CornerRadius="16" Background="Transparent" BorderThickness="0" Foreground="White" FontFamily="Segoe Fluent Icons" FontSize="14" Content="&#xE100;" /><Button Name="MusicPlay" Width="32" Height="32" Padding="0" CornerRadius="16" BorderThickness="0" Foreground="White" Background="{ThemeResource SystemControlHighlightAccentBrush}" FontFamily="Segoe Fluent Icons" FontSize="14" Content="&#xE768;" /><Button Name="MusicNext" Width="32" Height="32" Padding="0" CornerRadius="16" Background="Transparent" BorderThickness="0" Foreground="White" FontFamily="Segoe Fluent Icons" FontSize="14" Content="&#xE101;" /></StackPanel></StackPanel></Grid></Grid>)X";
}

static std::wstring WeatherWidget() {
    return LR"X(<Grid><Border Name="WeatherBg" CornerRadius="8"><Border.Background><ImageBrush ImageSource="https://images.unsplash.com/photo-1754764979352-ed7ce14dc545?fm=jpg&amp;q=60&amp;w=400&amp;auto=format&amp;fit=crop" Stretch="UniformToFill" /></Border.Background></Border><Border CornerRadius="8" Opacity="0.5" IsHitTestVisible="False"><Border.Background><AcrylicBrush BackgroundSource="Backdrop" TintColor="#00000000" TintOpacity="0" TintLuminosityOpacity="0" FallbackColor="#00000000" /></Border.Background></Border><Border CornerRadius="8" IsHitTestVisible="False"><Border.Background><LinearGradientBrush StartPoint="0,0" EndPoint="0,1"><GradientStop Color="#1A000000" Offset="0"/><GradientStop Color="#66000000" Offset="1"/></LinearGradientBrush></Border.Background></Border><StackPanel VerticalAlignment="Center" Margin="14,0,10,0"><Grid><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions><Image Name="WeatherIcon" Source="https://cdn.jsdelivr.net/gh/microsoft/fluentui-emoji@latest/assets/Sun%20behind%20cloud/3D/sun_behind_cloud_3d.png" Width="40" Height="40" VerticalAlignment="Center" Margin="0,0,6,0" /><StackPanel Grid.Column="1" VerticalAlignment="Center"><TextBlock Name="WeatherTemp" Text="28°C" Foreground="White" FontSize="24" FontWeight="SemiBold" /><TextBlock Name="WeatherCondition" Text="Partly cloudy" Foreground="White" Opacity="0.6" FontSize="11" Margin="0,-2,0,0" TextTrimming="CharacterEllipsis" /></StackPanel></Grid><TextBlock Name="WeatherAqi" Text="AQI 52 (Good)" Foreground="White" FontSize="12" Margin="0,8,0,0" /><TextBlock Name="WeatherCity" Text="Hyderabad" Foreground="White" Opacity="0.85" FontSize="12" Margin="0,2,0,0" /></StackPanel></Grid>)X";
}

static std::wstring NetWidget() {
    return StatTile(L"Net", L"NET", L"#40E6004D", L"#F5628E", true,
        Info(L"&#xE74A;", L"NetUpValue", L"&#58; 3.1 MB/s", false),
        Info(L"&#xE74B;", L"NetDownValue", L"&#58; 12.4 MB/s", true));
}
static std::wstring RamWidget() {
    return StatTile(L"Ram", L"RAM", L"#380046C8", L"#5C9EFA", false,
        Info(L"&#xEB44;", L"RamPercent", L"48%", false),
        Info(L"&#xEEA0;", L"RamUsage", L"15.2 / 32 GB", true));
}
static std::wstring CpuWidget() {
    return StatTile(L"Cpu", L"CPU", L"#47006E8C", L"#39B8E3", false,
        Info(L"&#xEEA1;", L"CpuPercent", L"8%", false, L"CpuTemp", L"(68°C)"),
        Info(L"&#xE950;", L"CpuFreq", L"4.22 GHz", true));
}
static std::wstring GpuWidget() {
    return StatTile(L"Gpu", L"GPU", L"#599600AA", L"#DE68F2", false,
        Info(L"&#xE81E;", L"GpuPercent", L"2%", false, L"GpuTemp", L"(43°C)"),
        Info(L"&#xEEA0;", L"GpuVram", L"2.4 / 16 GB", true));
}

static std::wstring ClockWidget() {
    return LR"X(<Border CornerRadius="8" Background="#20888888" BorderBrush="#10000000" BorderThickness="1"><StackPanel VerticalAlignment="Center" HorizontalAlignment="Center"><TextBlock Text="Clock" FontSize="14" Opacity="0.85" HorizontalAlignment="Center" /><TextBlock Name="ClockTime" Text="00:00" FontSize="40" FontWeight="SemiBold" HorizontalAlignment="Center" Margin="0,2,0,2" /><TextBlock Name="ClockDate" Text="" FontSize="15" Opacity="0.85" HorizontalAlignment="Center" /></StackPanel></Border>)X";
}

// ----------------------- Account Widget Helper -----------------------

static std::wstring CurrentUserSid() {
    std::wstring out;
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return out;
}

// Windows records the account picture files per user in the registry
static std::wstring AccountPicturePath() {
    std::wstring sid = CurrentUserSid();
    if (sid.empty()) return L"";
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + sid;
    for (auto v : {L"Image448", L"Image424", L"Image240", L"Image208", L"Image1080", L"Image192"}) {
        wchar_t p[MAX_PATH];
        DWORD sz = sizeof(p);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), v, RRF_RT_REG_SZ, nullptr, p, &sz) == ERROR_SUCCESS)
            return p;
    }
    return L"";
}

static std::vector<BYTE> ReadFileBytes(const std::wstring& path) {
    std::vector<BYTE> d;
    if (path.empty()) return d;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return d;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 32 * 1024 * 1024) {
        d.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (!ReadFile(h, d.data(), (DWORD)d.size(), &rd, nullptr) || rd != d.size()) d.clear();
    }
    CloseHandle(h);
    return d;
}

static std::wstring UserDisplayName() {
    typedef BOOLEAN(WINAPI * GetUserNameExFn)(int, LPWSTR, PULONG);
    if (HMODULE m = LoadLibraryW(L"secur32.dll")) {
        auto f = (GetUserNameExFn)GetProcAddress(m, "GetUserNameExW");
        wchar_t b[256];
        ULONG sz = 256;
        if (f && f(3 /* NameDisplay */, b, &sz) && b[0]) return b;
    }
    wchar_t un[256];
    DWORD n = GetEnvironmentVariableW(L"USERNAME", un, 256);
    return (n && n < 256) ? std::wstring(un) : std::wstring(L"User");
}

static void WireProfile(FrameworkElement const& r) {
    // walk the tree directly instead of FindName
    auto hb = r.try_as<ContentControl>();
    if (!hb) return;
    auto border = hb.Content().try_as<Border>();
    if (!border) return;
    auto grid = border.Child().try_as<Grid>();
    if (!grid) return;
    auto nameTb = grid.Children().GetAt(0).try_as<TextBlock>();
    if (!nameTb) return;

    nameTb.Text(UserDisplayName());

    auto brush = border.Background().try_as<Media::ImageBrush>();
    if (!brush) return;
    auto dispatcher = Window::Current().Dispatcher();

    std::thread([=] {
        try { init_apartment(apartment_type::multi_threaded); } catch (...) {}
        try {
            auto bytes = ReadFileBytes(AccountPicturePath());
            if (bytes.empty()) return;
            winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            winrt::Windows::Storage::Streams::DataWriter w(stream);
            w.WriteBytes(bytes);
            w.StoreAsync().get();
            w.DetachStream();
            stream.Seek(0);
            dispatcher.RunAsync(CoreDispatcherPriority::Normal, [=] {
                try {
                    Media::Imaging::BitmapImage bmp;
                    bmp.SetSourceAsync(stream);
                    brush.ImageSource(bmp);
                } catch (...) {}
            });
        } catch (...) {}
    }).detach();
}

// ---------- Quick Launch ----------
static const GUID kIidBufferByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
struct IBufferByteAccessQL : ::IUnknown {
    virtual HRESULT __stdcall Buffer(uint8_t** value) = 0;
};
static const PROPERTYKEY kPkeyAumid = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
static const PROPERTYKEY kPkeyTargetPath = {{0xB9B4B3FC, 0x2B51, 0x4A42, {0xB5, 0xD8, 0x32, 0x41, 0x46, 0xAF, 0xCF, 0x25}}, 2};

static const PROPERTYKEY kPkeyFileDesc = {{0x0CEF7D53, 0xFA64, 0x11D1, {0xA2, 0x03, 0x00, 0x00, 0xF8, 0x1F, 0xED, 0xEE}}, 3};

struct AppInfo { std::wstring key, name, aumid, exe; };   // key: "A|<aumid>" or "E|<exe path>"
struct IconData { int w = 0, h = 0, src = 0; std::vector<BYTE> px; };   // src: 1 = app ID / window icon, 0 = exe fallback

static std::mutex g_qlMu;
static std::vector<std::wstring> g_recent;              // keys, most recent first
static std::map<std::wstring, AppInfo> g_apps;          // key -> info
static std::map<std::wstring, IconData> g_icons;
static std::atomic<bool> g_qlStarted{false}, g_qlStop{false};
static std::thread g_qlThread;
static const size_t kRecentMax = 16;

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
static bool IsUwpAumid(const std::wstring& a) { return a.find(L'!') != std::wstring::npos; }

static AppInfo AppFromKey(const std::wstring& key) {
    AppInfo a;
    a.key = key;
    if (key.rfind(L"A|", 0) == 0) a.aumid = key.substr(2);
    else if (key.rfind(L"E|", 0) == 0) a.exe = key.substr(2);
    return a;
}

static void SaveRecentLocked() {
    std::wstring s;
    for (auto& k : g_recent) {
        auto& a = g_apps[k];
        s += k; s += L'\t'; s += a.exe; s += L'\t'; s += a.name; s += L'\n';
    }
    Wh_SetStringValue(L"quickLaunchRecent", s.c_str());
}

static void LoadRecent() {
    std::vector<wchar_t> buf(16384, 0);
    Wh_GetStringValue(L"quickLaunchRecent", buf.data(), buf.size());
    std::wstring s = buf.data();
    std::lock_guard<std::mutex> lk(g_qlMu);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find(L'\n', pos);
        if (e == std::wstring::npos) e = s.size();
        std::wstring line = s.substr(pos, e - pos);
        pos = e + 1;
        size_t t1 = line.find(L'\t');
        size_t t2 = t1 == std::wstring::npos ? std::wstring::npos : line.find(L'\t', t1 + 1);
        std::wstring key = line.substr(0, t1);
        if (key.rfind(L"A|", 0) != 0 && key.rfind(L"E|", 0) != 0) continue;
        if (key.rfind(L"E|", 0) == 0 && Lower(key).find(L"\\windowsapps\\") != std::wstring::npos) continue;
        AppInfo a = AppFromKey(key);
        if (t1 != std::wstring::npos && !a.aumid.empty())
            a.exe = line.substr(t1 + 1, t2 == std::wstring::npos ? std::wstring::npos : t2 - t1 - 1);
        if (t2 != std::wstring::npos) a.name = line.substr(t2 + 1);
        if (g_recent.size() < kRecentMax) { g_apps[key] = a; g_recent.push_back(key); }
    }
}

static void RecordUse(const AppInfo& a, bool front) {
    std::lock_guard<std::mutex> lk(g_qlMu);
    auto it = std::find(g_recent.begin(), g_recent.end(), a.key);
    if (!front && it != g_recent.end()) return;
    std::wstring oldName = g_apps[a.key].name;
    g_apps[a.key] = a;
    if (g_apps[a.key].name.empty()) g_apps[a.key].name = oldName;
    if (it != g_recent.end()) g_recent.erase(it);
    if (front) g_recent.insert(g_recent.begin(), a.key);
    else g_recent.push_back(a.key);
    if (g_recent.size() > kRecentMax) g_recent.resize(kRecentMax);
    SaveRecentLocked();
}

// move an existing entry to the front (used when you click it)
static void BumpRecent(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_qlMu);
    auto it = std::find(g_recent.begin(), g_recent.end(), key);
    if (it == g_recent.end()) return;
    g_recent.erase(it);
    g_recent.insert(g_recent.begin(), key);
    SaveRecentLocked();
}

static bool IsAppWindow(HWND h) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return false;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked))) && cloaked) return false;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64)) {
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcsncmp(cls, L"Shell_", 6)) return false;
    }
    return true;
}

static void WindowIdentity(HWND h, std::wstring& aumid, std::wstring& exe) {
    aumid.clear(); exe.clear();
    com_ptr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(ps.put())))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kPkeyAumid, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) aumid = pv.pwszVal;
        PropVariantClear(&pv);
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t b[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(b);
        if (QueryFullProcessImageNameW(p, 0, b, &n)) exe = b;
        // packaged apps (new Notepad, etc.) whose window doesn't expose an app ID
        if (aumid.empty() && Lower(exe).find(L"applicationframehost.exe") == std::wstring::npos) {
            typedef LONG(WINAPI * GetAumidFn)(HANDLE, UINT32*, PWSTR);
            static GetAumidFn fn = (GetAumidFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId");
            if (fn) {
                wchar_t ab[256];
                UINT32 an = 256;
                if (fn(p, &an, ab) == ERROR_SUCCESS) aumid = ab;
            }
        }
        CloseHandle(p);
    }
}

// identify the app a window belongs to; false = ignore this window
static bool KeyForWindow(HWND h, AppInfo& out) {
    std::wstring wa, we;
    WindowIdentity(h, wa, we);
    std::wstring le = Lower(we);
    size_t sl = le.find_last_of(L'\\');
    std::wstring file = sl == std::wstring::npos ? le : le.substr(sl + 1);
    static const wchar_t* skip[] = {L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe",
                                    L"textinputhost.exe", L"shellexperiencehost.exe", L"lockapp.exe"};
    for (auto s : skip) if (file == s) return false;

    out = AppInfo{};
    if (!wa.empty()) {
        out.key = L"A|" + wa;
        out.aumid = wa;
        out.exe = IsUwpAumid(wa) ? L"" : we;
        return true;
    }
    if (!we.empty() && file != L"applicationframehost.exe") {
        out.key = L"E|" + we;
        out.exe = we;
        return true;
    }
    return false;
}

static bool AppMatches(const AppInfo& a, const std::wstring& wa, const std::wstring& we) {
    if (!a.aumid.empty() && !wa.empty() && Lower(a.aumid) == Lower(wa)) return true;
    if (!a.exe.empty() && !we.empty() && Lower(a.exe) == Lower(we)) return true;
    return false;
}

static bool GrabIcon(IShellItemImageFactory* f, IconData& out) {
    if (!f) return false;
    HBITMAP hb = nullptr;
    if (FAILED(f->GetImage(SIZE{64, 64}, SIIGBF_ICONONLY, &hb)) || !hb) return false;
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.w = w; out.h = h;
    out.px.assign((size_t)w * h * 4, 0);
    HDC dc = GetDC(nullptr);
    int ok = GetDIBits(dc, hb, 0, h, out.px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(hb);
    if (!ok) return false;
    bool anyAlpha = false;
    for (size_t i = 3; i < out.px.size(); i += 4) if (out.px[i]) { anyAlpha = true; break; }
    if (!anyAlpha) for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    return true;
}

static HICON WindowHicon(HWND h) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &r) && r) return (HICON)r;
    r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &r) && r) return (HICON)r;
    if (HICON i = (HICON)GetClassLongPtrW(h, GCLP_HICON)) return i;
    return (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
}

// the icon the taskbar button shows for this window
static bool GrabWindowIcon(HWND h, IconData& out) {
    HICON ic = WindowHicon(h);
    if (!ic) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        memset(bits, 0, 64 * 64 * 4);
        DrawIconEx(mem, 0, 0, ic, 64, 64, 0, nullptr, DI_NORMAL);
        GdiFlush();
        out.w = out.h = 64;
        out.px.assign((BYTE*)bits, (BYTE*)bits + 64 * 64 * 4);
        SelectObject(mem, old);
        bool anyAlpha = false;
        for (size_t i = 3; i < out.px.size(); i += 4) if (out.px[i]) { anyAlpha = true; break; }
        if (!anyAlpha)   // old-style icon without an alpha channel
            for (size_t i = 0; i < out.px.size(); i += 4)
                if (out.px[i] | out.px[i + 1] | out.px[i + 2]) out.px[i + 3] = 255;
        ok = true;
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return ok;
}

static std::wstring IconValueName(const std::wstring& key) {
    wchar_t b[48];
    swprintf_s(b, L"qlicon_%llx", (unsigned long long)std::hash<std::wstring>{}(key));
    return b;
}

static void SaveIcon(const std::wstring& key, const IconData& ic) {
    if (ic.w <= 0 || ic.w > 64 || ic.h <= 0 || ic.h > 64 || ic.px.size() != (size_t)ic.w * ic.h * 4) return;
    std::vector<BYTE> buf(12 + ic.px.size());
    int hdr[3] = {ic.w, ic.h, ic.src};
    memcpy(buf.data(), hdr, 12);
    memcpy(buf.data() + 12, ic.px.data(), ic.px.size());
    Wh_SetBinaryValue(IconValueName(key).c_str(), buf.data(), buf.size());
}

static bool LoadSavedIcon(const std::wstring& key, IconData& ic) {
    std::vector<BYTE> buf(12 + 64 * 64 * 4);
    size_t n = Wh_GetBinaryValue(IconValueName(key).c_str(), buf.data(), buf.size());
    if (n < 12) return false;
    int hdr[3];
    memcpy(hdr, buf.data(), 12);
    if (hdr[0] <= 0 || hdr[0] > 64 || hdr[1] <= 0 || hdr[1] > 64 || n != 12 + (size_t)hdr[0] * hdr[1] * 4) return false;
    ic.w = hdr[0]; ic.h = hdr[1]; ic.src = hdr[2];
    ic.px.assign(buf.begin() + 12, buf.begin() + n);
    return true;
}

// icon + name: app ID first, then the window's own icon, then the exe
static void LoadAppIcon(const AppInfo& a, HWND hwnd, IconData& ic, std::wstring& name) {
    auto fromItem = [&](const std::wstring& path, bool wantIcon, bool useDesc) -> bool {
        com_ptr<IShellItem2> item;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())))) return false;
        PWSTR p = nullptr;
        if (name.empty()) {
            if (useDesc) {
                if (SUCCEEDED(item->GetString(kPkeyFileDesc, &p)) && p) { name = p; CoTaskMemFree(p); }
            } else if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) { name = p; CoTaskMemFree(p); }
        }
        if (!wantIcon) return true;
        ic = IconData{};
        auto f = item.try_as<IShellItemImageFactory>();
        return f && GrabIcon(f.get(), ic);
    };

    bool got = false;
    if (!a.aumid.empty()) {
        got = fromItem(L"shell:AppsFolder\\" + a.aumid, true, false);
        if (got) ic.src = 1;
    }
    if (!got && hwnd) {
        ic = IconData{};
        got = GrabWindowIcon(hwnd, ic);
        if (got) ic.src = 1;
    }
    if (!got && !a.exe.empty()) {
        got = fromItem(a.exe, true, true);
        if (got) ic.src = 0;
    }
    if (!got) ic = IconData{};
    if (name.empty() && !a.exe.empty()) fromItem(a.exe, false, true);
    if (name.empty()) {
        std::wstring path = !a.exe.empty() ? a.exe : a.aumid;
        size_t sl = path.find_last_of(L'\\');
        name = sl == std::wstring::npos ? path : path.substr(sl + 1);
        if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".exe") name.resize(name.size() - 4);
    }
}

struct QlHit { DWORD pid; AppInfo app; HWND hwnd; };

static BOOL CALLBACK QlScanProc(HWND h, LPARAM lp) {
    auto* v = (std::vector<QlHit>*)lp;
    if (!IsAppWindow(h)) return TRUE;
    AppInfo a;
    if (!KeyForWindow(h, a)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    v->push_back(QlHit{pid, a, h});
    return TRUE;
}

struct QlFindCtx { const AppInfo* app; HWND found; };

static BOOL CALLBACK QlFindProc(HWND h, LPARAM lp) {
    auto* c = (QlFindCtx*)lp;
    if (!IsAppWindow(h)) return TRUE;
    std::wstring wa, we;
    WindowIdentity(h, wa, we);
    if (AppMatches(*c->app, wa, we)) { c->found = h; return FALSE; }
    return TRUE;
}

static void QlTracker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadRecent();
    {
        std::vector<std::wstring> keys;
        { std::lock_guard<std::mutex> lk(g_qlMu); keys = g_recent; }
        for (auto& k : keys) {
            IconData ic;
            if (LoadSavedIcon(k, ic)) { std::lock_guard<std::mutex> lk(g_qlMu); g_icons[k] = std::move(ic); }
        }
    }

    std::set<std::pair<DWORD, std::wstring>> known;
    bool first = true;

    while (!g_qlStop) {
        std::vector<QlHit> hits;
        EnumWindows(QlScanProc, (LPARAM)&hits);

        std::set<std::pair<DWORD, std::wstring>> cur;
        std::vector<QlHit> fresh;         // z-order, topmost first
        for (auto& h : hits) {
            auto key = std::make_pair(h.pid, h.app.key);
            if (cur.insert(key).second && !known.count(key)) fresh.push_back(h);
        }
        if (first) {
            for (auto& h : fresh) RecordUse(h.app, false);          // seed with what's already open
        } else {
            for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) RecordUse(it->app, true);
        }
        known = cur;
        first = false;

        // capture the real taskbar icon while the window is open
        for (auto& h : fresh) {
            IconData ic;
            std::wstring name;
            LoadAppIcon(h.app, h.hwnd, ic, name);
            std::lock_guard<std::mutex> lk(g_qlMu);
            if (std::find(g_recent.begin(), g_recent.end(), h.app.key) == g_recent.end()) continue;
            if (ic.w > 0) {
                auto it = g_icons.find(h.app.key);
                if (it == g_icons.end() || it->second.w == 0 || it->second.src <= ic.src) {
                    g_icons[h.app.key] = ic;
                    SaveIcon(h.app.key, ic);
                }
            }
            auto ap = g_apps.find(h.app.key);
            if (ap != g_apps.end() && !name.empty() && ap->second.name != name) {
                ap->second.name = name;
                SaveRecentLocked();
            }
        }

        // remembered apps that still have no icon (not open right now)
        std::vector<std::wstring> want;
        { std::lock_guard<std::mutex> lk(g_qlMu); want = g_recent; }
        for (auto& key : want) {
            AppInfo a;
            {
                std::lock_guard<std::mutex> lk(g_qlMu);
                if (g_icons.count(key)) continue;
                a = g_apps[key];
            }
            IconData ic;
            std::wstring name = a.name;
            LoadAppIcon(a, nullptr, ic, name);
            std::lock_guard<std::mutex> lk(g_qlMu);
            if (!name.empty() && g_apps[key].name != name) { g_apps[key].name = name; SaveRecentLocked(); }
            if (ic.w > 0) SaveIcon(key, ic);
            g_icons[key] = std::move(ic);            // w == 0 means "no icon", not retried
        }

        for (int i = 0; i < 15 && !g_qlStop; i++) Sleep(100);
    }
    CoUninitialize();
}

// focus the app's window if it's open
static void ForceForeground(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, ft, FALSE);
    if (GetForegroundWindow() != h) {
        // last resort: a synthetic Alt tap makes Windows allow the focus change
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(h);
    }
}

// focus the app's window if it's open; otherwise launch it and focus it once its window appears
static void ActivateApp(std::wstring key) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppInfo app;
    {
        std::lock_guard<std::mutex> lk(g_qlMu);
        auto it = g_apps.find(key);
        if (it == g_apps.end()) { CoUninitialize(); return; }
        app = it->second;
    }

    QlFindCtx ctx{&app, nullptr};
    EnumWindows(QlFindProc, (LPARAM)&ctx);
    if (ctx.found) {
        ForceForeground(ctx.found);
        CoUninitialize();
        return;
    }

    if (IsUwpAumid(app.aumid) || app.exe.empty()) {
        std::wstring target = L"shell:AppsFolder\\" + app.aumid;
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", app.exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // wait for the new window, then focus it
    for (int i = 0; i < 50 && !g_qlStop; i++) {
        Sleep(200);
        ctx.found = nullptr;
        EnumWindows(QlFindProc, (LPARAM)&ctx);
        if (ctx.found) {
            Sleep(150);                 // let the app finish showing itself
            ForceForeground(ctx.found);
            break;
        }
    }
    CoUninitialize();
}

static std::wstring QuickLaunchWidget() {
    return LR"X(<Border CornerRadius="8" Background="#20888888" BorderBrush="#10000000" BorderThickness="1" Padding="10"><Grid><TextBlock Text="Quick Launch" FontSize="13" FontWeight="SemiBold" VerticalAlignment="Top" /><Grid Name="QlGrid" Margin="0,26,0,0" /><TextBlock Name="QlEmpty" Text="Apps you open appear here" FontSize="11" Opacity="0.6" TextWrapping="Wrap" TextAlignment="Center" HorizontalAlignment="Center" VerticalAlignment="Center" Margin="0,14,0,0" /></Grid></Border>)X";
}

static void WireQuickLaunch(FrameworkElement const& r) {
    auto grid = Get<Grid>(r, L"QlGrid");
    auto empty = Get<UIElement>(r, L"QlEmpty");
    if (!grid || !empty) return;
    if (!g_qlStarted.exchange(true)) g_qlThread = std::thread(QlTracker);

    // 4 icons per 1x1; scales with the tile's span: 8 for 2x1 / 1x2, 16 for 2x2
    int cols = 2 * Grid::GetColumnSpan(r), rows = 2 * Grid::GetRowSpan(r);
    int total = cols * rows;
    for (int i = 0; i < cols; i++) {
        ColumnDefinition d; d.Width(GridLength{1, GridUnitType::Star});
        grid.ColumnDefinitions().Append(d);
    }
    for (int i = 0; i < rows; i++) {
        RowDefinition d; d.Height(GridLength{1, GridUnitType::Star});
        grid.RowDefinitions().Append(d);
    }

    struct Cell { Button btn; Image img; };
    auto cells = std::make_shared<std::vector<Cell>>();
    auto shown = std::make_shared<std::vector<std::wstring>>(total);
    auto bmps = std::make_shared<std::map<std::wstring, Media::Imaging::WriteableBitmap>>();

    for (int i = 0; i < total; i++) {
        Button b;
        b.HorizontalAlignment(HorizontalAlignment::Stretch);
        b.VerticalAlignment(VerticalAlignment::Stretch);
        b.MinWidth(0); b.MinHeight(0);
        b.Padding(Thickness{0, 0, 0, 0});
        b.BorderThickness(Thickness{0, 0, 0, 0});
        b.CornerRadius(CornerRadius{6, 6, 6, 6});
        b.Background(Media::SolidColorBrush(Windows::UI::Colors::Transparent()));
        Image img;
        img.Width(32); img.Height(32);
        b.Content(img);
        Grid::SetRow(b, i / cols);
        Grid::SetColumn(b, i % cols);
        b.Visibility(Visibility::Collapsed);
        b.Click([shown, i](auto&&, auto&&) {
            std::wstring a = (*shown)[i];
            if (a.empty()) return;
            BumpRecent(a);
            std::thread(ActivateApp, a).detach();
        });
        grid.Children().Append(b);
        cells->push_back(Cell{b, img});
    }

    auto refresh = [=] {
        std::vector<std::pair<std::wstring, std::wstring>> items;   // aumid, name
        std::vector<std::pair<std::wstring, IconData>> need;
        {
            std::lock_guard<std::mutex> lk(g_qlMu);
            for (auto& a : g_recent) {
                if ((int)items.size() >= total) break;
                auto ic = g_icons.find(a);
                if (ic == g_icons.end() || ic->second.w == 0) continue;
                std::wstring nm = a;
                auto ap = g_apps.find(a);
                if (ap != g_apps.end() && !ap->second.name.empty()) nm = ap->second.name;
                items.push_back({a, nm});
                if (!bmps->count(a)) need.push_back({a, ic->second});
            }
        }
        for (auto& n : need) {
            Media::Imaging::WriteableBitmap wb(n.second.w, n.second.h);
            auto pixbuf = wb.PixelBuffer();
            IBufferByteAccessQL* access = nullptr;
            if (SUCCEEDED(((::IUnknown*)winrt::get_abi(pixbuf))->QueryInterface(kIidBufferByteAccess, (void**)&access)) && access) {
                uint8_t* dst = nullptr;
                if (SUCCEEDED(access->Buffer(&dst)) && dst) {
                    memcpy(dst, n.second.px.data(), n.second.px.size());
                    wb.Invalidate();
                    bmps->insert_or_assign(n.first, wb);
                }
                access->Release();
            }
        }
        bool same = true;
        for (int i = 0; i < total; i++) {
            const std::wstring want = i < (int)items.size() ? items[i].first : L"";
            if ((*shown)[i] != want) { same = false; break; }
        }
        if (same) return;
        for (int i = 0; i < total; i++) {
            auto& c = (*cells)[i];
            if (i < (int)items.size()) {
                (*shown)[i] = items[i].first;
                c.img.Source(bmps->at(items[i].first));
                ToolTipService::SetToolTip(c.btn, box_value(hstring(items[i].second)));
                c.btn.Visibility(Visibility::Visible);
            } else {
                (*shown)[i].clear();
                c.btn.Visibility(Visibility::Collapsed);
            }
        }
        Show(empty, items.empty());
    };

    DispatcherTimer t;
    t.Interval(std::chrono::seconds(1));
    t.Tick([=](auto&&, auto&&) { refresh(); });
    t.Start();
    refresh();
}
// ===========================================================================
// System stats (NET / RAM / CPU / GPU tiles)
//   explorer.exe       : collector thread samples PDH / DXGI / D3DKMT and hands every
//                        sample to the Start menu over a named pipe (no files)
//   Start menu process : reads the pipe once per second and draws the tiles
// ===========================================================================
namespace sysstats {

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml::Media;

using XPolygon = winrt::Windows::UI::Xaml::Shapes::Polygon;
using XPolyline = winrt::Windows::UI::Xaml::Shapes::Polyline;

static constexpr int kSamples = 31;     // graph points (about 30 seconds)
static constexpr double kW = 152.0;     // tile width and height in the styler XAML
static constexpr double kH = 152.0;

static HANDLE g_stopEvent = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_collectorMutex = nullptr;

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

struct Stats {
    long long netUp = -1, netDown = -1;           // bytes per second
    long long ramUsed = -1, ramTotal = -1;        // bytes
    int ramPct10 = -1;                            // tenths of a percent
    int cpuPct10 = -1, cpuMHz = -1, cpuTemp10 = -1;
    int gpuPct10 = -1, gpuTemp10 = -1;
    long long gpuVramUsed = -1, gpuVramTotal = -1;
};

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

// ---------------------------------------------------------------------------
// Named pipe: collector (server, explorer.exe) -> Start menu (client)
// ---------------------------------------------------------------------------

static const wchar_t* kPipeName = L"\\\\.\\pipe\\StartMenuWidgetsStats";
static constexpr uint32_t kMagic = 0x53575354;

struct PipeFrame {
    uint32_t magic;
    uint32_t size;
    ULONGLONG stamp;   // GetTickCount64 is system wide, so the reader can check freshness
    Stats s;
};

struct PipeServer {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE connEvt = nullptr;
    HANDLE writeEvt = nullptr;
    OVERLAPPED connOv = {};
    OVERLAPPED writeOv = {};
    bool connected = false;
    bool connecting = false;

    bool Open() {
        // the Start menu is a sandboxed (AppContainer) process: let ALL APPLICATION PACKAGES read the pipe
        std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
        std::wstring sid = CurrentUserSid();
        if (!sid.empty()) sddl += L"(A;;GA;;;" + sid + L")";
        sddl += L"(A;;GR;;;S-1-15-2-1)";

        PSECURITY_DESCRIPTOR psd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &psd, nullptr)) {
            Wh_Log(L"Could not build the pipe security descriptor (error %lu)", GetLastError());
            return false;
        }
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = psd;
        sa.bInheritHandle = FALSE;
        pipe = CreateNamedPipeW(kPipeName,
                                PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 4096, 4096, 0, &sa);
        DWORD err = GetLastError();
        LocalFree(psd);
        if (pipe == INVALID_HANDLE_VALUE) {
            Wh_Log(L"Could not create the stats pipe (error %lu)", err);
            return false;
        }
        connEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BeginConnect();
        return true;
    }

    void BeginConnect() {
        connected = false;
        connecting = false;
        ResetEvent(connEvt);
        ZeroMemory(&connOv, sizeof(connOv));
        connOv.hEvent = connEvt;
        if (ConnectNamedPipe(pipe, &connOv)) {
            connected = true;
            return;
        }
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) connecting = true;
        else if (e == ERROR_PIPE_CONNECTED) connected = true;
        else Wh_Log(L"ConnectNamedPipe failed (error %lu)", e);
    }

    void Drop() {
        DisconnectNamedPipe(pipe);
        BeginConnect();
    }

    void Publish(Stats const& s) {
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (connecting && WaitForSingleObject(connEvt, 0) == WAIT_OBJECT_0) {
            connecting = false;
            connected = true;
        }
        if (!connected) return;

        static bool loggedConnect = false;
        if (!loggedConnect) {
            loggedConnect = true;
            Wh_Log(L"Start menu connected to the stats pipe");
        }

        PipeFrame f = {};
        f.magic = kMagic;
        f.size = sizeof(PipeFrame);
        f.stamp = GetTickCount64();
        f.s = s;

        ResetEvent(writeEvt);
        ZeroMemory(&writeOv, sizeof(writeOv));
        writeOv.hEvent = writeEvt;
        DWORD n = 0;
        BOOL ok = WriteFile(pipe, &f, sizeof(f), nullptr, &writeOv);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(writeEvt, 500) != WAIT_OBJECT_0) CancelIoEx(pipe, &writeOv);
            ok = GetOverlappedResult(pipe, &writeOv, &n, TRUE);
        } else if (ok) {
            ok = GetOverlappedResult(pipe, &writeOv, &n, TRUE);
        }
        if (!ok || n != sizeof(f)) {
            loggedConnect = false;
            Drop();   // reader went away (or stopped reading): wait for the next one
        }
    }

    void Close() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (connEvt) { CloseHandle(connEvt); connEvt = nullptr; }
        if (writeEvt) { CloseHandle(writeEvt); writeEvt = nullptr; }
    }
};

static DWORD WINAPI CollectorWorker(LPVOID) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    {
        PipeServer pipe;
        pipe.Open();
        CollectorInit();

        int cfgVer = -1;
        int tick = 0;
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
            pipe.Publish(s);
            if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
        }
        CollectorShutdown();
        pipe.Close();
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

struct Tiles {
    std::map<std::wstring, FrameworkElement> el;
    std::map<std::wstring, TranslateTransform> tt;
    FrameworkElement anchor{nullptr};
    bool valid = false;
    DWORD owner = 0;
};

static const size_t kKnownCount = 23;   // named elements across the four tiles
static std::mutex g_tilesLock;
static Tiles g_t;
static int g_appliedVersion = -1;

template <typename T>
static T El(const wchar_t* name) {
    auto it = g_t.el.find(name);
    if (it == g_t.el.end()) return T{nullptr};
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

// called on the UI thread right after a stat tile has been created
static void Register(FrameworkElement const& root, std::initializer_list<const wchar_t*> names) {
    std::lock_guard<std::mutex> l(g_tilesLock);
    DWORD me = GetCurrentThreadId();
    if (g_t.owner != me) g_t = Tiles{};
    g_t.owner = me;
    for (auto n : names) {
        auto fe = root.FindName(n).try_as<FrameworkElement>();
        if (!fe) continue;
        g_t.el.insert_or_assign(std::wstring(n), fe);
        g_t.tt.erase(std::wstring(n));
    }
    auto it = g_t.el.find(L"NetLine");
    g_t.valid = g_t.el.size() >= kKnownCount && it != g_t.el.end();
    if (g_t.valid) g_t.anchor = it->second;
    g_appliedVersion = -1;
}

static void ApplyOnUiThread(std::shared_ptr<UiFrame> f) {
    try {
        std::lock_guard<std::mutex> l(g_tilesLock);
        if (!g_t.valid || g_t.owner != GetCurrentThreadId()) return;
        if (!g_t.anchor.IsLoaded()) return;
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
            auto it = g_t.tt.find(nm);
            if (it == g_t.tt.end()) {
                TranslateTransform t;
                cv.RenderTransform(t);
                it = g_t.tt.emplace(nm, t).first;
            }
            it->second.X(-f->gridOffset);
        }
        g_appliedVersion = f->version;
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

// used when the collector in explorer.exe is not reachable: RAM and CPU can be read in-process
static Stats LocalFallback() {
    Stats s;
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
        ULONGLONG used = ms.ullTotalPhys - ms.ullAvailPhys;
        s.ramUsed = (long long)used;
        s.ramPct10 = (int)((used * 1000ULL + ms.ullTotalPhys / 2) / ms.ullTotalPhys);
        s.ramTotal = (long long)ms.ullTotalPhys;
    }
    int p = CpuPct10FromTimes();
    if (p >= 0) s.cpuPct10 = p;
    return s;
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
        HANDLE pipe = INVALID_HANDLE_VALUE;
        Stats last;
        ULONGLONG stamp = 0;
        bool haveFrame = false;
        int lastState = -1;
        ULONGLONG lastOpenLog = 0;
        for (;;) {
            if (pipe == INVALID_HANDLE_VALUE) {
                pipe = CreateFileW(kPipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (pipe == INVALID_HANDLE_VALUE) {
                    ULONGLONG t = GetTickCount64();
                    if (lastOpenLog == 0 || t - lastOpenLog > 30000) {
                        lastOpenLog = t;
                        Wh_Log(L"Cannot open the stats pipe (error %lu). Is the collector running in explorer.exe?",
                               GetLastError());
                    }
                }
            }
            if (pipe != INVALID_HANDLE_VALUE) {
                for (;;) {  // take everything that is queued, keep the newest sample
                    DWORD avail = 0;
                    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
                        CloseHandle(pipe);
                        pipe = INVALID_HANDLE_VALUE;
                        break;
                    }
                    if (avail < sizeof(PipeFrame)) break;
                    PipeFrame fr = {};
                    DWORD rd = 0;
                    if (!ReadFile(pipe, &fr, sizeof(fr), &rd, nullptr) || rd != sizeof(fr)) {
                        CloseHandle(pipe);
                        pipe = INVALID_HANDLE_VALUE;
                        break;
                    }
                    if (fr.magic == kMagic && fr.size == sizeof(PipeFrame)) {
                        last = fr.s;
                        stamp = fr.stamp;
                        haveFrame = true;
                    }
                }
            }

            ULONGLONG now = GetTickCount64();
            bool fresh = haveFrame && (now < stamp || (now - stamp) < 5000);
            if ((int)fresh != lastState) {
                lastState = (int)fresh;
                Wh_Log(L"%s", fresh ? L"Receiving stats from the collector" : L"No fresh stats from the collector");
            }
            Stats s = fresh ? last : LocalFallback();
            DispatchToViews(BuildFrame(true, s, hist, gridOffset, ++version));
            if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
        }
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    }
    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
    return 0;
}

// ---- tile wiring (the main mod calls these right after it creates each tile) ----

static void WireNet(FrameworkElement const& r) {
    Register(r, {L"NetGridV", L"NetFill", L"NetLine", L"NetSendLine", L"NetUpValue", L"NetDownValue"});
}
static void WireRam(FrameworkElement const& r) {
    Register(r, {L"RamGridV", L"RamFill", L"RamLine", L"RamPercent", L"RamUsage"});
}
static void WireCpu(FrameworkElement const& r) {
    Register(r, {L"CpuGridV", L"CpuFill", L"CpuLine", L"CpuPercent", L"CpuTemp", L"CpuFreq"});
}
static void WireGpu(FrameworkElement const& r) {
    Register(r, {L"GpuGridV", L"GpuFill", L"GpuLine", L"GpuPercent", L"GpuTemp", L"GpuVram"});
}

// ---- lifecycle ----

static bool AcquireCollectorRole() {
    g_collectorMutex = CreateMutexW(nullptr, FALSE, L"Local\\StartMenuWidgetsStatsCollector");
    if (!g_collectorMutex) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_collectorMutex);
        g_collectorMutex = nullptr;
        return false;
    }
    return true;
}

static bool Start(bool isUi) {
    if (!isUi && !AcquireCollectorRole()) {
        Wh_Log(L"Another stats collector is already running");
        return true;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, isUi ? UiWorker : CollectorWorker, nullptr, 0, nullptr);
    return g_stopEvent && g_thread;
}

static void Stop() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
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

}  // namespace sysstats

// ===========================================================================
// Media widget (Music tile): current media session, art, seekbar and controls
// ===========================================================================
namespace mediaw {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;

using IInsp = winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::ColorHelper;

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

struct Ui {
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
static Ui g_w;
static std::wstring g_appliedArtKey;
static int g_appliedHasArt = -1;
static int g_appliedLight = -1;

// seekbar interaction (UI thread only)
static bool g_dragging = false;
static bool g_hover = false;
static double g_holdFrac = 0;
static ULONGLONG g_holdUntil = 0;
static int64_t g_startTicks = 0;
static int64_t g_endTicks = 0;

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

// called on the UI thread right after the Music tile has been created
static void Wire(FrameworkElement const& r) {
    Ui w;
    w.app = Get<TextBlock>(r, L"MusicAppName");
    w.title = Get<TextBlock>(r, L"MusicTitle");
    w.artist = Get<TextBlock>(r, L"MusicArtist");
    w.glyph = Get<TextBlock>(r, L"MusicArtGlyph");
    w.art = Get<Border>(r, L"MusicArt");
    w.bg = Get<Border>(r, L"MusicBg");
    w.tint = Get<Border>(r, L"MusicBgTint");
    w.seekTrack = Get<Border>(r, L"MusicSeekTrack");
    w.seekFill = Get<Border>(r, L"MusicSeekFill");
    w.seekHost = Get<Grid>(r, L"MusicSeekHost");
    w.seekThumb = Get<FrameworkElement>(r, L"MusicSeekThumb");
    w.prev = Get<Button>(r, L"MusicPrev");
    w.play = Get<Button>(r, L"MusicPlay");
    w.next = Get<Button>(r, L"MusicNext");
    if (!w.app || !w.title || !w.artist || !w.glyph || !w.art || !w.bg || !w.tint || !w.seekTrack ||
        !w.seekFill || !w.seekHost || !w.seekThumb || !w.prev || !w.play || !w.next) {
        return;
    }

    std::lock_guard<std::mutex> l(g_widgetLock);
    g_w = w;
    g_w.valid = true;
    g_w.ownerThread = GetCurrentThreadId();
    g_appliedArtKey.clear();
    g_appliedHasArt = -1;
    g_appliedLight = -1;
    g_dragging = false;
    g_hover = false;
    HookControls();
    Wh_Log(L"Media widget hooked");
}

static void ApplyOnUiThread(std::shared_ptr<MediaState> s) {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        if (!g_w.valid || g_w.ownerThread != GetCurrentThreadId()) return;  // widget lives on another view
        if (!g_w.play.IsLoaded()) return;

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

static bool Start() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_stopEvent && g_thread;
}

static void Stop() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

}  // namespace mediaw

// ===========================================================================
// Weather widget: Open-Meteo, ipwho.is, Fluent Emoji and Unsplash (no API keys)
// ===========================================================================
namespace weatherw {

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;

using IInsp = winrt::Windows::Foundation::IInspectable;

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

struct Ui {
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
static Ui g_w;
static int g_appliedVersion = -1;
static std::wstring g_appliedIcon;
static std::wstring g_appliedBg;

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

static JsonObject GetJson(std::wstring const& url) {
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

static Location ResolveLocation(Config const& cfg) {
    Location loc;
    if (!cfg.city.empty()) {
        std::wstring url =
            L"https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" +
            std::wstring(Uri::EscapeComponent(winrt::hstring(cfg.city)));
        auto json = GetJson(url);
        if (!json.HasKey(L"results")) {
            throw winrt::hresult_error(E_FAIL, L"City not found");
        }
        auto r = json.GetNamedArray(L"results").GetObjectAt(0);
        loc.lat = r.GetNamedNumber(L"latitude");
        loc.lon = r.GetNamedNumber(L"longitude");
        loc.city = std::wstring(std::wstring_view(r.GetNamedString(L"name", L"")));
    } else {
        auto json = GetJson(L"https://ipwho.is/?fields=success,city,latitude,longitude");
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

static std::shared_ptr<WeatherState> FetchWeather(Location const& loc, Config const& cfg, int version) {
    auto st = std::make_shared<WeatherState>();
    st->version = version;
    st->city = loc.city;

    std::wstring coords = L"&latitude=" + std::to_wstring(loc.lat) + L"&longitude=" + std::to_wstring(loc.lon);

    // Weather
    std::wstring wurl =
        L"https://api.open-meteo.com/v1/forecast?current=temperature_2m,weather_code,is_day&temperature_unit=" +
        std::wstring(cfg.fahrenheit ? L"fahrenheit" : L"celsius") + coords;
    auto w = GetJson(wurl).GetNamedObject(L"current");
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
        auto a = GetJson(aurl).GetNamedObject(L"current");
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

// called on the UI thread right after the Weather tile has been created
static void Wire(FrameworkElement const& r) {
    Ui w;
    w.temp = Get<TextBlock>(r, L"WeatherTemp");
    w.condition = Get<TextBlock>(r, L"WeatherCondition");
    w.aqi = Get<TextBlock>(r, L"WeatherAqi");
    w.city = Get<TextBlock>(r, L"WeatherCity");
    w.icon = Get<Image>(r, L"WeatherIcon");
    w.bg = Get<Border>(r, L"WeatherBg");
    if (!w.temp || !w.condition || !w.aqi || !w.city || !w.icon || !w.bg) return;

    std::lock_guard<std::mutex> l(g_widgetLock);
    g_w = w;
    g_w.valid = true;
    g_w.ownerThread = GetCurrentThreadId();
    g_appliedVersion = -1;
    g_appliedIcon.clear();
    g_appliedBg.clear();
    g_w.iconFailed = g_w.icon.ImageFailed([](IInsp const&, ExceptionRoutedEventArgs const&) {
        Wh_Log(L"Weather icon failed to load: %s", g_appliedIcon.c_str());
    });
    g_w.hooked = true;
    Wh_Log(L"Weather widget hooked");
}

static void ApplyOnUiThread(std::shared_ptr<WeatherState> s) {
    try {
        std::lock_guard<std::mutex> l(g_widgetLock);
        if (!g_w.valid || g_w.ownerThread != GetCurrentThreadId()) return;  // widget lives on another view
        if (!g_w.temp.IsLoaded()) return;

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

static DWORD WINAPI Worker(LPVOID) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    {
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
                        loc = ResolveLocation(cfg);
                        haveLoc = true;
                        locCfgVersion = cfgVer;
                        locTime = now;
                    }
                    last = FetchWeather(loc, cfg, ++version);
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
    }

    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
    return 0;
}

static bool Start() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_refreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_stopEvent && g_refreshEvent && g_thread;
}

static void SettingsChanged() {
    LoadSettings();
    if (g_refreshEvent) SetEvent(g_refreshEvent);
}

static void Stop() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
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

}  // namespace weatherw

// ---------- Widget table: row, column, row span, column span, builder, optional wire-up ----------
struct Widget {
    int row, col, row_span, col_span;
    std::function<std::wstring()> build;
    void (*wire)(FrameworkElement const&) = nullptr;   // runs on the UI thread right after the tile is created
};
static const Widget kWidgets[] = {
    {0, 0, 1, 1, ProfileWidget, WireProfile},    {0, 1, 1, 2, MusicWidget, mediaw::Wire},                                            {0, 3, 1, 1, WeatherWidget, weatherw::Wire},
    {1, 0, 1, 1, NetWidget, sysstats::WireNet},  {1, 1, 1, 1, RamWidget, sysstats::WireRam}, {1, 2, 1, 1, ClockWidget},                  {1, 3, 1, 1, StopwatchWidget, WireStopwatch},
    {2, 0, 1, 1, CpuWidget, sysstats::WireCpu},  {2, 1, 1, 1, GpuWidget, sysstats::WireGpu}, {2, 2, 1, 1, TimerWidget, WireTimer},      {2, 3, 1, 1, QuickLaunchWidget, WireQuickLaunch},
};
static Grid BuildRoot() {
    Grid g;
    g.Name(kRootName);
    g.HorizontalAlignment(HorizontalAlignment::Left);
    g.VerticalAlignment(VerticalAlignment::Top);
    g.ColumnSpacing(kGap);
    g.RowSpacing(kGap);
    for (int i = 0; i < kCols; i++) {
        ColumnDefinition d; d.Width(GridLength{kTile, GridUnitType::Pixel});
        g.ColumnDefinitions().Append(d);
    }
    for (int i = 0; i < kRows; i++) {
        RowDefinition d; d.Height(GridLength{kTile, GridUnitType::Pixel});
        g.RowDefinitions().Append(d);
    }
    for (auto& w : kWidgets) {
        try {
            std::wstring x = w.build();
            x.insert(x.find_first_of(L" >"), kNs);
            auto el = Markup::XamlReader::Load(x).as<FrameworkElement>();
            Grid::SetRow(el, w.row);
            Grid::SetColumn(el, w.col);
            Grid::SetRowSpan(el, w.row_span);
            Grid::SetColumnSpan(el, w.col_span);
            g.Children().Append(el);
            if (w.wire) { try { w.wire(el); } catch (...) {} }
        } catch (...) {}
    }
    return g;
}

// ---------- Tree patching ----------
struct Targets {
    TextBlock header{nullptr};
    GridView list{nullptr};
    Border bg{nullptr};
};

static bool IsOurs(Border const& b) {
    auto c = b.Child().try_as<FrameworkElement>();
    return c && c.Name() == kRootName;
}

static void Walk(DependencyObject o, Targets& t) {
    if (auto fe = o.try_as<FrameworkElement>()) {
        auto n = fe.Name();
        if (n == L"TopLevelSuggestionsListHeaderText") t.header = fe.try_as<TextBlock>();
        else if (n == L"RecommendedList") t.list = fe.try_as<GridView>();
        else if (n == L"TopLevelSuggestionsBackground") {
            t.bg = fe.try_as<Border>();
            if (t.bg && IsOurs(t.bg)) return;
        }
    }
    int count = Media::VisualTreeHelper::GetChildrenCount(o);
    for (int i = 0; i < count; i++) Walk(Media::VisualTreeHelper::GetChild(o, i), t);
}

// static void Dump(DependencyObject o, int d) {
//     if (auto fe = o.try_as<FrameworkElement>()) {
//         if (fe.Name() == kRootName || fe.ActualWidth() == 0 || fe.Visibility() == Visibility::Collapsed) return;
//         if (!fe.Name().empty()) {
//             auto cls = std::wstring(get_class_name(fe));
//             cls = cls.substr(cls.rfind(L'.') + 1);
//             auto y = fe.TransformToVisual(nullptr).TransformPoint({0, 0}).Y;
//             auto m = fe.Margin();
//             if(y > 480) {
//                 Wh_Log(L"%d %ls#%ls y=%.0f %.0fx%.0f m=%.0f,%.0f,%.0f,%.0f", d, cls.c_str(), fe.Name().c_str(),
//                    y, fe.ActualWidth(), fe.ActualHeight(), m.Left, m.Top, m.Right, m.Bottom);
//             }
            
//         }
//     }
//     int n = Media::VisualTreeHelper::GetChildrenCount(o);
//     for (int i = 0; i < n; i++) Dump(Media::VisualTreeHelper::GetChild(o, i), d + 1);
// }

static void EnsureClips(Grid const& wr) {
    for (auto c : wr.Children()) {
        auto el = c.try_as<FrameworkElement>();
        if (!el || el.ActualWidth() == 0) continue;
        auto vis = Hosting::ElementCompositionPreview::GetElementVisual(el);
        if (vis.Clip()) continue;
        Windows::Foundation::Numerics::float2 r{8, 8};
        vis.Clip(vis.Compositor().CreateRectangleClip(0, 0, (float)el.ActualWidth(), (float)el.ActualHeight(), r, r, r, r));
    }
}

static FrameworkElement FindNamed(DependencyObject o, std::wstring_view name) {
    if (auto fe = o.try_as<FrameworkElement>()) {
        if (std::wstring_view(fe.Name()) == name) return fe;
    }
    int count = Media::VisualTreeHelper::GetChildrenCount(o);
    for (int i = 0; i < count; i++) {
        if (auto r = FindNamed(Media::VisualTreeHelper::GetChild(o, i), name)) return r;
    }
    return nullptr;
}

static void UpdateClock(Grid const& wr) {
    auto time = FindNamed(wr, L"ClockTime").try_as<TextBlock>();
    auto date = FindNamed(wr, L"ClockDate").try_as<TextBlock>();
    if (!time || !date) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t t[16];
    swprintf_s(t, L"%02u:%02u", st.wHour, st.wMinute);   // 24h, like the template
    wchar_t d[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd, MMM d", d, ARRAYSIZE(d), nullptr);

    if (time.Text() != t) time.Text(t);
    if (date.Text() != d) date.Text(d);
}

static void Apply(UIElement root) {
    // if (GetAsyncKeyState(VK_F9) & 0x8000) Dump(root, 0);
    Targets t;
    Walk(root, t);
    if (t.header && t.header.Text() != L"Widgets") t.header.Text(L"Widgets");
    if (t.list && t.list.Visibility() != Visibility::Collapsed) t.list.Visibility(Visibility::Collapsed);
    if (t.bg && !IsOurs(t.bg)) {
        t.bg.Background(Media::SolidColorBrush(Windows::UI::Colors::Transparent()));
        t.bg.Child(BuildRoot());
    }
    Grid wr{nullptr};
    if (t.bg) wr = t.bg.Child().try_as<Grid>();
    if (!wr || wr.Name() != kRootName || wr.ActualWidth() == 0) return;

    EnsureClips(wr);
    UpdateClock(wr);

    // widgets left edge = heading text margin minus the container's own margin
    if (t.header) {
        auto pc = Media::VisualTreeHelper::GetParent(t.bg).try_as<FrameworkElement>();
        double want = t.header.Margin().Left - (pc ? pc.Margin().Left : 0);
        auto m = wr.Margin();
        if (m.Left != want) { m.Left = want; wr.Margin(m); }
    }
}

// ---------- Mod plumbing ----------
static std::atomic<bool> g_stop{false};
static std::thread g_thread;

static void Worker() {
    try { init_apartment(apartment_type::multi_threaded); } catch (...) {}
    while (!g_stop) {
        try {
            for (auto view : CoreApplication::Views()) {
                view.Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [] {
                    try {
                        if (auto root = Window::Current().Content()) Apply(root);
                    } catch (...) {}
                });
            }
        } catch (...) {}
        for (int i = 0; i < 10 && !g_stop; i++) Sleep(100);
    }
}

void RestartStartMenu() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"StartMenuExperienceHost.exe") == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProcess) {
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
}

static bool g_isStartHost = false;

static bool IsStartHostProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"StartMenuExperienceHost.exe") == 0;
}

BOOL Wh_ModInit() {
    g_isStartHost = IsStartHostProcess();
    sysstats::LoadSettings();

    if (!g_isStartHost) {
        // explorer.exe: only the stats collector lives here
        Wh_Log(L"Init in explorer.exe (stats collector)");
        return sysstats::Start(false);
    }

    Wh_Log(L"Init in the Start menu process");
    weatherw::LoadSettings();
    g_thread = std::thread(Worker);
    sysstats::Start(true);
    mediaw::Start();
    weatherw::Start();
    return TRUE;
}

void Wh_ModSettingsChanged() {
    sysstats::LoadSettings();
    if (g_isStartHost) weatherw::SettingsChanged();
}

void Wh_ModUninit() {
    if (!g_isStartHost) {
        sysstats::Stop();
        return;
    }
    g_qlStop = true;
    g_stop = true;
    weatherw::Stop();
    mediaw::Stop();
    sysstats::Stop();
    if (g_qlThread.joinable()) g_qlThread.join();
    if (g_thread.joinable()) g_thread.join();
    RestartStartMenu();
}
