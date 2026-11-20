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
MAX_DIST = 4000
READ_TIME = 0.11
DEAD_MIN = 50.0
DEAD_MAX = 140.0

SHM_MAP = '/dev/shm/ugv_slam_map.raw'
SHM_POSE = '/dev/shm/ugv_slam_pose.json'
SHM_ESP = '/dev/shm/ugv_esp_telemetry.json'
WRITE_INTERVAL = 0.5

GRID = 1000
RES = 0.02
cx = GRID // 2
cy = int(GRID * 0.75)  # start point in the bottom quarter (forward = up)
NUM_POINTS = 432
OCC_INC = 30
FREE_DEC = 8

HECTOR_BIN = '/root/carto_standalone/build/carto_standalone'

esp = {'odl': 0, 'odr': 0, 'gz': 0.0, 'mx': 0, 'my': 0, 'v': 0, 'ts': 0.0}
GYRO_DEG_PER_LSB = 0.06016  # calibrated +-2000 dps
ENC_MM_PER_TICK = 31.75     # calibrated wheel encoders
gyro_bias = 0.0
last_gyro_ts = 0.0
odom_x = 0.0
odom_y = 0.0
odom_th = 0.0
prev_odl = None
prev_odr = None
theta_lock = threading.Lock()


def esp_reader():
    global gyro_bias, last_gyro_ts, odom_x, odom_y, odom_th, prev_odl, prev_odr
    try:
        s = serial.Serial(ESP, ESP_BAUD, timeout=0.5)
    except Exception as e:
        print("ESP32 open failed:", e)
        return
    # measure gyro bias over 1.5s (robot stationary at startup)
    biases = []
    t0 = time.time()
    while time.time() - t0 < 1.5:
        try:
            d = s.readline()
        except Exception:
            time.sleep(0.2)
            continue
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
                if prev_odl is not None:
                    dl = (esp['odl'] - prev_odl) * ENC_MM_PER_TICK / 1000.0
                    dr = (esp['odr'] - prev_odr) * ENC_MM_PER_TICK / 1000.0
                    dxy = (dr + dl) / 2.0
                    odom_x += dxy * math.sin(odom_th)
                    odom_y -= dxy * math.cos(odom_th)
                odom_th -= (esp['gz'] - gyro_bias) * GYRO_DEG_PER_LSB * math.pi / 180.0 * dt
                prev_odl = esp['odl']
                prev_odr = esp['odr']
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
        try:
            n = ser.in_waiting
            if n > 0:
                buf += ser.read(n)
            else:
                time.sleep(0.003)
        except Exception:
            time.sleep(0.05)
            continue
        while len(buf) >= 47:
            if buf[0] != 0x54:
                buf = buf[1:]
                continue
            pkt = buf[:47]
            buf = buf[47:]
            start = (pkt[5] << 8 | pkt[4]) * 0.01
            end = (pkt[43] << 8 | pkt[42]) * 0.01
            diff = (end - start + 360.0) % 360.0
            step = diff / (APF - 1)
            for i in range(APF):
                off = 6 + i * 3
                d = pkt[off + 1] << 8 | pkt[off]
                a = (start + i * step + 180) % 360
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


def rasterize(robot_pts, rx, ry, rth):
    rx0 = int(round(rx / RES)) + cx
    ry0 = int(round(ry / RES)) + cy
    px = robot_pts[:, 0]
    py = robot_pts[:, 1]
    c = math.cos(rth)
    s = math.sin(rth)
    wx = rx + c * px - s * py
    wy = ry + s * px + c * py
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


def rebuild_map():
    global grid
    grid = np.full((GRID, GRID), 128, np.uint8)
    for kf in keyframes:
        x, y, th, rpts = kf
        rasterize(rpts, x, y, th)


keyframes = []  # (x, y, th, robot_pts)
last_kf = (0.0, 0.0)
scan_n = 0


