import math, subprocess
import numpy as np

N = 360
angles = np.linspace(0, 360, N, endpoint=False)

def ray_rect(cx, cy, a, hw, hh):
    d = 1e9
    c = math.cos(math.radians(a)); s = math.sin(math.radians(a))
    for tx in (-hw, hw):
        if abs(c) > 1e-6:
            t = (tx - cx) / c
            if t > 0:
                y = cy + t * s
                if -hh <= y <= hh: d = min(d, t)
    for ty in (-hh, hh):
        if abs(s) > 1e-6:
            t = (ty - cy) / s
            if t > 0:
                x = cx + t * c
                if -hw <= x <= hw: d = min(d, t)
    return d

def scan_line(cx, cy, th):
    pts = []
    for a in angles:
        d = ray_rect(cx, cy, a + th, 4.0, 3.0)
        dbox = ray_rect(cx - 2.0, cy - 2.0, a + th, 0.75, 0.75)
        d = min(d, dbox)
        ra = math.radians(a)
        pts.append("%.3f %.3f" % (d * math.cos(ra), d * math.sin(ra)))
    return " ".join(pts)

lines = [scan_line(0, 0, 0), scan_line(0.1, 0.05, 0.03)]
open('/tmp/room_scans.txt', 'w').write("\n".join(lines) + "\n")
out = subprocess.run(['/root/hector_standalone/build/hector_standalone'],
                     stdin=open('/tmp/room_scans.txt'), capture_output=True, text=True)
print(out.stdout.strip().splitlines()[-2:])
