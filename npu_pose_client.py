"""
Клиент для NPU pose-сервера (YOLO11_pose на A733).

Отправляет JPEG-кадр по UNIX-сокету, получает JSON со списком детекций
(bbox + class + confidence + keypoints[17]).

Протокол:
    Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
    Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]
"""
import socket
import struct
import json
import time
import cv2


SOCKET_PATH = "/tmp/npu_pose_raw.sock"


class NPUPoseClient:
    def __init__(self, socket_path=SOCKET_PATH, timeout=2.0):
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = None
        self._connect()

    def _connect(self):
        attempts = 0
        while attempts < 20:  # 20 × 0.5 = 10 секунд максимум
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(self.timeout)
                self.sock.connect(self.socket_path)
                print(f"[NPUPoseClient] Connected to {self.socket_path}", flush=True)
                return
            except (FileNotFoundError, ConnectionRefusedError) as e:
                attempts += 1
                if attempts % 10 == 1:
                    print(f"[NPUPoseClient] Waiting for server ({e})...", flush=True)
                time.sleep(0.5)
        raise ConnectionError(f"[NPUPoseClient] Server {self.socket_path} not available after 10s")

    def detect(self, frame):
        """
        Отправляет кадр (np.array BGR) в pose-сервер.
        Возвращает список детекций:
            [{'class': 'person', 'class_id': 0, 'confidence': 0.85, 'bbox': [x0,y0,x1,y1],
              'keypoints': [[x,y,prob], ...]}, ...]
        """
        # Отправляем raw BGR (без JPEG)
        h, w = frame.shape[:2]
        channels = 3

        try:
            self.sock.sendall(struct.pack('>I', w))
            self.sock.sendall(struct.pack('>I', h))
            self.sock.sendall(struct.pack('>I', channels))
            self.sock.sendall(frame.tobytes())

            json_len_data = self._recv_n(4)
            if len(json_len_data) < 4:
                return []
            json_len = struct.unpack('>I', json_len_data)[0]
            if json_len == 0:
                return []

            json_data = self._recv_n(json_len)
            if len(json_data) < json_len:
                return []

            return json.loads(json_data.decode('utf-8'))

        except (socket.timeout, BrokenPipeError, ConnectionResetError) as e:
            print(f"[NPUPoseClient] Connection error: {e}, reconnecting...", flush=True)
            self._reconnect()
            return []

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


if __name__ == '__main__':
    import sys

    img_path = sys.argv[1] if len(sys.argv) > 1 else \
        "/root/ugv_rpi/yolo11_pose/model/COCO_train2014_000000500390.jpg"

    frame = cv2.imread(img_path)
    if frame is None:
        print(f"Cannot read {img_path}")
        sys.exit(1)

    print(f"Testing pose on {img_path} ({frame.shape})...")
    client = NPUPoseClient()
    t0 = time.time()
    dets = client.detect(frame)
    t1 = time.time()

    print(f"Detection time: {(t1-t0)*1000:.1f} ms")
    print(f"Found {len(dets)} persons:")
    for d in dets:
        kp_count = len(d.get('keypoints', []))
        print(f"  {d['class']}: {d['confidence']*100:.0f}% at {d['bbox']}, keypoints: {kp_count}")
