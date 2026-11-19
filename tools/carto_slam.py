"""
Cartographer-style 2D SLAM (simplified port to numpy):
- submaps (probability grids, 35 scans each)
- correlative scan matching (coarse-to-fine, bilinear score)
- loop closure (match scan against finished submaps + score threshold)
- pose graph (simple global correction)

Parameters mirror mapping_2d.lua of the Waveshare stack.
"""
import numpy as np
import math

SUBMAP_SIZE = 400          # cells per side
RESOLUTION = 0.05          # m/cell (5 cm)
NUM_RANGE_DATA = 35        # scans per submap
MIN_RANGE = 0.1            # m
MAX_RANGE = 3.5            # m
MIN_SCORE = 0.55           # loop closure acceptance (mapping_2d.lua: 0.65)
GLOBAL_MIN_SCORE = 0.60    # global localization threshold (0.7)

P_HIT = 0.55               # occupancy increment
P_MISS = 0.49              # free-space decrement
UNKNOWN = 0.5


def _clip(v, lo, hi):
    return v if v < hi else (v if v > lo else v)


class Submap:
    def __init__(self, pose):
        self.grid = np.full((SUBMAP_SIZE, SUBMAP_SIZE), UNKNOWN, np.float32)
        self.pose = pose            # (x, y, theta) world frame
        self.num_scans = 0
        self.finished = False

    def world_to_local(self, wx, wy):
        sx, sy, sth = self.pose
        dx = wx - sx
        dy = wy - sy
        c = math.cos(sth)
        s = math.sin(sth)
        return c * dx + s * dy, -s * dx + c * dy

    def local_to_cell(self, lx, ly):
        return (lx / RESOLUTION + SUBMAP_SIZE / 2.0,
                ly / RESOLUTION + SUBMAP_SIZE / 2.0)

    def insert_scan(self, scan_pts_world, robot_world):
        """scan_pts_world: (N,2) world points; robot_world: (x,y) world."""
        rx, ry = robot_world
        rlx, rly = self.world_to_local(rx, ry)
        rcxx, rcyy = self.local_to_cell(rlx, rly)
        rcx = int(round(rcxx))
        rcy = int(round(rcyy))
        for wx, wy in scan_pts_world:
            lx, ly = self.world_to_local(wx, wy)
            cx, cy = self.local_to_cell(lx, ly)
            gx = int(round(cx))
            gy = int(round(cy))
            if not (0 <= gx < SUBMAP_SIZE and 0 <= gy < SUBMAP_SIZE):
                continue
            # free space along the ray (coarse, step 2)
            nn = max(abs(gx - rcx), abs(gy - rcy))
            if nn > 1:
                for t in range(1, nn, 2):
                    fx = rcx + int(round((gx - rcx) * t / nn))
                    fy = rcy + int(round((gy - rcy) * t / nn))
                    if 0 <= fx < SUBMAP_SIZE and 0 <= fy < SUBMAP_SIZE:
                        p = self.grid[fy, fx]
                        if p > P_MISS:
                            self.grid[fy, fx] = P_MISS
            # occupied endpoint
            p = self.grid[gy, gx]
            if p < P_HIT:
                self.grid[gy, gx] = P_HIT
        self.num_scans += 1

    def score(self, scan_pts_world):
        """Sum of the (bilinear-interpolated) probabilities at the scan points."""
        pts = np.asarray(scan_pts_world, np.float32).reshape(-1, 2)
        if pts.shape[0] == 0:
            return 0.0
        sx, sy, sth = self.pose
        dx = pts[:, 0] - sx
        dy = pts[:, 1] - sy
        c = math.cos(sth)
        s = math.sin(sth)
        lx = c * dx + s * dy
        ly = -s * dx + c * dy
        gx = lx / RESOLUTION + SUBMAP_SIZE / 2.0
        gy = ly / RESOLUTION + SUBMAP_SIZE / 2.0
        x0 = np.floor(gx).astype(np.int32)
        y0 = np.floor(gy).astype(np.int32)
        fx = (gx - x0).astype(np.float32)
        fy = (gy - y0).astype(np.float32)
        ok = (x0 >= 0) & (x0 < SUBMAP_SIZE - 1) & (y0 >= 0) & (y0 < SUBMAP_SIZE - 1)
        x0 = x0[ok]
        y0 = y0[ok]
        fx = fx[ok]
        fy = fy[ok]
        if x0.size == 0:
            return 0.0
        p00 = self.grid[y0, x0]
        p10 = self.grid[y0, x0 + 1]
        p01 = self.grid[y0 + 1, x0]
        p11 = self.grid[y0 + 1, x0 + 1]
        p = (p00 * (1 - fx) * (1 - fy) + p10 * fx * (1 - fy) +
             p01 * (1 - fx) * fy + p11 * fx * fy)
        return float(np.sum(p))


