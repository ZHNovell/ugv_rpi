"""
Стресс-тест NPU-моделей на реальном видеопотоке.
"""
import time
import cv2
import numpy as np
import threading
import psutil
from npu_client import NPUClient
from npu_pose_client import NPUPoseClient
from npu_seg_client import NPUSegClient
from npu_depth_client import NPUDepthClient


def get_temp():
    try:
        with open('/sys/class/thermal/thermal_zone0/temp') as f:
            return int(f.read().strip()) / 1000.0
    except:
        return 0.0


def monitor_system(stop_event, samples, interval=2.0):
    """Мониторинг CPU/RAM/TEMP во время теста."""
    while not stop_event.is_set():
        samples.append({
            't': time.time(),
            'cpu': psutil.cpu_percent(interval=None),
            'ram': psutil.virtual_memory().percent,
            'temp': get_temp(),
        })
        time.sleep(interval)


def main():
    # Открываем камеру
    cap = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("ERROR: cannot open camera")
        return 1

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    cap.set(cv2.CAP_PROP_FPS, 30)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    print(f"Camera: {cap.get(cv2.CAP_PROP_FRAME_WIDTH)}x{cap.get(cv2.CAP_PROP_FRAME_HEIGHT)} @ {cap.get(cv2.CAP_PROP_FPS)} fps")
    print()

    # Подключаем клиентов
    print("=== Подключение клиентов ===")
    clients = {
        'YOLO26s (objects)': NPUClient(),
        'YOLO11_pose': NPUPoseClient(),
        'YOLO11_seg': NPUSegClient(),
        'YOLO26n_depth': NPUDepthClient(),
    }
    print()

    DURATION = 70  # секунд

    # Запускаем мониторинг
    stop_event = threading.Event()
    samples = []
    mon_thread = threading.Thread(target=monitor_system, args=(stop_event, samples), daemon=True)
    mon_thread.start()

    # === Тест 1: По очереди ===
    print(f"=== ТЕСТ 1: По очереди ({DURATION} сек на модель) ===")
    results_seq = {}
    for name, client in clients.items():
        times = []
        errors = 0
        t_start = time.time()
        frame_count = 0

        last_print = time.time()
        while time.time() - t_start < DURATION:
            ret, frame = cap.read()
            if not ret:
                break
            frame_count += 1

            t0 = time.time()
            try:
                r = client.detect(frame)
            except Exception as e:
                errors += 1
                continue
            t1 = time.time()
            times.append((t1 - t0) * 1000)

            # Прогресс каждые 2 сек
            if time.time() - last_print >= 2.0:
                elapsed = time.time() - t_start
                cur_avg = sum(times[-20:]) / len(times[-20:]) if len(times) >= 20 else (sum(times)/len(times) if times else 0)
                cur_fps = 1000 / cur_avg if cur_avg > 0 else 0
                cur_temp = get_temp()
                cur_cpu = psutil.cpu_percent(interval=None)
                print(f"  [{name}] {elapsed:.0f}/{DURATION}s  frames={frame_count}  FPS={cur_fps:.1f}  temp={cur_temp:.1f}°C  CPU={cur_cpu:.1f}%", flush=True)
                last_print = time.time()

        avg = sum(times) / len(times) if times else 0
        results_seq[name] = {
            'avg': avg,
            'min': min(times) if times else 0,
            'max': max(times) if times else 0,
            'fps': 1000/avg if avg > 0 else 0,
            'frames': frame_count,
            'errors': errors,
        }
        print(f"  {name}: avg={avg:.1f}ms  FPS={1000/avg if avg else 0:.1f}  frames={frame_count}  errors={errors}")

    # === Тест 2: Параллельно ===
    print()
    print(f"=== ТЕСТ 2: Параллельно ({DURATION} сек, все 4 модели) ===")

    results_par = {}
    for name in clients:
        results_par[name] = {'times': [], 'errors': 0}

    stop_par = threading.Event()
    t_start = time.time()
    frame_count_par = 0

    def run_client(name, client, stop_ev):
        while not stop_ev.is_set():
            with frame_lock:
                frame = latest_frame
            if frame is None:
                time.sleep(0.005)
                continue
            t0 = time.time()
            try:
                client.detect(frame)
            except:
                results_par[name]['errors'] += 1
                continue
            t1 = time.time()
            results_par[name]['times'].append((t1 - t0) * 1000)

    # Общий кадр
    latest_frame = None
    frame_lock = threading.Lock()

    threads = []
    for name, client in clients.items():
        t = threading.Thread(target=run_client, args=(name, client, stop_par), daemon=True)
        t.start()
        threads.append(t)

    # Читаем кадры
    last_print_par = time.time()
    while time.time() - t_start < DURATION:
        ret, frame = cap.read()
        if not ret:
            break
        frame_count_par += 1
        with frame_lock:
            latest_frame = frame

        # Прогресс каждые 2 сек
        if time.time() - last_print_par >= 2.0:
            elapsed = time.time() - t_start
            cur_temp = get_temp()
            cur_cpu = psutil.cpu_percent(interval=None)
            fps_par = frame_count_par / elapsed if elapsed > 0 else 0
            print(f"  [PARALLEL] {elapsed:.0f}/{DURATION}s  frames={frame_count_par}  FPS={fps_par:.1f}  temp={cur_temp:.1f}°C  CPU={cur_cpu:.1f}%", flush=True)
            last_print_par = time.time()

    stop_par.set()
    for t in threads:
        t.join(timeout=2.0)

    print(f"  Всего кадров: {frame_count_par}")
    print(f"  Общий FPS: {frame_count_par / DURATION:.1f}")
    print()
    for name, res in results_par.items():
        times = res['times']
        avg = sum(times) / len(times) if times else 0
        print(f"  {name}: avg={avg:.1f}ms  FPS={1000/avg if avg else 0:.1f}  calls={len(times)}  errors={res['errors']}")

    # Останавливаем мониторинг
    stop_event.set()
    mon_thread.join(timeout=2.0)

    # === Итоги по системе ===
    print()
    print("=== Системная нагрузка (во время теста) ===")
    if samples:
        cpus = [s['cpu'] for s in samples]
        rams = [s['ram'] for s in samples]
        temps = [s['temp'] for s in samples]
        print(f"  CPU: avg={sum(cpus)/len(cpus):.1f}%  max={max(cpus):.1f}%")
        print(f"  RAM: avg={sum(rams)/len(rams):.1f}%  max={max(rams):.1f}%")
        print(f"  TEMP: avg={sum(temps)/len(temps):.1f}°C  max={max(temps):.1f}°C  min={min(temps):.1f}°C")

    cap.release()
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())
