// System Resource Monitor
// ---------------------------------------------------------------------------
// Native port of CPUFANSPEEDMONITOR.py (Kivy + psutil + wmi), so it runs
// without Python.
//
// Shows the same four readings as the original (CPU usage, CPU temperature
// from the WMI thermal zone, RAM usage, GPU usage + temperature) and keeps
// its "Start Monitoring" button, now a Start/Stop toggle. Monitoring starts
// automatically.
//
// Additions: a 2-minute sparkline per reading, CPU name, core count, RAM
// used/total, GPU fan speed, uptime, a CIMV2 fallback for CPU temperature that
// works without admin rights on many PCs, and CSV logging.
// ---------------------------------------------------------------------------
#include "common/AppCommon.h"

#include <wbemidl.h>

static const AppIdentity APP_ID = {L"CoolCupMan.Afterburner.SystemResourceMonitor", L"System Resource Monitor",
                                   L"Afterburner.SystemResourceMonitor.Wnd", L"SystemResourceMonitor"};
static const DWORD ACCENT = pal::purple;
static const size_t SPARK = 120;

enum { BTN_START = 1, BTN_LOG, BTN_FOLDER };

struct Metrics {
    double cpu = NAN, cpuTemp = NAN, ram = NAN, ramUsedGb = NAN, ramTotalGb = NAN;
    double gpuUtil = NAN, gpuTemp = NAN, gpuFan = NAN;
    std::wstring tempSource, gpuName, gpuErr;
    SYSTEMTIME st{};
};

struct State {
    HWND hwnd = nullptr;
    AppPaths paths;
    Poller<Metrics> poller;
    bool demo = false;
    Metrics cur;
    bool hasData = false;
    std::deque<double> hCpu, hTemp, hRam, hGpu;
    std::wstring cpuName;
    int cores = 0;
    std::vector<UiButton> buttons;
    FILE* log = nullptr;
    std::wstring toast;
    ULONGLONG toastUntil = 0;
    std::wstring snapshot;
    size_t snapshotAfter = 20, total = 0;
};
static State st;

// ---------------------------------------------------------------------------
// Data sources (all run on the poller thread, COM MTA)
// ---------------------------------------------------------------------------
static ULONGLONG FtToU(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// psutil.cpu_percent equivalent from GetSystemTimes deltas
static double CpuPercent() {
    static ULONGLONG lastIdle = 0, lastTotal = 0;
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return NAN;
    ULONGLONG i = FtToU(idle), t = FtToU(kernel) + FtToU(user);  // kernel includes idle
    double r = NAN;
    if (lastTotal && t > lastTotal) r = 100.0 * (1.0 - (double)(i - lastIdle) / (double)(t - lastTotal));
    lastIdle = i;
    lastTotal = t;
    return std::isnan(r) ? r : Clampd(r, 0, 100);
}

struct Wmi {
    IWbemServices* svc = nullptr;
    bool tried = false;
    void Connect(const wchar_t* ns) {
        tried = true;
        IWbemLocator* loc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc)))
            return;
        BSTR b = SysAllocString(ns);
        if (SUCCEEDED(loc->ConnectServer(b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)))
            CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        SysFreeString(b);
        loc->Release();
    }
    // First positive value of `prop` returned by the WQL query (converted to double)
    double First(const wchar_t* wql, const wchar_t* prop) {
        if (!svc) return NAN;
        IEnumWbemClassObject* en = nullptr;
        BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(wql);
        HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(q);
        if (FAILED(hr) || !en) return NAN;
        double result = NAN;
        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            if (en->Next(3000, 1, &obj, &got) != WBEM_S_NO_ERROR || !got) break;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(prop, 0, &v, nullptr, nullptr)) &&
                SUCCEEDED(VariantChangeType(&v, &v, 0, VT_R8)) && v.dblVal > 0)
                result = v.dblVal;
            VariantClear(&v);
            obj->Release();
            if (!std::isnan(result)) break;
        }
        en->Release();
        return result;
    }
    ~Wmi() { if (svc) svc->Release(); }
};

