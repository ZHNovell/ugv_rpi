import serial, time
ser = serial.Serial('/dev/ttyACM1', 230400, timeout=1)
buf = b''
t0 = time.time()
pts = []  # (raw_angle_deg, distance_mm)
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
# sort by angle, bin into 30-deg sectors
print('total points', len(pts))
sectors = {}
for ang, d in pts:
    s = int(ang // 30) * 30
    if s not in sectors:
        sectors[s] = []
    sectors[s].append(d)
print('sector(deg): count, min_d, max_d')
for s in sorted(sectors):
    ds = [d for d in sectors[s] if 150 < d <= 12000]
    if ds:
        print('%4d: n=%3d min=%6.1f max=%6.1f' % (s, len(ds), min(ds), max(ds)))
    else:
        print('%4d: n=  0  (EMPTY/GAP)' % s)
