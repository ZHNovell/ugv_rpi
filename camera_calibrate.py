"""
Калибровка камеры по шахматной доске.

1. Снимает 20-30 кадров (нажми ПРОБЕЛ для сохранения, ESC для выхода).
2. Прогоняет calibrateCamera.
3. Сохраняет camera_matrix + dist_coeffs в camera_calib.npz.
"""
import cv2
import numpy as np
import os
import sys


# Параметры шахматной доски (ИЗМЕРЬ ЛИНЕЙКОЙ!)
# Внутренние углы: 9x6 (для доски 10x7 квадратов)
CHESSBOARD = (9, 6)
SQUARE_SIZE = 0.030  # 30 мм = 0.030 м (ИЗМЕРЬ СВОЮ!)

SAVE_DIR = '/root/ugv_rpi/calib_frames'
OUTPUT = '/root/ugv_rpi/camera_calib.npz'


def main():
    os.makedirs(SAVE_DIR, exist_ok=True)

    # Открываем камеру
    cap = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("ERROR: cannot open camera")
        return 1

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    cap.set(cv2.CAP_PROP_FPS, 30)

    print(f"Camera: {cap.get(cv2.CAP_PROP_FRAME_WIDTH)}x{cap.get(cv2.CAP_PROP_FRAME_HEIGHT)}")
    print(f"Шахматная доска: {CHESSBOARD[0]}x{CHESSBOARD[1]} внутренних углов")
    print(f"Размер квадрата: {SQUARE_SIZE*1000:.1f} мм")
    print()
    print("УПРАВЛЕНИЕ:")
    print("  ПРОБЕЛ — сохранить кадр (когда доска видна полностью)")
    print("  ESC    — выход (когда набрано 20+ кадров)")
    print()
    print("Показывай доску с РАЗНЫХ ракурсов:")
    print("  - близко/далеко")
    print("  - наклон влево/вправо/вверх/вниз")
    print("  - в разных углах кадра")
    print()

    count = 0
    while True:
        ret, frame = cap.read()
        if not ret:
            break

        # Ищем углы
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        ret_cb, corners = cv2.findChessboardCorners(gray, CHESSBOARD, None)

        display = frame.copy()
        if ret_cb:
            # Уточняем углы
            corners_refined = cv2.cornerSubPix(
                gray, corners, (11, 11), (-1, -1),
                (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
            )
            cv2.drawChessboardCorners(display, CHESSBOARD, corners_refined, ret_cb)
            cv2.putText(display, f"OK! Press SPACE to save ({count} saved)",
                        (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
        else:
            cv2.putText(display, f"Chessboard NOT found ({count} saved)",
                        (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 255), 2)

        cv2.imshow('Calibration', display)
        key = cv2.waitKey(1) & 0xFF

        if key == ord(' ') and ret_cb:
            # Сохраняем
            path = f'{SAVE_DIR}/frame_{count:02d}.png'
            cv2.imwrite(path, frame)
            count += 1
            print(f"Сохранён: {path} ({count})")

        elif key == 27:  # ESC
            break

    cap.release()
    cv2.destroyAllWindows()

    if count < 20:
        print(f"\n⚠️  Слишком мало кадров ({count}). Нужно 20+.")
        return 1

    print(f"\n=== Калибровка ({count} кадров) ===")

    # Загружаем все кадры
    objp = np.zeros((CHESSBOARD[0] * CHESSBOARD[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0:CHESSBOARD[0], 0:CHESSBOARD[1]].T.reshape(-1, 2)
    objp *= SQUARE_SIZE

    objpoints = []
    imgpoints = []
    h, w = None, None

    for i in range(count):
        path = f'{SAVE_DIR}/frame_{i:02d}.png'
        img = cv2.imread(path)
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        h, w = gray.shape
        ret_cb, corners = cv2.findChessboardCorners(gray, CHESSBOARD, None)
        if ret_cb:
            corners_refined = cv2.cornerSubPix(
                gray, corners, (11, 11), (-1, -1),
                (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
            )
            objpoints.append(objp)
            imgpoints.append(corners_refined)

    print(f"Использовано кадров: {len(objpoints)}")

    ret, mtx, dist, rvecs, tvecs = cv2.calibrateCamera(
        objpoints, imgpoints, (w, h), None, None
    )

    print(f"\n=== Результаты ===")
    print(f"RMS reprojection error: {ret:.4f} (чем меньше, тем лучше; <1.0 — отлично)")
    print(f"\ncamera_matrix:")
    print(mtx)
    print(f"\ndist_coeffs:")
    print(dist)

    # Средняя ошибка
    total_error = 0
    for i in range(len(objpoints)):
        imgpoints2, _ = cv2.projectPoints(objpoints[i], rvecs[i], tvecs[i], mtx, dist)
        error = cv2.norm(imgpoints[i], imgpoints2, cv2.NORM_L2) / len(imgpoints2)
        total_error += error
    print(f"\nСредняя ошибка: {total_error / len(objpoints):.4f} px")

    # Сохраняем
    np.savez(OUTPUT, camera_matrix=mtx, dist_coeffs=dist, rms=ret)
    print(f"\n✅ Сохранено: {OUTPUT}")
    print(f"\nДля использования в aruco_client.py:")
    print(f"  CALIB_FOCAL_X = {mtx[0,0]:.2f}")
    print(f"  CALIB_FOCAL_Y = {mtx[1,1]:.2f}")
    print(f"  CALIB_CX = {mtx[0,2]:.2f}")
    print(f"  CALIB_CY = {mtx[1,2]:.2f}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