class CorrelativeScanMatcher:
    @staticmethod
    def match(scan_pts_world, submap, init_pose, dth_deg=5.0, dxy=0.1, fine=True):
        """Rotation-first correlative search (fast). Returns (best_pose, best_score)."""
        ix, iy, ith = init_pose
        best = (ix, iy, ith)
        best_score = -1.0
        # 1. coarse rotation (1D)
        for dth in np.arange(-2 * dth_deg, 2 * dth_deg + 1e-9, dth_deg / 2.0):
            pose = (ix, iy, ith + math.radians(dth))
            sc = Submap_score_pose(scan_pts_world, submap, pose)
            if sc > best_score:
                best_score = sc
                best = pose
        # 2. translation at best rotation (2D)
        bx, by, bth = best
        for dx in np.arange(-dxy, dxy + 1e-9, dxy / 5.0):
            for dy in np.arange(-dxy, dxy + 1e-9, dxy / 5.0):
                pose = (bx + dx, by + dy, bth)
                sc = Submap_score_pose(scan_pts_world, submap, pose)
                if sc > best_score:
                    best_score = sc
                    best = pose
        # 3. fine rotation + translation (2-step)
        bx, by, bth = best
        for dth in np.arange(-dth_deg / 2.0, dth_deg / 2.0 + 1e-9, dth_deg / 10.0):
            pose = (bx, by, bth + math.radians(dth))
            sc = Submap_score_pose(scan_pts_world, submap, pose)
            if sc > best_score:
                best_score = sc
                best = pose
        bx, by, bth = best
        for dx in np.arange(-dxy / 5.0, dxy / 5.0 + 1e-9, dxy / 25.0):
            for dy in np.arange(-dxy / 5.0, dxy / 5.0 + 1e-9, dxy / 25.0):
                pose = (bx + dx, by + dy, bth)
                sc = Submap_score_pose(scan_pts_world, submap, pose)
                if sc > best_score:
                    best_score = sc
                    best = pose
        return best, best_score


def Submap_score_pose(scan_pts_world, submap, pose):
    """Score the scan transformed by `pose` against the submap."""
    pts = np.asarray(scan_pts_world, np.float32).reshape(-1, 2)
    if pts.shape[0] == 0:
        return 0.0
    px, py, pth = pose
    c = math.cos(pth)
    s = math.sin(pth)
    # pose -> world, then world -> submap local
    wx = px + c * pts[:, 0] - s * pts[:, 1]
    wy = py + s * pts[:, 0] + c * pts[:, 1]
    sx, sy, sth = submap.pose
    dx = wx - sx
    dy = wy - sy
    c2 = math.cos(sth)
    s2 = math.sin(sth)
    lx = c2 * dx + s2 * dy
    ly = -s2 * dx + c2 * dy
    gx = lx / RESOLUTION + SUBMAP_SIZE / 2.0
    gy = ly / RESOLUTION + SUBMAP_SIZE / 2.0
    x0 = np.floor(gx).astype(np.int32)
    y0 = np.floor(gy).astype(np.int32)
    fx = (gx - x0).astype(np.float32)
    fy = (gy - y0).astype(np.float32)
    ok = (x0 >= 0) & (x0 < SUBMAP_SIZE - 1) & (y0 >= 0) & (y0 < SUBMAP_SIZE - 1)
    if not np.any(ok):
        return 0.0
    x0 = x0[ok]
    y0 = y0[ok]
    fx = fx[ok]
    fy = fy[ok]
    p00 = submap.grid[y0, x0]
    p10 = submap.grid[y0, x0 + 1]
    p01 = submap.grid[y0 + 1, x0]
    p11 = submap.grid[y0 + 1, x0 + 1]
    p = (p00 * (1 - fx) * (1 - fy) + p10 * fx * (1 - fy) +
         p01 * (1 - fx) * fy + p11 * fx * fy)
    return float(np.sum(p))


