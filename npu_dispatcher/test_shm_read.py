#!/usr/bin/env python3
# Чтение BGR из SHM /ugv_bgr_shm
import mmap
import struct
import time
import cv2
import numpy as np
import os

SHM_NAME = "/dev/shm/ugv_bgr_shm"
SHM_SLOTS = 4
HEADER_SIZE = 64
INPUT_W = 1920
INPUT_H = 1088
BGR_SIZE = INPUT_W * INPUT_H * 3
SHM_SIZE = HEADER_SIZE + SHM_SLOTS * BGR_SIZE

def open_shm():
    fd = os.open(SHM_NAME, os.O_RDWR)
    mm = mmap.mmap(fd, SHM_SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    return fd, mm

def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    frame_id, width, height, channels, write_idx = struct.unpack('<QIIII', data[:24])
    return frame_id, width, height, channels, write_idx

def read_bgr(mm, write_idx):
    offset = HEADER_SIZE + write_idx * BGR_SIZE
    mm.seek(offset)
    data = mm.read(BGR_SIZE)
    return np.frombuffer(data, dtype=np.uint8).reshape((INPUT_H, INPUT_W, 3))

if __name__ == "__main__":
    print(f"=== SHM Reader ({SHM_NAME}) ===")
    fd, mm = open_shm()
    print(f"SHM opened: fd={fd}, size={SHM_SIZE}")

    last_frame_id = 0
    frames = 0
    t0 = time.time()

    try:
        while frames < 100:
            frame_id, width, height, channels, write_idx = read_header(mm)
            if frame_id == last_frame_id:
                time.sleep(0.001)
                continue
            last_frame_id = frame_id

            bgr = read_bgr(mm, write_idx)
            frames += 1

            if frames % 20 == 0:
                dt = time.time() - t0
                fps = frames / dt
                print(f"Frame {frames}: id={frame_id}, slot={write_idx}, "
                      f"bgr={bgr.shape}, mean={bgr.mean():.1f}, fps={fps:.1f}")

    except KeyboardInterrupt:
        print("Interrupted")

    dt = time.time() - t0
    print(f"\nTotal: {frames} frames in {dt:.2f}s → {frames/dt:.1f} FPS")
    mm.close()
    os.close(fd)
    print("DONE")
