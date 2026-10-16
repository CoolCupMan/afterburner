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
    int temp;
    int fan;
};

std::vector<DataPoint> data_points;
DataPoint current_data = {0, 0};

const int WIDTH = 900;
const int HEIGHT = 600;
const int MARGIN = 50;
const int MAX_TEMP = 100;
const int MAX_FAN = 100;

// Utility function to map values
double map_value(double value, double in_min, double in_max, double out_min, double out_max) {
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

    // Labels
    TextOut(hdc, 10, MARGIN - 20, "Fan %", 6);
    TextOut(hdc, clientRect.right / 2 - 50, clientRect.bottom - 30, "Temperature (°C)", 18);

    // Draw ticks on Y
    for (int i = 10; i <= 100; i += 10) {
        int y = (int)map_value(i, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);
        MoveToEx(hdc, MARGIN - 5, y, NULL);
        LineTo(hdc, MARGIN + 5, y);

        char label[8];
        sprintf_s(label, "%d", i);
        TextOut(hdc, MARGIN - 40, y - 8, label, strlen(label));
    }

    // Draw ticks on X
    for (int i = 10; i <= 100; i += 10) {
        int x = (int)map_value(i, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
        MoveToEx(hdc, x, clientRect.bottom - MARGIN - 5, NULL);
        LineTo(hdc, x, clientRect.bottom - MARGIN + 5);

        char label[8];
        sprintf_s(label, "%d", i);
        TextOut(hdc, x - 10, clientRect.bottom - MARGIN + 10, label, strlen(label));
    }

    // Draw lines for data
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

    // Draw current point
    int cx = (int)map_value(current_data.temp, 0, MAX_TEMP, MARGIN, clientRect.right - MARGIN);
    int cy = (int)map_value(current_data.fan, 0, MAX_FAN, clientRect.bottom - MARGIN, MARGIN);
    HBRUSH greenBrush = CreateSolidBrush(RGB(0, 255, 0));
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, greenBrush);
    Ellipse(hdc, cx - 5, cy - 5, cx + 5, cy + 5);
    SelectObject(hdc, oldBrush);
    DeleteObject(greenBrush);
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            int t, f;
            if (query_gpu_data(t, f)) {
                current_data.temp = t;
                current_data.fan = f;
                data_points.push_back(current_data);
                if (data_points.size() > 100)
                    data_points.erase(data_points.begin());
                InvalidateRect(hwnd, NULL, TRUE);
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

// Entry point for Windows GUI app
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
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
