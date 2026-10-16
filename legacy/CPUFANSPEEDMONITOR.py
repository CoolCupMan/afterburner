from kivy.app import App
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.label import Label
from kivy.uix.button import Button
from kivy.clock import Clock
import psutil
import subprocess
import re
import wmi

class SystemResourceMonitor(BoxLayout):
    def __init__(self, **kwargs):
        super(SystemResourceMonitor, self).__init__(orientation='vertical', **kwargs)

        # Labels
        self.cpu_usage_label = Label(text="CPU Usage: --%", font_size=20)
        self.cpu_temp_label = Label(text="CPU Temp: --°C", font_size=20)
        self.ram_usage_label = Label(text="RAM Usage: --%", font_size=20)
        self.gpu_label = Label(text="GPU Usage: --%, Temp: --°C", font_size=20)

        # Start Button
        self.start_button = Button(text="Start Monitoring", size_hint=(1, 0.3))
        self.start_button.bind(on_press=self.start_monitoring)

        # Add widgets
        self.add_widget(self.cpu_usage_label)
        self.add_widget(self.cpu_temp_label)
        self.add_widget(self.ram_usage_label)
        self.add_widget(self.gpu_label)
        self.add_widget(self.start_button)

        self.monitor_event = None
        self.wmi_conn = wmi.WMI(namespace="root\\wmi")

    def start_monitoring(self, instance):
        if self.monitor_event:
            self.monitor_event.cancel()

        # Prime CPU usage
        psutil.cpu_percent(interval=None)
        self.monitor_event = Clock.schedule_interval(self.update_metrics, 1)

    def update_metrics(self, dt):
        # CPU usage
        cpu_usage = psutil.cpu_percent(interval=0.1)
        self.cpu_usage_label.text = f"CPU Usage: {cpu_usage}%"

        # CPU temp
        cpu_temp = self.get_cpu_temp_wmi()
        if cpu_temp is not None:
            self.cpu_temp_label.text = f"CPU Temp: {cpu_temp:.1f}°C"
        else:
            self.cpu_temp_label.text = "CPU Temp: N/A"

        # RAM usage
        ram_usage = psutil.virtual_memory().percent
        self.ram_usage_label.text = f"RAM Usage: {ram_usage}%"

        # GPU usage
        gpu_info = self.get_gpu_info()
        if gpu_info:
            usage, temp = gpu_info
            self.gpu_label.text = f"GPU Usage: {usage}%, Temp: {temp}°C"
        else:
            self.gpu_label.text = "GPU Usage: N/A"

    def get_cpu_temp_wmi(self):
        try:
            temps = self.wmi_conn.MSAcpi_ThermalZoneTemperature()
            for sensor in temps:
                # Convert from tenths of Kelvin to Celsius
                temp_c = (sensor.CurrentTemperature / 10.0) - 273.15
                # Return first valid temperature found
                if temp_c > 0:
                    return temp_c
        except Exception as e:
            print(f"WMI CPU Temp error: {e}")
        return None

    def get_gpu_info(self):
        try:
            output = subprocess.check_output(
                ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu",
                 "--format=csv,noheader,nounits"],
                encoding='utf-8'
            )
            match = re.match(r"(\d+), (\d+)", output.strip())
            if match:
                usage, temp = match.groups()
                return int(usage), int(temp)
        except Exception as e:
            print(f"nvidia-smi error: {e}")
        return None


class MonitorApp(App):
    def build(self):
        return SystemResourceMonitor()


if __name__ == '__main__':
    MonitorApp().run()
