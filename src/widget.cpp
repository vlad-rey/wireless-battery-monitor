#include "widget.h"

#include "i18n.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>
#include <climits>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
UINT g_clickMsg;
bool g_light, g_hover;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
bool g_stacked;     // icon above the percentage (menu option, PR #525)
bool g_vertical;    // the primary taskbar is docked left or right: one block per device, stacked (PR #321)
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
SIZE g_size{};
HINSTANCE g_inst;

// CN: the strip shows one device at a time, so a second and third device never push the strip over the
// task buttons. g_index is the device on screen; while g_animStart is set, the device the wheel just left
// (g_animFrom) is still drawn on its way out and the new one (g_animTo) slides in.
bool g_single = false;
int g_index = 0;
int g_animFrom = -1, g_animTo = -1, g_animDir = 1;
ULONGLONG g_animStart = 0;
const UINT kAnimTimer = 1;              // WM_TIMER id that drives the slide
const UINT kWheelMsg = WM_APP + 21;     // posted from the low-level hook to the strip
const UINT kTrayMsg = WM_APP + 22;      // posted by the lookup thread when a tray position changed
const int kSlideMs = 190;               // one full slide
HHOOK g_wheelHook = nullptr;

// 0..1, eased out so the device settles instead of stopping dead. 1 = nothing to animate.
double SlideProgress() {
    if (!g_animStart) return 1.0;
    double t = (double)(GetTickCount64() - g_animStart) / (double)kSlideMs;
    if (t >= 1.0) return 1.0;
    t = 1.0 - t;
    return 1.0 - t * t * t;
}

// Copies of the strip on secondary-monitor taskbars (Shell_SecondaryTrayWnd), so the levels stay visible
// while a game runs fullscreen on the main screen. Same drawing, same click and tooltip; each one sits
// left of its taskbar's clock. The primary strip above is unchanged, and with one monitor none exist.
struct Mirror {
    HWND bar = nullptr, wnd = nullptr;
    bool hover = false;
    SIZE size{};
    int dpi = 0;
    int lastLeft = INT_MIN;  // client x it was last placed at; INT_MIN = not placed (hidden)
    RECT lastBar{};          // taskbar rectangle at the last placement: a change (auto-hide) asks for a new lookup
};
std::vector<Mirror> g_mirrors;
// Which taskbars show the strip: "all", "main", or one monitor's id (MonitorId), e.g. only monitor 3.
// With one monitor chosen, the primary strip is hidden; if that monitor is not connected, the primary
// strip is shown instead so the levels never disappear.
std::wstring g_choice = L"all";
bool g_primaryHidden = false;
HWND g_dpiBar;  // while a mirror is painted or placed, Px() uses its taskbar's DPI

const wchar_t* kClass = L"WirelessBatteryWidget";

int Dpi() {
    HWND bar = g_dpiBar ? g_dpiBar : g_taskbar;
    UINT d = bar ? GetDpiForWindow(bar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on WirelessBattery.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

// Where a Windows 11 secondary taskbar's clock and task buttons are. Its XAML has no child windows for
// them, so the same lookup thread reads them with UI Automation (the clock has AutomationId
// "SystemTrayIcon", task buttons class "Taskbar.TaskListButtonAutomationPeer"; found by PR #107).
//
// An auto-hidden taskbar exposes none of these elements while it is hidden, so a hidden taskbar counts as
// not measured, and the last measurement taken while it was shown is kept for as long as the taskbar keeps
// its width (KeepShown). Hiding only moves a taskbar off the screen edge; the x positions stay valid.
struct Span {
    HWND bar = nullptr;
    bool measured = false;   // UI Automation answered for this taskbar while it was on screen
    bool hasClock = false;   // the clock can be turned off on secondary taskbars
    int clockLeft = 0;       // screen x
    bool hasTasks = false;
    int tasksRight = 0;      // right edge of the last task button left of the clock (screen x)
    LONG barLeft = 0, barRight = 0;  // taskbar edges when measured (screen x)
};

// The fresh measurement, or the previous one for the same taskbar when the fresh one could not be taken
// (taskbar auto-hidden) and the taskbar has not been resized or moved sideways since.
Span KeepShown(const Span& fresh, const Span& old) {
    if (!fresh.measured && old.measured && old.bar == fresh.bar && old.barLeft == fresh.barLeft && old.barRight == fresh.barRight)
        return old;
    return fresh;
}

// The main taskbar's tray position from an earlier run, so a widget on an auto-hidden taskbar starts in the
// right place before the taskbar is first shown. Stored as the distance from the taskbar's right edge to the
// first tray button, valid only for the same taskbar width and DPI. Guarded by g_wbLock.
struct SavedTray {
    bool loaded = false, valid = false;
    DWORD fromRight = 0, width = 0, dpi = 0;
} g_savedTray;
const wchar_t kSettingsKey[] = L"Software\\WirelessBattery";

void LoadSavedTray() {  // call with g_wbLock held exclusively
    if (g_savedTray.loaded) return;
    g_savedTray.loaded = true;
    DWORD v[3] = {}, sz = sizeof(DWORD);
    const wchar_t* names[3] = { L"TrayFromRight", L"TrayBarWidth", L"TrayDpi" };
    for (int i = 0; i < 3; i++, sz = sizeof(DWORD))
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, names[i], RRF_RT_REG_DWORD, nullptr, &v[i], &sz) != ERROR_SUCCESS) return;
    g_savedTray = { true, true, v[0], v[1], v[2] };
}

void SaveTray(DWORD fromRight, DWORD width, DWORD dpi) {  // call with g_wbLock held exclusively
    LoadSavedTray();
    if (g_savedTray.valid && g_savedTray.fromRight == fromRight && g_savedTray.width == width && g_savedTray.dpi == dpi) return;
    g_savedTray = { true, true, fromRight, width, dpi };
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TrayFromRight", REG_DWORD, &fromRight, sizeof(fromRight));
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TrayBarWidth", REG_DWORD, &width, sizeof(width));
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TrayDpi", REG_DWORD, &dpi, sizeof(dpi));
}

