import serial
import numpy as np
import math
import time
import json
import os
import threading
import subprocess

LIDAR = '/dev/ttyACM1'
ESP = '/dev/ttyACM0'
LIDAR_BAUD = 230400
ESP_BAUD = 115200
APF = 12
MAX_DIST = 12000
READ_TIME = 0.11
DEAD_MIN = 50.0
DEAD_MAX = 140.0

SHM_MAP = '/dev/shm/ugv_slam_map.raw'
SHM_POSE = '/dev/shm/ugv_slam_pose.json'
SHM_ESP = '/dev/shm/ugv_esp_telemetry.json'
WRITE_INTERVAL = 0.5

GRID = 1000
RES = 0.02
cx = cy = GRID // 2
NUM_POINTS = 432
OCC_INC = 30
FREE_DEC = 8

HECTOR_BIN = '/root/hector_standalone/build/hector_standalone'

esp = {'odl': 0, 'odr': 0, 'gz': 0.0, 'mx': 0, 'my': 0, 'v': 0, 'ts': 0.0}
GYRO_DEG_PER_LSB = 0.06016  # calibrated +-2000 dps
gyro_bias = 0.0
last_gyro_ts = 0.0
theta_gyro = 0.0
theta_lock = threading.Lock()


def esp_reader():
    global gyro_bias, last_gyro_ts, theta_gyro
    try:
        s = serial.Serial(ESP, ESP_BAUD, timeout=0.5)
    except Exception as e:
        print("ESP32 open failed:", e)
        return
    # measure gyro bias over 1.5s (robot stationary at startup)
    biases = []
    t0 = time.time()
    while time.time() - t0 < 1.5:
        d = s.readline()
        if not d:
            continue
        try:
            j = json.loads(d)
            if j.get('T') == 1001:
                biases.append(j.get('gz', 0))
        except Exception:
            pass
    gyro_bias = sum(biases) / len(biases) if biases else 0.0
    last_gyro_ts = time.time()
    print("gyro_bias = %.2f LSB" % gyro_bias, flush=True)
    while True:
        try:
            d = s.readline()
        except Exception:
            time.sleep(0.2)
            continue
        if not d:
            continue
        try:
            j = json.loads(d)
            if j.get('T') != 1001:
                continue
            esp['odl'] = j.get('odl', 0)
            esp['odr'] = j.get('odr', 0)
            esp['gz'] = j.get('gz', 0)
            esp['mx'] = j.get('mx', 0)
            esp['my'] = j.get('my', 0)
            esp['v'] = j.get('v', 0)
            esp['ts'] = time.time()
            now = time.time()
            dt = now - last_gyro_ts
            last_gyro_ts = now
            with theta_lock:
                theta_gyro -= (esp['gz'] - gyro_bias) * GYRO_DEG_PER_LSB * math.pi / 180.0 * dt
            tmp = SHM_ESP + '.tmp'
            with open(tmp, 'w') as f:
                json.dump(esp, f)
            os.rename(tmp, SHM_ESP)
        except Exception:
            pass


def read_scan():
    angles, dists = [], []
    t0 = time.time()
    buf = b''
    while time.time() - t0 < READ_TIME:
        n = ser.in_waiting
        if n > 0:
            buf += ser.read(n)
        else:
            time.sleep(0.003)
        while len(buf) >= 47:
            if buf[0] != 0x54:
                buf = buf[1:]
                continue
            pkt = buf[:47]
            buf = buf[47:]
            start = (pkt[5] << 8 | pkt[4]) * 0.01
            for i in range(APF):
                off = 6 + i * 3
                d = pkt[off + 1] << 8 | pkt[off]
                a = (start + i * 0.83333 + 180) % 360
                if 150 < d <= MAX_DIST:
                    angles.append(a)
                    dists.append(d)
    if len(angles) < 30:
        return None
    order = np.argsort(angles)
    a = np.array(angles, np.float32)[order]
    d = np.array(dists, np.float32)[order]
    grid_a = np.linspace(0.0, 360.0, NUM_POINTS, endpoint=False)
    idx = np.searchsorted(a, grid_a)
    idx = np.clip(idx, 1, len(a) - 1)
    left_a = a[idx - 1]
    right_a = a[idx]
    left_d = d[idx - 1]
    right_d = d[idx]
    near = np.where(np.abs(grid_a - left_a) <= np.abs(right_a - grid_a), left_d, right_d)
    gap = np.minimum(np.abs(grid_a - left_a), np.abs(right_a - grid_a))
    r = np.where(gap < 1.5, near, 0.0).astype(np.float32)
    crop = (grid_a >= DEAD_MIN) & (grid_a <= DEAD_MAX)
    r[crop] = 0.0
    return r, grid_a


