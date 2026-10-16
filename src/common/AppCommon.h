// AppCommon.h - shared by every app of the Afterburner suite.
//
// Each app passes its own AppIdentity. The unique AppUserModelID, window class,
// single-instance mutex and data folder let all apps install and run side by side
// without sharing taskbar groups, settings or log files.
#pragma once
#pragma GCC diagnostic ignored "-Wunused-function"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// App identity (unique per app -> parallel installation)
// ---------------------------------------------------------------------------
struct AppIdentity {
    const wchar_t* appId;        // AppUserModelID, also the mutex name
    const wchar_t* name;         // display name
    const wchar_t* windowClass;  // unique window class
    const wchar_t* folder;       // data sub-folder below %LOCALAPPDATA%\Afterburner
};

struct AppPaths {
    std::wstring exeDir;   // folder of the .exe
    std::wstring dataDir;  // where settings, logs and screenshots go (ends with '\')
    std::wstring iniPath;
    bool portable = false;
};

static HANDLE g_instanceMutex = nullptr;

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring d = path;
    return d.substr(0, d.find_last_of(L"\\/") + 1);
}

static bool DirWritable(const std::wstring& dir) {
    std::wstring probe = dir + L".afterburner_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Sets the AppUserModelID, enforces one instance per app (unless allowMulti),
// and resolves the data folder: next to the exe when writable (portable use),
// otherwise %LOCALAPPDATA%\Afterburner\<folder>\ (installed under Program Files).
// Returns false when another instance of the same app is running; that window
// is brought to the front.
static bool InitAppIdentity(const AppIdentity& id, AppPaths& paths, bool allowMulti) {
    typedef HRESULT(WINAPI * SetAumid)(PCWSTR);
    if (HMODULE sh = GetModuleHandleW(L"shell32.dll"))
        if (auto fn = (SetAumid)(void*)GetProcAddress(sh, "SetCurrentProcessExplicitAppUserModelID")) fn(id.appId);

    std::wstring mutexName = std::wstring(L"Local\\") + id.appId;
    g_instanceMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!allowMulti && g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(id.windowClass, nullptr)) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return false;
    }

    paths.exeDir = ExeDir();
    if (GetFileAttributesW((paths.exeDir + L"portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES ||
        (GetFileAttributesW((paths.exeDir + L"installed.txt").c_str()) == INVALID_FILE_ATTRIBUTES &&
         DirWritable(paths.exeDir))) {
        paths.dataDir = paths.exeDir;
        paths.portable = true;
    } else {
        wchar_t base[MAX_PATH] = L"";
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, base)))
            GetTempPathW(MAX_PATH, base);
        paths.dataDir = std::wstring(base) + L"\\Afterburner\\" + id.folder + L"\\";
        SHCreateDirectoryExW(nullptr, paths.dataDir.c_str(), nullptr);
    }
    paths.iniPath = paths.dataDir + id.folder + L".ini";
    return true;
}

static void OpenFolder(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Dark title bar on Windows 10 1809+ / 11 (no-op elsewhere)
static void DarkTitleBar(HWND hwnd) {
    typedef HRESULT(WINAPI * DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto fn = (DwmSetAttr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE;
            if (FAILED(fn(hwnd, 20, &dark, sizeof(dark)))) fn(hwnd, 19, &dark, sizeof(dark));
        }
    }
}

static int IniGetInt(const std::wstring& ini, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(L"Settings", key, def, ini.c_str());
}
static void IniSetInt(const std::wstring& ini, const wchar_t* key, int v) {
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"Settings", key, b, ini.c_str());
}