// MSAcpi_ThermalZoneTemperature (root\wmi, tenths of Kelvin) like the .py,
// then the perf-counter thermal zone (root\cimv2, Kelvin) which usually
// works without administrator rights.
static double CpuTemp(std::wstring& source) {
    static Wmi acpi, perf;
    static bool secInit = false;
    if (!secInit) {
        secInit = true;
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE, nullptr);
    }
    if (!acpi.tried) acpi.Connect(L"ROOT\\WMI");
    double t = acpi.First(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature", L"CurrentTemperature");
    if (!std::isnan(t) && t / 10.0 - 273.15 > 0) {
        source = L"ACPI thermal zone";
        return t / 10.0 - 273.15;
    }
    if (!perf.tried) perf.Connect(L"ROOT\\CIMV2");
    t = perf.First(L"SELECT Temperature FROM Win32_PerfFormattedData_Counters_ThermalZoneInformation", L"Temperature");
    if (!std::isnan(t) && t - 273.15 > 0) {
        source = L"thermal zone counter";
        return t - 273.15;
    }
    source = L"not exposed by this PC (try Run as administrator)";
    return NAN;
}

static Metrics* Poll() {
    static DemoGpu sim;
    static double demoCpu = 20;
    Metrics* m = new Metrics;
    GetLocalTime(&m->st);
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        m->ram = ms.dwMemoryLoad;
        m->ramTotalGb = ms.ullTotalPhys / 1073741824.0;
        m->ramUsedGb = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
    }
    if (st.demo) {
        sim.Step();
        demoCpu = Clampd(demoCpu + (sim.Rnd() - 0.5) * 18 + (sim.load * 70 - demoCpu) * 0.2, 2, 100);
        m->cpu = std::round(demoCpu * 10) / 10;
        m->cpuTemp = 38 + demoCpu * 0.45 + (sim.Rnd() - 0.5) * 2;
        m->tempSource = L"simulated";
        m->gpuUtil = std::round(sim.load * 100);
        m->gpuTemp = std::round(sim.temp);
        m->gpuFan = std::round(sim.fan);
        m->gpuName = L"Simulated GPU";
        return m;
    }
    m->cpu = CpuPercent();
    m->cpuTemp = CpuTemp(m->tempSource);
    std::vector<std::vector<std::wstring>> rows;
    if (NvSmi(L"utilization.gpu,temperature.gpu,fan.speed,name", rows, m->gpuErr) && rows[0].size() >= 4) {
        m->gpuUtil = ParseNum(rows[0][0]);
        m->gpuTemp = ParseNum(rows[0][1]);
        m->gpuFan = ParseNum(rows[0][2]);
        m->gpuName = rows[0][3];
    }
    return m;
}

static std::wstring CpuName() {
    wchar_t buf[256] = L"";
    DWORD sz = sizeof(buf);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                 RRF_RT_REG_SZ, nullptr, buf, &sz);
    std::wstring s = Trim(buf);
    return s.empty() ? L"CPU" : s;
}

// ---------------------------------------------------------------------------
static void Toast(const std::wstring& t) {
    st.toast = t;
    st.toastUntil = GetTickCount64() + 3000;
    InvalidateRect(st.hwnd, nullptr, FALSE);
}

static void DrawTile(Graphics& gr, const RectF& r, const wchar_t* label, const std::wstring& value, Color vc,
                     const std::wstring& sub, const std::deque<double>& hist, DWORD color, double lo, double hi) {
    Card(gr, r);
    TextAt(gr, label, r.X + 18 * S, r.Y + 14 * S, 11, Col(pal::dim), FontStyleBold);
    TextAt(gr, value, r.X + 16 * S, r.Y + 30 * S, 34, vc, FontStyleBold);
    TextIn(gr, sub, RectF(r.X + 18 * S, r.Y + 78 * S, r.Width - 36 * S, 18 * S), 11, Col(pal::dim), FontStyleRegular, false);
    RectF sp(r.X + 18 * S, r.Y + 104 * S, r.Width - 36 * S, r.Height - 120 * S);
    if (sp.Height > 20 * S) DrawSpark(gr, sp, hist, SPARK, color, lo, hi);
}

