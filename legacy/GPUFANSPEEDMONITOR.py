import numpy as np
import subprocess
import re
from kivy.app import App
from kivy.clock import Clock
from kivy.core.window import Window
from kivy.graphics import Color, Line, Ellipse, Rectangle
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.widget import Widget
from kivy.uix.label import Label
from kivy.uix.button import Button

Window.size = (900, 600)
Window.clearcolor = (0.12, 0.12, 0.12, 1)


class FanCurveObserver(Widget):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.margin_left = 80
        self.margin_bottom = 60
        self.margin_right = 20
        self.margin_top = 20

        # Observed data points: list of (temp, fan_speed)
        self.observed = []
        self.current_temp = 0
        self.current_fan_speed = 0

        Clock.schedule_interval(self.update_graph, 1 / 30)

    def temperature_to_x(self, temp):
        graph_w = self.width - (self.margin_left + self.margin_right)
        return self.x + self.margin_left + (temp / 100.0) * graph_w

    def fan_to_y(self, fan):
        graph_h = self.height - (self.margin_top + self.margin_bottom)
        return self.y + self.margin_bottom + (fan / 100.0) * graph_h

    def update_graph(self, *args):
        self.canvas.clear()
        with self.canvas:
            # Background
            Color(0.2, 0.2, 0.2, 1)
            Rectangle(pos=self.pos, size=self.size)

            # Axes
            Color(1, 1, 1, 1)
            x0 = self.x + self.margin_left
            y0 = self.y + self.margin_bottom
            x_max = self.x + self.width - self.margin_right
            y_max = self.y + self.height - self.margin_top
            Line(points=[x0, y0, x0, y_max], width=1.5)
            Line(points=[x0, y0, x_max, y0], width=1.5)

            # Ticks & labels
            for fan in range(0, 101, 10):
                y = self.fan_to_y(fan)
                Line(points=[x0 - 5, y, x0, y], width=1)
                label = Label(text=f"{fan}%", font_size=12, color=(1,1,1,1))
                label.texture_update()
                Rectangle(texture=label.texture,
                          pos=(x0 - 10 - label.texture_size[0], y - label.texture_size[1] / 2),
                          size=label.texture_size)
            for temp in range(0, 101, 10):
                x = self.temperature_to_x(temp)
                Line(points=[x, y0, x, y0 + 5], width=1)
                label = Label(text=f"{temp}°C", font_size=12, color=(1,1,1,1))
                label.texture_update()
                Rectangle(texture=label.texture,
                          pos=(x - label.texture_size[0] / 2, y0 - label.texture_size[1] - 4),
                          size=label.texture_size)

            # Title labels
            x_label = Label(text="X-Axis: Temp °C", font_size=14, color=(1,1,1,1))
            x_label.texture_update()
            Rectangle(texture=x_label.texture,
                      pos=(self.x + (self.width - x_label.texture_size[0]) / 2, self.y + 5),
                      size=x_label.texture_size)
            y_label = Label(text="Y-Axis:\nFan %", font_size=14, color=(1,1,1,1))
            y_label.texture_update()
            Rectangle(texture=y_label.texture,
                      pos=(self.x + 5, self.y + (self.height - y_label.texture_size[1]) / 2),
                      size=y_label.texture_size)

            # Plot observed points
            if self.observed:
                Color(0, 0.8, 0.8, 1)
                pts = []
                # Sort by temp
                obs_sorted = sorted(self.observed, key=lambda p: p[0])
                for (t, f) in obs_sorted:
                    pts.append(self.temperature_to_x(t))
                    pts.append(self.fan_to_y(f))
                # Optionally connect them by lines
                Line(points=pts, width=1.5)

                # Draw dots
                Color(1, 0.5, 0.2, 1)
                for (t, f) in obs_sorted:
                    x = self.temperature_to_x(t)
                    y = self.fan_to_y(f)
                    Ellipse(pos=(x - 4, y - 4), size=(8, 8))

            # Draw current point
            cx = self.temperature_to_x(self.current_temp)
            cy = self.fan_to_y(self.current_fan_speed)
            Color(1, 0, 0, 1)
            Ellipse(pos=(cx - 8, cy - 8), size=(16, 16))

    def update_gpu_data(self, dt):
        # Query via nvidia-smi
        try:
            out = subprocess.check_output(
                ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu,fan.speed",
                 "--format=csv,noheader,nounits"],
                encoding='utf-8'
            )
            parts = [p.strip().replace("%","") for p in out.split(',')]
            if len(parts) >= 3:
                temp = int(parts[1])
                fan_spd = int(parts[2])
            else:
                temp = 0
                fan_spd = 0
        except Exception:
            temp = 0
            fan_spd = 0

        self.current_temp = temp
        self.current_fan_speed = fan_spd
        # Add to observed list if valid
        if temp > 0 and fan_spd > 0:
            # Optionally avoid duplicates
            self.observed.append((temp, fan_spd))


class FanCurveObserverApp(App):
    def build(self):
        root = BoxLayout(orientation='vertical', padding=10, spacing=10)

        # Info label
        self.info_label = Label(text="GPU Temp: --°C  |  Fan Speed: --%", font_size=18, size_hint_y=0.1, color=(1,1,1,1))
        root.add_widget(self.info_label)

        self.viewer = FanCurveObserver()
        root.add_widget(self.viewer)

        # Bottom Close
        bottom = BoxLayout(size_hint_y=0.1)
        btn_close = Button(text="Close")
        btn_close.bind(on_press=self.stop)
        bottom.add_widget(btn_close)
        root.add_widget(bottom)

        Clock.schedule_interval(self.viewer.update_gpu_data, 1.0)
        Clock.schedule_interval(self.update_info_display, 1.0)

        return root

    def update_info_display(self, dt):
        t = self.viewer.current_temp
        fs = self.viewer.current_fan_speed
        self.info_label.text = f"GPU Temp: {t}°C  |  Fan Speed: {fs}%"


if __name__ == '__main__':
    import subprocess
    FanCurveObserverApp().run()
