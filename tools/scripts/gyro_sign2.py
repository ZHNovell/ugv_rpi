import json, time
samples = []
# Phase 1: baseline (robot still)
t0 = time.time()
while time.time() - t0 < 4.0:
    try:
        d = json.load(open('/dev/shm/ugv_esp_telemetry.json'))
        samples.append(d.get('gz', 0.0))
    except Exception:
        pass
    time.sleep(0.02)
bias = sum(samples) / len(samples) if samples else 0.0
with open('/tmp/gyro_baseline.txt', 'w') as f:
    f.write('%.2f' % bias)
print('BASELINE_DONE bias=%.1f' % bias, flush=True)
# Phase 2: detect rotation spike
peak_pos = 0.0
peak_neg = 0.0
t0 = time.time()
while time.time() - t0 < 40.0:
    try:
        d = json.load(open('/dev/shm/ugv_esp_telemetry.json'))
        gz = d.get('gz', 0.0)
    except Exception:
        continue
    dev = gz - bias
    if dev > peak_pos:
        peak_pos = dev
    if dev < peak_neg:
        peak_neg = dev
    time.sleep(0.02)
print('ROTATION_DONE peak_pos=%.1f peak_neg=%.1f' % (peak_pos, peak_neg), flush=True)