static void SaveWindowPlacement(HWND hwnd, const std::wstring& ini) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!hwnd || !GetWindowPlacement(hwnd, &wp)) return;
    IniSetInt(ini, L"X", wp.rcNormalPosition.left);
    IniSetInt(ini, L"Y", wp.rcNormalPosition.top);
    IniSetInt(ini, L"W", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    IniSetInt(ini, L"H", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
    IniSetInt(ini, L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED);
}

// Restores x/y/w/h from the ini; falls back to defaults if off-screen.
static void LoadWindowRect(const std::wstring& ini, int defW, int defH, int& x, int& y, int& w, int& h) {
    w = IniGetInt(ini, L"W", defW);
    h = IniGetInt(ini, L"H", defH);
    x = IniGetInt(ini, L"X", CW_USEDEFAULT);
    y = IniGetInt(ini, L"Y", CW_USEDEFAULT);
    RECT wr{x, y, x + w, y + h};
    if (x != CW_USEDEFAULT && !MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
}

// ---------------------------------------------------------------------------
// Palette, scale, helpers
// ---------------------------------------------------------------------------
namespace pal {
const DWORD bg = 0x0f1117, card = 0x171a23, cardBorder = 0x262b38,
            grid = 0x222735, axis = 0x5c667a, text = 0xe8ebf2, dim = 0x8b93a7,
            cyan = 0x22d3ee, green = 0x3dfc8b, red = 0xff4d5e, amber = 0xfbbf24,
            purple = 0xa78bfa, orange = 0xff7a45;
}

static float S = 1.f;  // DPI scale

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


// ---------------------------------------------------------------------------
// GDI+ / UI lifetime
// ---------------------------------------------------------------------------
static ULONG_PTR g_gdipToken = 0;

static void UiStartup() {
    SetProcessDPIAware();
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken, &gsi, nullptr);
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
}

static void UiShutdown() {
    for (auto& kv : g_fonts) delete kv.second;
    g_fonts.clear();
    delete g_family;
    delete g_monoFamily;
    g_family = g_monoFamily = nullptr;
    GdiplusShutdown(g_gdipToken);
    if (g_instanceMutex) CloseHandle(g_instanceMutex);
}

static HWND CreateAppWindow(const AppIdentity& id, HINSTANCE hInst, WNDPROC proc, const AppPaths& paths,
                            int defW, int defH) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = id.windowClass;
    RegisterClassExW(&wc);
    int x, y, w, h;
    LoadWindowRect(paths.iniPath, (int)(defW * S), (int)(defH * S), x, y, w, h);
    HWND hwnd = CreateWindowExW(0, id.windowClass, id.name, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                                hInst, nullptr);
    if (hwnd) DarkTitleBar(hwnd);
    return hwnd;
}

// Double-buffered paint of the whole client area through a GDI+ render callback.
template <class F>
static void PaintBuffered(HWND hwnd, F render) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W > 0 && H > 0) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bm = CreateCompatibleBitmap(hdc, W, H);
        HGDIOBJ old = SelectObject(mem, bm);
        {
            Graphics gr(mem);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            gr.SetPixelOffsetMode(PixelOffsetModeHalf);
            render(gr, W, H);
        }
        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
    }
    EndPaint(hwnd, &ps);
}

// Renders through the callback into a PNG of the given size.
template <class F>
static bool RenderToPng(const std::wstring& path, int W, int H, F render) {
    Bitmap bmp(W, H, PixelFormat32bppARGB);
    {
        Graphics gr(&bmp);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gr.SetPixelOffsetMode(PixelOffsetModeHalf);
        render(gr, W, H);
    }
    CLSID c;
    if (!GetEncoderClsid(L"image/png", &c)) return false;
    return bmp.Save(path.c_str(), &c, nullptr) == Ok;
}

// ---------------------------------------------------------------------------
// nvidia-smi helper for simple field lists (rows of trimmed CSV fields)
// ---------------------------------------------------------------------------
static bool NvSmi(const std::wstring& fields, std::vector<std::vector<std::wstring>>& rows, std::wstring& err) {
    static const wchar_t* exes[] = {
        L"nvidia-smi.exe",
        L"\"C:\\Windows\\System32\\nvidia-smi.exe\"",
        L"\"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe\"",
    };
    static int good = -1;
    bool launched = false;
    for (int i = 0; i < 3; i++) {
        int e = good >= 0 ? good : i;
        std::string raw;
        DWORD code = 1;
        std::wstring cmd = std::wstring(exes[e]) + L" --query-gpu=" + fields + L" --format=csv,noheader,nounits";
        if (!RunCapture(cmd, raw, code)) {
            if (good >= 0) break;
            continue;
        }
        launched = true;
        rows.clear();
        std::wstring text = Utf8ToWide(raw);
        size_t p = 0;
        while (p < text.size()) {
            size_t n = text.find(L'\n', p);
            std::wstring line = Trim(text.substr(p, n == std::wstring::npos ? std::wstring::npos : n - p));
            p = n == std::wstring::npos ? text.size() : n + 1;
            if (!line.empty()) rows.push_back(SplitCsv(line));
        }
        if (code == 0 && !rows.empty()) {
            good = e;
            return true;
        }
        err = Trim(text).substr(0, 160);
        break;
    }
    if (!launched) err = L"nvidia-smi not found (NVIDIA driver installed?). Press D for demo mode.";
    else if (err.empty()) err = L"nvidia-smi returned no data. Press D for demo mode.";
    return false;
}

