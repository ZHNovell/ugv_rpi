import serial
import numpy as np
import math
import time
import json
import os
import threading

LIDAR = '/dev/ttyACM1'
ESP = '/dev/ttyACM0'
LIDAR_BAUD = 230400
ESP_BAUD = 115200
APF = 12
MAX_DIST = 12000
READ_TIME = 0.11

SHM_MAP = '/dev/shm/ugv_slam_map.raw'
SHM_POSE = '/dev/shm/ugv_slam_pose.json'
SHM_ESP = '/dev/shm/ugv_esp_telemetry.json'
WRITE_INTERVAL = 0.5

GRID = 1000
RES = 0.02
cx = cy = GRID // 2
NUM_POINTS = 432
WHEELBASE = 0.175  # meters (from Waveshare base_node.cpp)
ENC_MM_PER_TICK = 31.75  # calibrated: 1 m = ~31.5 ticks (measured 31/32 ticks over 1 m)
GYRO_DEG_PER_LSB = 0.06016  # calibrated: 360 deg CW -> 45.68 deg (k=7.88), ~16.6 LSB/(deg/s) = +-2000
gyro_bias = 0.0
comp_offset = None
last_gyro_ts = 0.0


def esp_reader():
    global prev_odl, prev_odr, gyro_bias, last_gyro_ts, comp_offset
    try:
        s = serial.Serial(ESP, ESP_BAUD, timeout=0.5)
    except Exception as e:
        print("ESP32 open failed:", e)
        return
    # measure gyro bias + compass offset over 1.5s (robot must be stationary at startup)
    biases = []
    comp_heads = []
    t0 = time.time()
    while time.time() - t0 < 1.5:
        d = s.readline()
        if not d:
            continue
        try:
            j = json.loads(d)
            if j.get('T') == 1001:
                biases.append(j.get('gz', 0))
                mx = j.get('mx', 0)
                my = j.get('my', 0)
                if mx != 0 or my != 0:
                    comp_heads.append(math.atan2(my, mx))
        except Exception:
            pass
    gyro_bias = sum(biases) / len(biases) if biases else 0.0
    comp_offset = sum(comp_heads) / len(comp_heads) if comp_heads else 0.0
    last_gyro_ts = time.time()
    print("gyro_bias = %.2f LSB  comp_offset = %.2f rad" % (gyro_bias, comp_offset), flush=True)
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
            # rotation from gyro (immune to wheel slip), translation from encoders
            if prev_odl is not None:
                dl = (esp['odl'] - prev_odl) * ENC_MM_PER_TICK / 1000.0  # meters
                dr = (esp['odr'] - prev_odr) * ENC_MM_PER_TICK / 1000.0
                dxy = (dr + dl) / 2.0
                with pose_lock:
                    global pose_x, pose_y, pose_th
                    pose_x += dxy * math.sin(pose_th)
                    pose_y -= dxy * math.cos(pose_th)
                    pose_th -= (esp['gz'] - gyro_bias) * GYRO_DEG_PER_LSB * math.pi / 180.0 * dt
                    # compass fusion DISABLED: raw magnetometer is distorted by
                    # motors/metal (hard/soft iron), needs calibration before use
                    if False and comp_offset is not None:
                        ch = math.atan2(esp['my'], esp['mx']) - comp_offset
                        diff = ch - pose_th
                        diff = (diff + math.pi) % (2 * math.pi) - math.pi
                        pose_th += 0.02 * diff
            prev_odl = esp['odl']
            prev_odr = esp['odr']
            # publish telemetry
            tmp = SHM_ESP + '.tmp'
            with open(tmp, 'w') as f:
                json.dump(esp, f)
            os.rename(tmp, SHM_ESP)
        except Exception:
            pass

esp = {'odl': 0, 'odr': 0, 'gz': 0.0, 'mx': 0, 'my': 0, 'v': 0, 'ts': 0.0}
pose_x = 0.0
pose_y = 0.0
pose_th = 0.0
pose_lock = threading.Lock()
prev_odl = None
prev_odr = None


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
    return r, grid_a


