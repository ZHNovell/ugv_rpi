"""
RF2O: Range Flow-based 2D LiDAR Odometry (ICRA'16)
Ported from rf2o_laser_odometry (MAPIR/Javier G. Monroy) to pure Python/numpy.
Estimates the planar motion (dx, dy, dtheta) between consecutive range scans.
"""
import numpy as np


class RF2O:
    def __init__(self, num_points, fov_rad=2.0 * np.pi, ctf_levels=5, iter_irls=5, fps=1.0):
        self.width = num_points
        self.fovh = float(fov_rad)
        self.ctf_levels = ctf_levels
        self.iter_irls = iter_irls
        self.fps = float(fps)
        self.cols = num_points
        self.g_mask = np.array([1.0 / 16.0, 1.0 / 4.0, 6.0 / 16.0, 1.0 / 4.0, 1.0 / 16.0], np.float32)

        self.pose = np.eye(3, dtype=np.float32)          # accumulated robot pose (2D homogeneous)
        self.kai_loc_old = np.zeros(3, np.float32)  # previous filtered velocity

        self.pyr_levels = int(round(np.log2(round(float(self.width) / float(self.cols))))) + ctf_levels

        self.range = []; self.range_old = []
        self.xx = []; self.xx_old = []
        self.yy = []; self.yy_old = []
        self.range_warped = []; self.xx_warped = []; self.yy_warped = []
        self.range_inter = []; self.xx_inter = []; self.yy_inter = []
        for i in range(self.pyr_levels):
            s = 2 ** i
            cols_i = int(np.ceil(self.width / float(s)))
            self.range.append(np.zeros(cols_i, np.float32))
            self.range_old.append(np.zeros(cols_i, np.float32))
            self.xx.append(np.zeros(cols_i, np.float32)); self.xx_old.append(np.zeros(cols_i, np.float32))
            self.yy.append(np.zeros(cols_i, np.float32)); self.yy_old.append(np.zeros(cols_i, np.float32))
            self.range_warped.append(np.zeros(cols_i, np.float32))
            self.xx_warped.append(np.zeros(cols_i, np.float32))
            self.yy_warped.append(np.zeros(cols_i, np.float32))
            self.range_inter.append(np.zeros(cols_i, np.float32))
            self.xx_inter.append(np.zeros(cols_i, np.float32))
            self.yy_inter.append(np.zeros(cols_i, np.float32))

        self.transformations = [np.eye(3, dtype=np.float32) for _ in range(ctf_levels)]
        self.dt = np.zeros(self.width, np.float32)
        self.dtita = np.zeros(self.width, np.float32)
        self.weights = np.zeros(self.width, np.float32)
        self.null = np.zeros(self.width, np.int32)
        self.rtita = np.zeros(self.width, np.float32)
        self.module_initialized = False

    # ------------------------------------------------------------------ #
    def odometry(self, ranges):
        """Feed a new scan (1D array of ranges). Returns (dx, dy, dtheta) increment."""
        ranges = np.asarray(ranges, np.float32).ravel()
        if ranges.shape[0] != self.width:
            raise ValueError("scan size mismatch")

        if not self.module_initialized:
            # first scan: just store it, no motion
            self.create_pyramid(ranges)
            self.module_initialized = True
            return 0.0, 0.0, 0.0

        self.create_pyramid(ranges)

        for level in range(self.ctf_levels):
            self.transformations[level].fill(0.0)
            np.fill_diagonal(self.transformations[level], 1.0)

            s = 2 ** (self.ctf_levels - (level + 1))
            cols_i = int(np.ceil(self.cols / float(s)))
            image_level = self.ctf_levels - level + int(round(np.log2(round(float(self.width) / float(self.cols))))) - 1
            self.cols_i = cols_i
            self.image_level = image_level
            self.level = level

            if level == 0:
                self.range_warped[image_level] = self.range[image_level].copy()
                self.xx_warped[image_level] = self.xx[image_level].copy()
                self.yy_warped[image_level] = self.yy[image_level].copy()
            else:
                self.perform_warping()

            self.calculate_coord()
            self.find_null_points()
            self.range_derivatives()
            self.compute_weights()

            if self.num_valid_range > 3:
                self.solve_nonlinear()
            else:
                continue

            if not self.filter_solution():
                return 0.0, 0.0, 0.0

        self.pose_update()
        # extract the increment (translation + rotation)
        dx = self.last_increment[0, 2]
        dy = self.last_increment[1, 2]
        dtheta = np.arctan2(self.last_increment[1, 0], self.last_increment[0, 0])
        return float(dx), float(dy), float(dtheta)

    # ------------------------------------------------------------------ #
    def create_pyramid(self, range_wf):
        # push back
        self.range_old, self.range = self.range, self.range_old
        self.xx_old, self.xx = self.xx, self.xx_old
        self.yy_old, self.yy = self.yy, self.yy_old

        max_range_dif = 0.3

        for i in range(self.pyr_levels):
            s = 2 ** i
            cols_i = int(np.ceil(self.width / float(s)))
            r = self.range[i]
            if i == 0:
                for u in range(cols_i):
                    dcenter = range_wf[u]
                    if 1 < u < cols_i - 2:
                        if np.isfinite(dcenter) and dcenter > 0.0:
                            total = 0.0; w = 0.0
                            for l in range(-2, 3):
                                ad = abs(range_wf[u + l] - dcenter)
                                if ad < max_range_dif:
                                    aw = self.g_mask[2 + l] * (max_range_dif - ad)
                                    w += aw; total += aw * range_wf[u + l]
                            r[u] = total / w if w > 0 else 0.0
                        else:
                            r[u] = 0.0
                    else:
                        if np.isfinite(dcenter) and dcenter > 0.0:
                            total = 0.0; w = 0.0
                            for l in range(-2, 3):
                                indu = u + l
                                if 0 <= indu < cols_i:
                                    ad = abs(range_wf[indu] - dcenter)
                                    if ad < max_range_dif:
                                        aw = self.g_mask[2 + l] * (max_range_dif - ad)
                                        w += aw; total += aw * range_wf[indu]
                            r[u] = total / w if w > 0 else 0.0
                        else:
                            r[u] = 0.0
            else:
                r_prev = self.range[i - 1]
                cols_prev = len(r_prev)
                for u in range(cols_i):
                    u2 = 2 * u
                    dcenter = r_prev[u2] if u2 < cols_prev else 0.0
                    if 0 < u < cols_i - 1:
                        if dcenter > 0.0:
                            total = 0.0; w = 0.0
                            for l in range(-2, 3):
                                indu = u2 + l
                                if 0 <= indu < cols_prev:
                                    ad = abs(r_prev[indu] - dcenter)
                                    if ad < max_range_dif:
                                        aw = self.g_mask[2 + l] * (max_range_dif - ad)
                                        w += aw; total += aw * r_prev[indu]
                            r[u] = total / w if w > 0 else 0.0
                        else:
                            r[u] = 0.0
                    else:
                        if dcenter > 0.0:
                            total = 0.0; w = 0.0
                            for l in range(-2, 3):
                                indu = u2 + l
                                if 0 <= indu < cols_prev:
                                    ad = abs(r_prev[indu] - dcenter)
                                    if ad < max_range_dif:
                                        aw = self.g_mask[2 + l] * (max_range_dif - ad)
                                        w += aw; total += aw * r_prev[indu]
                            r[u] = total / w if w > 0 else 0.0
                        else:
                            r[u] = 0.0
            # compute xy coords
            xx = self.xx[i]; yy = self.yy[i]
            for u in range(cols_i):
                if r[u] > 0.0:
                    tita = -0.5 * self.fovh + u * self.fovh / float(cols_i - 1)
                    xx[u] = r[u] * np.cos(tita)
                    yy[u] = r[u] * np.sin(tita)
                else:
                    xx[u] = 0.0; yy[u] = 0.0

    # ------------------------------------------------------------------ #
    def perform_warping(self):
        il = self.image_level
        acu_trans = np.eye(3, dtype=np.float32)
        for i in range(1, self.level + 1):
            acu_trans = self.transformations[i - 1] @ acu_trans

        cols_i = self.cols_i
        wacu = np.zeros(cols_i, np.float32)
        self.range_warped[il].fill(0.0)
        cols_lim = float(cols_i - 1)
        kdtita = cols_lim / self.fovh

        for j in range(cols_i):
            if self.range[il][j] > 0.0:
                x_w = acu_trans[0, 0] * self.xx[il][j] + acu_trans[0, 1] * self.yy[il][j] + acu_trans[0, 2]
                y_w = acu_trans[1, 0] * self.xx[il][j] + acu_trans[1, 1] * self.yy[il][j] + acu_trans[1, 2]
                tita_w = np.arctan2(y_w, x_w)
                range_w = np.sqrt(x_w * x_w + y_w * y_w)
                uwarp = kdtita * (tita_w + 0.5 * self.fovh)
                if 0.0 <= uwarp < cols_lim:
                    uwarp_l = int(uwarp)
                    uwarp_r = uwarp_l + 1
                    delta_r = float(uwarp_r) - uwarp
                    delta_l = uwarp - float(uwarp_l)
                    if abs(round(uwarp) - uwarp) < 0.05:
                        idx = int(round(uwarp))
                        self.range_warped[il][idx] += range_w
                        wacu[idx] += 1.0
                    else:
                        wr = delta_l * delta_l
                        self.range_warped[il][uwarp_r] += wr * range_w
                        wacu[uwarp_r] += wr
                        wl = delta_r * delta_r
                        self.range_warped[il][uwarp_l] += wl * range_w
                        wacu[uwarp_l] += wl

        for u in range(cols_i):
            if wacu[u] > 0.0:
                tita = -0.5 * self.fovh + u / kdtita
                self.range_warped[il][u] /= wacu[u]
                self.xx_warped[il][u] = self.range_warped[il][u] * np.cos(tita)
                self.yy_warped[il][u] = self.range_warped[il][u] * np.sin(tita)
            else:
                self.range_warped[il][u] = 0.0
                self.xx_warped[il][u] = 0.0
                self.yy_warped[il][u] = 0.0

    # ------------------------------------------------------------------ #
    def calculate_coord(self):
        il = self.image_level
        for u in range(self.cols_i):
            if self.range_old[il][u] == 0.0 or self.range_warped[il][u] == 0.0:
                self.range_inter[il][u] = 0.0
                self.xx_inter[il][u] = 0.0
                self.yy_inter[il][u] = 0.0
            else:
                self.range_inter[il][u] = 0.5 * (self.range_old[il][u] + self.range_warped[il][u])
                self.xx_inter[il][u] = 0.5 * (self.xx_old[il][u] + self.xx_warped[il][u])
                self.yy_inter[il][u] = 0.5 * (self.yy_old[il][u] + self.yy_warped[il][u])

    # ------------------------------------------------------------------ #
    def find_null_points(self):
        il = self.image_level
        self.num_valid_range = 0
        for u in range(1, self.cols_i - 1):
            if self.range_inter[il][u] == 0.0:
                self.null[u] = 1
            else:
                self.num_valid_range += 1
                self.null[u] = 0

    # ------------------------------------------------------------------ #
    def range_derivatives(self):
        il = self.image_level
        cols_i = self.cols_i
        self.rtita.fill(1.0)
        for u in range(cols_i - 1):
            dista = self.xx_inter[il][u + 1] - self.xx_inter[il][u]
            dista *= dista
            distb = self.yy_inter[il][u + 1] - self.yy_inter[il][u]
            distb *= distb
            dist = dista + distb
            if dist > 0.0:
                self.rtita[u] = np.sqrt(dist)

        for u in range(1, cols_i - 1):
            self.dtita[u] = (self.rtita[u - 1] * (self.range_inter[il][u + 1] - self.range_inter[il][u]) +
                             self.rtita[u] * (self.range_inter[il][u] - self.range_inter[il][u - 1])) / \
                            (self.rtita[u] + self.rtita[u - 1])
        self.dtita[0] = self.dtita[1]
        self.dtita[cols_i - 1] = self.dtita[cols_i - 2]

        for u in range(cols_i):
            self.dt[u] = self.fps * (self.range_warped[il][u] - self.range_old[il][u])

    # ------------------------------------------------------------------ #
    def compute_weights(self):
        il = self.image_level
        cols_i = self.cols_i
        self.weights.fill(0.0)
        kdtita = 1.0
        kdt = kdtita / (self.fps * self.fps)
        k2d = 0.2
        for u in range(1, cols_i - 1):
            if self.null[u] == 0:
                ini_dtita = self.range_old[il][u + 1] - self.range_old[il][u - 1]
                final_dtita = self.range_warped[il][u + 1] - self.range_warped[il][u - 1]
                dtitat = ini_dtita - final_dtita
                dtita2 = self.dtita[u + 1] - self.dtita[u - 1]
                w_der = kdt * (self.dt[u] * self.dt[u]) + \
                        kdtita * (self.dtita[u] * self.dtita[u]) + \
                        k2d * (abs(dtitat) + abs(dtita2))
                if w_der > 0:
                    self.weights[u] = np.sqrt(1.0 / w_der)
        mx = self.weights.max()
        if mx > 0:
            self.weights = self.weights / mx

    # ------------------------------------------------------------------ #
    def _fill_system(self, A, B, cont, kdtita):
        il = self.image_level
        for u in range(1, self.cols_i - 1):
            if self.null[u] == 0:
                tw = self.weights[u]
                tita = -0.5 * self.fovh + u / kdtita
                ri = self.range_inter[il][u]
                A[cont, 0] = tw * (np.cos(tita) + self.dtita[u] * kdtita * np.sin(tita) / ri)
                A[cont, 1] = tw * (np.sin(tita) - self.dtita[u] * kdtita * np.cos(tita) / ri)
                A[cont, 2] = tw * (-self.yy[il][u] * np.cos(tita) + self.xx[il][u] * np.sin(tita) - self.dtita[u] * kdtita)
                B[cont, 0] = tw * (-self.dt[u])
                cont += 1
        return cont

    def solve_nonlinear(self):
        il = self.image_level
        cols_i = self.cols_i
        kdtita = float(cols_i - 1) / self.fovh
        n = self.num_valid_range
        A = np.zeros((n, 3), np.float32)
        B = np.zeros((n, 1), np.float32)
        self._fill_system(A, B, 0, kdtita)

        AtA = A.T @ A
        AtB = A.T @ B
        Var = np.linalg.solve(AtA + 1e-6 * np.eye(3), AtB).ravel()
        res = (A @ Var.reshape(3, 1) - B).ravel()

        # average dt for the Cauchy constant
        aver_dt = np.mean(np.abs(self.dt[1:cols_i - 1][self.null[1:cols_i - 1] == 0]))
        if aver_dt < 1e-6:
            aver_dt = 1e-6
        k = 10.0 / aver_dt

        for _ in range(self.iter_irls):
            res_weight = np.sqrt(1.0 / (1.0 + (k * res) * (k * res)))
            Aw = A * res_weight[:, None]
            Bw = B * res_weight[:, None]
            AtA = Aw.T @ Aw
            AtB = Aw.T @ Bw
            Var = np.linalg.solve(AtA + 1e-6 * np.eye(3), AtB).ravel()
            res = (A @ Var.reshape(3, 1) - B).ravel()

        self.cov_odo = (1.0 / (n - 3)) * np.linalg.inv(AtA) * (res @ res)
        self.kai_loc_level = Var

    # ------------------------------------------------------------------ #
    def filter_solution(self):
        cov = self.cov_odo
        try:
            eigvals, eigvecs = np.linalg.eigh(cov)
        except np.linalg.LinAlgError:
            return False
        Bii = eigvecs
        kai_b = np.linalg.solve(Bii, self.kai_loc_level)

        acu_trans = np.eye(3, dtype=np.float32)
        for i in range(self.level):
            acu_trans = self.transformations[i] @ acu_trans

        kai_loc_sub = np.zeros(3, np.float32)
        kai_loc_sub[0] = -self.fps * acu_trans[0, 2]
        kai_loc_sub[1] = -self.fps * acu_trans[1, 2]
        if acu_trans[0, 0] > 1.0:
            kai_loc_sub[2] = 0.0
        else:
            kai_loc_sub[2] = -self.fps * np.arccos(np.clip(acu_trans[0, 0], -1, 1)) * np.sign(acu_trans[1, 0])
        kai_loc_sub += self.kai_loc_old

        kai_b_old = np.linalg.solve(Bii, kai_loc_sub)

        cf = 15e3 * np.exp(-float(self.level))
        df = 0.05 * np.exp(-float(self.level))
        kai_b_fil = np.zeros(3, np.float32)
        for i in range(3):
            kai_b_fil[i] = (kai_b[i] + (cf * eigvals[i] + df) * kai_b_old[i]) / (1.0 + cf * eigvals[i] + df)

        kai_loc_fil = np.linalg.solve(Bii, kai_b_fil)

        incrx = kai_loc_fil[0] / self.fps
        incry = kai_loc_fil[1] / self.fps
        rot = kai_loc_fil[2] / self.fps

        T = self.transformations[self.level]
        T[0, 0] = np.cos(rot); T[0, 1] = -np.sin(rot); T[0, 2] = incrx
        T[1, 0] = np.sin(rot); T[1, 1] = np.cos(rot); T[1, 2] = incry
        T[2, 0] = 0.0; T[2, 1] = 0.0; T[2, 2] = 1.0
        return True

    # ------------------------------------------------------------------ #
    def pose_update(self):
        acu_trans = np.eye(3, dtype=np.float32)
        for i in range(1, self.ctf_levels + 1):
            acu_trans = self.transformations[i - 1] @ acu_trans

        self.kai_loc = np.zeros(3, np.float32)
        self.kai_loc[0] = self.fps * acu_trans[0, 2]
        self.kai_loc[1] = self.fps * acu_trans[1, 2]
        if acu_trans[0, 0] > 1.0:
            self.kai_loc[2] = 0.0
        else:
            self.kai_loc[2] = self.fps * np.arccos(np.clip(acu_trans[0, 0], -1, 1)) * np.sign(acu_trans[1, 0])

        # increment pose (2D)
        rot = self.kai_loc[2] / self.fps
        inc = np.eye(3, dtype=np.float32)
        inc[0, 0] = np.cos(rot); inc[0, 1] = -np.sin(rot); inc[0, 2] = acu_trans[0, 2]
        inc[1, 0] = np.sin(rot); inc[1, 1] = np.cos(rot); inc[1, 2] = acu_trans[1, 2]
        self.pose = self.pose @ inc
        self.last_increment = inc

        # update kai_loc_old
        phi = np.arctan2(self.pose[1, 0], self.pose[0, 0])
        self.kai_loc_old[0] = self.kai_loc[0] * np.cos(phi) + self.kai_loc[1] * np.sin(phi)
        self.kai_loc_old[1] = -self.kai_loc[0] * np.sin(phi) + self.kai_loc[1] * np.cos(phi)
        self.kai_loc_old[2] = self.kai_loc[2]
