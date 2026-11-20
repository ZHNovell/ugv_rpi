import json, time
prev = None
updates = 0
t0 = time.time()
while time.time() - t0 < 3:
    try:
        ts = json.load(open('/dev/shm/ugv_esp_telemetry.json'))['ts']
    except Exception:
        time.sleep(0.05)
        continue
    if ts != prev:
        updates += 1
        prev = ts
    time.sleep(0.02)
print("telemetry updates in 3s:", updates, "(~%.1f Hz)" % (updates / 3.0))