grid = np.full((GRID, GRID), 128, np.uint8)
traj = []


OCC_INC = 30   # +probability for a hit (wall)
FREE_DEC = 8   # -probability for free space along the ray


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
    # occupied endpoints (vectorized)
    grid[gyv, gxv] = np.clip(grid[gyv, gxv].astype(np.int16) + OCC_INC, 0, 255).astype(np.uint8)
    # free space along rays (coarse, small numpy per point)
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


def scan_match(ranges, angles, rx, ry, rth):
    """Scan-to-map correlation (numpy-vectorized): find a small (dx, dy, dth) that
    best aligns the current scan to the accumulated occupancy map."""
    pts = []
    for d, a in zip(ranges[::4], angles[::4]):
        if d <= 0:
            continue
        ra = a * math.pi / 180.0
        pts.append((d / 1000.0 * math.cos(ra), d / 1000.0 * math.sin(ra)))
    if len(pts) < 20:
        return 0.0, 0.0, 0.0
    P = np.array(pts, np.float32)
    best_score = -1
    base_score = -1
    best = (0.0, 0.0, 0.0)
    for dth in (-1.0, -0.5, 0.0, 0.5, 1.0):
        dth_r = math.radians(dth)
        c = math.cos(rth + dth_r)
        s = math.sin(rth + dth_r)
        for dx in (-0.02, 0.0, 0.02):
            for dy in (-0.02, 0.0, 0.02):
                wx = (rx + dx) + c * P[:, 0] - s * P[:, 1]
                wy = (ry + dy) + s * P[:, 0] + c * P[:, 1]
                gx = np.round(wx / RES).astype(np.int32) + cx
                gy = np.round(wy / RES).astype(np.int32) + cy
                valid = (gx >= 0) & (gx < GRID) & (gy >= 0) & (gy < GRID)
                gxv = gx[valid]
                gyv = gy[valid]
                score = int(np.count_nonzero(grid[gyv, gxv] > 128))
                if dth == 0.0 and dx == 0.0 and dy == 0.0:
                    base_score = score
                if score > best_score:
                    best_score = score
                    best = (dx, dy, dth_r)
    # reject ambiguous corrections (e.g. long corridor with parallel walls)
    if best_score - base_score < 3:
        return (0.0, 0.0, 0.0)
    return best


def write_shm():
    tmp = SHM_MAP + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(grid.tobytes())
    os.rename(tmp, SHM_MAP)
    tmp = SHM_POSE + '.tmp'
    with pose_lock:
        data = {'x': pose_x, 'y': pose_y, 'theta': pose_th, 'ts': time.time(),
                'grid': GRID, 'res': RES, 'traj': traj[-2000:]}
    with open(tmp, 'w') as f:
        json.dump(data, f)
    os.rename(tmp, SHM_POSE)


threading.Thread(target=esp_reader, daemon=True).start()
ser = serial.Serial(LIDAR, LIDAR_BAUD, timeout=1)
last_write = 0.0
print("SLAM daemon v4 (wheel odometry) running. Ctrl+C to stop.", flush=True)

try:
    while True:
        scan = read_scan()
        if scan is None:
            continue
        ranges, angles = scan
        with pose_lock:
            rx, ry, rth = pose_x, pose_y, pose_th
        dx, dy, dth = scan_match(ranges, angles, rx, ry, rth)
        if dx != 0.0 or dy != 0.0 or dth != 0.0:
            with pose_lock:
                pose_x += dx
                pose_y += dy
                pose_th += dth
            rx, ry, rth = pose_x, pose_y, pose_th
        update_map(ranges, angles, rx, ry, rth)
        traj.append([rx, ry])
        if time.time() - last_write > WRITE_INTERVAL:
            write_shm()
            last_write = time.time()
except KeyboardInterrupt:
    pass
finally:
    write_shm()
    ser.close()
    print("saved. pose x=%.2f y=%.2f th=%.1f" % (pose_x, pose_y, math.degrees(pose_th)))
