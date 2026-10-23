import threading
import time
import yaml

class PTStabilizer:
    def __init__(self, base, config_path):
        self.base = base
        with open(config_path, 'r') as f:
            self.config = yaml.safe_load(f)
        
        self.enabled = False
        self.target_pitch = 0.0
        self.filtered_pitch = 0.0
        self.filter_initialized = False
        self.thread = None
        self.running = False
        
        # Коэффициент P и фильтр из config.yaml
        self.p_gain = self.config['args_config'].get('pt_p_gain', 1.0)
        self.filter_alpha = self.config['args_config'].get('pt_filter_alpha', 0.15)
        self.servo_spd = self.config['args_config'].get('pt_servo_spd', 2000)
        self.servo_acc = self.config['args_config'].get('pt_servo_acc', 50)

    def set_enabled(self, enabled):
        self.enabled = enabled
        if not enabled:
            self.filter_initialized = False
            print("[PTStabilizer] Disabled", flush=True)
        else:
            print("[PTStabilizer] Enabled", flush=True)

    def set_target(self, target_pitch):
        self.target_pitch = target_pitch

    def handle_steady_command(self, json_data):
        if json_data.get('s') == 1:
            self.set_enabled(True)
            self.set_target(json_data.get('y', 0))
        else:
            self.set_enabled(False)

    def start(self):
        if self.thread is None or not self.thread.is_alive():
            self.running = True
            self.thread = threading.Thread(target=self._run, daemon=True)
            self.thread.start()
            print("[PTStabilizer] Started", flush=True)

    def stop(self):
        self.running = False

    def _run(self):
        while self.running:
            if not self.enabled:
                time.sleep(0.01)
                continue
            
            try:
                if self.base.base_data is None or 'p' not in self.base.base_data:
                    time.sleep(0.01)
                    continue
                
                current_pitch = self.base.base_data['p']
                
                # Прямая компенсация (как у Waveshare)
                target_tilt = self.target_pitch - current_pitch
                target_tilt = max(-30.0, min(90.0, target_tilt))
                
                self.base.gimbal_ctrl(0, target_tilt, self.servo_spd, self.servo_acc)
                
            except Exception as e:
                print(f"[PTStabilizer] Error: {e}", flush=True)
            
            time.sleep(0.05)  # 20 Гц (было 50 Гц)