def update_map(ranges, angles, rx, ry, rth):
    rx0 = int(round(rx / RES)) + cx
    ry0 = int(round(ry / RES)) + cy
    d = np.asarray(ranges, np.float32)
    a = np.asarray(angles, np.float32)
    valid = d > 0
    ra = a[valid] * (math.pi / 180.0)
    dd = d[valid] / 1000.0
    px = dd * np.cos(ra)
    py = dd * np.sin(ra)
    wx = rx + math.cos(rth) * px - math.sin(rth) * py
    wy = ry + math.sin(rth) * px + math.cos(rth) * py
    gx = np.round(wx / RES).astype(np.int32) + cx
    gy = np.round(wy / RES).astype(np.int32) + cy
    inb = (gx >= 0) & (gx < GRID) & (gy >= 0) & (gy < GRID)
    gxv = gx[inb]
    gyv = gy[inb]
    grid[gyv, gxv] = np.clip(grid[gyv, gxv].astype(np.int16) + OCC_INC, 0, 255).astype(np.uint8)
    for ex, ey in zip(gxv.tolist(), gyv.tolist()):
        nn = max(abs(ex - rx0), abs(ey - ry0))
        if nn <= 1:
            continue
        t = np.arange(2, nn, 3, dtype=np.float32)
        fx = rx0 + np.round((ex - rx0) * t / nn).astype(np.int32)
        fy = ry0 + np.round((ey - ry0) * t / nn).astype(np.int32)
        ok = (fx >= 0) & (fx < GRID) & (fy >= 0) & (fy < GRID)
        fxv = fx[ok]
        fyv = fy[ok]
        if fxv.size:
            grid[fyv, fxv] = np.clip(grid[fyv, fxv].astype(np.int16) - FREE_DEC, 0, 255).astype(np.uint8)


threading.Thread(target=esp_reader, daemon=True).start()
ser = serial.Serial(LIDAR, LIDAR_BAUD, timeout=1)
grid = np.full((GRID, GRID), 128, np.uint8)
traj = []
last_write = 0.0
pose = (0.0, 0.0, 0.0)

proc = subprocess.Popen([HECTOR_BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, text=True, bufsize=1)
print("SLAM daemon v6 (Hector scan-to-map) running. Ctrl+C to stop.", flush=True)

try:
    while True:
        scan = read_scan()
        if scan is None:
            continue
        ranges, angles = scan
        pts = []
        for dd, aa in zip(ranges, angles):
            if dd <= 0:
                continue
            ra = aa * math.pi / 180.0
            pts.append("%.3f %.3f" % (dd / 1000.0 * math.cos(ra), dd / 1000.0 * math.sin(ra)))
        if len(pts) < 30:
            continue
        with theta_lock:
            yaw_prior = theta_gyro
        proc.stdin.write("%.4f %s\n" % (yaw_prior, " ".join(pts)))
        proc.stdin.flush()
        out = proc.stdout.readline()
        try:
            px, py, pth = map(float, out.split())
            pose = (px, py, pth)
        except Exception:
            continue
        update_map(ranges, angles, px, py, pth)
        traj.append([px, py])
        if time.time() - last_write > WRITE_INTERVAL:
            tmp = SHM_MAP + '.tmp'
            with open(tmp, 'wb') as f:
                f.write(grid.tobytes())
            os.rename(tmp, SHM_MAP)
            tmp = SHM_POSE + '.tmp'
            data = {'x': px, 'y': py, 'theta': pth, 'ts': time.time(),
                    'grid': GRID, 'res': RES, 'traj': traj[-2000:]}
            with open(tmp, 'w') as f:
                json.dump(data, f)
            os.rename(tmp, SHM_POSE)
            last_write = time.time()
except KeyboardInterrupt:
    pass
finally:
    with open(SHM_MAP, 'wb') as f:
        f.write(grid.tobytes())
    proc.stdin.close()
    proc.terminate()
    ser.close()
    print("saved. pose x=%.2f y=%.2f th=%.1f" % (pose[0], pose[1], math.degrees(pose[2])))