def loop_closure(cur_robot_pts, px, py, pth):
    c = math.cos(pth)
    s = math.sin(pth)
    cur_world = np.column_stack([px + c * cur_robot_pts[:, 0] - s * cur_robot_pts[:, 1],
                                 py + s * cur_robot_pts[:, 0] + c * cur_robot_pts[:, 1]])
    best = None
    best_score = 0.0
    best_idx = -1
    for idx, kf in enumerate(keyframes):
        kx, ky, kth, rpts = kf
        if math.hypot(kx - px, ky - py) > 2.0:
            continue
        kc = math.cos(kth)
        ks = math.sin(kth)
        k_world = np.column_stack([kx + kc * rpts[:, 0] - ks * rpts[:, 1],
                                   ky + ks * rpts[:, 0] + kc * rpts[:, 1]])
        d2 = ((cur_world[:, None, :] - k_world[None, :, :]) ** 2).sum(-1)
        dmin = d2.min(axis=1)
        score = float((dmin < 0.01).mean())  # fraction within 0.1 m
        if score > best_score:
            best_score = score
            best = (kx, ky, kth)
            best_idx = idx
    return best, best_score, best_idx


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
        P_robot = []
        for dd, aa in zip(ranges, angles):
            if dd <= 0:
                continue
            ra = aa * math.pi / 180.0
            xr = dd / 1000.0 * math.cos(ra)
            yr = dd / 1000.0 * math.sin(ra)
            pts.append("%.3f %.3f" % (-yr, xr))
            P_robot.append((-yr, xr))
        if len(pts) < 30:
            continue
        P_robot = np.array(P_robot, np.float32).reshape(-1, 2)
        with theta_lock:
            ox, oy, oth = odom_x, odom_y, odom_th
        if not traj:
            with open('/tmp/scan_debug.txt', 'w') as f:
                f.write('oth %.4f ox %.3f oy %.3f n %d\n' % (oth, ox, oy, len(P_robot)))
                for xr, yr in P_robot:
                    f.write('%.3f %.3f\n' % (xr, yr))
        proc.stdin.write("%.3f %.3f %.4f %s\n" % (-oy, ox, oth, " ".join(pts)))
        proc.stdin.flush()
        out = proc.stdout.readline()
        try:
            vals = list(map(float, out.split()))
            lx, ly = vals[0], vals[1]
            Tx, Ty, Tyaw = vals[3], vals[4], vals[5]
            px, py, pth = vals[6], vals[7], vals[8]
            pose = (px, py, pth)
        except Exception:
            continue
        traj.append([lx, ly])
        # keyframes + loop closure (pose graph)
        if math.hypot(px - last_kf[0], py - last_kf[1]) > 0.5:
            keyframes.append((px, py, pth, P_robot))
            last_kf = (px, py)
        scan_n += 1
        if False and scan_n % 10 == 0 and len(keyframes) > 10:
            best, best_score, best_idx = loop_closure(P_robot, px, py, pth)
            if best is not None and best_score > 0.6:
                bx, by, bth = best
                off = math.hypot(bx - px, by - py)
                if 0.3 < off < 1.5:
                    dx = bx - px
                    dy = by - py
                    dth = bth - pth
                    N = len(keyframes)
                    # distribute the correction (pose graph)
                    for i in range(best_idx, N):
                        frac = (i - best_idx) / float(max(1, N - 1 - best_idx))
                        kx, ky, kth, rpts = keyframes[i]
                        keyframes[i] = (kx + dx * frac, ky + dy * frac, kth + dth * frac, rpts)
                    # reset Hector + re-feed corrected keyframes (fix its map too)
                    proc.stdin.write("RESET\n")
                    proc.stdin.flush()
                    proc.stdout.readline()
                    for kf in keyframes:
                        kx, ky, kth, rpts = kf
                        kpts = " ".join("%.3f %.3f" % (p[0], p[1]) for p in rpts)
                        proc.stdin.write("%.3f %.3f %.4f %s\n" % (kx, ky, kth, kpts))
                    proc.stdin.flush()
                    for _ in range(len(keyframes)):
                        proc.stdout.readline()
                    rebuild_map()
                    with theta_lock:
                        odom_x += dx
                        odom_y += dy
                        odom_th += dth
                    px, py, pth = bx, by, bth
                    pose = (px, py, pth)
                    print("loop closed: %.2f m (reset+refeed)" % off, flush=True)
        if time.time() - last_write > WRITE_INTERVAL:
            tmp = SHM_MAP + '.tmp'
            try:
                with open('/dev/shm/carto_map.raw', 'rb') as src:
                    with open(tmp, 'wb') as f:
                        f.write(src.read())
                os.rename(tmp, SHM_MAP)
            except Exception:
                pass
            tmp = SHM_POSE + '.tmp'
            data = {'x': px, 'y': py, 'theta': pth, 'ts': time.time(),
                    'grid': GRID, 'res': 0.05, 'traj': traj[-2000:],
                    'Tx': Tx, 'Ty': Ty, 'Tyaw': Tyaw}
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