static void Render(Graphics& gr, int Wi, int Hi) {
    float W = (float)Wi, H = (float)Hi;
    gr.Clear(Col(pal::bg));
    bool running = !st.poller.paused;
    std::wstring pill = !running ? L"STOPPED" : st.demo ? L"DEMO" : L"MONITORING";
    DWORD pc = !running ? pal::amber : st.demo ? pal::purple : pal::green;
    float hh = DrawAppHeader(gr, W, APP_ID.name, pill, pc, ACCENT);
    float pad = 12 * S;

    // system line
    wchar_t b[256];
    ULONGLONG up = GetTickCount64() / 1000;
    swprintf(b, 256, L"%ls  ·  %d logical cores  ·  uptime %llud %02lluh %02llum", st.cpuName.c_str(), st.cores,
             up / 86400, (up / 3600) % 24, (up / 60) % 60);
    TextAt(gr, b, pad + 4 * S, hh + 10 * S, 12, Col(pal::text, 210));

    // buttons
    float btnH = 38 * S, by = H - pad - btnH;
    st.buttons[0].text = running ? L"Stop Monitoring" : L"Start Monitoring";
    st.buttons[0].accent = running ? pal::amber : pal::green;
    st.buttons[1].text = st.log ? L"Stop CSV log" : L"Start CSV log";
    float bx = pad;
    float widths[] = {170 * S, 140 * S, 140 * S};
    for (size_t i = 0; i < st.buttons.size(); i++) {
        st.buttons[i].r = RectF(bx, by, widths[i], btnH);
        DrawButton(gr, st.buttons[i]);
        bx += widths[i] + 10 * S;
    }
    TextAt(gr, L"Space start/stop · L log · D demo · O data folder", W - pad, by + 12 * S, 11, Col(pal::dim), FontStyleRegular, 2);

    // 2x2 tiles
    float top = hh + 36 * S, gh = by - pad - top;
    float tw = (W - 3 * pad) / 2, th = (gh - pad) / 2;
    const Metrics& m = st.cur;
    std::wstring sub;

    DrawTile(gr, RectF(pad, top, tw, th), L"CPU USAGE", Fmt(m.cpu, 1, L" %"), Col(pal::cyan),
             L"all cores, like psutil.cpu_percent()", st.hCpu, pal::cyan, 0, 100);

    sub = m.tempSource.empty() ? L"" : L"source: " + m.tempSource;
    DrawTile(gr, RectF(2 * pad + tw, top, tw, th), L"CPU TEMP",
             std::isnan(m.cpuTemp) ? L"N/A" : Fmt(m.cpuTemp, 1, L" °C"), TempColor(m.cpuTemp), sub, st.hTemp,
             pal::orange, 20, 100);

    swprintf(b, 256, L"%.1f of %.1f GiB in use", m.ramUsedGb, m.ramTotalGb);
    DrawTile(gr, RectF(pad, top + th + pad, tw, th), L"RAM USAGE", Fmt(m.ram, 0, L" %"), Col(pal::green),
             std::isnan(m.ramUsedGb) ? L"" : b, st.hRam, pal::green, 0, 100);

    if (std::isnan(m.gpuUtil) && st.hasData) sub = m.gpuErr.empty() ? L"no NVIDIA GPU found" : m.gpuErr;
    else {
        swprintf(b, 256, L"%ls  ·  %ls  ·  fan %ls", m.gpuName.c_str(), Fmt(m.gpuTemp, 0, L" °C").c_str(),
                 Fmt(m.gpuFan, 0, L" %").c_str());
        sub = b;
    }
    std::wstring gv = std::isnan(m.gpuUtil) ? L"N/A" : Fmt(m.gpuUtil, 0, L" %") + L"  ·  " + Fmt(m.gpuTemp, 0, L"°C");
    DrawTile(gr, RectF(2 * pad + tw, top + th + pad, tw, th), L"GPU USAGE · TEMP", gv, Col(pal::purple), sub,
             st.hGpu, pal::purple, 0, 100);

    DrawToast(gr, W, by - 16 * S, st.toast, st.toastUntil);
}

// ---------------------------------------------------------------------------
static void ToggleLog() {
    if (st.log) {
        fclose(st.log);
        st.log = nullptr;
        Toast(L"CSV log stopped");
        return;
    }
    std::wstring p = st.paths.dataDir + L"system_log_" + Stamp() + L".csv";
    st.log = _wfopen(p.c_str(), L"wb");
    if (!st.log) { Toast(L"Could not create log file"); return; }
    fputs("time,cpu_pct,cpu_temp_c,ram_pct,ram_used_gib,gpu_util_pct,gpu_temp_c,gpu_fan_pct\n", st.log);
    Toast(L"Logging to " + p.substr(p.find_last_of(L'\\') + 1));
}

