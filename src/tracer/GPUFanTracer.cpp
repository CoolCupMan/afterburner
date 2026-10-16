// GPU Fan Tracer / GPU Fan Tracer Plus
// ---------------------------------------------------------------------------
// Native successor of GPUFANHEATDIAGRAMMTRACER.cpp (classic) and
// GPUFANHEATDIAGRAMMTRACERMORE2.cpp (built with -DTRACER_PLUS).
//
// Classic: fan % vs temperature trace of the last 100 readings + green "now" ball.
// Plus:    adds red dots for every historic reading, 0-based ticks and an
//          in-app reading log (replaces the std::cout debug output, which a
//          GUI app never shows).
//
// Fixes over the originals: whole client rect is painted (not rcPaint),
// double buffering, nvidia-smi runs hidden on a worker thread, correct
// Unicode label lengths.
// ---------------------------------------------------------------------------
#include "common/AppCommon.h"

#ifdef TRACER_PLUS
static const AppIdentity APP_ID = {L"CoolCupMan.Afterburner.GPUFanTracerPlus", L"GPU Fan Tracer Plus",
                                   L"Afterburner.GPUFanTracerPlus.Wnd", L"GPUFanTracerPlus"};
static const DWORD ACCENT = pal::red;
static const int TICK_START = 0;
#else
static const AppIdentity APP_ID = {L"CoolCupMan.Afterburner.GPUFanTracer", L"GPU Fan Tracer",
                                   L"Afterburner.GPUFanTracer.Wnd", L"GPUFanTracer"};
static const DWORD ACCENT = pal::cyan;
static const int TICK_START = 10;
#endif

static const size_t MAX_POINTS = 100;  // as in the originals
static const int LOG_LINES = 200;
static const int INTERVALS[] = {1000, 2000, 4000, 8000};

struct Reading {
    double temp = NAN, fan = NAN;
    SYSTEMTIME st{};
    std::wstring err;
};

struct State {
    HWND hwnd = nullptr;
    AppPaths paths;
    Poller<Reading> poller;
    bool demo = false;
    std::deque<Reading> points;
    std::deque<Reading> log;  // Plus: reading log
    std::wstring status = L"Waiting for first reading…";
    bool error = false;
    std::wstring toast;
    ULONGLONG toastUntil = 0;
    std::wstring snapshot;
    size_t snapshotAfter = 40, total = 0;
};
static State st;

static void Toast(const std::wstring& t) {
    st.toast = t;
    st.toastUntil = GetTickCount64() + 3000;
    InvalidateRect(st.hwnd, nullptr, FALSE);
}

static Reading* Poll() {
    static DemoGpu sim;
    Reading* r = new Reading;
    GetLocalTime(&r->st);
    if (st.demo) {
        sim.Step();
        r->temp = std::round(sim.temp);
        r->fan = std::round(sim.fan);
        return r;
    }
    std::vector<std::vector<std::wstring>> rows;
    if (NvSmi(L"temperature.gpu,fan.speed", rows, r->err) && rows[0].size() >= 2) {
        r->temp = ParseNum(rows[0][0]);
        r->fan = ParseNum(rows[0][1]);
        if (std::isnan(r->temp)) r->err = L"nvidia-smi returned no temperature";
    } else if (r->err.empty()) {
        r->err = L"Unexpected nvidia-smi output";
    }
    return r;
}

