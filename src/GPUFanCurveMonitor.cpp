// GPU Fan Curve Monitor
// ---------------------------------------------------------------------------
// Merged + enhanced successor of the GPUFANHEATDIAGRAMM* prototypes.
//
//  * Pure Win32 + GDI+ (no GTK runtime needed), anti-aliased, double-buffered
//  * nvidia-smi polled on a background thread, hidden (no console flashing)
//  * Fan-% vs temperature plot with fading colour trace, history dots,
//    glowing "now" marker, crosshair, temperature zones and a learned
//    (averaged) fan curve
//  * Scrolling timeline of temperature, fan, GPU load and power
//  * Live info panel: temp, fan, power, load, VRAM, clocks, session stats
//  * Multi-GPU (Tab / 0-9), overheat alert with beep + taskbar flash
//  * CSV export, continuous CSV logging, PNG screenshot, copy to clipboard
//  * Always-on-top, pause, adjustable interval, demo mode, settings in .ini
//
// Build (MinGW-w64):  see build.sh / README.md
// ---------------------------------------------------------------------------

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const wchar_t* APP_NAME = L"GPU Fan Curve Monitor";
static const wchar_t* CLASS_NAME = L"GpuFanCurveMonitorWnd";

static const size_t MAX_HISTORY = 3600;   // samples kept per GPU
static const int CURVE_POINTS = 240;      // trace length on the fan curve
static const int TIMELINE_POINTS = 300;   // samples on the timeline
static const int INTERVALS[] = {500, 1000, 2000, 3000, 4000, 5000, 10000};

#define WM_APP_DATA (WM_APP + 1)
#define WM_APP_ERR (WM_APP + 2)
#define TIMER_UI 1

enum Cmd {
    CMD_PAUSE = 100, CMD_CLEAR, CMD_NEXTGPU, CMD_FASTER, CMD_SLOWER,
    CMD_ALERT_UP, CMD_ALERT_DOWN, CMD_LOG, CMD_EXPORT, CMD_SHOT, CMD_COPY,
    CMD_TOP, CMD_DEMO, CMD_BEEP, CMD_HELP, CMD_EXIT
};

namespace pal {
const DWORD bg = 0x0f1117, card = 0x171a23, cardBorder = 0x262b38,
            grid = 0x222735, axis = 0x5c667a, text = 0xe8ebf2, dim = 0x8b93a7,
            cyan = 0x22d3ee, green = 0x3dfc8b, red = 0xff4d5e, amber = 0xfbbf24,
            purple = 0xa78bfa, orange = 0xff7a45;
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
struct Sample {
    int idx = 0;
    std::wstring name;
    double temp = NAN, fan = NAN, power = NAN, powerLimit = NAN;
    double util = NAN, memUtil = NAN, clk = NAN, memClk = NAN;
    double memUsed = NAN, memTotal = NAN;
    ULONGLONG tick = 0;
    SYSTEMTIME st{};
};

struct GpuHistory {
    std::wstring name;
    std::deque<Sample> samples;
    double minT = 1e9, maxT = -1e9, sumT = 0;
    double minF = 1e9, maxF = -1e9, sumF = 0;
    size_t nT = 0, nF = 0, total = 0;
    double peakPower = NAN;
    double bSum[101] = {};
    int bCnt[101] = {};
    ULONGLONG firstTick = 0;

    void Add(const Sample& s) {
        name = s.name;
        samples.push_back(s);
        if (samples.size() > MAX_HISTORY) samples.pop_front();
        if (!std::isnan(s.temp)) {
            if (s.temp < minT) minT = s.temp;
            if (s.temp > maxT) maxT = s.temp;
            sumT += s.temp; nT++;
        }
        if (!std::isnan(s.fan)) {
            if (s.fan < minF) minF = s.fan;
            if (s.fan > maxF) maxF = s.fan;
            sumF += s.fan; nF++;
        }
        if (!std::isnan(s.temp) && !std::isnan(s.fan)) {
            int b = (int)std::lround(s.temp);
            if (b < 0) b = 0;
            if (b > 100) b = 100;
            bSum[b] += s.fan;
            bCnt[b]++;
        }
        if (!std::isnan(s.power) && (std::isnan(peakPower) || s.power > peakPower)) peakPower = s.power;
        if (!firstTick) firstTick = s.tick;
        total++;
    }
};

struct App {
    HWND hwnd = nullptr;
    HANDLE thread = nullptr, stopEvent = nullptr, wakeEvent = nullptr;
    std::atomic<int> intervalMs{2000};
    std::atomic<bool> demo{false}, paused{false};

    std::map<int, GpuHistory> gpus;
    int sel = -1;
    std::wstring status = L"Waiting for first reading…";
    bool error = false;

    int alertTemp = 85;
    bool beep = true, topmost = false, help = false, alerting = false, flashOn = false;
    FILE* log = nullptr;
    std::wstring logPath;

    std::wstring toast;
    ULONGLONG toastUntil = 0;

    std::wstring exeDir, iniPath;
    ULONG_PTR gdipToken = 0;

