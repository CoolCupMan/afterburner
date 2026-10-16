// GPU Fan Speed Monitor
// ---------------------------------------------------------------------------
// Native port of GPUFANSPEEDMONITOR.py (Kivy "FanCurveObserver"), so it runs
// without Python, Kivy or numpy.
//
// Same idea as the original: an info line "GPU Temp | Fan Speed", the
// observed (temperature, fan) pairs plotted as a curve sorted by temperature
// with orange dots, a big red "current" dot, and a Close button. Readings are
// taken every second and pairs with temp or fan 0 are ignored, as before.
//
// Improvements: identical pairs are de-duplicated and counted (dot size grows
// with how often a pair was seen), the curve uses the average fan per °C so
// it no longer zig-zags, GPU load (queried but unused in the .py) is shown,
// plus Pause / Reset / Save buttons, hidden nvidia-smi on a worker thread
// and redraws only when data arrives instead of 30 times a second.
// ---------------------------------------------------------------------------
#include "common/AppCommon.h"

static const AppIdentity APP_ID = {L"CoolCupMan.Afterburner.GPUFanSpeedMonitor", L"GPU Fan Speed Monitor",
                                   L"Afterburner.GPUFanSpeedMonitor.Wnd", L"GPUFanSpeedMonitor"};
static const DWORD ACCENT = pal::orange;

enum { BTN_PAUSE = 1, BTN_RESET, BTN_SAVE, BTN_CLOSE };

struct Reading {
    double util = NAN, temp = NAN, fan = NAN;
    std::wstring err;
};

struct State {
    HWND hwnd = nullptr;
    AppPaths paths;
    Poller<Reading> poller;
    bool demo = false, error = false;
    Reading cur;
    std::map<std::pair<int, int>, int> observed;  // (temp, fan) -> count
    size_t total = 0;
    std::wstring status = L"Waiting for first reading…";
    std::vector<UiButton> buttons;
    std::wstring toast;
    ULONGLONG toastUntil = 0;
    std::wstring snapshot;
    size_t snapshotAfter = 40;
};
static State st;

static Reading* Poll() {
    static DemoGpu sim;
    Reading* r = new Reading;
    if (st.demo) {
        sim.Step();
        r->temp = std::round(sim.temp);
        r->fan = std::round(sim.fan);
        r->util = std::round(sim.load * 100);
        return r;
    }
    std::vector<std::vector<std::wstring>> rows;
    if (NvSmi(L"utilization.gpu,temperature.gpu,fan.speed", rows, r->err) && rows[0].size() >= 3) {
        r->util = ParseNum(rows[0][0]);
        r->temp = ParseNum(rows[0][1]);
        r->fan = ParseNum(rows[0][2]);
    } else if (r->err.empty()) {
        r->err = L"Unexpected nvidia-smi output";
    }
    return r;
}

static void Toast(const std::wstring& t) {
    st.toast = t;
    st.toastUntil = GetTickCount64() + 3000;
    InvalidateRect(st.hwnd, nullptr, FALSE);
}