// ---------------------------------------------------------------------------
static void Render(Graphics& gr, int Wi, int Hi) {
    float W = (float)Wi, H = (float)Hi;
    gr.Clear(Col(pal::bg));
    std::wstring pill = st.poller.paused ? L"PAUSED" : st.demo ? L"DEMO" : st.error ? L"NO SIGNAL" : L"LIVE";
    DWORD pc = st.poller.paused ? pal::amber : st.demo ? pal::purple : st.error ? pal::red : pal::green;
    float hh = DrawAppHeader(gr, W, APP_ID.name, pill, pc, ACCENT);
    float pad = 12 * S, footer = 26 * S;

#ifdef TRACER_PLUS
    float logW = W > 820 * S ? 260 * S : 0;
#else
    float logW = 0;
#endif
    RectF card(pad, hh + pad, W - 2 * pad - (logW ? logW + pad : 0), H - hh - footer - 1.5f * pad);
    Card(gr, card);

    // plot area (margins mirror the originals' MARGIN = 50, scaled)
    float L = card.X + 56 * S, R = card.X + card.Width - 24 * S, T = card.Y + 60 * S, B = card.Y + card.Height - 50 * S;
    TextAt(gr, L"Fan %  vs  Temperature (°C)", card.X + 16 * S, card.Y + 11 * S, 13, Col(pal::text), FontStyleBold);
    wchar_t b[128];
    swprintf(b, 128, L"last %zu of %zu readings", st.points.size(), MAX_POINTS);
    TextAt(gr, b, card.X + card.Width - 16 * S, card.Y + 13 * S, 11, Col(pal::dim), FontStyleRegular, 2);
    if (R - L > 40 * S && B - T > 40 * S) {
        auto X = [&](double t) { return (float)(L + Clampd(t, 0, 100) / 100 * (R - L)); };
        auto Y = [&](double f) { return (float)(B - Clampd(f, 0, 100) / 100 * (B - T)); };
        Pen gp(Col(pal::grid), 1), tp(Col(pal::axis), 1.5f * S);
        for (int v = TICK_START; v <= 100; v += 10) {
            gr.DrawLine(&gp, X(v), T, X(v), B);
            gr.DrawLine(&gp, L, Y(v), R, Y(v));
            gr.DrawLine(&tp, L - 5 * S, Y(v), L + 5 * S, Y(v));
            gr.DrawLine(&tp, X(v), B - 5 * S, X(v), B + 5 * S);
            swprintf(b, 128, L"%d", v);
            RectF m = Measure(gr, b, 11);
            TextAt(gr, b, L - 9 * S, Y(v) - m.Height / 2, 11, Col(pal::dim), FontStyleRegular, 2);
            TextAt(gr, b, X(v), B + 8 * S, 11, Col(pal::dim), FontStyleRegular, 1);
        }
        gr.DrawLine(&tp, L, T, L, B);
        gr.DrawLine(&tp, L, B, R, B);
        TextAt(gr, L"Fan %", L, T - 24 * S, 11, Col(pal::text, 200), FontStyleBold, 1);
        TextAt(gr, L"Temperature (°C)", (L + R) / 2, B + 28 * S, 12, Col(pal::text, 200), FontStyleRegular, 1);

        if (st.points.empty()) {
            TextIn(gr, st.status, RectF(L + 20 * S, (T + B) / 2 - 20 * S, R - L - 40 * S, 60 * S), 13, Col(pal::dim));
        } else {
            // trace (cyan, as in the originals)
            Pen lp(Col(pal::cyan), 2 * S);
            lp.SetLineJoin(LineJoinRound);
            std::vector<PointF> pts;
            for (auto& p : st.points) pts.push_back(PointF(X(p.temp), Y(p.fan)));
            if (pts.size() > 1) gr.DrawLines(&lp, pts.data(), (INT)pts.size());
#ifdef TRACER_PLUS
            // red dots for all historic points
            SolidBrush rb(Col(pal::red, 220));
            for (auto& p : pts) gr.FillEllipse(&rb, p.X - 3 * S, p.Y - 3 * S, 6 * S, 6 * S);
#endif
            // green "now" ball
            const Reading& c = st.points.back();
            float cx = X(c.temp), cy = Y(c.fan), rr = 6 * S;
            SolidBrush glow(Col(pal::green, 50));
            gr.FillEllipse(&glow, cx - rr * 2.2f, cy - rr * 2.2f, rr * 4.4f, rr * 4.4f);
            SolidBrush gb(Col(pal::green));
            gr.FillEllipse(&gb, cx - rr, cy - rr, rr * 2, rr * 2);
            swprintf(b, 128, L"%.0f°C · %.0f%%", c.temp, c.fan);
            RectF m = Measure(gr, b, 12, FontStyleBold);
            float bx = cx + 14 * S, by = cy - m.Height - 16 * S;
            if (bx + m.Width + 14 * S > R) bx = cx - 14 * S - m.Width - 14 * S;
            if (by < T) by = cy + 12 * S;
            FillRound(gr, RectF(bx, by, m.Width + 14 * S, m.Height + 6 * S), 5 * S, Col(pal::bg, 235), Col(pal::green, 200));
            TextAt(gr, b, bx + 7 * S, by + 3 * S, 12, Col(pal::text), FontStyleBold);
        }
    }

#ifdef TRACER_PLUS
    if (logW) {
        RectF lc(W - pad - logW, hh + pad, logW, card.Height);
        Card(gr, lc);
        TextAt(gr, L"Reading log", lc.X + 16 * S, lc.Y + 11 * S, 13, Col(pal::text), FontStyleBold);
        float y = lc.Y + 40 * S;
        gr.SetClip(lc);
        for (auto it = st.log.rbegin(); it != st.log.rend() && y < lc.Y + lc.Height - 18 * S; ++it) {
            const Reading& r = *it;
            swprintf(b, 128, L"%02d:%02d:%02d", r.st.wHour, r.st.wMinute, r.st.wSecond);
            TextAt(gr, b, lc.X + 16 * S, y, 11, Col(pal::dim), FontStyleRegular, 0, true);
            if (r.err.empty()) {
                swprintf(b, 128, L"GPU %.0f °C  Fan %.0f %%", r.temp, r.fan);
                TextAt(gr, b, lc.X + 82 * S, y, 11, TempColor(r.temp), FontStyleRegular, 0, true);
            } else {
                TextAt(gr, L"no reading", lc.X + 82 * S, y, 11, Col(pal::red), FontStyleRegular, 0, true);
            }
            y += 18 * S;
        }
        gr.ResetClip();
    }
#endif

    swprintf(b, 128, L"every %g s  ·  Space pause · +/- interval · C clear · P screenshot · D demo · O data folder",
             st.poller.intervalMs / 1000.0);
    TextAt(gr, b, W / 2, H - footer + 4 * S, 11, Col(pal::dim, 200), FontStyleRegular, 1);
    DrawToast(gr, W, H - footer - 12 * S, st.toast, st.toastUntil);
}