// Simple simulated GPU for demo mode
struct DemoGpu {
    double temp = 36, fan = 30, t = 0, load = 0.3;
    unsigned seed = 4242;
    double Rnd() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32767.0; }
    void Step() {
        t += 3.0;
        load = 0.5 + 0.45 * std::sin(t / 60.0) + 0.15 * std::sin(t / 11.0);
        if (Rnd() > 0.93) load += 0.3;
        load = Clampd(load, 0.02, 1.0);
        temp += (33 + load * 52 - temp) * 0.12 + (Rnd() - 0.5) * 0.8;
        double ft = Clampd(temp < 45 ? 30 : 30 + (temp - 45) * 1.7, 30, 100);
        fan += (ft - fan) * 0.25;
    }
};

// ---------------------------------------------------------------------------
// Background poller: runs fn() every intervalMs on its own thread (COM MTA
// initialised) and posts the heap result to hwnd as LPARAM of msg.
// ---------------------------------------------------------------------------
#include <atomic>
#include <functional>

template <class R>
struct Poller {
    HWND hwnd = nullptr;
    UINT msg = WM_APP + 10;
    std::atomic<int> intervalMs{1000};
    std::atomic<bool> paused{false};
    std::function<R*()> fn;
    HANDLE thread = nullptr, stopEv = nullptr, wakeEv = nullptr;