// True when the taskbar is not entirely on its monitor: an auto-hidden taskbar slid off the screen edge.
bool OffScreen(HWND bar, const RECT& br) {
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const RECT& m = mi.rcMonitor;
    return br.left < m.left || br.top < m.top || br.right > m.right || br.bottom > m.bottom;
}

// While an auto-hidden taskbar is hidden, only a strip of a few pixels stays on screen, and pointing at it
// brings the taskbar up. A widget there would take the mouse instead, so it lets the mouse through
// (WS_EX_TRANSPARENT on a layered window) until the taskbar is shown again.
void SetClickThrough(HWND w, bool on) {
    if (!w) return;
    LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    LONG want = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (want != ex) SetWindowLongW(w, GWL_EXSTYLE, want);
}
std::vector<Span> g_spans;   // guarded by g_wbLock
// The main taskbar measured the same way. On some Windows 11 builds the TrayNotifyWnd window starts
// right of where the XAML tray (hidden-icons chevron, language, network, volume, clock) is drawn, so a
// widget placed by TrayNotifyWnd alone covers the clock. Guarded by g_wbLock.
Span g_primarySpan;
bool g_wbMirrors;            // guarded by g_wbLock: measure secondary taskbars too

IUIAutomationCondition* StringCondition(IUIAutomation* uia, PROPERTYID prop, const wchar_t* value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    IUIAutomationCondition* c = nullptr;
    if (v.bstrVal) uia->CreatePropertyCondition(prop, v, &c);
    VariantClear(&v);
    return c;
}

Span MeasureSpan(IUIAutomation* uia, HWND bar) {
    Span s;
    s.bar = bar;
    RECT br{};
    IUIAutomationElement* root = nullptr;
    if (!GetWindowRect(bar, &br)) return s;
    s.barLeft = br.left;
    s.barRight = br.right;
    if (OffScreen(bar, br) || FAILED(uia->ElementFromHandle(bar, &root)) || !root) return s;
    s.measured = true;
    auto inBar = [&](const RECT& r) { return r.right > r.left && r.left >= br.left && r.right <= br.right; };
    if (IUIAutomationCondition* clock = StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, clock, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {  // leftmost of the clock area's buttons
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) &&
                    (!s.hasClock || r.left < s.clockLeft)) {
                    s.clockLeft = r.left;
                    s.hasClock = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        clock->Release();
    }
    if (IUIAutomationCondition* task = StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, task, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) &&
                    (!s.hasClock || r.right <= s.clockLeft) && (!s.hasTasks || r.right > s.tasksRight)) {
                    s.tasksRight = r.right;
                    s.hasTasks = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        task->Release();
    }
    root->Release();
    return s;
}

bool IsXamlTaskbar(HWND bar) {
    return FindWindowExW(bar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
}

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);  // no widget (tray-icon mode): sleep until one
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockShared(&g_wbLock);
        bool mirrors = g_wbMirrors;
        ReleaseSRWLockShared(&g_wbLock);
        std::vector<Span> spans;
        if (bar && mirrors && uia)
            for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
                 t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr))
                if (IsXamlTaskbar(t)) spans.push_back(MeasureSpan(uia, t));  // Windows 10 bars have real child windows
        Span primary = bar && uia && IsWindow(bar) ? MeasureSpan(uia, bar) : Span{};
        AcquireSRWLockExclusive(&g_wbLock);
        bool moved = false;  // a tray position the widget should follow right away
        for (Span& s : spans) {
            bool known = false;
            for (const Span& old : g_spans)
                if (old.bar == s.bar) {
                    known = true;
                    s = KeepShown(s, old);
                    moved |= s.measured != old.measured || s.hasClock != old.hasClock || s.clockLeft != old.clockLeft;
                }
            moved |= !known && s.measured;
        }
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
            Span kept = KeepShown(primary, g_primarySpan);
            moved |= kept.measured != g_primarySpan.measured || kept.hasClock != g_primarySpan.hasClock ||
                     kept.clockLeft != g_primarySpan.clockLeft;
            g_primarySpan = kept;
            if (primary.measured && primary.hasClock && primary.barRight > primary.clockLeft)
                SaveTray((DWORD)(primary.barRight - primary.clockLeft), (DWORD)(primary.barRight - primary.barLeft), GetDpiForWindow(bar));
        }
        g_spans = std::move(spans);
        ReleaseSRWLockExclusive(&g_wbLock);
        if (moved && g_wnd) PostMessageW(g_wnd, kTrayMsg, 0, 0);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        case DeviceKind::Phone:    return 0xE8EA;  // CellPhone
        default:                   return bluetooth ? 0xE702 : 0xE772;  // Bluetooth / Devices
    }
}
constexpr wchar_t kBolt = 0xE945;            // LightningBolt
constexpr wchar_t kBatteryUnknown = 0xE996;  // shown when there is nothing to display

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback, int fallbackPx = 0) {
    for (const wchar_t* f : { face, fallback }) {
        int h_px = (f == fallback && fallbackPx > 0) ? fallbackPx : px;
        HFONT h = CreateFontW(-h_px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

// Equal-width digits: "9%" and "100%" then occupy the same box, so the strip keeps its size while the
// wheel walks through devices. Non-numeric text ("?") falls back to the plain mask.
bool IsNumberText(const std::wstring& s) {
    if (s.size() < 2 || s.back() != L'%') return false;
    for (size_t i = 0; i + 1 < s.size(); i++)
        if (s[i] < L'0' || s[i] > L'9') return false;
    return true;
}

int DigitAdvance(HFONT font) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ o = SelectObject(dc, font);
    int w = 0;
    for (wchar_t c = L'0'; c <= L'9'; c++) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, &c, 1, &sz);
        w = std::max(w, (int)sz.cx);
    }
    SelectObject(dc, o);
    DeleteDC(dc);
    return w;
}

