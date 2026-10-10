# YOLO11_pose на NPU A733 (Orange Pi 4 Pro)

Готовая сборка YOLO11_pose для Allwinner A733 с NPU pose-сервером через UNIX-сокет.

Определяет людей и 17 keypoints (скелет) в реальном времени через NPU. Заменяет MediaPipe Pose (который не работает на A733).

## Что в папке

- yolo11s-pose_9_uint8_a733.nb — модель YOLO11_pose (uint8, 7 МБ).
- npu_pose_server — собранный C++ pose-сервер.
- npu_pose_server.cpp — исходник сервера.
- yolo11_pose_9_post.cpp — постобработка (bbox + class + 17 keypoints).
- yolo11_pose_9_pre.cpp — препроцессинг (letterbox 640×640).
- model_config.h — конфиг (CLASS_NUM=1 person, NUM_POINTS=17).
- CMakeLists.txt.patched — пропатченный CMakeLists (нативная сборка).
- build_native.sh — скрипт нативной сборки.
- model/ — тестовое изображение (COCO с людьми).

## Ключевые характеристики

- 9 выходов: bbox (cv2) + class (cv3) + keypoints (cv4) × 3 уровня (80×80, 40×40, 20×20).
- 1 класс — person.
- 17 keypoints — нос, глаза, уши, плечи, локти, запястья, бёдра, колени, лодыжки.
- Detection time: ~62 мс.
- FPS: ~28 (с отрисовкой скелета).

## Быстрый тест

    cd /root/ugv_rpi/yolo11_pose/
    LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
    ./npu_pose_server /dev/shm/yolo11s-pose_9_uint8_a733.nb

Сервер загрузит модель, создаст сокет /tmp/npu_pose.sock и будет ждать JPEG-кадры.

## Python-клиент

npu_pose_client.py (в корне репозитория):
- Подключается к /tmp/npu_pose.sock.
- detect(frame) возвращает список с bbox и keypoints[17].

## Как собрать заново

    cd /root/ugv_rpi/yolo11_pose/
    ./build_native.sh

## Ключевые фиксы для нативной сборки

1. SYS_ARCH=linux_aarch64 — принудительно.
2. OpenCV через pkg-config — не find_package.
3. Убрать _GLIBCXX_USE_CXX11_ABI=0.
4. MODEL_ZOO_HOME_DIR — абсолютный путь.
5. Исключить main.cpp, npu_server.cpp, npu_pose_server.cpp, yolo11_6_*.cpp из сборки.
6. Собирать только npu_pose_server (не yolo11_demo).

## Протокол

Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]

JSON: [{"class_id": 0, "class": "person", "confidence": 0.94, "bbox": [x0,y0,x1,y1], "keypoints": [[x,y,prob], ...17]}, ...]

## Автозапуск

Сервис npu-pose-server.service:
- Загружает модель один раз при старте.
- Слушает /tmp/npu_pose.sock.
- Автоматически перезапускается при падении.

Сокет отдельный от YOLO26 (/tmp/npu11.sock) — можно запускать оба сервера одновременно.

## Интеграция с роботом

- Кнопка MP POSE в веб-интерфейсе → запуск pose-воркера.
- Переключение OBJECTS ↔ MP POSE — мгновенное (без subprocess).
- Отрисовка скелета — 17 точек + 19 соединений (COCO skeleton).
- CPU — минимальная нагрузка.
- FPS — ~28.

## Конвертация модели

Источник: Model Zoo v1.1.0 от Allwinner (examples/yolo11_pose/).

Пайплайн:
1. Ultralytics: yolo11s-pose.pt → ONNX (opset 15).
2. onnx_extract.py — вырезать 9 выходов (cv2/cv3/cv4 × 3 уровня).
3. Docker ubuntu-npu:v2.0.10.2:
   - ./pegasus_import.sh yolo11s-pose_9
   - ./pegasus_quantize.sh yolo11s-pose_9 uint8 12
   - ./pegasus_export_ovx_nbg.sh yolo11s-pose_9 uint8 a733
4. Результат: yolo11s-pose_9_uint8_a733.nb (7 МБ).