static void Push(std::deque<double>& d, double v) {
    d.push_back(v);
    if (d.size() > SPARK) d.pop_front();
}

static void OnButton(int id) {
    switch (id) {
    case BTN_START:
        st.poller.paused = !st.poller.paused;
        if (!st.poller.paused) st.poller.Wake();
        break;
    case BTN_LOG: ToggleLog(); break;
    case BTN_FOLDER: OpenFolder(st.paths.dataDir); break;
    }
    InvalidateRect(st.hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    case WM_APP + 10: {
        Metrics* m = (Metrics*)lp;
        if (!st.poller.paused) {
            st.cur = *m;
            st.hasData = true;
            st.total++;
            Push(st.hCpu, m->cpu);
            Push(st.hTemp, m->cpuTemp);
            Push(st.hRam, m->ram);
            Push(st.hGpu, m->gpuUtil);
            if (st.log) {
                auto n = [](double v) { return std::isnan(v) ? std::string() : std::to_string(v); };
                fprintf(st.log, "%04d-%02d-%02d %02d:%02d:%02d,%s,%s,%s,%s,%s,%s,%s\n", m->st.wYear, m->st.wMonth,
                        m->st.wDay, m->st.wHour, m->st.wMinute, m->st.wSecond, n(m->cpu).c_str(),
                        n(m->cpuTemp).c_str(), n(m->ram).c_str(), n(m->ramUsedGb).c_str(), n(m->gpuUtil).c_str(),
                        n(m->gpuTemp).c_str(), n(m->gpuFan).c_str());
                fflush(st.log);
            }
            wchar_t t[128];
            swprintf(t, 128, L"CPU %ls · RAM %ls — %ls", Fmt(m->cpu, 0, L"%").c_str(), Fmt(m->ram, 0, L"%").c_str(),
                     APP_ID.name);
            SetWindowTextW(hwnd, t);
        }
        delete m;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (!st.snapshot.empty() && st.total >= st.snapshotAfter) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            RenderToPng(st.snapshot, rc.right, rc.bottom, Render);
            st.snapshot.clear();
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        bool changed = false;
        HitButton(st.buttons, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), true, &changed);
        if (changed) InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        for (auto& b : st.buttons) b.hot = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (int id = HitButton(st.buttons, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), false)) OnButton(id);
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE: OnButton(BTN_START); break;
        case 'L': OnButton(BTN_LOG); break;
        case 'O': OnButton(BTN_FOLDER); break;
        case 'D':
            st.demo = !st.demo;
            st.hCpu.clear(); st.hTemp.clear(); st.hRam.clear(); st.hGpu.clear();
            Toast(st.demo ? L"Demo mode" : L"Live mode");
            st.poller.Wake();
            break;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintBuffered(hwnd, Render);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {(LONG)(640 * S), (LONG)(520 * S)};
        return 0;
    case WM_CLOSE:
        SaveWindowPlacement(hwnd, st.paths.iniPath);
        st.poller.Stop();
        if (st.log) fclose(st.log);
        st.log = nullptr;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CommonArgs a = ParseCommonArgs();
    if (!a.shortcut.empty()) return SetShortcutAppId(a.shortcut, APP_ID.appId) ? 0 : 1;
    if (!InitAppIdentity(APP_ID, st.paths, a.multi)) return 0;
    UiStartup();
    st.demo = a.demo;
    st.snapshot = a.snapshot;
    st.snapshotAfter = a.snapshot.empty() ? 20 : a.samples;
    st.cpuName = CpuName();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    st.cores = (int)si.dwNumberOfProcessors;
    st.poller.intervalMs = a.intervalMs ? a.intervalMs : 1000;  // 1 s like the original
    st.buttons = {{BTN_START, L"Stop Monitoring", pal::amber},
                  {BTN_LOG, L"Start CSV log", pal::cyan},
                  {BTN_FOLDER, L"Data folder", pal::dim}};

    HWND hwnd = CreateAppWindow(APP_ID, hInst, WndProc, st.paths, 980, 700);
    if (!hwnd) return 1;
    st.hwnd = hwnd;
    ShowWindow(hwnd, (IniGetInt(st.paths.iniPath, L"Maximized", 0) && st.snapshot.empty()) ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(hwnd);
    st.poller.hwnd = hwnd;
    st.poller.fn = Poll;
    st.poller.Start();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UiShutdown();
    return 0;
}
