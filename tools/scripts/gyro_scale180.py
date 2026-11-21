import json, time
samples = []
t0 = time.time()
while time.time() - t0 < 3.0:
    try:
        d = json.load(open('/dev/shm/ugv_esp_telemetry.json'))
        samples.append(d.get('gz', 0.0))
    except Exception:
        pass
    time.sleep(0.02)
bias = sum(samples) / len(samples) if samples else 0.0
print('baseline bias=%.1f' % bias, flush=True)
last_ts = None
total_raw = 0.0
t0 = time.time()
while time.time() - t0 < 45.0:
    try:
        d = json.load(open('/dev/shm/ugv_esp_telemetry.json'))
        gz = d.get('gz', 0.0)
        ts = d.get('ts', 0.0)
    except Exception:
        continue
    if last_ts is not None and ts > last_ts:
        dt = ts - last_ts
        if 0 < dt < 0.5:
            total_raw += (gz - bias) * dt
    last_ts = ts
    time.sleep(0.02)
print('TOTAL_RAW=%.1f' % total_raw, flush=True)
print('if 180deg turn -> scale = %.5f deg/s per LSB' % (180.0 / total_raw if total_raw else 0), flush=True)
