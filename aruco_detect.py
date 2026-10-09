"""
Детекция ArUco-маркеров с камеры в реальном времени.

Использование:
    python3 aruco_detect.py [--display] [--save-image]

Опции:
    --display    — показывать окно с видео (требует X11 или VNC)
    --save-image — сохранить кадр с детекцией в /tmp/aruco_frame.png
"""
import cv2
import cv2.aruco as aruco
import numpy as np
import sys
import time

# Настройки
SOCKET_PATH = "/tmp/npu11.sock"  # не используется, но для совместимости
ARUCO_DICT_ID = aruco.DICT_4X4_50
MARKER_SIZE_M = 0.146  # 10 см — реальный размер маркера (в метрах)
CAMERA_ID = 0
FRAME_W = 1280
FRAME_H = 720


def main():
    display = "--display" in sys.argv
    save_image = "--save-image" in sys.argv

    # Открываем камеру
    cap = cv2.VideoCapture(CAMERA_ID, cv2.CAP_V4L2)
    if not cap.isOpened():
        print(f"ERROR: cannot open camera {CAMERA_ID}")
        return 1

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_W)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_H)
    cap.set(cv2.CAP_PROP_FPS, 30)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    print(f"Camera: {cap.get(cv2.CAP_PROP_FRAME_WIDTH)}x{cap.get(cv2.CAP_PROP_FRAME_HEIGHT)} @ {cap.get(cv2.CAP_PROP_FPS)} fps")

    # ArUco
    aruco_dict = aruco.getPredefinedDictionary(ARUCO_DICT_ID)
    params = aruco.DetectorParameters()
    detector = aruco.ArucoDetector(aruco_dict, params)

    # Калибровка камеры (упрощённая — без реальной калибровки)
    # fx, fy ~ фокусное расстояние в пикселях, cx, cy ~ центр
    focal_length = 1041.7  # откалибровано по ArUco (14.6 см на 1.355 м)
    center = (FRAME_W / 2, FRAME_H / 2)
    camera_matrix = np.array([
        [focal_length, 0, center[0]],
        [0, focal_length, center[1]],
        [0, 0, 1]
    ], dtype=np.float64)
    dist_coeffs = np.zeros((5, 1))  # без дисторсии (для теста)

    # 3D-координаты углов маркера (в метрах, центр маркера — начало координат)
    half = MARKER_SIZE_M / 2
    obj_points = np.array([
        [-half,  half, 0],
        [ half,  half, 0],
        [ half, -half, 0],
        [-half, -half, 0]
    ], dtype=np.float32)

    frame_count = 0
    start_time = time.time()
    fps = 0

    print("Press Ctrl+C to stop. Press 'q' if display is on.")

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("Frame read failed")
                break

            frame_count += 1
            if frame_count % 30 == 0:
                elapsed = time.time() - start_time
                fps = 30 / elapsed
                start_time = time.time()

            # Детекция
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            corners, ids, rejected = detector.detectMarkers(gray)

            if ids is not None:
                aruco.drawDetectedMarkers(frame, corners, ids)

                for i, mid in enumerate(ids.flatten()):
                    # Оценка позы (position + rotation)
                    ok, rvec, tvec = cv2.solvePnP(
                        obj_points,
                        corners[i][0],
                        camera_matrix,
                        dist_coeffs,
                        flags=cv2.SOLVEPNP_IPPE_SQUARE
                    )
                    if ok:
                        x, y, z = tvec.flatten()
                        # Расстояние
                        dist = np.linalg.norm(tvec)
                        print(f"ID={mid}: X={x:+.3f}m Y={y:+.3f}m Z={z:+.3f}m dist={dist:.3f}m fps={fps:.1f}")

                        # Рисуем оси
                        cv2.drawFrameAxes(frame, camera_matrix, dist_coeffs, rvec, tvec, MARKER_SIZE_M * 0.5)

                        # Подпись
                        label = f"ID={mid} d={dist:.2f}m"
                        c = corners[i][0].mean(axis=0).astype(int)
                        cv2.putText(frame, label, (c[0] - 60, c[1] - 20),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
            else:
                # Раз в 30 кадров пишем, что ничего нет
                if frame_count % 30 == 0:
                    print(f"No markers. fps={fps:.1f}")

            if save_image and frame_count % 30 == 0:
                cv2.imwrite('/tmp/aruco_frame.png', frame)

            if display:
                cv2.imshow('ArUco', frame)
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break

    except KeyboardInterrupt:
        print("\nStopped by user")
    finally:
        cap.release()
        if display:
            cv2.destroyAllWindows()

    return 0


if __name__ == "__main__":
    sys.exit(main())
