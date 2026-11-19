import serial
import numpy as np
import math
import time
import json
import os

SER = '/dev/ttyACM1'
BAUD = 230400
APF = 12
MAX_DIST = 12000
READ_TIME = 0.11

SHM_MAP = '/dev/shm/ugv_slam_map.raw'
SHM_POSE = '/dev/shm/ugv_slam_pose.json'
WRITE_INTERVAL = 0.5

GRID = 1000
RES = 0.02
cx = cy = GRID // 2


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
    angles = np.array(angles)[order][::4]
    dists = np.array(dists)[order][::4]
    r = np.radians(angles)
    return np.column_stack([np.cos(r) * dists / 1000.0, np.sin(r) * dists / 1000.0])


def align_to_map(pts, x, y, th, grid, max_iter=25):
    """Scan-to-map ICP: register current scan against accumulated occupancy grid.
    Returns corrected (x, y, th)."""
    WIN = 6  # half-window in cells (6 * 2cm = 12 cm search radius)
    for _ in range(max_iter):
        R = np.array([[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]])
        pts_w = (R @ pts.T).T + np.array([x, y])
        src = []
        dst = []
        for px, py in pts_w:
            gx = int(round(px / RES)) + cx
            gy = int(round(py / RES)) + cy
            if not (0 <= gx < GRID and 0 <= gy < GRID):
                continue
            best = None
            for dy in range(-WIN, WIN + 1):
                ny = gy + dy
                if ny < 0 or ny >= GRID:
                    continue
                row = grid[ny]
                for dx in range(-WIN, WIN + 1):
                    nx = gx + dx
                    if 0 <= nx < GRID and row[nx] == 0:
                        d2 = dx * dx + dy * dy
                        if best is None or d2 < best[0]:
                            best = (d2, (nx - cx) * RES, (ny - cy) * RES)
            if best is not None:
                src.append((px, py))
                dst.append((best[1], best[2]))
        if len(src) < 20:
            return x, y, th
        src = np.array(src)
        dst = np.array(dst)
        cs = src.mean(0)
        cd = dst.mean(0)
        H = (src - cs).T @ (dst - cd)
        U, S, Vt = np.linalg.svd(H)
        Rc = Vt.T @ U.T
        if np.linalg.det(Rc) < 0:
            Vt[-1, :] *= -1
            Rc = Vt.T @ U.T
        tc = cd - Rc @ cs
        dth = math.atan2(Rc[1, 0], Rc[0, 0])
        xy = Rc @ np.array([x, y]) + tc
        x, y = xy[0], xy[1]
        th += dth
        if np.linalg.norm(tc) < 1e-3 and abs(dth) < 1e-4:
            break
    return x, y, th


grid = np.full((GRID, GRID), 128, np.uint8)
pose_x = 0.0
pose_y = 0.0
pose_th = 0.0
traj = []
n = 0


def update_map(pts_w, rx, ry):
    gx0 = int(round(rx / RES)) + cx
    gy0 = int(round(ry / RES)) + cy
    for px, py in pts_w:
        gx = int(round(px / RES)) + cx
        gy = int(round(py / RES)) + cy
        if not (0 <= gx < GRID and 0 <= gy < GRID):
            continue
        grid[gy, gx] = 0
        nn = max(abs(gx - gx0), abs(gy - gy0))
        if nn == 0:
            continue
        for t in range(1, nn):
            fx = gx0 + int(round((gx - gx0) * t / nn))
            fy = gy0 + int(round((gy - gy0) * t / nn))
            if 0 <= fx < GRID and 0 <= fy < GRID:
                grid[fy, fx] = 255


def write_shm():
    tmp = SHM_MAP + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(grid.tobytes())
    os.rename(tmp, SHM_MAP)
    tmp = SHM_POSE + '.tmp'
    data = {'x': pose_x, 'y': pose_y, 'theta': pose_th, 'ts': time.time(),
            'scans': n, 'grid': GRID, 'res': RES, 'traj': traj[-2000:]}
    with open(tmp, 'w') as f:
        json.dump(data, f)
    os.rename(tmp, SHM_POSE)


ser = serial.Serial(SER, BAUD, timeout=1)
prev = None
last_write = 0.0
print("SLAM daemon (scan-to-map) running. Ctrl+C to stop.", flush=True)

try:
    while True:
        pts = read_scan()
        if pts is None:
            continue
        if prev is None:
            update_map(pts, 0.0, 0.0)
            prev = pts
            continue
        # scan-to-map registration
        nx, ny, nth = align_to_map(pts, pose_x, pose_y, pose_th, grid)
        # reject big jumps (registration likely failed)
        if abs(nx - pose_x) > 0.5 or abs(ny - pose_y) > 0.5 or abs(nth - pose_th) > 0.6:
            nx, ny, nth = pose_x, pose_y, pose_th
        pose_x, pose_y, pose_th = nx, ny, nth
        Rw = np.array([[math.cos(pose_th), -math.sin(pose_th)],
                       [math.sin(pose_th), math.cos(pose_th)]])
        pts_w = (Rw @ pts.T).T + np.array([pose_x, pose_y])
        update_map(pts_w, pose_x, pose_y)
        prev = pts
        traj.append([pose_x, pose_y])
        n += 1
        if time.time() - last_write > WRITE_INTERVAL:
            write_shm()
            last_write = time.time()
except KeyboardInterrupt:
    pass
finally:
    write_shm()
    ser.close()
    print("saved. scans=%d pose x=%.2f y=%.2f th=%.1f" % (n, pose_x, pose_y, math.degrees(pose_th)))