static void Render(Graphics& gr, int Wi, int Hi) {
    float W = (float)Wi, H = (float)Hi;
    gr.Clear(Col(pal::bg));
    std::wstring pill = st.poller.paused ? L"PAUSED" : st.demo ? L"DEMO" : st.error ? L"NO SIGNAL" : L"LIVE";
    DWORD pc = st.poller.paused ? pal::amber : st.demo ? pal::purple : st.error ? pal::red : pal::green;
    float hh = DrawAppHeader(gr, W, APP_ID.name, pill, pc, ACCENT);
    float pad = 12 * S;

    // info line (the original's top label), as three tiles
    float infoH = 64 * S, tileW = (W - 4 * pad) / 3;
    struct Tile { const wchar_t* label; std::wstring value; Color c; } tiles[3] = {
        {L"GPU TEMP", std::isnan(st.cur.temp) ? L"--°C" : Fmt(st.cur.temp, 0, L"°C"), TempColor(st.cur.temp)},
        {L"FAN SPEED", std::isnan(st.cur.fan) ? L"--%" : Fmt(st.cur.fan, 0, L"%"), Col(pal::cyan)},
        {L"GPU LOAD", std::isnan(st.cur.util) ? L"--%" : Fmt(st.cur.util, 0, L"%"), Col(pal::purple)},
    };
    for (int i = 0; i < 3; i++) {
        RectF r(pad + i * (tileW + pad), hh + pad, tileW, infoH);
        Card(gr, r);
        TextAt(gr, tiles[i].label, r.X + 16 * S, r.Y + 10 * S, 10, Col(pal::dim), FontStyleBold);
        TextAt(gr, tiles[i].value, r.X + 16 * S, r.Y + 24 * S, 24, tiles[i].c, FontStyleBold);
    }

    // buttons row (bottom)
    float btnH = 36 * S, by = H - pad - btnH;
    float bw = 110 * S, bx = W - pad - bw;
    for (auto it = st.buttons.rbegin(); it != st.buttons.rend(); ++it) {
        it->r = RectF(bx, by, bw, btnH);
        if (it->id == BTN_PAUSE) it->text = st.poller.paused ? L"Resume" : L"Pause";
        DrawButton(gr, *it);
        bx -= bw + 10 * S;
    }
    wchar_t b[128];
    swprintf(b, 128, L"%zu readings · %zu unique points", st.total, st.observed.size());
    TextAt(gr, b, pad + 4 * S, by + 10 * S, 12, Col(pal::dim));

    // graph card
    RectF card(pad, hh + pad + infoH + pad, W - 2 * pad, by - (hh + 3 * pad + infoH));
    Card(gr, card);
    TextAt(gr, L"Observed fan curve", card.X + 16 * S, card.Y + 11 * S, 13, Col(pal::text), FontStyleBold);
    float L = card.X + 70 * S, R = card.X + card.Width - 24 * S, T = card.Y + 40 * S, B = card.Y + card.Height - 52 * S;
    if (R - L < 60 * S || B - T < 40 * S) return;
    auto X = [&](double t) { return (float)(L + Clampd(t, 0, 100) / 100 * (R - L)); };
    auto Y = [&](double f) { return (float)(B - Clampd(f, 0, 100) / 100 * (B - T)); };
    Pen gp(Col(pal::grid), 1), ap(Col(pal::text, 220), 1.5f * S);
    for (int v = 0; v <= 100; v += 10) {
        gr.DrawLine(&gp, X(v), T, X(v), B);
        gr.DrawLine(&gp, L, Y(v), R, Y(v));
        gr.DrawLine(&ap, L - 5 * S, Y(v), L, Y(v));
        gr.DrawLine(&ap, X(v), B, X(v), B + 5 * S);
        swprintf(b, 128, L"%d%%", v);
        RectF m = Measure(gr, b, 11);
        TextAt(gr, b, L - 10 * S, Y(v) - m.Height / 2, 11, Col(pal::dim), FontStyleRegular, 2);
        swprintf(b, 128, L"%d°C", v);
        TextAt(gr, b, X(v), B + 8 * S, 11, Col(pal::dim), FontStyleRegular, 1);
    }
    gr.DrawLine(&ap, L, T, L, B);
    gr.DrawLine(&ap, L, B, R, B);
    TextAt(gr, L"X-Axis: Temp °C", (L + R) / 2, B + 28 * S, 12, Col(pal::text, 200), FontStyleRegular, 1);
    gr.TranslateTransform(card.X + 16 * S, (T + B) / 2);
    gr.RotateTransform(-90);
    TextAt(gr, L"Y-Axis: Fan %", 0, 0, 12, Col(pal::text, 200), FontStyleRegular, 1);
    gr.ResetTransform();

    if (st.observed.empty() && std::isnan(st.cur.temp)) {
        TextIn(gr, st.status, RectF(L + 20 * S, (T + B) / 2 - 20 * S, R - L - 40 * S, 60 * S), 13, Col(pal::dim));
        return;
    }

    // curve through the average fan per temperature (sorted by temp)
    std::map<int, std::pair<double, int>> perTemp;
    int maxCount = 1;
    for (auto& kv : st.observed) {
        auto& pt = perTemp[kv.first.first];
        pt.first += (double)kv.first.second * kv.second;
        pt.second += kv.second;
        if (kv.second > maxCount) maxCount = kv.second;
    }
    std::vector<PointF> line;
    for (auto& kv : perTemp) line.push_back(PointF(X(kv.first), Y(kv.second.first / kv.second.second)));
    if (line.size() > 1) {
        Pen lp(Col(0x00cccc), 2 * S);  // the original's (0, 0.8, 0.8)
        lp.SetLineJoin(LineJoinRound);
        gr.DrawLines(&lp, line.data(), (INT)line.size());
    }
    // orange dots, size by frequency
    for (auto& kv : st.observed) {
        float rr = (3.f + 4.f * (float)std::sqrt((double)kv.second / maxCount)) * S;
        SolidBrush ob(Color(230, 255, 128, 51));  // (1, 0.5, 0.2)
        gr.FillEllipse(&ob, X(kv.first.first) - rr, Y(kv.first.second) - rr, rr * 2, rr * 2);
    }
    // current point: red, 16 px as in the original
    if (!std::isnan(st.cur.temp) && !std::isnan(st.cur.fan)) {
        float cx = X(st.cur.temp), cy = Y(st.cur.fan), rr = 8 * S;
        SolidBrush glow(Color(60, 255, 0, 0));
        gr.FillEllipse(&glow, cx - rr * 2, cy - rr * 2, rr * 4, rr * 4);
        SolidBrush rb(Color(255, 255, 0, 0));
        gr.FillEllipse(&rb, cx - rr, cy - rr, rr * 2, rr * 2);
    }
    DrawToast(gr, W, card.Y + card.Height - 12 * S, st.toast, st.toastUntil);
}

