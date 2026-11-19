import serial
import numpy as np
import math
import time
from PIL import Image

SER = '/dev/ttyACM1'
BAUD = 230400
ANGLE_PER_FRAME = 12
MAX_DIST = 12000
DURATION = 1.5  # seconds (>= 1 revolution at 10 Hz)

angles = []
dists = []
ser = serial.Serial(SER, BAUD, timeout=1)
t0 = time.time()
while time.time() - t0 < DURATION:
    hdr = ser.read(1)
    if hdr != b'\x54':
        continue
    data = hdr + ser.read(46)
    b = [x for x in data]
    start_angle = (b[5] << 8 | b[4]) * 0.01
    for i in range(ANGLE_PER_FRAME):
        off = 6 + i * 3
        dist = b[off + 1] << 8 | b[off]
        ang = (start_angle + i * 0.83333 + 180) % 360
        if 0 < dist <= MAX_DIST:
            angles.append(ang)
            dists.append(dist)
ser.close()
print("points: %d, angle range: %.1f..%.1f deg" % (len(angles), min(angles), max(angles)))

GRID = 800
RES = 0.02
grid = np.full((GRID, GRID), 128, dtype=np.uint8)
cx = cy = GRID // 2

for ang, dist in zip(angles, dists):
    r = math.radians(ang)
    x_m = dist / 1000.0 * math.cos(r)
    y_m = dist / 1000.0 * math.sin(r)
    gx = int(round(x_m / RES)) + cx
    gy = int(round(y_m / RES)) + cy
    if 0 <= gx < GRID and 0 <= gy < GRID:
        grid[gy, gx] = 0
        n = max(abs(gx - cx), abs(gy - cy))
        for t in range(1, n):
            fx = cx + int(round((gx - cx) * t / n))
            fy = cy + int(round((gy - cy) * t / n))
            if 0 <= fx < GRID and 0 <= fy < GRID:
                grid[fy, fx] = 255

img = Image.fromarray(grid, 'L').convert('RGB')
from PIL import ImageDraw
d = ImageDraw.Draw(img)
d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=(255, 0, 0))  # robot
d.line([cx, cy, cx, cy - 70], fill=(255, 0, 0), width=3)  # FWD arrow
d.polygon([(cx, cy - 85), (cx - 10, cy - 65), (cx + 10, cy - 65)], fill=(255, 0, 0))
d.text((cx + 12, cy - 100), "FWD", fill=(255, 0, 0))
d.text((cx + 12, cy + 85), "REAR", fill=(0, 0, 255))
d.text((cx - 100, cy + 8), "LEFT", fill=(0, 200, 0))
d.text((cx + 60, cy + 8), "RIGHT", fill=(0, 200, 0))
img.save('/tmp/lidar_map.png')

# ASCII render (80 x 40)
W = 80
H = 40
print("=== MAP (%.0fm x %.0fm, '#'=wall, '.'=free, ' '=unknown, 'O'=robot) ===" % (GRID * RES, GRID * RES))
for row in range(H):
    line = ''
    for col in range(W):
        y0 = row * GRID // H
        y1 = (row + 1) * GRID // H
        x0 = col * GRID // W
        x1 = (col + 1) * GRID // W
        block = grid[y0:y1, x0:x1]
        if block.size == 0:
            line += ' '
        elif np.any(block == 0):
            line += '#'
        elif np.any(block == 255):
            line += '.'
        else:
            line += ' '
    line = line.replace(' ' * (W - len(line)), '')
    # mark robot at center
    if row == H // 2:
        mid = W // 2
        line = line[:mid] + 'O' + line[mid + 1:]
    print(line)