Mask FixedNumber(const std::wstring& s, HFONT font, bool darkOnLight, int digitCell, int pctCell) {
    const int digits = (int)s.size() - 1;  // everything but the trailing '%'
    std::vector<Mask> glyphs;
    int h = 0;
    for (wchar_t c : s) {
        glyphs.push_back(RenderMask(std::wstring(1, c), font, darkOnLight));
        h = std::max(h, glyphs.back().h);
    }
    Mask out;
    out.w = 3 * digitCell + pctCell;
    out.h = h;
    out.a.assign((size_t)out.w * out.h, 0);
    for (size_t i = 0; i < glyphs.size(); i++) {
        const Mask& m = glyphs[i];
        const bool pct = i + 1 == glyphs.size();
        const int cellX = pct ? digits * digitCell : (int)i * digitCell;  // left aligned, '%' right after
        const int cellW = pct ? pctCell : digitCell;
        const int x = cellX + (cellW - m.w) / 2, y = (h - m.h) / 2;
        for (int j = 0; j < m.h; j++)
            for (int k = 0; k < m.w; k++) {
                const int X = x + k, Y = y + j;
                if (X < 0 || Y < 0 || X >= out.w || Y >= out.h) continue;
                BYTE& d = out.a[(size_t)Y * out.w + X];
                d = (BYTE)std::max<int>(d, m.a[(size_t)j * m.w + k]);
            }
    }
    return out;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
};

// Soft glow around a glyph, like a lit RGB device: the coverage mask padded by r pixels, box-blurred
// twice and brightened so a thin icon outline still gives a visible glow.
Mask Halo(const Mask& m, int r) {
    Mask g;
    g.w = m.w + 2 * r;
    g.h = m.h + 2 * r;
    std::vector<int> a((size_t)g.w * g.h, 0), tmp(a.size(), 0);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) a[(y + r) * g.w + x + r] = m.a[y * m.w + x];
    int n = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (x + k >= 0 && x + k < g.w) s += a[y * g.w + x + k];
                tmp[y * g.w + x] = s / n;
            }
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (y + k >= 0 && y + k < g.h) s += tmp[(y + k) * g.w + x];
                a[y * g.w + x] = s / n;
            }
    }
    g.a.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) g.a[i] = (BYTE)std::min(255, a[i] * 3);
    return g;
}

// One drawn piece of the strip. Normal pieces follow each other from left to right. A piece with an
// `anchor` (index of an earlier piece) is a caption: drawn over that piece, `bx`/`by` from its top-left
// corner, and taking no room of its own. Branded devices add a glow behind the glyph and a level bar.
struct Piece {
    Mask m;
    Rgb color;
    int alpha;
    int gapAfter;
    int anchor = -1;
    int bx = 0, by = 0;
    int barPct = -1;  // >= 0: thin level bar under the text (branded devices)
    Rgb barColor{};
    Mask halo;        // glow drawn behind the glyph (branded devices)
    int haloR = 0, haloAlpha = 0;
    int px = INT_MIN, py = 0;  // fixed top-left position (stacked and vertical layouts); INT_MIN = flow left to right
};