    void Start() {
        stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wakeEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread = CreateThread(nullptr, 0, Proc, this, 0, nullptr);
    }
    void Wake() { if (wakeEv) SetEvent(wakeEv); }
    void Stop() {
        if (!thread) return;
        SetEvent(stopEv);
        WaitForSingleObject(thread, 7000);
        CloseHandle(thread);
        CloseHandle(stopEv);
        CloseHandle(wakeEv);
        thread = nullptr;
    }
    static DWORD WINAPI Proc(LPVOID p) {
        Poller* s = (Poller*)p;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HANDLE ev[2] = {s->stopEv, s->wakeEv};
        for (;;) {
            if (!s->paused) {
                R* r = s->fn();
                if (r && !PostMessageW(s->hwnd, s->msg, 0, (LPARAM)r)) delete r;
            }
            if (WaitForMultipleObjects(2, ev, FALSE, (DWORD)s->intervalMs.load()) == WAIT_OBJECT_0) break;
        }
        CoUninitialize();
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
struct UiButton {
    int id;
    std::wstring text;
    DWORD accent;
    RectF r;
    bool hot = false;
};

static void DrawButton(Graphics& gr, UiButton& b) {
    FillRound(gr, b.r, 7 * S, Col(b.accent, b.hot ? 70 : 34), Col(b.accent, b.hot ? 230 : 140), 1.2f * S);
    RectF m = Measure(gr, b.text, 12, FontStyleBold);
    TextAt(gr, b.text, b.r.X + b.r.Width / 2, b.r.Y + (b.r.Height - m.Height) / 2, 12, Col(pal::text), FontStyleBold, 1);
}

static int HitButton(std::vector<UiButton>& bs, float x, float y, bool setHot, bool* changed = nullptr) {
    int hit = 0;
    for (auto& b : bs) {
        bool in = b.r.Contains(x, y);
        if (setHot && in != b.hot) {
            b.hot = in;
            if (changed) *changed = true;
        }
        if (in) hit = b.id;
    }
    return hit;
}

// Filled sparkline of values in [lo, hi]; NaNs break the line.
static void DrawSpark(Graphics& gr, const RectF& r, const std::deque<double>& v, size_t slots, DWORD color,
                      double lo = 0, double hi = 100) {
    Pen gp(Col(pal::grid), 1);
    for (int i = 0; i <= 4; i++) gr.DrawLine(&gp, r.X, r.Y + r.Height * i / 4, r.X + r.Width, r.Y + r.Height * i / 4);
    if (v.size() < 2 || slots < 2) return;
    size_t n = v.size() < slots ? v.size() : slots, start = v.size() - n;
    float step = r.Width / (slots - 1);
    std::vector<PointF> run;
    LinearGradientBrush fb(PointF(0, r.Y), PointF(0, r.Y + r.Height + 1), Col(color, 90), Col(color, 0));
    Pen pen(Col(color), 2 * S);
    pen.SetLineJoin(LineJoinRound);
    auto flush = [&]() {
        if (run.size() > 1) {
            std::vector<PointF> poly = run;
            poly.push_back(PointF(run.back().X, r.Y + r.Height));
            poly.push_back(PointF(run.front().X, r.Y + r.Height));
            gr.FillPolygon(&fb, poly.data(), (INT)poly.size());
            gr.DrawLines(&pen, run.data(), (INT)run.size());
        }
        run.clear();
    };
    for (size_t i = 0; i < n; i++) {
        double x = v[start + i];
        if (std::isnan(x)) { flush(); continue; }
        float px = r.X + r.Width - (float)(n - 1 - i) * step;
        float py = (float)(r.Y + r.Height - Clampd((x - lo) / (hi - lo), 0, 1) * r.Height);
        run.push_back(PointF(px, py));
    }
    flush();
}

// Top bar: logo, title, status pill. Returns the bar height.
static float DrawAppHeader(Graphics& gr, float W, const std::wstring& title, const std::wstring& pill,
                           DWORD pillColor, DWORD accent) {
    float hh = 48 * S, pad = 14 * S, cy = hh / 2;
    LinearGradientBrush lg(PointF(pad, cy - 10 * S), PointF(pad + 20 * S, cy + 10 * S), Col(accent), Col(pal::orange));
    gr.FillEllipse(&lg, pad, cy - 10 * S, 20 * S, 20 * S);
    SolidBrush inner(Col(pal::bg));
    gr.FillEllipse(&inner, pad + 5 * S, cy - 5 * S, 10 * S, 10 * S);
    float x = pad + 30 * S;
    RectF tm = Measure(gr, title, 14, FontStyleBold);
    TextAt(gr, title, x, cy - tm.Height / 2, 14, Col(pal::text), FontStyleBold);
    x += tm.Width + 12 * S;
    if (!pill.empty()) Pill(gr, x, cy, pill, pillColor);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[16];
    swprintf(b, 16, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    RectF cm = Measure(gr, b, 13);
    TextAt(gr, b, W - pad, cy - cm.Height / 2, 13, Col(pal::dim), FontStyleRegular, 2);
    Pen sep(Col(pal::cardBorder), 1);
    gr.DrawLine(&sep, 0.f, hh - 0.5f, W, hh - 0.5f);
    return hh;
}

static void DrawToast(Graphics& gr, float W, float bottom, const std::wstring& toast, ULONGLONG until) {
    if (toast.empty() || GetTickCount64() >= until) return;
    RectF m = Measure(gr, toast, 12, FontStyleBold);
    float tw = m.Width + 28 * S, th = 32 * S;
    RectF tr((W - tw) / 2, bottom - th, tw, th);
    FillRound(gr, tr, th / 2, Col(0x1f2430, 245), Col(pal::cyan, 180), 1.5f * S);
    TextAt(gr, toast, W / 2, tr.Y + (th - m.Height) / 2, 12, Col(pal::text), FontStyleBold, 1);
}

// ---------------------------------------------------------------------------
// Common command line
//   --demo  --interval <ms>  --new-instance  --snapshot <png> [--samples N]
//   --set-shortcut-appid <file.lnk>   (used by the installer, then exits)
// ---------------------------------------------------------------------------
struct CommonArgs {
    bool demo = false, multi = false;
    int intervalMs = 0;
    std::wstring snapshot, shortcut;
    size_t samples = 40;
};

static CommonArgs ParseCommonArgs() {
    CommonArgs a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        std::wstring s = argv[i];
        bool next = i + 1 < argc;
        if (s == L"--demo") a.demo = true;
        else if (s == L"--new-instance") a.multi = true;
        else if (s == L"--interval" && next) a.intervalMs = _wtoi(argv[++i]);
        else if (s == L"--snapshot" && next) a.snapshot = argv[++i];
        else if (s == L"--samples" && next) a.samples = (size_t)_wtoi(argv[++i]);
        else if (s == L"--set-shortcut-appid" && next) a.shortcut = argv[++i];
    }
    if (argv) LocalFree(argv);
    if (!a.snapshot.empty()) a.multi = true;
    if (a.intervalMs && a.intervalMs < 50) a.intervalMs = 50;
    return a;
}

// Stamps the AppUserModelID into a .lnk so a pinned/Start-menu shortcut and the
// running window share one taskbar button (and never merge with sibling apps).
#include <propsys.h>
static bool SetShortcutAppId(const std::wstring& lnk, const wchar_t* appId) {
    static const PROPERTYKEY PKEY_AUMID = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READWRITE))) {
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(link->QueryInterface(__uuidof(IPropertyStore), (void**)&ps))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    size_t bytes = (wcslen(appId) + 1) * sizeof(wchar_t);
                    pv.vt = VT_LPWSTR;
                    pv.pwszVal = (LPWSTR)CoTaskMemAlloc(bytes);
                    if (pv.pwszVal) {
                        memcpy(pv.pwszVal, appId, bytes);
                        ok = SUCCEEDED(ps->SetValue(PKEY_AUMID, pv)) && SUCCEEDED(ps->Commit()) &&
                             SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
                    }
                    PropVariantClear(&pv);
                    ps->Release();
                }
            }
            pf->Release();
        }
        link->Release();
    }
    CoUninitialize();
    return ok;
}
