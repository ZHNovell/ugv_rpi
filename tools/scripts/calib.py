import serial, time
ser = serial.Serial('/dev/ttyACM1', 230400, timeout=1)
buf = b''
t0 = time.time()
pts = []
while time.time() - t0 < 0.6:
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
        step = diff / 11.0
        for i in range(12):
            off = 6 + i * 3
            d = pkt[off + 1] << 8 | pkt[off]
            ang = (start + i * step) % 360.0
            pts.append((ang, d))
ser.close()
# 1-degree bins, find gaps (no valid points) and close walls
import math
bins = {}
for ang, d in pts:
    b = int(round(ang)) % 360
    bins.setdefault(b, []).append(d)
print('angle-by-angle (1 deg bins):')
gap_start = None
gaps = []
for b in range(360):
    ds = [d for d in bins.get(b, []) if 150 < d <= 12000]
    if not ds:
        if gap_start is None:
            gap_start = b
    else:
        if gap_start is not None:
            gaps.append((gap_start, b-1))
            gap_start = None
if gap_start is not None:
    gaps.append((gap_start, 359))
print('GAPS (dead zone, no returns):', gaps)
# close wall: angles with min distance < 700mm
close = []
for b in range(360):
    ds = [d for d in bins.get(b, []) if 150 < d <= 700]
    if ds:
        close.append((b, min(ds)))
print('CLOSE WALL (<0.7m) angles:', close)