    // command-line snapshot mode
    std::wstring snapshotPath;
    size_t snapshotAfter = 40;
};

static App g;
static float S = 1.f;  // DPI scale

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static double Clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float Minf(float a, float b) { return a < b ? a : b; }

static Color Col(DWORD rgb, BYTE a = 255) {
    return Color(a, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb);
}

static Color TempColor(double t, BYTE a = 255) {
    struct Stop { double t; DWORD c; };
    static const Stop st[] = {{30, 0x38bdf8}, {50, 0x3dfc8b}, {70, 0xfbbf24}, {85, 0xff7a45}, {95, 0xff3355}};
    const int n = sizeof(st) / sizeof(st[0]);
    if (std::isnan(t)) return Col(pal::dim, a);
    if (t <= st[0].t) return Col(st[0].c, a);
    for (int i = 0; i < n - 1; i++) {
        if (t <= st[i + 1].t) {
            double f = (t - st[i].t) / (st[i + 1].t - st[i].t);
            auto ch = [&](int sh) {
                double c0 = (st[i].c >> sh) & 255, c1 = (st[i + 1].c >> sh) & 255;
                return (BYTE)(c0 + (c1 - c0) * f);
            };
            return Color(a, ch(16), ch(8), ch(0));
        }
    }
    return Col(st[n - 1].c, a);
}

static std::wstring Fmt(double v, int prec, const wchar_t* suffix = L"") {
    if (std::isnan(v)) return L"N/A";
    wchar_t b[64];
    swprintf(b, 64, L"%.*f%ls", prec, v, suffix);
    return b;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static double ParseNum(const std::wstring& raw) {
    std::wstring s = Trim(raw);
    if (s.empty() || s[0] == L'[' || s == L"N/A") return NAN;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str()) return NAN;
    return v;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

static void Toast(const std::wstring& msg) {
    g.toast = msg;
    g.toastUntil = GetTickCount64() + 3500;
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// nvidia-smi (hidden process, timeout protected)
// ---------------------------------------------------------------------------
static bool RunCapture(const std::wstring& cmd, std::string& out, DWORD& exitCode, DWORD timeoutMs = 5000) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool exited = false;
    char chunk[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // pipe closed
        if (avail) {
            DWORD n = 0;
            DWORD want = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
            if (!ReadFile(rd, chunk, want, &n, nullptr) || !n) break;
            out.append(chunk, n);
            continue;
        }
        if (exited) break;
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            exited = true;
            continue;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return true;
}

static std::vector<std::wstring> SplitCsv(const std::wstring& line) {
    std::vector<std::wstring> f;
    size_t p = 0;
    for (;;) {
        size_t c = line.find(L',', p);
        if (c == std::wstring::npos) {
            f.push_back(Trim(line.substr(p)));
            break;
        }
        f.push_back(Trim(line.substr(p, c - p)));
        p = c + 1;
    }
    return f;
}

// Fields: full query has the name last so commas inside names are harmless.
static const wchar_t* Q_FULL =
    L"--query-gpu=index,temperature.gpu,fan.speed,power.draw,power.limit,utilization.gpu,"
    L"utilization.memory,clocks.gr,clocks.mem,memory.used,memory.total,name --format=csv,noheader,nounits";
static const wchar_t* Q_MIN = L"--query-gpu=index,temperature.gpu,fan.speed,name --format=csv,noheader,nounits";

static bool ParseOutput(const std::string& raw, bool full, std::vector<Sample>& out) {
    std::wstring text = Utf8ToWide(raw);
    size_t p = 0;
    const size_t nNum = full ? 11 : 3;
    SYSTEMTIME st;
    GetLocalTime(&st);
    ULONGLONG tick = GetTickCount64();
    while (p < text.size()) {
        size_t e = text.find(L'\n', p);
        std::wstring line = Trim(text.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p));
        p = (e == std::wstring::npos) ? text.size() : e + 1;
        if (line.empty()) continue;
        auto f = SplitCsv(line);
        if (f.size() < nNum + 1) continue;
        Sample s;
        double idx = ParseNum(f[0]);
        if (std::isnan(idx)) continue;
        s.idx = (int)idx;
        s.temp = ParseNum(f[1]);
        s.fan = ParseNum(f[2]);
        if (full) {
            s.power = ParseNum(f[3]);
            s.powerLimit = ParseNum(f[4]);
            s.util = ParseNum(f[5]);
            s.memUtil = ParseNum(f[6]);
            s.clk = ParseNum(f[7]);
            s.memClk = ParseNum(f[8]);
            s.memUsed = ParseNum(f[9]);
            s.memTotal = ParseNum(f[10]);
        }
        std::wstring name;
        for (size_t i = nNum; i < f.size(); i++) name += (i > nNum ? L"," : L"") + f[i];
        s.name = name;
        s.st = st;
        s.tick = tick;
        if (std::isnan(s.temp)) continue;
        out.push_back(s);
    }
    return !out.empty();
}

static bool NvQuery(std::vector<Sample>& out, std::wstring& err) {
    static const wchar_t* exes[] = {
        L"nvidia-smi.exe",
        L"\"C:\\Windows\\System32\\nvidia-smi.exe\"",
        L"\"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe\"",
    };
    static int goodExe = -1;
    static int mode = 0;  // 0 = try full, 1 = full works, 2 = minimal only

    bool launched = false;
    for (int i = 0; i < 3; i++) {
        int e = goodExe >= 0 ? goodExe : i;
        for (int pass = 0; pass < 2; pass++) {
            bool full = (mode == 2) ? false : (pass == 0);
            if (mode == 2 && pass == 1) break;
            std::string raw;
            DWORD code = 1;
            std::wstring cmd = std::wstring(exes[e]) + L" " + (full ? Q_FULL : Q_MIN);
            if (!RunCapture(cmd, raw, code)) break;  // exe not found -> next exe
            launched = true;
            out.clear();
            if (code == 0 && ParseOutput(raw, full, out)) {
                goodExe = e;
                if (mode == 0) mode = full ? 1 : 2;
                return true;
            }
            if (!full) break;
            if (mode == 1) break;
            std::wstring w = Trim(Utf8ToWide(raw));
            if (!w.empty()) err = w.substr(0, 160);
        }
        if (goodExe >= 0) break;
    }
    if (!launched) err = L"nvidia-smi not found (NVIDIA driver installed?). Press D for demo mode.";
    else if (err.empty()) err = L"nvidia-smi returned no usable data. Press D for demo mode.";
    return false;
}

// Simulated GPUs for demo mode (and for trying the UI without NVIDIA hardware)
static bool DemoQuery(std::vector<Sample>& out) {
    struct Sim { double temp, fan, t; unsigned seed; };
    static Sim sims[2] = {{38, 30, 0, 12345}, {34, 28, 17, 999}};
    static const wchar_t* names[2] = {L"Simulated GeForce RTX 4090", L"Simulated GeForce RTX 3060"};
    static const double limits[2] = {450, 170};
    static const double vram[2] = {24564, 12288};
    SYSTEMTIME st;
    GetLocalTime(&st);
    ULONGLONG tick = GetTickCount64();
    for (int i = 0; i < 2; i++) {
        Sim& s = sims[i];
        auto rnd = [&]() { s.seed = s.seed * 1103515245u + 12345u; return ((s.seed >> 16) & 0x7fff) / 32767.0; };
        s.t += 3.0;
        double load = 0.5 + 0.45 * std::sin(s.t / (60.0 + i * 25)) + 0.15 * std::sin(s.t / 11.0);
        if (rnd() > 0.93) load += 0.3;
        load = Clampd(load, 0.02, 1.0);
        double target = 33 + load * (i == 0 ? 52 : 44);
        s.temp += (target - s.temp) * 0.12 + (rnd() - 0.5) * 0.8;
        double fanTarget = s.temp < 45 ? 30 : 30 + (s.temp - 45) * (i == 0 ? 1.7 : 1.4);
        fanTarget = Clampd(fanTarget, 30, 100);
        s.fan += (fanTarget - s.fan) * 0.25;
        Sample x;
        x.idx = i;
        x.name = names[i];
        x.temp = std::round(s.temp);
        x.fan = std::round(s.fan);
        x.powerLimit = limits[i];
        x.power = Clampd(limits[i] * (0.08 + load * 0.9) + (rnd() - 0.5) * 8, 10, limits[i]);
        x.util = std::round(Clampd(load * 100 + (rnd() - 0.5) * 6, 0, 100));
        x.memUtil = std::round(Clampd(load * 70 + (rnd() - 0.5) * 6, 0, 100));
        x.clk = std::round(300 + load * (i == 0 ? 2400 : 1500));
        x.memClk = load > 0.1 ? (i == 0 ? 10501 : 7501) : 405;
        x.memTotal = vram[i];
        x.memUsed = std::round(vram[i] * (0.12 + load * 0.55));
        x.st = st;
        x.tick = tick;
        out.push_back(x);
    }
    return true;
}

static DWORD WINAPI Worker(LPVOID) {
    HANDLE evs[2] = {g.stopEvent, g.wakeEvent};
    for (;;) {
        if (!g.paused) {
            auto* v = new std::vector<Sample>;
            std::wstring err;
            bool ok = g.demo ? DemoQuery(*v) : NvQuery(*v, err);
            if (ok) {
                if (!PostMessageW(g.hwnd, WM_APP_DATA, 0, (LPARAM)v)) delete v;
            } else {
                delete v;
                auto* e = new std::wstring(err);
                if (!PostMessageW(g.hwnd, WM_APP_ERR, 0, (LPARAM)e)) delete e;
            }
        }
        DWORD r = WaitForMultipleObjects(2, evs, FALSE, (DWORD)g.intervalMs.load());
        if (r == WAIT_OBJECT_0) break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static int IniInt(const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(L"Settings", key, def, g.iniPath.c_str());
}
static void IniSet(const wchar_t* key, int v) {
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"Settings", key, b, g.iniPath.c_str());
}

static void SaveSettings() {
    IniSet(L"IntervalMs", g.intervalMs);
    IniSet(L"AlertTemp", g.alertTemp);
    IniSet(L"Beep", g.beep);
    IniSet(L"TopMost", g.topmost);
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (g.hwnd && GetWindowPlacement(g.hwnd, &wp)) {
        IniSet(L"X", wp.rcNormalPosition.left);
        IniSet(L"Y", wp.rcNormalPosition.top);
        IniSet(L"W", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        IniSet(L"H", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        IniSet(L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED);
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers (GDI+)
// ---------------------------------------------------------------------------
static std::map<int, Font*> g_fonts;
static FontFamily* g_family = nullptr;
static FontFamily* g_monoFamily = nullptr;

static Font* GetFont(float px, int style, bool mono = false) {
    int key = ((int)(px * 10) * 8 + style) * 2 + (mono ? 1 : 0);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    Font* f = new Font(mono ? g_monoFamily : g_family, px * S, style, UnitPixel);
    g_fonts[key] = f;
    return f;
}

static RectF Measure(Graphics& gr, const std::wstring& s, float px, int style = FontStyleRegular) {
    RectF bb;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    gr.MeasureString(s.c_str(), -1, GetFont(px, style), PointF(0, 0), &fmt, &bb);
    return bb;
}

// align: 0 left, 1 center, 2 right. Returns text width.
static float TextAt(Graphics& gr, const std::wstring& s, float x, float y, float px, Color c,
                    int style = FontStyleRegular, int align = 0, bool mono = false) {
    Font* f = GetFont(px, style, mono);
    SolidBrush b(c);
    StringFormat fmt;
    fmt.SetAlignment(align == 0 ? StringAlignmentNear : align == 1 ? StringAlignmentCenter : StringAlignmentFar);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    gr.DrawString(s.c_str(), -1, f, PointF(x, y), &fmt, &b);
    RectF bb;
    gr.MeasureString(s.c_str(), -1, f, PointF(0, 0), &fmt, &bb);
    return bb.Width;
}

static void TextIn(Graphics& gr, const std::wstring& s, const RectF& r, float px, Color c,
                   int style = FontStyleRegular, bool wrap = true) {
    SolidBrush b(c);
    StringFormat fmt;
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    gr.DrawString(s.c_str(), -1, GetFont(px, style), r, &fmt, &b);
}

static void RoundPath(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d < 1) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(Graphics& gr, RectF r, float rad, Color fill, Color border = Color(0, 0, 0, 0), float bw = 1) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    RoundPath(p, r, rad);
    SolidBrush b(fill);
    gr.FillPath(&b, &p);
    if (border.GetA()) {
        Pen pen(border, bw);
        gr.DrawPath(&pen, &p);
    }
}

static float Pill(Graphics& gr, float x, float cy, const std::wstring& s, DWORD color, bool right = false) {
    RectF m = Measure(gr, s, 11, FontStyleBold);
    float w = m.Width + 14 * S, h = 20 * S;
    if (right) x -= w;
    FillRound(gr, RectF(x, cy - h / 2, w, h), h / 2, Col(color, 38), Col(color, 150));
    TextAt(gr, s, x + w / 2, cy - m.Height / 2, 11, Col(color), FontStyleBold, 1);
    return w;
}

static void Card(Graphics& gr, const RectF& r) {
    FillRound(gr, r, 10 * S, Col(pal::card), Col(pal::cardBorder));
}

static void LegendItem(Graphics& gr, float& x, float cy, const std::wstring& label, Color c, int kind) {
    // kind: 0 line, 1 dot, 2 dashed line; drawn right-to-left
    RectF m = Measure(gr, label, 11);
    x -= m.Width;
    TextAt(gr, label, x, cy - m.Height / 2, 11, Col(pal::dim));
    x -= 6 * S;
    if (kind == 1) {
        SolidBrush b(c);
        gr.FillEllipse(&b, x - 8 * S, cy - 4 * S, 8 * S, 8 * S);
        x -= 8 * S;
    } else {
        Pen p(c, 2.5f * S);
        if (kind == 2) p.SetDashStyle(DashStyleDash);
        gr.DrawLine(&p, x - 16 * S, cy, x, cy);
        x -= 16 * S;
    }
    x -= 14 * S;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static GpuHistory* CurGpu() {
    auto it = g.gpus.find(g.sel);
    return it == g.gpus.end() ? nullptr : &it->second;
}

static void DrawHeader(Graphics& gr, float W, float hh) {
    float pad = 14 * S, cy = hh / 2;
    // logo
    LinearGradientBrush lg(PointF(pad, cy - 10 * S), PointF(pad + 20 * S, cy + 10 * S), Col(pal::cyan), Col(pal::orange));
    gr.FillEllipse(&lg, pad, cy - 10 * S, 20 * S, 20 * S);
    SolidBrush inner(Col(pal::bg));
    gr.FillEllipse(&inner, pad + 5 * S, cy - 5 * S, 10 * S, 10 * S);
    SolidBrush dot(Col(pal::green));
    gr.FillEllipse(&dot, pad + 7.5f * S, cy - 2.5f * S, 5 * S, 5 * S);

    float x = pad + 30 * S;
    RectF tm = Measure(gr, L"GPU FAN CURVE MONITOR", 14, FontStyleBold);
    TextAt(gr, L"GPU FAN CURVE MONITOR", x, cy - tm.Height / 2, 14, Col(pal::text), FontStyleBold);
    x += tm.Width + 12 * S;

    bool hasData = !g.gpus.empty();
    if (g.paused) x += Pill(gr, x, cy, L"PAUSED", pal::amber);
    else if (g.demo) x += Pill(gr, x, cy, L"DEMO", pal::purple);
    else if (g.error && !hasData) x += Pill(gr, x, cy, L"NO SIGNAL", pal::red);
    else if (g.error) x += Pill(gr, x, cy, L"STALE", pal::amber);
    else x += Pill(gr, x, cy, g.flashOn ? L"● LIVE" : L"○ LIVE", pal::green);
    x += 8 * S;

    wchar_t b[96];
    swprintf(b, 96, L"every %.1f s", g.intervalMs / 1000.0);
    RectF bm = Measure(gr, b, 11);
    TextAt(gr, b, x, cy - bm.Height / 2, 11, Col(pal::dim));
    x += bm.Width + 10 * S;

    if (g.gpus.size() > 1) {
        int pos = 1, n = (int)g.gpus.size();
        for (auto& kv : g.gpus) { if (kv.first == g.sel) break; pos++; }
        swprintf(b, 96, L"GPU %d/%d  [Tab]", pos, n);
        x += Pill(gr, x, cy, b, pal::cyan) + 8 * S;
    }
    if (g.topmost) x += Pill(gr, x, cy, L"ON TOP", pal::dim) + 8 * S;
    if (g.log) x += Pill(gr, x, cy, g.flashOn ? L"● REC" : L"○ REC", pal::red) + 8 * S;

    // right side: clock + alert
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(b, 96, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    RectF cm = Measure(gr, b, 13);
    float rx = W - pad;
    TextAt(gr, b, rx, cy - cm.Height / 2, 13, Col(pal::dim), FontStyleRegular, 2);
    rx -= cm.Width + 12 * S;
    if (g.alerting) {
        double hot = -1;
        for (auto& kv : g.gpus)
            if (!kv.second.samples.empty() && kv.second.samples.back().temp > hot) hot = kv.second.samples.back().temp;
        swprintf(b, 96, L"⚠ OVERHEAT %.0f°C", hot);
        Pill(gr, rx, cy, b, g.flashOn ? pal::red : pal::orange, true);
    }

    Pen sep(Col(pal::cardBorder), 1);
    gr.DrawLine(&sep, 0.f, hh - 0.5f, W, hh - 0.5f);
}

static void DrawCurveCard(Graphics& gr, const RectF& r) {
    Card(gr, r);
    TextAt(gr, L"Fan Curve", r.X + 16 * S, r.Y + 11 * S, 13, Col(pal::text), FontStyleBold);
    float lx = r.X + r.Width - 16 * S, ly = r.Y + 20 * S;
    LegendItem(gr, lx, ly, L"now", Col(pal::green), 1);
    LegendItem(gr, lx, ly, L"learned curve", Col(pal::amber), 2);
    LegendItem(gr, lx, ly, L"history", Col(pal::red, 200), 1);
    LegendItem(gr, lx, ly, L"trace", Col(pal::cyan), 0);

    float L = r.X + 58 * S, R = r.X + r.Width - 22 * S, T = r.Y + 44 * S, B = r.Y + r.Height - 48 * S;
    if (R - L < 60 * S || B - T < 50 * S) return;
    auto X = [&](double t) { return (float)(L + Clampd(t, 0, 100) / 100.0 * (R - L)); };
    auto Y = [&](double f) { return (float)(B - Clampd(f, 0, 100) / 100.0 * (B - T)); };

    // temperature zones
    SolidBrush zc(Col(pal::green, 10)), zw(Col(pal::amber, 14)), zh(Col(pal::red, 20));
    gr.FillRectangle(&zc, X(0), T, X(60) - X(0), B - T);
    gr.FillRectangle(&zw, X(60), T, X(80) - X(60), B - T);
    gr.FillRectangle(&zh, X(80), T, X(100) - X(80), B - T);
    TextAt(gr, L"COOL", X(30), T + 4 * S, 10, Col(pal::green, 90), FontStyleBold, 1);
    TextAt(gr, L"WARM", X(70), T + 4 * S, 10, Col(pal::amber, 110), FontStyleBold, 1);
    TextAt(gr, L"HOT", X(90), T + 4 * S, 10, Col(pal::red, 130), FontStyleBold, 1);

    // grid + labels
    Pen gp(Col(pal::grid), 1);
    for (int v = 0; v <= 100; v += 10) {
        gr.DrawLine(&gp, X(v), T, X(v), B);
        gr.DrawLine(&gp, L, Y(v), R, Y(v));
        wchar_t b[8];
        swprintf(b, 8, L"%d", v);
        TextAt(gr, b, X(v), B + 6 * S, 11, Col(pal::dim), FontStyleRegular, 1);
        RectF m = Measure(gr, b, 11);
        TextAt(gr, b, L - 8 * S, Y(v) - m.Height / 2, 11, Col(pal::dim), FontStyleRegular, 2);
    }
    Pen ap(Col(pal::axis), 1.5f * S);
    gr.DrawLine(&ap, L, B, R, B);
    gr.DrawLine(&ap, L, T, L, B);
    TextAt(gr, L"GPU Temperature (°C)", (L + R) / 2, B + 24 * S, 12, Col(pal::text, 200), FontStyleRegular, 1);
    gr.TranslateTransform(r.X + 14 * S, (T + B) / 2);
    gr.RotateTransform(-90);
    TextAt(gr, L"Fan Speed (%)", 0, 0, 12, Col(pal::text, 200), FontStyleRegular, 1);
    gr.ResetTransform();

    // alert threshold
    Pen alp(Col(pal::red, 170), 1.5f * S);
    alp.SetDashStyle(DashStyleDash);
    gr.DrawLine(&alp, X(g.alertTemp), T, X(g.alertTemp), B);
    wchar_t ab[24];
    swprintf(ab, 24, L"ALERT %d°", g.alertTemp);
    TextAt(gr, ab, X(g.alertTemp) - 4 * S, B - 18 * S, 10, Col(pal::red, 220), FontStyleBold, 2);

    GpuHistory* h = CurGpu();
    if (!h || h->samples.empty()) {
        TextIn(gr, g.status, RectF(L + 20 * S, (T + B) / 2 - 20 * S, R - L - 40 * S, 60 * S), 13, Col(pal::dim));
        return;
    }

    gr.SetClip(RectF(L - 12 * S, T - 12 * S, R - L + 24 * S, B - T + 24 * S));

    // learned (averaged, smoothed) fan curve
    {
        std::vector<PointF> run;
        Pen lp(Col(pal::amber, 210), 2.5f * S);
        lp.SetDashStyle(DashStyleDash);
        int lo = (int)std::floor(h->minT), hi = (int)std::ceil(h->maxT);
        for (int t = (lo < 0 ? 0 : lo); t <= (hi > 100 ? 100 : hi); t++) {
            double sum = 0;
            int cnt = 0;
            for (int k = t - 2; k <= t + 2; k++)
                if (k >= 0 && k <= 100) { sum += h->bSum[k]; cnt += h->bCnt[k]; }
            if (cnt) run.push_back(PointF(X(t), Y(sum / cnt)));
            else if (run.size() > 1) { gr.DrawLines(&lp, run.data(), (INT)run.size()); run.clear(); }
            else run.clear();
        }
        if (run.size() > 1) gr.DrawLines(&lp, run.data(), (INT)run.size());
    }

    // trace + history dots (fading in age)
    std::vector<const Sample*> pts;
    for (auto it = h->samples.rbegin(); it != h->samples.rend() && (int)pts.size() < CURVE_POINTS; ++it)
        if (!std::isnan(it->temp) && !std::isnan(it->fan)) pts.push_back(&*it);
    std::reverse(pts.begin(), pts.end());
    size_t n = pts.size();
    for (size_t i = 0; i < n; i++) {
        double age = n > 1 ? (double)i / (n - 1) : 1.0;
        BYTE a = (BYTE)(35 + 220 * age);
        if (i > 0) {
            Pen tp(TempColor(pts[i]->temp, a), (1.2f + 1.6f * (float)age) * S);
            tp.SetStartCap(LineCapRound);
            tp.SetEndCap(LineCapRound);
            gr.DrawLine(&tp, X(pts[i - 1]->temp), Y(pts[i - 1]->fan), X(pts[i]->temp), Y(pts[i]->fan));
        }
        SolidBrush db(Col(pal::red, (BYTE)(40 + 160 * age)));
        float rr = 2.6f * S;
        gr.FillEllipse(&db, X(pts[i]->temp) - rr, Y(pts[i]->fan) - rr, rr * 2, rr * 2);
    }

    // current point
    if (n) {
        const Sample& c = *pts.back();
        float cx = X(c.temp), cy = Y(c.fan);
        Pen ch(Col(pal::green, 90), 1);
        ch.SetDashStyle(DashStyleDot);
        gr.DrawLine(&ch, cx, cy, cx, B);
        gr.DrawLine(&ch, L, cy, cx, cy);
        const float glow[] = {24, 17, 12};
        const BYTE ga[] = {22, 40, 70};
        for (int i = 0; i < 3; i++) {
            SolidBrush gb(Col(pal::green, ga[i]));
            float rr = glow[i] * S;
            gr.FillEllipse(&gb, cx - rr, cy - rr, rr * 2, rr * 2);
        }
        SolidBrush cb(Col(pal::green));
        Pen cbp(Col(0xffffff, 230), 1.5f * S);
        float rr = 7 * S;
        gr.FillEllipse(&cb, cx - rr, cy - rr, rr * 2, rr * 2);
        gr.DrawEllipse(&cbp, cx - rr, cy - rr, rr * 2, rr * 2);

        gr.ResetClip();
        wchar_t lb[48];
        swprintf(lb, 48, L"%.0f°C  ·  %.0f%%", c.temp, c.fan);
        RectF m = Measure(gr, lb, 12, FontStyleBold);
        float bw = m.Width + 16 * S, bh = m.Height + 8 * S;
        float bx = cx + 16 * S, by = cy - bh - 12 * S;
        if (bx + bw > R) bx = cx - 16 * S - bw;
        if (by < T) by = cy + 14 * S;
        FillRound(gr, RectF(bx, by, bw, bh), 6 * S, Col(pal::bg, 235), TempColor(c.temp, 220), 1.5f * S);
        TextAt(gr, lb, bx + bw / 2, by + 4 * S, 12, Col(pal::text), FontStyleBold, 1);
    }
    gr.ResetClip();
}

static void DrawSeries(Graphics& gr, const std::vector<PointF>& p, const std::vector<bool>& ok, const Pen& pen) {
    std::vector<PointF> run;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i < p.size() && ok[i]) { run.push_back(p[i]); continue; }
        if (run.size() > 1) gr.DrawLines(&pen, run.data(), (INT)run.size());
        run.clear();
    }
}

static void DrawTimelineCard(Graphics& gr, const RectF& r) {
    Card(gr, r);
    TextAt(gr, L"Timeline", r.X + 16 * S, r.Y + 11 * S, 13, Col(pal::text), FontStyleBold);
    float lx = r.X + r.Width - 16 * S, ly = r.Y + 20 * S;
    LegendItem(gr, lx, ly, L"power % of limit", Col(pal::amber, 200), 2);
    LegendItem(gr, lx, ly, L"GPU load %", Col(pal::purple), 0);
    LegendItem(gr, lx, ly, L"fan %", Col(pal::cyan), 0);
    LegendItem(gr, lx, ly, L"temp °C", Col(pal::orange), 0);

    float L = r.X + 58 * S, R = r.X + r.Width - 22 * S, T = r.Y + 42 * S, B = r.Y + r.Height - 28 * S;
    if (R - L < 60 * S || B - T < 30 * S) return;
    auto Y = [&](double v) { return (float)(B - Clampd(v, 0, 100) / 100.0 * (B - T)); };

    Pen gp(Col(pal::grid), 1);
    for (int v = 0; v <= 100; v += 25) {
        gr.DrawLine(&gp, L, Y(v), R, Y(v));
        wchar_t b[8];
        swprintf(b, 8, L"%d", v);
        RectF m = Measure(gr, b, 11);
        TextAt(gr, b, L - 8 * S, Y(v) - m.Height / 2, 11, Col(pal::dim), FontStyleRegular, 2);
    }
    Pen alp(Col(pal::red, 110), 1);
    alp.SetDashStyle(DashStyleDash);
    gr.DrawLine(&alp, L, Y(g.alertTemp), R, Y(g.alertTemp));

    GpuHistory* h = CurGpu();
    if (!h || h->samples.empty()) return;

    int slots = TIMELINE_POINTS;
    int fit = (int)((R - L) / (2 * S));
    if (fit < slots) slots = fit < 10 ? 10 : fit;
    size_t n = h->samples.size() < (size_t)slots ? h->samples.size() : (size_t)slots;
    size_t start = h->samples.size() - n;
    float step = (R - L) / (slots - 1);

    std::vector<PointF> pt(n), pf(n), pu(n), pp(n);
    std::vector<bool> ot(n), of(n), ou(n), op(n);
    for (size_t i = 0; i < n; i++) {
        const Sample& s = h->samples[start + i];
        float x = R - (float)(n - 1 - i) * step;
        pt[i] = PointF(x, Y(s.temp)); ot[i] = !std::isnan(s.temp);
        pf[i] = PointF(x, Y(s.fan)); of[i] = !std::isnan(s.fan);
        pu[i] = PointF(x, Y(s.util)); ou[i] = !std::isnan(s.util);
        double pw = (!std::isnan(s.power) && !std::isnan(s.powerLimit) && s.powerLimit > 0) ? s.power / s.powerLimit * 100 : NAN;
        pp[i] = PointF(x, Y(pw)); op[i] = !std::isnan(pw);
    }

    // area fill under temperature
    {
        std::vector<PointF> run;
        LinearGradientBrush fb(PointF(0, T), PointF(0, B + 1), Col(pal::orange, 80), Col(pal::orange, 0));
        for (size_t i = 0; i <= n; i++) {
            if (i < n && ot[i]) { run.push_back(pt[i]); continue; }
            if (run.size() > 1) {
                std::vector<PointF> poly = run;
                poly.push_back(PointF(run.back().X, B));
                poly.push_back(PointF(run.front().X, B));
                gr.FillPolygon(&fb, poly.data(), (INT)poly.size());
            }
            run.clear();
        }
    }
    Pen ppen(Col(pal::amber, 170), 1.5f * S);
    ppen.SetDashStyle(DashStyleDash);
    DrawSeries(gr, pp, op, ppen);
    Pen upen(Col(pal::purple, 220), 1.5f * S);
    DrawSeries(gr, pu, ou, upen);
    Pen fpen(Col(pal::cyan), 2.2f * S);
    fpen.SetLineJoin(LineJoinRound);
    DrawSeries(gr, pf, of, fpen);
    Pen tpen(Col(pal::orange), 2.4f * S);
    tpen.SetLineJoin(LineJoinRound);
    DrawSeries(gr, pt, ot, tpen);

    // time axis
    double span = (h->samples.back().tick - h->samples[start].tick) / 1000.0;
    wchar_t b[48];
    if (span >= 120) swprintf(b, 48, L"−%.1f min", span / 60);
    else swprintf(b, 48, L"−%.0f s", span);
    TextAt(gr, b, pt[0].X, B + 6 * S, 11, Col(pal::dim), FontStyleRegular, n > 1 ? 1 : 2);
    TextAt(gr, L"now", R, B + 6 * S, 11, Col(pal::dim), FontStyleRegular, 2);
}

static float Bar(Graphics& gr, float x, float y, float w, const std::wstring& label, const std::wstring& value,
                 double frac, DWORD color) {
    TextAt(gr, label, x, y, 11, Col(pal::dim));
    TextAt(gr, value, x + w, y, 11, Col(pal::text), FontStyleBold, 2);
    float by = y + 18 * S, bh = 7 * S;
    FillRound(gr, RectF(x, by, w, bh), bh / 2, Col(pal::grid));
    if (!std::isnan(frac)) {
        float fw = (float)(w * Clampd(frac, 0, 1));
        if (fw > bh) {
            LinearGradientBrush lb(PointF(x, by), PointF(x + w, by), Col(color, 150), Col(color));
            GraphicsPath p;
            RoundPath(p, RectF(x, by, fw, bh), bh / 2);
            gr.FillPath(&lb, &p);
        }
    }
    return 34 * S;
}

static void DrawInfoCard(Graphics& gr, const RectF& r) {
    Card(gr, r);
    gr.SetClip(r);
    float x = r.X + 18 * S, w = r.Width - 36 * S, y = r.Y + 14 * S;
    GpuHistory* h = CurGpu();
    if (!h || h->samples.empty()) {
        TextAt(gr, L"NO DATA", x, y, 11, Col(pal::dim), FontStyleBold);
        TextIn(gr, g.status, RectF(x, y + 22 * S, w, 140 * S), 12, Col(g.error ? pal::red : pal::text, 220));
        TextIn(gr, L"Shortcuts: H help · D demo mode · right-click for menu", RectF(x, y + 170 * S, w, 60 * S), 11, Col(pal::dim));
        gr.ResetClip();
        return;
    }
    const Sample& c = h->samples.back();
    wchar_t b[128];
    swprintf(b, 128, L"GPU %d", c.idx);
    TextAt(gr, b, x, y, 11, Col(pal::dim), FontStyleBold);
    y += 17 * S;
    RectF nameR(x, y, w, 40 * S);
    TextIn(gr, h->name, nameR, 14, Col(pal::text), FontStyleBold);
    RectF nm;
    StringFormat fmt;
    gr.MeasureString(h->name.c_str(), -1, GetFont(14, FontStyleBold), RectF(0, 0, w, 40 * S), &fmt, &nm);
    y += Minf(nm.Height, 40 * S) + 10 * S;

    // big readouts
    float col = w / 2;
    std::wstring ts = std::isnan(c.temp) ? L"--" : Fmt(c.temp, 0);
    float tw = TextAt(gr, ts, x - 3 * S, y, 44, TempColor(c.temp), FontStyleBold);
    TextAt(gr, L"°C", x - 3 * S + tw - 6 * S, y + 8 * S, 16, TempColor(c.temp, 200), FontStyleBold);
    std::wstring fs = std::isnan(c.fan) ? L"--" : Fmt(c.fan, 0);
    float fw = TextAt(gr, fs, x + col - 3 * S, y, 44, Col(pal::cyan), FontStyleBold);
    TextAt(gr, L"%", x + col - 3 * S + fw - 6 * S, y + 8 * S, 16, Col(pal::cyan, 200), FontStyleBold);
    y += 56 * S;
    TextAt(gr, L"TEMPERATURE", x, y, 10, Col(pal::dim), FontStyleBold);
    TextAt(gr, L"FAN SPEED", x + col, y, 10, Col(pal::dim), FontStyleBold);
    y += 22 * S;

    // trend arrows vs ~30 s ago
    if (h->samples.size() > 3) {
        const Sample* old = &h->samples.front();
        for (auto it = h->samples.rbegin(); it != h->samples.rend(); ++it)
            if (c.tick - it->tick >= 30000) { old = &*it; break; }
        double dt = c.temp - old->temp, df = c.fan - old->fan;
        auto arrow = [](double d) { return d > 0.5 ? L"▲ " : d < -0.5 ? L"▼ " : L"► "; };
        if (!std::isnan(dt)) {
            swprintf(b, 128, L"%ls%+.0f°", arrow(dt), dt);
            TextAt(gr, b, x, y - 6 * S, 10, Col(dt > 0.5 ? pal::orange : dt < -0.5 ? pal::cyan : pal::dim));
        }
        if (!std::isnan(df)) {
            swprintf(b, 128, L"%ls%+.0f%%", arrow(df), df);
            TextAt(gr, b, x + col, y - 6 * S, 10, Col(pal::dim));
        }
        y += 14 * S;
    }

    std::wstring pv = std::isnan(c.power) ? L"N/A"
                      : std::isnan(c.powerLimit) ? Fmt(c.power, 0, L" W")
                                                 : Fmt(c.power, 0) + L" / " + Fmt(c.powerLimit, 0, L" W");
    y += Bar(gr, x, y, w, L"Power", pv, (std::isnan(c.powerLimit) || c.powerLimit <= 0) ? NAN : c.power / c.powerLimit, pal::amber);
    y += Bar(gr, x, y, w, L"GPU load", Fmt(c.util, 0, L" %"), c.util / 100, pal::purple);
    y += Bar(gr, x, y, w, L"Memory controller", Fmt(c.memUtil, 0, L" %"), c.memUtil / 100, pal::green);
    std::wstring vv = std::isnan(c.memUsed) ? L"N/A" : Fmt(c.memUsed / 1024, 1) + L" / " + Fmt(c.memTotal / 1024, 1, L" GiB");
    y += Bar(gr, x, y, w, L"VRAM", vv, (std::isnan(c.memTotal) || c.memTotal <= 0) ? NAN : c.memUsed / c.memTotal, pal::cyan);

    y += 2 * S;
    TextAt(gr, L"CORE CLOCK", x, y, 10, Col(pal::dim), FontStyleBold);
    TextAt(gr, L"MEMORY CLOCK", x + col, y, 10, Col(pal::dim), FontStyleBold);
    y += 15 * S;
    TextAt(gr, Fmt(c.clk, 0, L" MHz"), x, y, 13, Col(pal::text), FontStyleBold);
    TextAt(gr, Fmt(c.memClk, 0, L" MHz"), x + col, y, 13, Col(pal::text), FontStyleBold);
    y += 28 * S;

    Pen sep(Col(pal::cardBorder), 1);
    gr.DrawLine(&sep, x, y, x + w, y);
    y += 10 * S;
    TextAt(gr, L"SESSION", x, y, 10, Col(pal::dim), FontStyleBold);
    y += 18 * S;

    auto row = [&](const std::wstring& k, const std::wstring& v) {
        TextAt(gr, k, x, y, 11, Col(pal::dim));
        TextAt(gr, v, x + w, y, 11, Col(pal::text), FontStyleRegular, 2);
        y += 19 * S;
    };
    if (h->nT) {
        swprintf(b, 128, L"%.0f  ·  %.1f  ·  %.0f °C", h->minT, h->sumT / h->nT, h->maxT);
        row(L"Temp min · avg · max", b);
    }
    if (h->nF) {
        swprintf(b, 128, L"%.0f  ·  %.1f  ·  %.0f %%", h->minF, h->sumF / h->nF, h->maxF);
        row(L"Fan min · avg · max", b);
    }
    row(L"Peak power", Fmt(h->peakPower, 0, L" W"));
    double mins = (c.tick - h->firstTick) / 60000.0;
    swprintf(b, 128, L"%zu  ·  %.1f min", h->total, mins);
    row(L"Samples · duration", b);
    swprintf(b, 128, L"%d °C  ·  beep %ls", g.alertTemp, g.beep ? L"on" : L"off");
    row(L"Alert threshold", b);
    if (g.log) {
        std::wstring fn = g.logPath.substr(g.logPath.find_last_of(L"\\/") + 1);
        TextIn(gr, L"● Logging to " + fn, RectF(x, y + 2 * S, w, 34 * S), 11, Col(pal::red));
    }
    gr.ResetClip();
}

static void DrawHelp(Graphics& gr, float W, float H) {
    SolidBrush dim(Color(170, 0, 0, 0));
    gr.FillRectangle(&dim, 0.f, 0.f, W, H);
    static const wchar_t* keys[][2] = {
        {L"Space", L"Pause / resume sampling"},
        {L"Tab / 0-9", L"Next GPU / select GPU by index"},
        {L"+  /  -", L"Faster / slower sampling interval"},
        {L"↑  /  ↓", L"Raise / lower overheat alert threshold"},
        {L"B", L"Toggle alert beep"},
        {L"L", L"Start / stop continuous CSV logging"},
        {L"S", L"Export history to CSV…"},
        {L"P", L"Save PNG screenshot next to the .exe"},
        {L"Ctrl+C", L"Copy screenshot to clipboard"},
        {L"T", L"Toggle always-on-top"},
        {L"C", L"Clear history and statistics"},
        {L"D", L"Toggle demo mode (simulated GPUs)"},
        {L"H / F1", L"Show / hide this help"},
        {L"Right-click", L"Context menu with all actions"},
    };
    const int n = sizeof(keys) / sizeof(keys[0]);
    float cw = 480 * S, chh = (70 + n * 24 + 40) * S;
    RectF r((W - cw) / 2, (H - chh) / 2, cw, chh);
    FillRound(gr, r, 12 * S, Col(pal::card), Col(pal::cyan, 120), 1.5f * S);
    TextAt(gr, L"Keyboard shortcuts", r.X + 24 * S, r.Y + 20 * S, 16, Col(pal::text), FontStyleBold);
    float y = r.Y + 60 * S;
    for (int i = 0; i < n; i++) {
        RectF km = Measure(gr, keys[i][0], 11, FontStyleBold);
        FillRound(gr, RectF(r.X + 24 * S, y - 2 * S, km.Width + 12 * S, 20 * S), 4 * S, Col(pal::grid), Col(pal::cardBorder));
        TextAt(gr, keys[i][0], r.X + 30 * S, y, 11, Col(pal::cyan), FontStyleBold);
        TextAt(gr, keys[i][1], r.X + 150 * S, y, 12, Col(pal::text));
        y += 24 * S;
    }
    TextAt(gr, L"Press H or Esc to close", r.X + cw / 2, r.Y + chh - 30 * S, 11, Col(pal::dim), FontStyleRegular, 1);
}

static void Render(Graphics& gr, int Wi, int Hi) {
    float W = (float)Wi, H = (float)Hi;
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    gr.SetPixelOffsetMode(PixelOffsetModeHalf);
    gr.Clear(Col(pal::bg));

    float header = 48 * S, footer = 26 * S, pad = 12 * S;
    DrawHeader(gr, W, header);

    float panelW = Clampd(W * 0.27f, 250 * S, 330 * S);
    if (W < 760 * S) panelW = 0;
    float x0 = pad, y0 = header + pad, x1 = W - pad - (panelW > 0 ? panelW + pad : 0), y1 = H - footer - pad / 2;
    float ch = (y1 - y0 - pad);
    float curveH = ch * 0.62f;
    DrawCurveCard(gr, RectF(x0, y0, x1 - x0, curveH));
    DrawTimelineCard(gr, RectF(x0, y0 + curveH + pad, x1 - x0, ch - curveH));
    if (panelW > 0) DrawInfoCard(gr, RectF(x1 + pad, y0, panelW, y1 - y0));

    TextAt(gr, L"Space pause · Tab GPU · +/- interval · ↑/↓ alert · L log · S export · P screenshot · T on top · D demo · H help",
           W / 2, H - footer + 4 * S, 11, Col(pal::dim, 200), FontStyleRegular, 1);

    if (!g.toast.empty() && GetTickCount64() < g.toastUntil) {
        RectF m = Measure(gr, g.toast, 12, FontStyleBold);
        float tw = m.Width + 28 * S, th = 32 * S;
        RectF tr((W - tw) / 2, H - footer - th - 14 * S, tw, th);
        FillRound(gr, tr, th / 2, Col(0x1f2430, 245), Col(pal::cyan, 180), 1.5f * S);
        TextAt(gr, g.toast, W / 2, tr.Y + (th - m.Height) / 2, 12, Col(pal::text), FontStyleBold, 1);
    }

    if (g.help) DrawHelp(gr, W, H);

    if (g.alerting && g.flashOn) {
        Pen bp(Col(pal::red, 220), 4 * S);
        gr.DrawRectangle(&bp, 2 * S, 2 * S, W - 4 * S, H - 4 * S);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static bool GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    auto* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; i++)
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; return true; }
    return false;
}

static void ClientSize(int& W, int& H) {
    RECT rc{0, 0, 1200, 760};
    if (g.hwnd) GetClientRect(g.hwnd, &rc);
    W = rc.right > 0 ? rc.right : 1200;
    H = rc.bottom > 0 ? rc.bottom : 760;
}

static bool SavePng(const std::wstring& path) {
    int W, H;
    ClientSize(W, H);
    Bitmap bmp(W, H, PixelFormat32bppARGB);
    {
        Graphics gr(&bmp);
        Render(gr, W, H);
    }
    CLSID c;
    if (!GetEncoderClsid(L"image/png", &c)) return false;
    return bmp.Save(path.c_str(), &c, nullptr) == Ok;
}

static void Screenshot() {
    std::wstring path = g.exeDir + L"gpu_fancurve_" + Stamp() + L".png";
    if (SavePng(path)) Toast(L"Screenshot saved: " + path.substr(path.find_last_of(L'\\') + 1));
    else Toast(L"Could not save screenshot (folder not writable?)");
}

static void CopyToClipboard() {
    int W, H;
    ClientSize(W, H);
    Bitmap bmp(W, H, PixelFormat32bppRGB);
    {
        Graphics gr(&bmp);
        Render(gr, W, H);
    }
    HBITMAP hb = nullptr;
    if (bmp.GetHBITMAP(Color(0, 0, 0), &hb) != Ok || !hb) return;
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_BITMAP, hb)) hb = nullptr;
        CloseClipboard();
        Toast(L"Screenshot copied to clipboard");
    }
    if (hb) DeleteObject(hb);
}

static void WriteCsvHeader(FILE* f) {
    fputs("time,gpu_index,gpu_name,temp_c,fan_pct,power_w,power_limit_w,gpu_util_pct,mem_util_pct,"
          "core_clock_mhz,mem_clock_mhz,vram_used_mib,vram_total_mib\n", f);
}

static void WriteCsvRow(FILE* f, const Sample& s) {
    auto num = [&](double v) {
        if (std::isnan(v)) fputs(",", f);
        else fprintf(f, ",%g", v);
    };
    std::string name = WideToUtf8(s.name);
    std::string q;
    for (char ch : name) { if (ch == '"') q += '"'; q += ch; }
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d,%d,\"%s\"", s.st.wYear, s.st.wMonth, s.st.wDay, s.st.wHour,
            s.st.wMinute, s.st.wSecond, s.st.wMilliseconds, s.idx, q.c_str());
    num(s.temp); num(s.fan); num(s.power); num(s.powerLimit); num(s.util); num(s.memUtil);
    num(s.clk); num(s.memClk); num(s.memUsed); num(s.memTotal);
    fputs("\n", f);
}

static void ExportCsv() {
    if (g.gpus.empty()) { Toast(L"Nothing to export yet"); return; }
    wchar_t file[MAX_PATH];
    swprintf(file, MAX_PATH, L"gpu_history_%ls.csv", Stamp().c_str());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    FILE* f = _wfopen(file, L"wb");
    if (!f) { Toast(L"Could not write file"); return; }
    WriteCsvHeader(f);
    size_t rows = 0;
    for (auto& kv : g.gpus)
        for (auto& s : kv.second.samples) { WriteCsvRow(f, s); rows++; }
    fclose(f);
    wchar_t b[64];
    swprintf(b, 64, L"Exported %zu rows", rows);
    Toast(b);
}

static void ToggleLog() {
    if (g.log) {
        fclose(g.log);
        g.log = nullptr;
        Toast(L"Logging stopped");
        return;
    }
    g.logPath = g.exeDir + L"gpu_log_" + Stamp() + L".csv";
    g.log = _wfopen(g.logPath.c_str(), L"wb");
    if (!g.log) { Toast(L"Could not create log file (folder not writable?)"); return; }
    WriteCsvHeader(g.log);
    fflush(g.log);
    Toast(L"Logging to " + g.logPath.substr(g.logPath.find_last_of(L'\\') + 1));
}

static void SetTopmost(bool on) {
    g.topmost = on;
    SetWindowPos(g.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static void ChangeInterval(int dir) {
    const int n = sizeof(INTERVALS) / sizeof(INTERVALS[0]);
    int cur = 0;
    for (int i = 0; i < n; i++) if (INTERVALS[i] <= g.intervalMs) cur = i;
    cur += dir;
    if (cur < 0) cur = 0;
    if (cur >= n) cur = n - 1;
    g.intervalMs = INTERVALS[cur];
    wchar_t b[48];
    swprintf(b, 48, L"Sampling every %.1f s", g.intervalMs / 1000.0);
    Toast(b);
}

static void UpdateTitle() {
    GpuHistory* h = CurGpu();
    if (!h || h->samples.empty()) { SetWindowTextW(g.hwnd, APP_NAME); return; }
    const Sample& c = h->samples.back();
    wchar_t b[160];
    swprintf(b, 160, L"%ls°C · Fan %ls%% — %ls", Fmt(c.temp, 0).c_str(), Fmt(c.fan, 0).c_str(), APP_NAME);
    SetWindowTextW(g.hwnd, b);
}

static void EvaluateAlert() {
    double hot = -1;
    for (auto& kv : g.gpus)
        if (!kv.second.samples.empty() && kv.second.samples.back().temp > hot) hot = kv.second.samples.back().temp;
    if (!g.alerting && hot >= g.alertTemp) {
        g.alerting = true;
        if (g.beep) MessageBeep(MB_ICONEXCLAMATION);
        FLASHWINFO fi{sizeof(fi), g.hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&fi);
    } else if (g.alerting && hot <= g.alertTemp - 3) {
        g.alerting = false;
    }
}

static void DoCommand(int id) {
    switch (id) {
    case CMD_PAUSE:
        g.paused = !g.paused;
        Toast(g.paused ? L"Paused" : L"Resumed");
        if (!g.paused) SetEvent(g.wakeEvent);
        break;
    case CMD_CLEAR:
        for (auto& kv : g.gpus) kv.second = GpuHistory();
        g.gpus.clear();
        g.alerting = false;
        Toast(L"History cleared");
        SetEvent(g.wakeEvent);
        break;
    case CMD_NEXTGPU:
        if (g.gpus.size() > 1) {
            auto it = g.gpus.upper_bound(g.sel);
            g.sel = (it == g.gpus.end() ? g.gpus.begin() : it)->first;
            UpdateTitle();
        }
        break;
    case CMD_FASTER: ChangeInterval(-1); SetEvent(g.wakeEvent); break;
    case CMD_SLOWER: ChangeInterval(+1); break;
    case CMD_ALERT_UP:
    case CMD_ALERT_DOWN: {
        g.alertTemp += id == CMD_ALERT_UP ? 1 : -1;
        if (g.alertTemp < 40) g.alertTemp = 40;
        if (g.alertTemp > 105) g.alertTemp = 105;
        wchar_t b[40];
        swprintf(b, 40, L"Alert threshold %d °C", g.alertTemp);
        Toast(b);
        g.alerting = false;
        EvaluateAlert();
        break;
    }
    case CMD_LOG: ToggleLog(); break;
    case CMD_EXPORT: ExportCsv(); break;
    case CMD_SHOT: Screenshot(); break;
    case CMD_COPY: CopyToClipboard(); break;
    case CMD_TOP: SetTopmost(!g.topmost); Toast(g.topmost ? L"Always on top" : L"Normal window"); break;
    case CMD_DEMO:
        g.demo = !g.demo;
        g.gpus.clear();
        g.sel = -1;
        g.alerting = false;
        g.error = false;
        g.status = g.demo ? L"Starting demo mode…" : L"Waiting for first reading…";
        Toast(g.demo ? L"Demo mode: simulated GPUs" : L"Live mode: nvidia-smi");
        SetEvent(g.wakeEvent);
        UpdateTitle();
        break;
    case CMD_BEEP: g.beep = !g.beep; Toast(g.beep ? L"Alert beep on" : L"Alert beep off"); break;
    case CMD_HELP: g.help = !g.help; break;
    case CMD_EXIT: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void ShowMenu(HWND hwnd, int x, int y) {
    HMENU m = CreatePopupMenu();
    auto item = [&](int id, const wchar_t* text, bool checked = false, bool enabled = true) {
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text);
    };
    item(CMD_PAUSE, L"Pause\tSpace", g.paused);
    item(CMD_NEXTGPU, L"Next GPU\tTab", false, g.gpus.size() > 1);
    item(CMD_FASTER, L"Faster sampling\t+");
    item(CMD_SLOWER, L"Slower sampling\t-");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(CMD_ALERT_UP, L"Raise alert threshold\tUp");
    item(CMD_ALERT_DOWN, L"Lower alert threshold\tDown");
    item(CMD_BEEP, L"Alert beep\tB", g.beep);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(CMD_LOG, L"Continuous CSV logging\tL", g.log != nullptr);
    item(CMD_EXPORT, L"Export history to CSV…\tS");
    item(CMD_SHOT, L"Save PNG screenshot\tP");
    item(CMD_COPY, L"Copy screenshot\tCtrl+C");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(CMD_TOP, L"Always on top\tT", g.topmost);
    item(CMD_DEMO, L"Demo mode\tD", g.demo);
    item(CMD_CLEAR, L"Clear history\tC");
    item(CMD_HELP, L"Keyboard shortcuts\tH");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(CMD_EXIT, L"Exit");
    if (x == -1 && y == -1) {
        POINT p{40, 40};
        ClientToScreen(hwnd, &p);
        x = p.x; y = p.y;
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) DoCommand(cmd);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        SetTimer(hwnd, TIMER_UI, 500, nullptr);
        return 0;

    case WM_APP_DATA: {
        std::vector<Sample>* v = (std::vector<Sample>*)lp;
        if (!g.paused) {
            for (auto& s : *v) {
                g.gpus[s.idx].Add(s);
                if (g.log) WriteCsvRow(g.log, s);
            }
            if (g.log) fflush(g.log);
            if (g.gpus.find(g.sel) == g.gpus.end() && !g.gpus.empty()) g.sel = g.gpus.begin()->first;
            g.error = false;
            g.status.clear();
            EvaluateAlert();
            UpdateTitle();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete v;
        if (!g.snapshotPath.empty()) {
            GpuHistory* h = CurGpu();
            if (h && h->total >= g.snapshotAfter) {
                SavePng(g.snapshotPath);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                g.snapshotPath.clear();
            }
        }
        return 0;
    }

    case WM_APP_ERR: {
        std::wstring* e = (std::wstring*)lp;
        g.error = true;
        g.status = *e;
        delete e;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_UI) {
            g.flashOn = !g.flashOn;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);  // full client rect (the prototypes used rcPaint by mistake)
        int W = rc.right, H = rc.bottom;
        if (W > 0 && H > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bm = CreateCompatibleBitmap(hdc, W, H);
            HGDIOBJ old = SelectObject(mem, bm);
            {
                Graphics gr(mem);
                Render(gr, W, H);
            }
            BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bm);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = (LONG)(640 * S);
        mm->ptMinTrackSize.y = (LONG)(460 * S);
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_SPACE: DoCommand(CMD_PAUSE); break;
        case VK_TAB: DoCommand(CMD_NEXTGPU); break;
        case VK_ADD: case VK_OEM_PLUS: DoCommand(CMD_FASTER); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: DoCommand(CMD_SLOWER); break;
        case VK_UP: DoCommand(CMD_ALERT_UP); break;
        case VK_DOWN: DoCommand(CMD_ALERT_DOWN); break;
        case VK_F1: case 'H': DoCommand(CMD_HELP); break;
        case VK_ESCAPE: if (g.help) DoCommand(CMD_HELP); break;
        case 'C': DoCommand(ctrl ? CMD_COPY : CMD_CLEAR); break;
        case 'L': DoCommand(CMD_LOG); break;
        case 'S': DoCommand(CMD_EXPORT); break;
        case 'P': DoCommand(CMD_SHOT); break;
        case 'T': DoCommand(CMD_TOP); break;
        case 'D': DoCommand(CMD_DEMO); break;
        case 'B': DoCommand(CMD_BEEP); break;
        default:
            if (wp >= '0' && wp <= '9' && g.gpus.count((int)(wp - '0'))) {
                g.sel = (int)(wp - '0');
                UpdateTitle();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_CONTEXTMENU:
        ShowMenu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_COMMAND:
        DoCommand(LOWORD(wp));
        return 0;

    case WM_CLOSE:
        SaveSettings();
        SetEvent(g.stopEvent);
        if (g.thread) {
            WaitForSingleObject(g.thread, 7000);
            CloseHandle(g.thread);
            g.thread = nullptr;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        if (g.log) { fclose(g.log); g.log = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDPIAware();

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g.exeDir = path;
    g.exeDir = g.exeDir.substr(0, g.exeDir.find_last_of(L"\\/") + 1);
    g.iniPath = g.exeDir + L"GPUFanCurveMonitor.ini";

    g.intervalMs = IniInt(L"IntervalMs", 2000);
    if (g.intervalMs < 250) g.intervalMs = 250;
    g.alertTemp = IniInt(L"AlertTemp", 85);
    g.beep = IniInt(L"Beep", 1) != 0;
    bool wantTop = IniInt(L"TopMost", 0) != 0;

    // Command line: --demo  --interval <ms>  --alert <C>  --log  --snapshot <file.png> [--samples N]
    bool startLog = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        std::wstring a = argv[i];
        bool hasNext = i + 1 < argc;
        if (a == L"--demo") g.demo = true;
        else if (a == L"--log") startLog = true;
        else if (a == L"--topmost") wantTop = true;
        else if (a == L"--interval" && hasNext) g.intervalMs = _wtoi(argv[++i]) < 50 ? 50 : _wtoi(argv[i]);
        else if (a == L"--alert" && hasNext) g.alertTemp = _wtoi(argv[++i]);
        else if (a == L"--snapshot" && hasNext) g.snapshotPath = argv[++i];
        else if (a == L"--samples" && hasNext) g.snapshotAfter = (size_t)_wtoi(argv[++i]);
    }
    if (argv) LocalFree(argv);

    GdiplusStartupInput gsi;
    GdiplusStartup(&g.gdipToken, &gsi, nullptr);
    g_family = new FontFamily(L"Segoe UI");
    if (g_family->GetLastStatus() != Ok) {
        delete g_family;
        g_family = FontFamily::GenericSansSerif()->Clone();
    }
    g_monoFamily = new FontFamily(L"Consolas");
    if (g_monoFamily->GetLastStatus() != Ok) {
        delete g_monoFamily;
        g_monoFamily = FontFamily::GenericMonospace()->Clone();
    }

    HDC sdc = GetDC(nullptr);
    S = GetDeviceCaps(sdc, LOGPIXELSY) / 96.0f;
    ReleaseDC(nullptr, sdc);
    if (S < 1) S = 1;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    int w = IniInt(L"W", (int)(1240 * S)), h = IniInt(L"H", (int)(780 * S));
    int x = IniInt(L"X", CW_USEDEFAULT), y = IniInt(L"Y", CW_USEDEFAULT);
    // keep the window on a visible monitor
    RECT wr{x, y, x + w, y + h};
    if (x != CW_USEDEFAULT && !MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW, x, y, w, h,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // dark title bar on Windows 10 1809+ / 11 (ignored elsewhere)
    typedef HRESULT(WINAPI * DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto fn = (DwmSetAttr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE;
            if (FAILED(fn(hwnd, 20, &dark, sizeof(dark)))) fn(hwnd, 19, &dark, sizeof(dark));
        }
    }

    if (wantTop) SetTopmost(true);
    if (startLog) ToggleLog();
    if (g.demo) g.status = L"Starting demo mode…";

    ShowWindow(hwnd, (IniInt(L"Maximized", 0) && g.snapshotPath.empty()) ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(hwnd);

    g.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    for (auto& kv : g_fonts) delete kv.second;
    g_fonts.clear();
    delete g_family;
    delete g_monoFamily;
    GdiplusShutdown(g.gdipToken);
    CloseHandle(g.stopEvent);
    CloseHandle(g.wakeEvent);
    return 0;
}