class CartographerSLAM:
    def __init__(self):
        self.submaps = []           # all submaps (incl. finished)
        self.active = None
        self.pose = (0.0, 0.0, 0.0)   # robot pose (world)
        self.loop_count = 0

    def add_scan(self, ranges, angles, odom_pose):
        """Feed a scan. ranges/angles in robot frame. odom_pose = (x,y,theta) prior."""
        # 1. build scan points (world, at the odom prior)
        pts = []
        for d, a in zip(ranges, angles):
            if not (MIN_RANGE * 1000.0 < d <= MAX_RANGE * 1000.0):
                continue
            ra = a * math.pi / 180.0
            pts.append((d / 1000.0 * math.cos(ra), d / 1000.0 * math.sin(ra)))
        pts = np.array(pts, np.float32).reshape(-1, 2)
        if pts.shape[0] < 20:
            return self.pose

        if self.active is None:
            self.active = Submap(self.pose)

        # 2. local scan matching against the active submap (refine the pose)
        pose, score = CorrelativeScanMatcher.match(pts, self.active, odom_pose,
                                                   dth_deg=2.0, dxy=0.05, fine=True)
        self.pose = pose

        # 3. transform points to world (at the refined pose) + insert
        px, py, pth = pose
        c = math.cos(pth)
        s = math.sin(pth)
        wx = px + c * pts[:, 0] - s * pts[:, 1]
        wy = py + s * pts[:, 0] + c * pts[:, 1]
        world_pts = np.column_stack([wx, wy])
        self.active.insert_scan(world_pts, (px, py))

        # 4. finish submap when full
        if self.active.num_scans >= NUM_RANGE_DATA:
            self.active.finished = True
            self.submaps.append(self.active)
            self.active = Submap(self.pose)

        # 5. loop closure: match against finished submaps (periodic)
        if len(self.submaps) >= 2 and self.active.num_scans % 10 == 0:
            self._loop_closure(pts)

        return self.pose

    def _loop_closure(self, scan_pts):
        """Match the current scan against finished submaps (excluding the last)."""
        best_score = 0.0
        best_submap = None
        best_pose = self.pose
        for sm in self.submaps[:-1]:
            pose, score = CorrelativeScanMatcher.match(
                scan_pts, sm, self.pose, dth_deg=10.0, dxy=0.3, fine=True)
            if score > best_score:
                best_score = score
                best_submap = sm
                best_pose = pose
        if best_submap is not None and best_score > MIN_SCORE * pts_len(scan_pts):
            # loop detected -> correct the drift (snap the pose to the matched pose)
            self.pose = best_pose
            self.loop_count += 1
            print("[loop] closed: score=%.1f pose=(%.2f,%.2f,%.1f)" %
                  (best_score, best_pose[0], best_pose[1], math.degrees(best_pose[2])),
                  flush=True)

    def merge_map(self, out_grid, cx, cy, res=0.02):
        """Rasterize all submaps into an output occupancy grid (vectorized)."""
        g = np.full(out_grid.shape, 128, np.uint8)
        for sm in self.submaps + ([self.active] if self.active else []):
            ys, xs = np.where(sm.grid[::2, ::2] > 0.45)
            if xs.size == 0:
                continue
            lx = (xs * 2 - SUBMAP_SIZE / 2.0) * RESOLUTION
            ly = (ys * 2 - SUBMAP_SIZE / 2.0) * RESOLUTION
            sx, sy, sth = sm.pose
            wx = sx + math.cos(sth) * lx - math.sin(sth) * ly
            wy = sy + math.sin(sth) * lx + math.cos(sth) * ly
            gx = np.round(wx / res).astype(np.int32) + cx
            gy = np.round(wy / res).astype(np.int32) + cy
            ok = (gx >= 0) & (gx < g.shape[1]) & (gy >= 0) & (gy < g.shape[0])
            g[gy[ok], gx[ok]] = 0
        return g


def pts_len(scan_pts):
    return np.asarray(scan_pts).reshape(-1, 2).shape[0]