// Three arrangements of the same pieces:
//  * normal: "[icon] 30%  [icon] 93%" in one row (unchanged default)
//  * stacked (menu option, from PR #525): each device's icon above its percentage, to save width;
//    used only when the taskbar is tall enough, otherwise the normal row
//  * vertical (left / right taskbar, from PR #321): one device per row block, stacked top to bottom
// `width` and `height` return the strip's size; in the vertical layout the width is the bar's.
std::vector<Piece> LayoutOf(const std::vector<const WidgetItem*>& items, int barW, int barH, int& width, int& height) {
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    HFONT icons = MakeFont(Px(16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT bolt = MakeFont(Px(11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT text = MakeFont(Px(13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI", Px(12));
    HFONT caption = MakeFont(std::max(8, Px(7)), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");  // never under 8 px

    // CN: with one device on the strip the box must not change size when the wheel switches device, so the
    // icon is always the same glyph and the percentage is drawn on fixed digit cells.
    const bool single = g_single && items.size() == 1 && !g_vertical;
    int digitCell = 0, pctCell = 0;
    if (single) {
        digitCell = DigitAdvance(text);
        pctCell = RenderMask(L"%", text, g_light).w + Px(1);
    }

    std::vector<Piece> out;
    struct Slot { int icon, label, bolt; };  // indices of one device's pieces
    std::vector<Slot> slots;
    if (items.empty()) out.push_back({ RenderMask(std::wstring(1, kBatteryUnknown), icons, g_light), fg, 110, 0 });
    for (size_t i = 0; i < items.size(); i++) {
        const auto& it = *items[i];
        int a = it.dim ? 120 : 255;
        Rgb tc = it.low ? red : fg;
        size_t glyph = out.size();
        slots.push_back({ (int)glyph, (int)glyph + 1, it.charging ? (int)glyph + 2 : -1 });
        bool branded = it.accent != Accent::None;
        BrandColor bc = AccentColor(it.accent, g_light);
        Rgb accent{ bc.r, bc.g, bc.b };
        const wchar_t glyphChar = Glyph(it.kind, it.bluetooth);
        Piece icon{ RenderMask(std::wstring(1, glyphChar), icons, g_light), branded ? accent : fg, a, Px(5) };
        if (branded && !it.dim) {
            icon.haloR = Px(3);
            icon.halo = Halo(icon.m, icon.haloR);
            icon.haloAlpha = g_light ? 60 : 140;
        }
        out.push_back(std::move(icon));
        std::wstring t = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"?";
        Mask labelMask = (single && IsNumberText(t)) ? FixedNumber(t, text, g_light, digitCell, pctCell)
                                                    : RenderMask(t, text, g_light);
        Piece label{ std::move(labelMask), tc, a, it.charging ? Px(2) : 0 };
        if (branded && it.percent >= 0) {
            label.barPct = it.percent;
            label.barColor = it.low ? red : accent;
        }
        out.push_back(std::move(label));
        if (it.charging) out.push_back({ RenderMask(std::wstring(1, kBolt), bolt, g_light), charge, a, 0 });
        out.back().gapAfter = i + 1 < items.size() ? Px(14) : 0;
        if (!it.label.empty()) {  // tiny caption in the bottom-right corner of the icon
            Piece c{ RenderMask(it.label, caption, g_light), fg, a, 0 };
            c.anchor = (int)glyph;
            c.bx = out[glyph].m.w - c.m.w + Px(3);
            c.by = out[glyph].m.h - c.m.h + Px(3);
            out.push_back(c);
        }
    }
    int boltRefW = 0;  // width of the charging bolt, reserved up front so the strip never resizes
    int widestIcon = 0;  // widest device glyph of the whole set, so the single-device strip keeps one width
    if (single) {
        boltRefW = RenderMask(std::wstring(1, kBolt), bolt, g_light).w;
        for (const auto& other : g_items)
            widestIcon = std::max(widestIcon, RenderMask(std::wstring(1, Glyph(other.kind, other.bluetooth)), icons, g_light).w);
    }
    DeleteObject(icons);
    DeleteObject(bolt);
    DeleteObject(text);
    DeleteObject(caption);

    if (single && slots.size() == 1) {
        const Slot& s = slots.front();
        Piece& ic = out[s.icon];
        Piece& lb = out[s.label];
        // The trailing empty digit cell already reads as space, so the right edge needs less padding than
        // the left one to look even.
        const int pad = Px(6), padRight = Px(3), rowGapS = Px(2), barRoomS = Px(5), iconGap = Px(3);
        const int iconCell = std::max(std::max(widestIcon, ic.m.w), Px(16));
        const int textCell = 3 * digitCell + pctCell;             // room for "100%"
        const int textW = std::max(textCell, lb.m.w);             // "?" and any custom text still fits
        // Space for the charging bolt is reserved only while some device is actually charging, so an
        // all-wireless setup keeps the shorter strip and the width still never changes while switching.
        bool anyCharging = false;
        for (const auto& other : g_items) anyCharging = anyCharging || other.charging;
        const int boltCell = anyCharging ? Px(2) + boltRefW : 0;
        const bool stackedS = g_stacked && barH >= ic.m.h + rowGapS + lb.m.h + barRoomS + Px(6);
        if (stackedS) {
            const int slotW = std::max(iconCell, textW + boltCell);
            const int top = (barH - (ic.m.h + rowGapS + lb.m.h + barRoomS)) / 2;
            ic.px = pad + (slotW - (ic.m.w + boltCell)) / 2;
            ic.py = top;
            if (s.bolt >= 0) {
                out[s.bolt].px = ic.px + ic.m.w + Px(2);
                out[s.bolt].py = top + (ic.m.h - out[s.bolt].m.h) / 2;
            }
            lb.px = pad + (slotW - lb.m.w) / 2;
            lb.py = top + ic.m.h + rowGapS;
            width = slotW + pad * 2;
        } else {
            ic.px = pad + (iconCell - ic.m.w) / 2;
            ic.py = (barH - ic.m.h) / 2;
            const int textX = pad + iconCell + iconGap;
            lb.px = textX;  // digits are left aligned inside their fixed cells, so the gap stays tight
            lb.py = (barH - lb.m.h) / 2;
            if (s.bolt >= 0) {
                out[s.bolt].px = textX + textW + Px(2);
                out[s.bolt].py = (barH - out[s.bolt].m.h) / 2;
            }
            width = pad + iconCell + iconGap + textW + boltCell + padRight;
        }
        height = barH;
        return out;
    }

    height = barH;
    const int rowGap = Px(2), barRoom = Px(5);  // barRoom: space for a brand level bar under the text
    int iconH = 0, textH = 0;
    for (const Slot& s : slots) {
        iconH = std::max(iconH, out[s.icon].m.h);
        textH = std::max(textH, out[s.label].m.h);
    }
    bool stacked = !g_vertical && g_stacked && !slots.empty() && barH >= iconH + rowGap + textH + barRoom + Px(6);
    if (g_vertical && !slots.empty()) {
        // One block per device, centred in the bar's width: icon, then the percentage (and bolt) under it.
        width = barW;
        int y = Px(6);
        for (const Slot& s : slots) {
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            y += ic.m.h + rowGap;
            lb.px = (barW - lb.m.w - boltW) / 2;
            lb.py = y;
            if (s.bolt >= 0) {
                out[s.bolt].px = lb.px + lb.m.w + Px(2);
                out[s.bolt].py = y + (lb.m.h - out[s.bolt].m.h) / 2;
            }
            y += lb.m.h + (lb.barPct >= 0 ? barRoom : 0) + Px(8);
        }
        height = y;
        return out;
    }
    if (stacked) {
        // Icon on top (bolt to its right), percentage centred under it; the pair centred in the bar's height.
        int top = (barH - (iconH + rowGap + textH + barRoom)) / 2;
        int x = Px(8);
        for (const Slot& s : slots) {
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            int slotW = std::max(ic.m.w + boltW, lb.m.w);
            ic.px = x + (slotW - ic.m.w - boltW) / 2;
            ic.py = top + (iconH - ic.m.h) / 2;
            if (s.bolt >= 0) {
                out[s.bolt].px = ic.px + ic.m.w + Px(2);
                out[s.bolt].py = top + (iconH - out[s.bolt].m.h) / 2;
            }
            lb.px = x + (slotW - lb.m.w) / 2;
            lb.py = top + iconH + rowGap;
            x += slotW + Px(10);
        }
        width = x - Px(10) + Px(8);
        return out;
    }
    width = Px(8) * 2;
    for (auto& p : out)
        if (p.anchor < 0) width += p.m.w + p.gapAfter;
    return out;
}

std::vector<Piece> Layout(int barW, int barH, int& width, int& height) {
    std::vector<const WidgetItem*> all;
    for (const auto& it : g_items) all.push_back(&it);
    return LayoutOf(all, barW, barH, width, height);
}

// The device the strip currently shows (-1 when there is none).
int CurrentIndex() {
    if (g_items.empty() || !g_single) return -1;
    return std::clamp(g_index, 0, (int)g_items.size() - 1);
}

// One device forward (+1) or back (-1), with the slide. The wheel walks the same order as the menu.
void PaintStrip(HWND wnd, HWND taskbar, bool hover, SIZE& size);
void PaintMirror(Mirror& m);

void CycleDevice(int step) {
    if (!g_single || g_items.size() < 2) return;
    int n = (int)g_items.size();
    int from = std::clamp(g_index, 0, n - 1);
    int to = ((from + step) % n + n) % n;
    if (to == from) return;
    g_animFrom = from;
    g_animTo = to;
    g_animDir = step > 0 ? 1 : -1;
    g_animStart = GetTickCount64();
    if (!g_animStart) g_animStart = 1;  // 0 means "no slide"
    g_index = to;
    if (g_wnd) SetTimer(g_wnd, kAnimTimer, 15, nullptr);
    for (auto& m : g_mirrors) PaintMirror(m);
    if (g_wnd) PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
}

// Draws the strip for one window: the primary one or a mirror on a secondary taskbar.
void PaintStrip(HWND wnd, HWND taskbar, bool hover, SIZE& size) {
    RECT bar;
    GetClientRect(taskbar, &bar);
    const int barW = bar.right - bar.left, barH = bar.bottom - bar.top;

    // What is on screen this frame: the current device, or two of them while the wheel slide runs.
    struct Shot {
        std::vector<Piece> pieces;
        int w = 0, h = 0, dx = 0, alpha = 255;
    };
    std::vector<Shot> shots;
    bool sliding = g_animStart != 0 && g_animFrom >= 0 && g_animTo >= 0 && g_animFrom < (int)g_items.size() &&
                   g_animTo < (int)g_items.size() && g_animFrom != g_animTo;
    if (sliding) {
        double u = SlideProgress();
        Shot out, in;
        out.pieces = LayoutOf({ &g_items[g_animFrom] }, barW, barH, out.w, out.h);
        in.pieces = LayoutOf({ &g_items[g_animTo] }, barW, barH, in.w, in.h);
        out.dx = -(g_animDir * (int)(u * out.w + 0.5));
        out.alpha = (int)((1.0 - u) * 255.0 + 0.5);
        in.dx = g_animDir * (int)((1.0 - u) * in.w + 0.5);
        in.alpha = (int)(u * 255.0 + 0.5);
        shots.push_back(std::move(out));
        shots.push_back(std::move(in));
    } else {
        Shot one;
        std::vector<const WidgetItem*> cur;
        if (g_single) {
            int i = CurrentIndex();
            if (i >= 0) cur.push_back(&g_items[i]);
            one.pieces = LayoutOf(cur, barW, barH, one.w, one.h);
        } else {
            one.pieces = Layout(barW, barH, one.w, one.h);
        }
        shots.push_back(std::move(one));
    }
    int W = 0, H = 0;
    for (const auto& s : shots) {
        W = std::max(W, s.w);
        H = std::max(H, s.h);
    }
    if (W <= 0 || H <= 0) return;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    for (auto& p : cv.px) p = 0x01000000;
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        for (int y = inset; y < H - inset; y++)
            for (int x = 0; x < W; x++) {
                int dx = x < r ? r - x : (x >= W - r ? x - (W - r - 1) : 0);
                int dy = y < inset + r ? inset + r - y : (y >= H - inset - r ? y - (H - inset - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    Rgb barColor = g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 };
    if (g_background) roundRect(Px(4), Px(6), barColor, 240);
    if (hover) roundRect(Px(4), Px(4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);

    Rgb track = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    for (const auto& shot : shots) {
        const int am = shot.alpha;  // the slide fades the device that comes or goes
        if (am <= 0) continue;
        const auto& pieces = shot.pieces;
        int x = Px(8) + shot.dx;
        std::vector<int> xs(pieces.size()), ys(pieces.size());
        for (size_t i = 0; i < pieces.size(); i++) {
            const Piece& p = pieces[i];
            const int a = p.alpha * am / 255;
            if (p.anchor < 0) {
                bool fixed = p.px != INT_MIN;  // stacked / vertical layout: Layout() placed it
                xs[i] = (fixed ? p.px : x) + (fixed ? shot.dx : 0);
                ys[i] = fixed ? p.py : (H - p.m.h) / 2;
                if (p.haloR) cv.Draw(p.halo, xs[i] - p.haloR, ys[i] - p.haloR, p.color, p.haloAlpha * a / 255);
                if (p.barPct >= 0) {
                    // The bar spans the text's width, like Synapse's battery bar; in the row layout text and bar
                    // are centred together.
                    int bh = std::max(2, Px(2)), gap = Px(3);
                    if (!fixed) ys[i] = (H - (p.m.h + gap + bh)) / 2;
                    int by = ys[i] + p.m.h + gap;
                    int fill = std::max(p.barPct > 0 ? 1 : 0, (p.barPct * p.m.w + 50) / 100);
                    for (int yy = by; yy < by + bh; yy++)
                        for (int xx = 0; xx < p.m.w; xx++)
                            cv.Blend(xs[i] + xx, yy, xx < fill ? p.barColor : track, xx < fill ? a : 45 * a / 255);
                }
                if (!fixed) x += p.m.w + p.gapAfter;
            } else {  // caption: one-pixel outline in the taskbar's colour so it reads over the icon's lines
                xs[i] = xs[p.anchor] + p.bx;
                ys[i] = ys[p.anchor] + p.by;
                for (int oy = -1; oy <= 1; oy++)
                    for (int ox = -1; ox <= 1; ox++)
                        if (ox || oy) cv.Draw(p.m, xs[i] + ox, ys[i] + oy, barColor, a);
            }
            cv.Draw(p.m, xs[i], ys[i], p.color, a);
        }
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void Paint() {
    if (!g_wnd) return;
    PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
    g_lastW = -1;  // force a re-position with the new size
}

void PaintMirror(Mirror& m) {
    g_dpiBar = m.bar;
    m.dpi = Dpi();
    PaintStrip(m.wnd, m.bar, m.hover, m.size);
    g_dpiBar = nullptr;
    m.lastLeft = INT_MIN;  // force a re-position with the new size
}

void HideMirror(Mirror& m) {
    if (m.lastLeft != INT_MIN || IsWindowVisible(m.wnd)) ShowWindow(m.wnd, SW_HIDE);
    m.lastLeft = INT_MIN;
}

// Places a mirror left of its taskbar's clock, or hides it when that spot is unknown or taken by task
// buttons: never guess and cover something.
void PositionMirror(Mirror& m) {
    RECT bar{};
    if (!GetWindowRect(m.bar, &bar) || (GetWindowLongW(m.bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)) { HideMirror(m); return; }
    if (!EqualRect(&bar, &m.lastBar)) {  // shown, hidden or resized: measure its clock again now
        m.lastBar = bar;
        if (g_wbWake) SetEvent(g_wbWake);
    }
    SetClickThrough(m.wnd, OffScreen(m.bar, bar));
    g_dpiBar = m.bar;
    if (Dpi() != m.dpi || m.size.cy != bar.bottom - bar.top) PaintMirror(m), g_dpiBar = m.bar;
    int right = 0, minLeft = bar.left;
    bool known = false;
    if (!IsXamlTaskbar(m.bar)) {  // Windows 10: the clock is a real child window (absent when it is turned off)
        HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
        RECT cr{};
        right = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? cr.left : bar.right - Px(8);
        known = true;
    } else {
        AcquireSRWLockShared(&g_wbLock);
        for (const auto& s : g_spans)
            if (s.bar == m.bar && s.measured) {
                right = s.hasClock ? s.clockLeft : bar.right - Px(8);
                if (s.hasTasks) minLeft = s.tasksRight + Px(4);
                known = true;
            }
        ReleaseSRWLockShared(&g_wbLock);
    }
    DWORD offset = 0, sz = sizeof(offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    int left = right - m.size.cx - Px(2);
    g_dpiBar = nullptr;
    if (!known || m.size.cx <= 0 || left < minLeft) { HideMirror(m); return; }
    POINT p{ left, bar.top };
    ScreenToClient(m.bar, &p);
    bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && p.x == m.lastLeft) return;
    m.lastLeft = p.x;
    SetWindowPos(m.wnd, HWND_TOP, p.x, 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AddTool(HWND w) {
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)w;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.empty() ? L"Wireless Battery" : g_tipText.c_str());
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void DestroyMirror(Mirror& m) {
    if (g_tip) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)m.wnd;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    if (m.wnd && IsWindow(m.wnd)) DestroyWindow(m.wnd);
    m.wnd = nullptr;
}

void SetMeasureMirrors(bool on) {
    AcquireSRWLockExclusive(&g_wbLock);
    bool changed = g_wbMirrors != on;
    g_wbMirrors = on;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (changed && on && g_wbWake) SetEvent(g_wbWake);  // measure the new taskbar now, not in 5 s
}

std::vector<HWND> SecondaryTaskbars() {
    std::vector<HWND> out;
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        RECT r{};
        if (IsWindowVisible(t) && GetWindowRect(t, &r) && r.right - r.left > r.bottom - r.top) out.push_back(t);  // horizontal only
    }
    return out;
}

// Keeps one mirror per secondary taskbar: adds them for new monitors, removes them for gone ones.
// Called from Reposition() about once a second; with one monitor it is a single FindWindowEx call.
// A monitor's lasting identity: its device interface name, which survives reboots, Explorer restarts and
// Windows renumbering the displays (falls back to "\\.\DISPLAYn").
std::wstring MonitorId(HWND bar) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONULL);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"";
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) return dd.DeviceID;
    return mi.szDevice;
}

// "Display 3 (right)": the number Windows gives the display and where it sits next to the main one.
std::wstring MonitorLabel(HWND bar) {
    MONITORINFOEXW mi{}, pm{};
    mi.cbSize = pm.cbSize = sizeof(MONITORINFOEXW);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return T(L"Other monitor");
    std::wstring dev = mi.szDevice;
    size_t d = dev.find(L"DISPLAY");
    std::wstring label = d != std::wstring::npos ? TF(L"Display {number}", { { L"number", dev.substr(d + 7) } }) : T(L"Other monitor");
    POINT origin{ 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm)) {
        int dx = (mi.rcMonitor.left + mi.rcMonitor.right - pm.rcMonitor.left - pm.rcMonitor.right) / 2;
        int dy = (mi.rcMonitor.top + mi.rcMonitor.bottom - pm.rcMonitor.top - pm.rcMonitor.bottom) / 2;
        label += std::wstring(L" (") + T(abs(dx) >= abs(dy) ? (dx < 0 ? L"left" : L"right") : (dy < 0 ? L"above" : L"below")) + L")";
    }
    return label;
}

// Hides or shows the strip on the main taskbar (it keeps running, just out of sight).
void SetPrimaryHidden(bool hide) {
    if (hide == g_primaryHidden || !g_wnd) return;
    g_primaryHidden = hide;
    if (hide) {
        ShowWindow(g_wnd, SW_HIDE);
        if (g_rebar && IsWindow(g_rebar)) {  // Windows 10: give the task buttons their full width back
            RECT r;
            GetClientRect(g_taskbar, &r);
            PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
        }
    }
    g_lastW = -1;  // re-place (and show) on the next pass
}

void SyncMirrors() {
    std::vector<HWND> all = g_wnd ? SecondaryTaskbars() : std::vector<HWND>{}, bars;
    bool chosenFound = false;
    if (g_choice == L"all") {
        bars = all;
    } else if (g_choice != L"main") {
        for (HWND b : all)
            if (MonitorId(b) == g_choice) {
                bars.push_back(b);
                chosenFound = true;
                break;
            }
    }
    SetPrimaryHidden(chosenFound);
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (std::find(bars.begin(), bars.end(), it->bar) == bars.end() || !IsWindow(it->wnd)) {
            DestroyMirror(*it);
            it = g_mirrors.erase(it);
        } else {
            ++it;
        }
    }
    for (HWND b : bars) {
        if (std::any_of(g_mirrors.begin(), g_mirrors.end(), [b](const Mirror& m) { return m.bar == b; })) continue;
        Mirror m;
        m.bar = b;
        m.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, b, nullptr,
                                g_inst, nullptr);
        if (!m.wnd) continue;
        AddTool(m.wnd);
        g_mirrors.push_back(m);
        PaintMirror(g_mirrors.back());
    }
    SetMeasureMirrors(!g_mirrors.empty());
    for (auto& m : g_mirrors) PositionMirror(m);
}

void PaintAll() {
    Paint();
    for (auto& m : g_mirrors) PaintMirror(m);
}

// Hover highlight for the strip under the mouse: the primary one or a mirror.
void SetHover(HWND h, bool on) {
    if (h == g_wnd) {
        if (g_hover == on) return;
        g_hover = on;
        Paint();
        widget::Reposition();
        return;
    }
    for (auto& m : g_mirrors)
        if (m.wnd == h && m.hover != on) {
            m.hover = on;
            PaintMirror(m);
            PositionMirror(m);
        }
}

// Catches the wheel over the strip even when Windows does not deliver scroll messages to the window
// under the cursor ("scroll inactive windows when I hover over them" turned off). Installed only while
// the pointer is on the strip, so the system-wide hook lives for a moment, not for the whole session.
LRESULT CALLBACK WheelProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        int delta = (int)(short)HIWORD(m->mouseData);
        if (delta) {
            RECT r;
            HWND target = nullptr;
            if (g_wnd && IsWindowVisible(g_wnd) && GetWindowRect(g_wnd, &r) && PtInRect(&r, m->pt))
                target = g_wnd;
            for (auto& mir : g_mirrors) {
                if (target) break;
                if (mir.wnd && IsWindowVisible(mir.wnd) && GetWindowRect(mir.wnd, &r) && PtInRect(&r, m->pt))
                    target = mir.wnd;
            }
            if (target) {  // swallow it here and let the window animate; the hook itself stays cheap
                PostMessageW(target, kWheelMsg, delta > 0 ? -1 : +1, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_wheelHook, code, wp, lp);
}

// The hook lives exactly as long as single-device mode has something to switch to. It is cheap (it looks
// at wheel messages and nothing else) and keeps the wheel working even when Windows sends scrolls to the
// focused window instead of the one under the cursor.
void UpdateWheelHook() {
    const bool want = g_single && g_items.size() > 1;
    if (want && !g_wheelHook)
        g_wheelHook = SetWindowsHookExW(WH_MOUSE_LL, WheelProc, g_inst, 0);
    else if (!want && g_wheelHook) { UnhookWindowsHookEx(g_wheelHook); g_wheelHook = nullptr; }
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            SetHover(h, true);
            return 0;
        }
        case WM_MOUSELEAVE:
            SetHover(h, false);
            return 0;
        case WM_MOUSEWHEEL: {  // hover-scrolling window: this arrives without the hook above
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (delta) CycleDevice(delta > 0 ? -1 : +1);
            return 0;
        }
        case kWheelMsg:  // posted by WheelProc
            CycleDevice((int)wp);
            return 0;
        case kTrayMsg:  // the lookup thread found the tray somewhere else: move now, not on the next timer tick
            widget::Reposition();
            return 0;
        case WM_TIMER:
            if (wp == kAnimTimer) {
                if (SlideProgress() >= 1.0) {
                    KillTimer(h, kAnimTimer);
                    g_animStart = 0;
                    g_animFrom = g_animTo;
                    PaintAll();
                    widget::Reposition();
                } else {
                    for (auto& m : g_mirrors) PaintMirror(m);
                    if (g_wnd) PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    g_vertical = tb.bottom - tb.top > tb.right - tb.left;  // left / right taskbar: devices stacked in a column
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;
    g_inst = inst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = notifyWnd;
        ti.uId = (UINT_PTR)g_wnd;
        ti.lpszText = const_cast<wchar_t*>(L"Wireless Battery");
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();
    if (!g_primaryHidden) ShowWindow(g_wnd, SW_SHOWNOACTIVATE);  // hidden when only another monitor is chosen
    return true;
}

void Destroy() {
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    for (auto& m : g_mirrors) DestroyMirror(m);
    g_mirrors.clear();
    SetMeasureMirrors(false);
    g_primaryHidden = false;  // the next Create() decides again
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_wheelHook) { UnhookWindowsHookEx(g_wheelHook); g_wheelHook = nullptr; }
    if (g_wnd) { KillTimer(g_wnd, kAnimTimer); DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hover = false;
    g_animStart = 0;
    g_animFrom = g_animTo = -1;
    g_index = 0;
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    // A device went away (or came back) mid-slide: drop the slide instead of pairing wrong devices.
    if (g_items.size() < 2 || g_animFrom >= (int)g_items.size() || g_animTo >= (int)g_items.size()) {
        g_animStart = 0;
        g_animFrom = g_animTo = -1;
        if (g_wnd) KillTimer(g_wnd, kAnimTimer);
    }
    g_index = g_items.empty() ? 0 : std::clamp(g_index, 0, (int)g_items.size() - 1);
    if (tooltip != g_tipText && g_tip) {
        g_tipText = tooltip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        ti.uId = (UINT_PTR)g_wnd;
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        for (const auto& m : g_mirrors) {
            ti.uId = (UINT_PTR)m.wnd;
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }
    UpdateWheelHook();
    PaintAll();
    Reposition();
}

void SetLightTheme(bool light) {
    g_light = light;
    ApplyTipTheme();
    PaintAll();
}

void SetBackground(bool on) {
    g_background = on;
    PaintAll();
}

void SetStacked(bool on) {
    g_stacked = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetSingleDevice(bool on) {
    g_single = on;
    g_animStart = 0;
    g_animFrom = g_animTo = -1;
    g_index = 0;
    if (g_wnd) KillTimer(g_wnd, kAnimTimer);
    UpdateWheelHook();
    PaintAll();
    if (g_wnd) Reposition();
}

void SetMonitorChoice(const std::wstring& choice) {
    g_choice = choice.empty() ? L"all" : choice;
    SyncMirrors();  // adds, removes or hides strips right away
    if (g_wnd) Reposition();
}

std::vector<MonitorOption> OtherMonitors() {
    std::vector<MonitorOption> out;
    for (HWND b : SecondaryTaskbars()) out.push_back({ MonitorId(b), MonitorLabel(b) });
    std::sort(out.begin(), out.end(), [](const MonitorOption& a, const MonitorOption& b) { return a.label < b.label; });
    return out;
}

bool Reposition() {
    if (!Alive()) return false;
    SyncMirrors();
    if (g_primaryHidden) return true;  // only another monitor's taskbar shows the strip
    if (Dpi() != g_lastDpi) { g_lastDpi = Dpi(); Paint(); }
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return true;
    RECT tr, bar;
    GetWindowRect(tray, &tr);
    GetWindowRect(g_taskbar, &bar);
    // An auto-hidden taskbar exposes its tray to UI Automation only while shown: measure again as soon as it moves.
    static RECT seenBar{};
    if (!EqualRect(&bar, &seenBar)) {
        seenBar = bar;
        if (g_wbWake) SetEvent(g_wbWake);
    }
    SetClickThrough(g_wnd, OffScreen(g_taskbar, bar));
    // The taskbar can be moved between a horizontal and a vertical edge while the app runs.
    bool vertical = bar.bottom - bar.top > bar.right - bar.left;
    if (vertical != g_vertical) {
        g_vertical = vertical;
        Paint();
    }
    if (g_vertical) {
        // Left or right taskbar (PR #321): the column sits just above the notification area, centred in
        // the bar's width.
        RECT client{};
        GetClientRect(g_taskbar, &client);
        POINT anchor{ bar.left, tr.top };
        ScreenToClient(g_taskbar, &anchor);
        int top = anchor.y - g_size.cy - Px(2);
        DWORD offset = 0, osz = sizeof(offset);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset,
                         &osz) == ERROR_SUCCESS)
            top -= Px((int)offset);
        top = std::clamp(top, 0, (int)std::max<LONG>(0, client.bottom - g_size.cy));
        int left = (int)std::max<LONG>(0, (client.right - g_size.cx) / 2);
        // Windows 10: shorten the task-button area when it would reach behind the column.
        if (g_rebar && IsWindow(g_rebar)) {
            RECT rr{};
            GetWindowRect(g_rebar, &rr);
            POINT tl{ rr.left, rr.top };
            ScreenToClient(g_taskbar, &tl);
            if (tl.y < top && tl.y + (rr.bottom - rr.top) > top)
                MoveWindow(g_rebar, tl.x, tl.y, rr.right - rr.left, top - tl.y, TRUE);
        }
        bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
        if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && g_lastW == g_size.cy && g_lastLeft == top)
            return true;
        g_lastTray = tr;
        g_lastBar = bar;
        g_lastW = g_size.cy;
        g_lastLeft = top;
        SetWindowPos(g_wnd, HWND_TOP, left, top, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int edge = rtl ? tr.right : tr.left;  // screen x we sit next to
    // Windows 10: News and Interests / Weather sits immediately left of the tray. The window that hosts it
    // depends on the build: ShellFeedsTaskbarViewWindow on some, DynamicContent2 on others (e.g. 19045,
    // where the search highlights button is DynamicContent1). Both are direct children of the taskbar and
    // only a visible one counts. Its rectangle is read on every pass because the weather text changes its
    // width, so the widget follows it.
    if (!rtl)
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    // Windows 11: when the XAML tray is drawn left of TrayNotifyWnd, sit left of the tray's first button.
    if (!rtl) {
        AcquireSRWLockExclusive(&g_wbLock);
        Span ps = g_primarySpan.bar == g_taskbar ? g_primarySpan : Span{};
        if (!ps.measured) {  // auto-hidden and not shown yet since start: use the position from the last run
            LoadSavedTray();
            const SavedTray& st = g_savedTray;
            if (st.valid && st.width == (DWORD)(bar.right - bar.left) && st.dpi == (DWORD)Dpi()) {
                ps.measured = ps.hasClock = true;
                ps.clockLeft = bar.right - (LONG)st.fromRight;
            }
        }
        ReleaseSRWLockExclusive(&g_wbLock);
        if (ps.measured && ps.hasClock && ps.clockLeft < edge && ps.clockLeft > bar.left + g_size.cx &&
            (!ps.hasTasks || ps.clockLeft > ps.tasksRight))
            edge = ps.clockLeft;
    }
    // Step past the Widgets button when it sits where we would go.
    RECT wb = WidgetsButtonRect();
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    int left = p.x - g_size.cx - Px(2);


    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
        left -= Px((int)customOffset);
    }

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && EqualRect(&wb, &g_lastWidgets) &&
        g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace widget