static void SaveCsv() {
    std::wstring p = st.paths.dataDir + L"fan_curve_" + Stamp() + L".csv";
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) { Toast(L"Could not write CSV"); return; }
    fputs("temp_c,fan_pct,count\n", f);
    for (auto& kv : st.observed) fprintf(f, "%d,%d,%d\n", kv.first.first, kv.first.second, kv.second);
    fclose(f);
    RECT rc;
    GetClientRect(st.hwnd, &rc);
    std::wstring png = p.substr(0, p.size() - 4) + L".png";
    RenderToPng(png, rc.right, rc.bottom, Render);
    Toast(L"Saved " + p.substr(p.find_last_of(L'\\') + 1) + L" + .png");
}

static void OnButton(int id) {
    switch (id) {
    case BTN_PAUSE: st.poller.paused = !st.poller.paused; st.poller.Wake(); break;
    case BTN_RESET: st.observed.clear(); st.total = 0; Toast(L"Observations reset"); break;
    case BTN_SAVE: SaveCsv(); break;
    case BTN_CLOSE: PostMessageW(st.hwnd, WM_CLOSE, 0, 0); break;
    }
    InvalidateRect(st.hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    case WM_APP + 10: {
        Reading* r = (Reading*)lp;
        if (r->err.empty()) {
            st.cur = *r;
            st.error = false;
            // as in the .py: only keep valid observations
            if (st.cur.temp > 0 && st.cur.fan > 0) {
                st.observed[{(int)std::lround(st.cur.temp), (int)std::lround(st.cur.fan)}]++;
                st.total++;
            }
            wchar_t t[96];
            swprintf(t, 96, L"GPU Temp: %.0f°C | Fan Speed: %.0f%% — %ls", st.cur.temp, st.cur.fan, APP_ID.name);
            SetWindowTextW(hwnd, t);
        } else {
            st.error = true;
            st.status = r->err;
        }
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
        case VK_SPACE: OnButton(BTN_PAUSE); break;
        case 'R': OnButton(BTN_RESET); break;
        case 'S': OnButton(BTN_SAVE); break;
        case 'O': OpenFolder(st.paths.dataDir); break;
        case 'D':
            st.demo = !st.demo;
            st.observed.clear();
            st.total = 0;
            st.cur = Reading();
            Toast(st.demo ? L"Demo mode" : L"Live mode");
            st.poller.Wake();
            break;
        case VK_ESCAPE: OnButton(BTN_CLOSE); break;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintBuffered(hwnd, Render);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {(LONG)(600 * S), (LONG)(460 * S)};
        return 0;
    case WM_CLOSE:
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
    st.poller.intervalMs = a.intervalMs ? a.intervalMs : 1000;  // 1 s like the Kivy Clock
    st.buttons = {{BTN_PAUSE, L"Pause", pal::amber},
                  {BTN_RESET, L"Reset", pal::cyan},
                  {BTN_SAVE, L"Save CSV", pal::green},
                  {BTN_CLOSE, L"Close", pal::red}};

    HWND hwnd = CreateAppWindow(APP_ID, hInst, WndProc, st.paths, 900, 640);
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
