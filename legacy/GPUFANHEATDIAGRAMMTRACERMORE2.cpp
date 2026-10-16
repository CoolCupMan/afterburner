#include <windows.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#define TIMER_ID 1
#define TIMER_INTERVAL 4000

struct DataPoint {
    int temp; // GPU temperature (°C)
    int fan;  // Fan speed (%)
};

std::vector<DataPoint> data_points;
DataPoint current_data = {0, 0};

const int WIDTH = 900;
const int HEIGHT = 600;
const int MARGIN = 50;
const int MAX_TEMP = 100;
const int MAX_FAN = 100;

// Map value from one range to another
double map_value(double value, double in_min, double in_max, double out_min, double out_max) {
    if (in_max == in_min) return out_min; // avoid division by zero
    return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Query GPU data using nvidia-smi
bool query_gpu_data(int& temp, int& fan) {
    FILE* pipe = _popen("nvidia-smi --query-gpu=temperature.gpu,fan.speed --format=csv,noheader,nounits", "r");
    if (!pipe) return false;

    char buffer[128];
    if (!fgets(buffer, sizeof(buffer), pipe)) {
        _pclose(pipe);
        return false;
    }
    _pclose(pipe);

    std::stringstream ss(buffer);
    std::string temp_str, fan_str;
    if (!getline(ss, temp_str, ',') || !getline(ss, fan_str, ',')) {
        return false;
    }

    try {
        temp = std::stoi(temp_str);
        fan = std::stoi(fan_str);
    } catch (...) {
        return false;
    }

    return true;
}

// Draw everything on window
void draw_graph(HDC hdc, RECT clientRect) {
    int graph_w = clientRect.right - clientRect.left - 2 * MARGIN;
    int graph_h = clientRect.bottom - clientRect.top - 2 * MARGIN;

    // Draw axes
    MoveToEx(hdc, MARGIN, MARGIN, NULL);
    LineTo(hdc, MARGIN, clientRect.bottom - MARGIN);  // Y axis
    LineTo(hdc, clientRect.right - MARGIN, clientRect.bottom - MARGIN); // X axis

    // Labels (use wide strings because TextOutW expects LPCWSTR)
    const wchar_t* yLabel = L"Fan %";
    const wchar_t* xLabel = L"Temperature (°C)";
    TextOutW(hdc, 10, MARGIN - 20, yLabel, lstrlenW(yLabel));
    TextOutW(hdc, (clientRect.right / 2) - 80, clientRect.bottom - MARGIN + 10, xLabel, lstrlenW(xLabel));

    // Draw ticks on Y axis (Fan %)
    for (int i = 0; i <= MAX_FAN; i += 10) {
        int y = (int)map_value(i, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);
        MoveToEx(hdc, MARGIN - 5, y, NULL);
        LineTo(hdc, MARGIN + 5, y);

        wchar_t label[8];
        wsprintfW(label, L"%d", i);
        TextOutW(hdc, MARGIN - 40, y - 8, label, lstrlenW(label));
    }

    // Draw ticks on X axis (Temperature)
    for (int i = 0; i <= MAX_TEMP; i += 10) {
        int x = (int)map_value(i, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
        MoveToEx(hdc, x, clientRect.bottom - MARGIN - 5, NULL);
        LineTo(hdc, x, clientRect.bottom - MARGIN + 5);

        wchar_t label[8];
        wsprintfW(label, L"%d", i);
        TextOutW(hdc, x - 10, clientRect.bottom - MARGIN + 20, label, lstrlenW(label));
    }

    // Draw lines connecting data points
    if (data_points.size() >= 2) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 255));
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);

        for (size_t i = 1; i < data_points.size(); ++i) {
            int x1 = (int)map_value(data_points[i - 1].temp, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
            int y1 = (int)map_value(data_points[i - 1].fan, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);
            int x2 = (int)map_value(data_points[i].temp, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
            int y2 = (int)map_value(data_points[i].fan, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);
            MoveToEx(hdc, x1, y1, NULL);
            LineTo(hdc, x2, y2);
        }

        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    // Draw current data point as green ball
    int cx = (int)map_value(current_data.temp, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
    int cy = (int)map_value(current_data.fan, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);

    HBRUSH greenBrush = CreateSolidBrush(RGB(0, 255, 0));
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, greenBrush);

    Ellipse(hdc, cx - 6, cy - 6, cx + 6, cy + 6);

    SelectObject(hdc, oldBrush);
    DeleteObject(greenBrush);

    // Optional: draw small red dots for all historical points for clarity
    HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
    HBRUSH oldBrush2 = (HBRUSH)SelectObject(hdc, redBrush);
    for (const auto& pt : data_points) {
        int px = (int)map_value(pt.temp, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
        int py = (int)map_value(pt.fan, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);
        Ellipse(hdc, px - 3, py - 3, px + 3, py + 3);
    }
    SelectObject(hdc, oldBrush2);
    DeleteObject(redBrush);
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            int t = 0, f = 0;
            if (query_gpu_data(t, f)) {
                current_data.temp = t;
                current_data.fan = f;

                // Debug prints (comment out if not needed)
                std::cout << "GPU Temp: " << t << " °C, Fan Speed: " << f << " %" << std::endl;

                data_points.push_back(current_data);
                if (data_points.size() > 100)
                    data_points.erase(data_points.begin());

                InvalidateRect(hwnd, NULL, TRUE); // Request redraw
            }
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        draw_graph(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"FanTempWindowClass";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"GPU Fan Curve Viewer (No GTK)", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, WIDTH, HEIGHT,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
