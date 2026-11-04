// ==WindhawkMod==
// @id              start-menu-widgets
// @name            Start Menu Widgets
// @description     Replaces the Recent section of the Start menu with a widgets grid
// @version         0.2
// @author          you
// @include         StartMenuExperienceHost.exe
// @compilerOptions -DWINRT_NO_MAKE_DETECTION -lole32 -loleaut32 -lruntimeobject -lwinmm -lshell32 -lgdi32 -ldwmapi
// ==/WindhawkMod==

#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <sddl.h>
#include <mmsystem.h>
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <chrono>
#include <memory>
#include <cstdio>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mutex>
#include <map>
#include <set>
#include <algorithm>


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

// ---------- Widget table: row, column, column span, builder ----------
struct Widget {
    int row, col, row_span, col_span;
    std::function<std::wstring()> build;
    void (*wire)(FrameworkElement const&) = nullptr;   // NEW
};
static const Widget kWidgets[] = {
    {0, 0, 1, 1, ProfileWidget,WireProfile},    {0, 1, 1, 2, MusicWidget},                                                          {0, 3, 1, 1, WeatherWidget},
    {1, 0, 1, 1, NetWidget},                    {1, 1, 1, 1, RamWidget},            {1, 2, 1, 1, ClockWidget},                      {1, 3, 1, 1, StopwatchWidget, WireStopwatch},
    {2, 0, 1, 1, CpuWidget},                    {2, 1, 1, 1, GpuWidget},            {2, 2, 1, 1, TimerWidget, WireTimer},           {2, 3, 1, 1, QuickLaunchWidget, WireQuickLaunch},
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

BOOL Wh_ModInit() {
    g_thread = std::thread(Worker);
    return TRUE;
}

void Wh_ModUninit() {
    g_qlStop = true;
    if (g_qlThread.joinable()) g_qlThread.join();
    g_stop = true;
    RestartStartMenu();
}