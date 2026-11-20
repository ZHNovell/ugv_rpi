import json, numpy as np, math
pose = json.load(open('/dev/shm/ugv_slam_pose.json'))
traj = pose.get('traj', [])
print("traj points:", len(traj))
if traj:
    print("start: x=%.2f y=%.2f" % (traj[0][0], traj[0][1]))
    print("end:   x=%.2f y=%.2f" % (traj[-1][0], traj[-1][1]))
    dx = traj[-1][0] - traj[0][0]
    dy = traj[-1][1] - traj[0][1]
    print("drift: dx=%.2f dy=%.2f dist=%.2f m" % (dx, dy, math.hypot(dx, dy)))
print("theta: %.1f deg" % math.degrees(pose.get('theta', 0)))

grid = np.fromfile('/dev/shm/ugv_slam_map.raw', dtype=np.uint8).reshape(1000, 1000)
H, W = 40, 90
sample = grid[np.linspace(0, 999, H).astype(int)][:, np.linspace(0, 999, W).astype(int)]
chars = ' .:+*#@'
for row in sample:
    print(''.join(chars[min(int(v) * 7 // 256, 6)] for v in row))
