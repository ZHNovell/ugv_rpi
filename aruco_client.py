"""
Клиент для ArUco-детекции (OpenCV CPU, не NPU).

Детектирует ArUco-маркеры в кадре, возвращает список:
    [{'id': 0, 'x': 0.20, 'y': 0.01, 'z': 1.37, 'dist': 1.38, 'corners': [...]}, ...]

Фильтр по ID: только 0-3 (наши маркеры).
"""
import cv2
import cv2.aruco as aruco
import numpy as np


# Настройки
ARUCO_DICT_ID = aruco.DICT_4X4_50
MARKER_SIZE_M = 0.146      # 14.6 см (чёрный квадрат)
FOCAL_LENGTH = 1041.7      # откалибровано по ArUco (14.6 см на 1.355 м)
ALLOWED_IDS = {0, 1, 2, 3} # фильтр: только наши маркеры


class ArucoClient:
    def __init__(self, frame_w=1280, frame_h=720):
        self.frame_w = frame_w
        self.frame_h = frame_h

        # ArUco
        self.aruco_dict = aruco.getPredefinedDictionary(ARUCO_DICT_ID)
        self.params = aruco.DetectorParameters()
        self.detector = aruco.ArucoDetector(self.aruco_dict, self.params)

        # Матрица камеры (откалибрована)
        center = (frame_w / 2, frame_h / 2)
        self.camera_matrix = np.array([
            [FOCAL_LENGTH, 0, center[0]],
            [0, FOCAL_LENGTH, center[1]],
            [0, 0, 1]
        ], dtype=np.float64)
        self.dist_coeffs = np.zeros((5, 1))

        # 3D-координаты углов маркера (центр — начало координат)
        half = MARKER_SIZE_M / 2
        self.obj_points = np.array([
            [-half,  half, 0],
            [ half,  half, 0],
            [ half, -half, 0],
            [-half, -half, 0]
        ], dtype=np.float32)

    def detect(self, frame):
        """
        Детектирует ArUco-маркеры в кадре.
        Возвращает список dict:
            [{'id': 0, 'x': 0.20, 'y': 0.01, 'z': 1.37, 'dist': 1.38}, ...]
        """
        if frame is None:
            return []

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        corners, ids, rejected = self.detector.detectMarkers(gray)

        if ids is None:
            return []

        results = []
        for i, mid in enumerate(ids.flatten()):
            if mid not in ALLOWED_IDS:
                continue

            # Оценка позы
            ok, rvec, tvec = cv2.solvePnP(
                self.obj_points,
                corners[i][0],
                self.camera_matrix,
                self.dist_coeffs,
                flags=cv2.SOLVEPNP_IPPE_SQUARE
            )
            if not ok:
                continue

            x, y, z = tvec.flatten()
            dist = float(np.linalg.norm(tvec))

            results.append({
                'id': int(mid),
                'x': float(x),
                'y': float(y),
                'z': float(z),
                'dist': dist,
                'corners': corners[i][0].tolist(),
                'rvec': rvec.flatten().tolist(),
                'tvec': tvec.flatten().tolist(),
            })

        return results

    def draw(self, frame, detections):
        """Рисует детекции на кадре."""
        if not detections:
            return frame

        for det in detections:
            # Углы
            pts = np.array(det['corners'], dtype=np.int32)
            cv2.polylines(frame, [pts], True, (0, 255, 0), 2)

            # Оси
            rvec = np.array(det['rvec'])
            tvec = np.array(det['tvec'])
            cv2.drawFrameAxes(frame, self.camera_matrix, self.dist_coeffs, rvec, tvec, MARKER_SIZE_M * 0.5)

            # Подпись
            c = pts.mean(axis=0).astype(int)
            label = f"ID={det['id']} d={det['dist']:.2f}m"
            cv2.putText(frame, label, (c[0] - 60, c[1] - 20),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)

        return frame


# === Тест ===
if __name__ == '__main__':
    import sys
    import time

    cap = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("ERROR: cannot open camera")
        sys.exit(1)

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    cap.set(cv2.CAP_PROP_FPS, 30)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    client = ArucoClient()

    print("Testing ArUco client. Press Ctrl+C to stop.")
    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                break
            dets = client.detect(frame)
            if dets:
                for d in dets:
                    print(f"ID={d['id']}: X={d['x']:+.3f} Y={d['y']:+.3f} Z={d['z']:+.3f} dist={d['dist']:.3f}m")
            else:
                print("No markers")
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
