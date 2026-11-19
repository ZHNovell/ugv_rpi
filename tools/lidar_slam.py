import serial
import numpy as np
import math
import time
from PIL import Image

SER = '/dev/ttyACM1'
BAUD = 230400
APF = 12
MAX_DIST = 12000
READ_TIME = 0.11  # ~1 revolution at 10 Hz


def read_scan():
    angles, dists = [], []
    t0 = time.time()
    while time.time() - t0 < READ_TIME:
        hdr = ser.read(1)
        if hdr != b'\x54':
            continue
        data = hdr + ser.read(46)
        b = [x for x in data]
        start = (b[5] << 8 | b[4]) * 0.01
        for i in range(APF):
            off = 6 + i * 3
            d = b[off + 1] << 8 | b[off]
            a = (start + i * 0.83333 + 180) % 360
            if 0 < d <= MAX_DIST:
                angles.append(a)
                dists.append(d)
    if len(angles) < 30:
        return None
    order = np.argsort(angles)
    angles = np.array(angles)[order][::3]
    dists = np.array(dists)[order][::3]
    r = np.radians(angles)
    return np.column_stack([np.cos(r) * dists / 1000.0, np.sin(r) * dists / 1000.0])


def icp(src, dst, max_iter=40, max_corr=0.4):
    s = src.copy()
    Rtot = np.eye(2)
    ttot = np.zeros(2)
    for _ in range(max_iter):
        diff = s[:, None, :] - dst[None, :, :]
        d2 = (diff ** 2).sum(axis=2)
        idx = np.argmin(d2, axis=1)
        nd = np.sqrt(d2[np.arange(len(s)), idx])
        valid = nd < max_corr
        if valid.sum() < 20:
            return None
        s_v = s[valid]
        d_v = dst[idx[valid]]
        cs = s_v.mean(0)
        cd = d_v.mean(0)
        H = (s_v - cs).T @ (d_v - cd)
        U, S, Vt = np.linalg.svd(H)
        R = Vt.T @ U.T
        if np.linalg.det(R) < 0:
            Vt[-1, :] *= -1
            R = Vt.T @ U.T
        t = cd - R @ cs
        s = (R @ s.T).T + t
        Rtot = R @ Rtot
        ttot = R @ ttot + t
        if np.linalg.norm(t) < 1e-3:
            break
    return Rtot, ttot


GRID = 1000
RES = 0.02  # 20 m map, 20 mm/pixel
grid = np.full((GRID, GRID), 128, np.uint8)
pose_x = 0.0
pose_y = 0.0
pose_th = 0.0
cx = cy = GRID // 2


def update_map(pts_world, rx, ry):
    gx0 = int(round(rx / RES)) + cx
    gy0 = int(round(-ry / RES)) + cy
    for px, py in pts_world:
        gx = int(round(px / RES)) + cx
        gy = int(round(py / RES)) + cy
        if not (0 <= gx < GRID and 0 <= gy < GRID):
            continue
        grid[gy, gx] = 0
        n = max(abs(gx - gx0), abs(gy - gy0))
        if n == 0:
            continue
        for t in range(1, n):
            fx = gx0 + int(round((gx - gx0) * t / n))
            fy = gy0 + int(round((gy - gy0) * t / n))
            if 0 <= fx < GRID and 0 <= fy < GRID:
                grid[fy, fx] = 255


ser = serial.Serial(SER, BAUD, timeout=1)
prev = None
n = 0
print("SLAM running: rotate/drive robot slowly. Ctrl+C to stop + save map.", flush=True)

try:
    while True:
        pts = read_scan()
        if pts is None:
            continue
        if prev is None:
            update_map(pts, 0.0, 0.0)
            prev = pts
            continue
        res = icp(pts, prev)
        if res is None:
            prev = pts
            continue
        R, t = res
        dtheta = math.atan2(R[1, 0], R[0, 0])
        tx = math.cos(pose_th) * t[0] - math.sin(pose_th) * t[1]
        ty = math.sin(pose_th) * t[0] + math.cos(pose_th) * t[1]
        pose_x += tx
        pose_y += ty
        pose_th += dtheta
        Rw = np.array([[math.cos(pose_th), -math.sin(pose_th)],
                       [math.sin(pose_th), math.cos(pose_th)]])
        pts_w = (Rw @ pts.T).T + np.array([pose_x, pose_y])
        update_map(pts_w, pose_x, pose_y)
        prev = pts
        n += 1
        if n % 30 == 0:
            print("pose x=%.2f y=%.2f th=%.1f deg" % (pose_x, pose_y, math.degrees(pose_th)), flush=True)
except KeyboardInterrupt:
    pass

ser.close()
Image.fromarray(grid, 'L').save('/tmp/lidar_slam_map.png')
print("saved /tmp/lidar_slam_map.png  pose x=%.2f y=%.2f th=%.1f deg" % (pose_x, pose_y, math.degrees(pose_th)))

W = 80
H = 40
print("=== MAP ('#'=wall, '.'=free, ' '=unknown, 'O'=start, 'X'=robot) ===")
rx = int(round(pose_x / RES)) + cx
ry = int(round(pose_y / RES)) + cy
for row in range(H):
    y0 = row * GRID // H
    y1 = (row + 1) * GRID // H
    chars = []
    for col in range(W):
        x0 = col * GRID // W
        x1 = (col + 1) * GRID // W
        block = grid[y0:y1, x0:x1]
        if np.any(block == 0):
            chars.append('#')
        elif np.any(block == 255):
            chars.append('.')
        else:
            chars.append(' ')
    if row == H // 2:
        chars[W // 2] = 'O'
    rcol = rx * W // GRID
    rrow = ry * H // GRID
    if rrow == row and 0 <= rcol < W:
        chars[rcol] = 'X'
    print(''.join(chars))


