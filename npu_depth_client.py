"""
Клиент для NPU depth-сервера (YOLO26n-depth на A733).

Отправляет JPEG-кадр по UNIX-сокету, получает JSON с картой глубины:
  - grid: сетка GRID_H x GRID_W средних значений глубины (метры).
  - near_rle: RLE-сжатая маска пикселей, где depth < near_threshold.

Протокол:
    Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
    Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]
"""
import socket
import struct
import json
import time
import cv2


SOCKET_PATH = "/tmp/npu_depth.sock"


class NPUDepthClient:
    def __init__(self, socket_path=SOCKET_PATH, timeout=2.0):
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = None
        self._connect()

    def _connect(self):
        attempts = 0
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(self.timeout)
                self.sock.connect(self.socket_path)
                print(f"[NPUDepthClient] Connected to {self.socket_path}", flush=True)
                return
            except (FileNotFoundError, ConnectionRefusedError) as e:
                attempts += 1
                if attempts % 10 == 1:
                    print(f"[NPUDepthClient] Waiting for server ({e})...", flush=True)
                time.sleep(0.5)

    def detect(self, frame):
        """
        Отправляет кадр (np.array BGR) в depth-сервер.
        Возвращает dict:
            {'grid_size': [W,H], 'grid': [256 float-значений],
             'near_threshold': 2.0, 'near_mask_size': [W,H], 'near_rle': [[v,c],...]}
        """
        ok, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
        if not ok:
            return {}

        jpeg_bytes = jpeg.tobytes()
        jpeg_len = len(jpeg_bytes)

        try:
            self.sock.sendall(struct.pack('>I', jpeg_len))
            self.sock.sendall(jpeg_bytes)

            json_len_data = self._recv_n(4)
            if len(json_len_data) < 4:
                return {}
            json_len = struct.unpack('>I', json_len_data)[0]
            if json_len == 0:
                return {}

            json_data = self._recv_n(json_len)
            if len(json_data) < json_len:
                return {}

            return json.loads(json_data.decode('utf-8'))

        except (socket.timeout, BrokenPipeError, ConnectionResetError) as e:
            print(f"[NPUDepthClient] Connection error: {e}, reconnecting...", flush=True)
            self._reconnect()
            return {}

    def _recv_n(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                break
            data += chunk
        return data

    def _reconnect(self):
        try:
            self.sock.close()
        except Exception:
            pass
        self._connect()

    def close(self):
        if self.sock:
            self.sock.close()


# === Тест ===
if __name__ == '__main__':
    import sys
    import numpy as np

    img_path = sys.argv[1] if len(sys.argv) > 1 else \
        "/root/ugv_rpi/yolo26_depth/model/rgb_00285.jpg"

    frame = cv2.imread(img_path)
    if frame is None:
        print(f"Cannot read {img_path}")
        sys.exit(1)

    print(f"Testing depth on {img_path} ({frame.shape})...")
    client = NPUDepthClient()
    t0 = time.time()
    result = client.detect(frame)
    t1 = time.time()

    if not result:
        print("Empty result")
        sys.exit(1)

    grid = result.get('grid', [])
    near_rle = result.get('near_rle', [])
    total_near = sum(c for v, c in near_rle) if near_rle else 0

    print(f"Detection time: {(t1-t0)*1000:.1f} ms")
    print(f"Grid size: {result.get('grid_size')}")
    print(f"Grid values (min/max/mean): "
          f"{min(grid):.2f} / {max(grid):.2f} / {sum(grid)/len(grid):.2f} m")
    print(f"Near threshold: {result.get('near_threshold')} m")
    print(f"Near pixels: {total_near} ({100.0*total_near/(frame.shape[0]*frame.shape[1]):.1f}%)")