// ---------------------------------------------------------------------------
static void Screenshot() {
    RECT rc;
    GetClientRect(st.hwnd, &rc);
    std::wstring p = st.paths.dataDir + APP_ID.folder + L"_" + Stamp() + L".png";
    Toast(RenderToPng(p, rc.right, rc.bottom, Render) ? L"Saved " + p.substr(p.find_last_of(L'\\') + 1)
                                                       : L"Could not save screenshot");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    case WM_APP + 10: {
        Reading* r = (Reading*)lp;
        if (r->err.empty()) {
            st.points.push_back(*r);
            if (st.points.size() > MAX_POINTS) st.points.pop_front();
            st.error = false;
            st.total++;
            wchar_t t[96];
            swprintf(t, 96, L"%.0f°C · Fan %.0f%% — %ls", r->temp, r->fan, APP_ID.name);
            SetWindowTextW(hwnd, t);
        } else {
            st.error = true;
            st.status = r->err;
        }
        st.log.push_back(*r);
        if (st.log.size() > (size_t)LOG_LINES) st.log.pop_front();
        delete r;
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
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintBuffered(hwnd, Render);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {(LONG)(520 * S), (LONG)(400 * S)};
        return 0;
    case WM_KEYDOWN: {
        const int n = sizeof(INTERVALS) / sizeof(INTERVALS[0]);
        int cur = 0;
        for (int i = 0; i < n; i++) if (INTERVALS[i] <= st.poller.intervalMs) cur = i;
        switch (wp) {
        case VK_SPACE:
            st.poller.paused = !st.poller.paused;
            Toast(st.poller.paused ? L"Paused" : L"Resumed");
            st.poller.Wake();
            break;
        case VK_ADD: case VK_OEM_PLUS: case VK_SUBTRACT: case VK_OEM_MINUS: {
            bool faster = wp == VK_ADD || wp == VK_OEM_PLUS;
            cur += faster ? -1 : 1;
            cur = cur < 0 ? 0 : cur >= n ? n - 1 : cur;
            st.poller.intervalMs = INTERVALS[cur];
            wchar_t b[48];
            swprintf(b, 48, L"Reading every %d s", INTERVALS[cur] / 1000);
            Toast(b);
            break;
        }
        case 'C': st.points.clear(); st.log.clear(); Toast(L"Cleared"); break;
        case 'P': Screenshot(); break;
        case 'O': OpenFolder(st.paths.dataDir); break;
        case 'D':
            st.demo = !st.demo;
            st.points.clear();
            Toast(st.demo ? L"Demo mode" : L"Live mode");
            st.poller.Wake();
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        IniSetInt(st.paths.iniPath, L"IntervalMs", st.poller.intervalMs);
        SaveWindowPlacement(hwnd, st.paths.iniPath);
        st.poller.Stop();
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
    st.snapshotAfter = a.samples;
    st.poller.intervalMs = a.intervalMs ? a.intervalMs : IniGetInt(st.paths.iniPath, L"IntervalMs", 4000);

#ifdef TRACER_PLUS
    HWND hwnd = CreateAppWindow(APP_ID, hInst, WndProc, st.paths, 1180, 680);
#else
    HWND hwnd = CreateAppWindow(APP_ID, hInst, WndProc, st.paths, 900, 600);
#endif
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
