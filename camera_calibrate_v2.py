"""
Калибровка камеры БЕЗ GUI (headless).

Автоматически сохраняет кадры, когда доска видна.
Показывает счётчик в консоли.
"""
import cv2
import numpy as np
import os
import sys
import time


CHESSBOARD = (5, 8)
SQUARE_SIZE = 0.030  # ИЗМЕРЬ ЛИНЕЙКОЙ! (30 мм = 0.030 м)

SAVE_DIR = '/root/ugv_rpi/calib_frames'
OUTPUT = '/root/ugv_rpi/camera_calib.npz'

TARGET_FRAMES = 25
SAVE_INTERVAL = 1.5  # сек между сохранениями


def main():
    os.makedirs(SAVE_DIR, exist_ok=True)

    cap = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("ERROR: cannot open camera")
        return 1

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    cap.set(cv2.CAP_PROP_FPS, 30)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    print(f"Camera: 1280x720")
    print(f"Шахматная доска: {CHESSBOARD[0]}x{CHESSBOARD[1]} внутренних углов")
    print(f"Размер квадрата: {SQUARE_SIZE*1000:.1f} мм")
    print()
    print(f"Цель: {TARGET_FRAMES} кадров")
    print(f"Автосохранение: каждые {SAVE_INTERVAL} сек (если доска видна)")
    print()
    print("ДВИГАЙ ДОСКУ:")
    print("  - разные расстояния (близко/средне/далеко)")
    print("  - разные положения (центр/углы/края)")
    print("  - разные наклоны (влево/вправо/вверх/вниз/диагональ)")
    print()
    print("Ctrl+C для выхода (когда набрано 20+)")
    print()

    count = 0
    last_save = 0
    last_print = 0

    try:
        while count < TARGET_FRAMES:
            ret, frame = cap.read()
            if not ret:
                break

            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            ret_cb, corners = cv2.findChessboardCorners(gray, CHESSBOARD, None)

            now = time.time()

            # Периодический вывод статуса
            if now - last_print >= 0.5:
                status = "OK" if ret_cb else "no"
                print(f"\r[{count}/{TARGET_FRAMES}]  {status}  ", end='', flush=True)
                last_print = now

            # Автосохранение
            if ret_cb and (now - last_save) >= SAVE_INTERVAL:
                corners_refined = cv2.cornerSubPix(
                    gray, corners, (11, 11), (-1, -1),
                    (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
                )
                path = f'{SAVE_DIR}/frame_{count:02d}.png'
                cv2.imwrite(path, frame)
                count += 1
                last_save = now
                print(f"\r✓ Сохранён {path}  ({count}/{TARGET_FRAMES})", flush=True)
                time.sleep(0.5)  # пауза чтобы успеть подвинуть

    except KeyboardInterrupt:
        print("\n\nПрервано пользователем")

    cap.release()

    if count < 15:
        print(f"\n⚠️  Слишком мало кадров ({count}). Нужно 15+.")
        return 1

    print(f"\n\n=== Калибровка ({count} кадров) ===")

    objp = np.zeros((CHESSBOARD[0] * CHESSBOARD[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0:CHESSBOARD[0], 0:CHESSBOARD[1]].T.reshape(-1, 2)
    objp *= SQUARE_SIZE

    objpoints = []
    imgpoints = []
    h, w = None, None

    for i in range(count):
        path = f'{SAVE_DIR}/frame_{i:02d}.png'
        img = cv2.imread(path)
        if img is None:
            continue
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
    print(f"RMS reprojection error: {ret:.4f} (хорошо <1.0, отлично <0.5)")
    print(f"\ncamera_matrix:")
    print(mtx)
    print(f"\ndist_coeffs:")
    print(dist)

    total_error = 0
    for i in range(len(objpoints)):
        imgpoints2, _ = cv2.projectPoints(objpoints[i], rvecs[i], tvecs[i], mtx, dist)
        error = cv2.norm(imgpoints[i], imgpoints2, cv2.NORM_L2) / len(imgpoints2)
        total_error += error
    print(f"\nСредняя ошибка: {total_error / len(objpoints):.4f} px")

    np.savez(OUTPUT, camera_matrix=mtx, dist_coeffs=dist, rms=ret, frame_w=w, frame_h=h)
    print(f"\n✅ Сохранено: {OUTPUT}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
