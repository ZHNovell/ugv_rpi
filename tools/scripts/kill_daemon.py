import os, signal
me = str(os.getpid())
for pid in os.listdir('/proc'):
    if not pid.isdigit():
        continue
    try:
        raw = open('/proc/%s/cmdline' % pid, 'rb').read()
    except Exception:
        continue
    args = raw.split(b'\x00')
    if len(args) >= 2 and b'python3' in args[0] and b'lidar_slam_daemon.py' in args[1] and pid != me:
        try:
            os.kill(int(pid), signal.SIGTERM)
            print('killed', pid)
        except Exception as e:
            print('skip', pid, e)
print('done')
