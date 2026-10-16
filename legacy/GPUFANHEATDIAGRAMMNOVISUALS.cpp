#include <windows.h>
#include <gtk/gtk.h>
#include <cairo.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct DataPoint {
    int temp; // temperature in Celsius
    int fan;  // fan speed percentage (0-100)
};

std::vector<DataPoint> data_points;
DataPoint current_data{0, 0};

const int WIDTH = 900;
const int HEIGHT = 600;

const int MARGIN_LEFT = 80;
const int MARGIN_RIGHT = 20;
const int MARGIN_TOP = 20;
const int MARGIN_BOTTOM = 80;

const int MAX_TEMP = 100;
const int MAX_FAN = 100;

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

double map_value(double value, double in_min, double in_max, double out_min, double out_max) {
    return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void draw_axes(cairo_t* cr, int width, int height) {
    int graph_w = width - (MARGIN_LEFT + MARGIN_RIGHT);
    int graph_h = height - (MARGIN_TOP + MARGIN_BOTTOM);

    // Draw x axis
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, MARGIN_LEFT, height - MARGIN_BOTTOM);
    cairo_line_to(cr, width - MARGIN_RIGHT, height - MARGIN_BOTTOM);
    cairo_stroke(cr);

    // Draw y axis
    cairo_move_to(cr, MARGIN_LEFT, MARGIN_TOP);
    cairo_line_to(cr, MARGIN_LEFT, height - MARGIN_BOTTOM);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    // Y axis ticks and labels (10 to 100 by 10)
    for (int val = 10; val <= 100; val += 10) {
        double y = map_value(val, 0, MAX_FAN, height - MARGIN_BOTTOM, MARGIN_TOP);
        cairo_move_to(cr, MARGIN_LEFT - 6, y);
        cairo_line_to(cr, MARGIN_LEFT, y);
        cairo_stroke(cr);

        char label[16];
        snprintf(label, sizeof(label), "%d", val);
        cairo_move_to(cr, MARGIN_LEFT - 40, y + 4);
        cairo_show_text(cr, label);
    }

    // Y axis label
    cairo_save(cr);
    cairo_translate(cr, 20, height / 2);
    cairo_rotate(cr, -M_PI / 2);
    cairo_set_font_size(cr, 14);
    cairo_show_text(cr, "Fan Speed (%) and RPM");
    cairo_restore(cr);

    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, MARGIN_LEFT - 20, MARGIN_TOP + 20);
    cairo_show_text(cr, "Y");

    // X axis ticks and labels
    for (int val = 10; val <= 100; val += 10) {
        double x = map_value(val, 0, MAX_TEMP, MARGIN_LEFT, width - MARGIN_RIGHT);
        cairo_move_to(cr, x, height - MARGIN_BOTTOM);
        cairo_line_to(cr, x, height - MARGIN_BOTTOM + 6);
        cairo_stroke(cr);

        char label[16];
        snprintf(label, sizeof(label), "%d", val);
        cairo_text_extents_t extents;
        cairo_text_extents(cr, label, &extents);
        cairo_move_to(cr, x - extents.width / 2, height - MARGIN_BOTTOM + 24);
        cairo_show_text(cr, label);
    }

    const char* xlabel = "Temperature (°C)";
    cairo_text_extents_t extents;
    cairo_text_extents(cr, xlabel, &extents);
    double xlabel_x = MARGIN_LEFT + graph_w / 2 - extents.width / 2;
    double xlabel_y = height - MARGIN_BOTTOM + 50;
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, xlabel_x, xlabel_y);
    cairo_show_text(cr, xlabel);
}

void draw_graph(cairo_t* cr, int width, int height) {
    int graph_w = width - (MARGIN_LEFT + MARGIN_RIGHT);

    if (data_points.size() < 2) return;

    cairo_set_source_rgb(cr, 0, 1, 1);
    cairo_set_line_width(cr, 2);

    for (size_t i = 1; i < data_points.size(); ++i) {
        double x1 = map_value(data_points[i - 1].temp, 0, MAX_TEMP, MARGIN_LEFT, MARGIN_LEFT + graph_w);
        double y1 = map_value(data_points[i - 1].fan, 0, MAX_FAN, HEIGHT - MARGIN_BOTTOM, MARGIN_TOP);
        double x2 = map_value(data_points[i].temp, 0, MAX_TEMP, MARGIN_LEFT, MARGIN_LEFT + graph_w);
        double y2 = map_value(data_points[i].fan, 0, MAX_FAN, HEIGHT - MARGIN_BOTTOM, MARGIN_TOP);

        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        cairo_stroke(cr);
    }

    double cx = map_value(current_data.temp, 0, MAX_TEMP, MARGIN_LEFT, MARGIN_LEFT + graph_w);
    double cy = map_value(current_data.fan, 0, MAX_FAN, HEIGHT - MARGIN_BOTTOM, MARGIN_TOP);

    cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0, 1, 0); // green
    cairo_fill(cr);
}

gboolean timeout_callback(gpointer user_data) {
    int t, f;
    if (query_gpu_data(t, f)) {
        current_data.temp = t;
        current_data.fan = f;
        data_points.push_back(current_data);
        if (data_points.size() > 100)
            data_points.erase(data_points.begin());
    }
    GtkWidget* drawing_area = GTK_WIDGET(user_data);
    gtk_widget_queue_draw(drawing_area);
    return TRUE;
}

gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int width = allocation.width;
    int height = allocation.height;

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_paint(cr);

    draw_axes(cr, width, height);
    draw_graph(cr, width, height);

    return FALSE;
}

// Your "main" logic moved here:
int main_entry(int argc, char** argv) {
    gtk_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GPU Fan Curve Viewer");
    gtk_window_set_default_size(GTK_WINDOW(window), WIDTH, HEIGHT);

    GtkWidget* drawing_area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(window), drawing_area);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_draw), nullptr);

    g_timeout_add_seconds(4, timeout_callback, drawing_area);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}

// Actual entry point for Windows GUI app
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    int argc = __argc;
    char** argv = __argv;
    return main_entry(argc, argv);
}
