"""
Клиент для NPU-сервера (YOLO11s на A733).

Отправляет JPEG-кадр по UNIX-сокету, получает JSON со списком детекций.

Протокол:
    Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
    Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]
"""
import socket
import struct
import json
import time
import numpy as np
import cv2


SOCKET_PATH = "/tmp/npu11.sock"


class NPUClient:
    def __init__(self, socket_path=SOCKET_PATH, timeout=2.0):
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = None
        self._connect()

    def _connect(self):
        """Подключается к серверу. Если не удалось — ждёт."""
        attempts = 0
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(self.timeout)
                self.sock.connect(self.socket_path)
                print(f"[NPUClient] Connected to {self.socket_path}", flush=True)
                return
            except (FileNotFoundError, ConnectionRefusedError) as e:
                attempts += 1
                if attempts % 10 == 1:
                    print(f"[NPUClient] Waiting for server ({e})...", flush=True)
                time.sleep(0.5)

    def detect(self, frame):
        """
        Отправляет кадр (np.array BGR) в NPU-сервер.
        Возвращает список детекций:
            [{'class': 'dog', 'class_id': 16, 'confidence': 0.92, 'bbox': [x0,y0,x1,y1]}, ...]
        """
        # Кодируем кадр в JPEG
        ok, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
        if not ok:
            return []

        jpeg_bytes = jpeg.tobytes()
        jpeg_len = len(jpeg_bytes)

        try:
            # Отправляем длину + JPEG
            self.sock.sendall(struct.pack('>I', jpeg_len))
            self.sock.sendall(jpeg_bytes)

            # Читаем длину JSON
            json_len_data = self._recv_n(4)
            if len(json_len_data) < 4:
                return []
            json_len = struct.unpack('>I', json_len_data)[0]
            if json_len == 0:
                return []

            # Читаем JSON
            json_data = self._recv_n(json_len)
            if len(json_data) < json_len:
                return []

            detections = json.loads(json_data.decode('utf-8'))
            return detections

        except (socket.timeout, BrokenPipeError, ConnectionResetError) as e:
            print(f"[NPUClient] Connection error: {e}, reconnecting...", flush=True)
            self._reconnect()
            return []

    def _recv_n(self, n):
        """Читает ровно n байт."""
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

    img_path = sys.argv[1] if len(sys.argv) > 1 else \
        "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolo11/model/dog.jpg"

    frame = cv2.imread(img_path)
    if frame is None:
        print(f"Cannot read {img_path}")
        sys.exit(1)

    print(f"Testing on {img_path} ({frame.shape})...")
    client = NPUClient()
    t0 = time.time()
    dets = client.detect(frame)
    t1 = time.time()

    print(f"Detection time: {(t1-t0)*1000:.1f} ms")
    print(f"Found {len(dets)} detections:")
    for d in dets:
        print(f"  {d['class']}: {d['confidence']*100:.0f}% at {d['bbox']}")
