# YOLO26s на NPU A733 (Orange Pi 4 Pro)

Готовая сборка YOLO26s для Allwinner A733 (NPU VIP9000NANODI_PLUS) с NPU-сервером через UNIX-сокет.

## Что в папке

- `yolo26s_6_pcq_a733.nb` — модель YOLO26s (PCQ, 9 МБ) для NPU A733.
- `npu_server` — собранный C++ NPU-сервер (UNIX-сокет).
- `npu_server.cpp` — исходник сервера.
- `yolo26_6_post.cpp` — постобработка YOLO26 (6 выходов).
- `yolo26_6_pre.cpp` — препроцессинг YOLO26.
- `model_config.h` — конфиг модели.
- `CMakeLists.txt.patched` — пропатченный CMakeLists.
- `build_native.sh` — скрипт нативной сборки.

## Почему YOLO26 лучше YOLO11

1. **DFL-free** — голова проще, лучше квантизуется.
2. **NMS-free** — постобработка ~6 мс вместо ~11 мс.
3. **ProgLoss + STAL** — лучше на мелких и удалённых объектах.

## Результаты

- FPS 29 (1080p30) / 60 (720p60).
- CPU ~36-38%.
- Отставание рамок <0.3 сек.

## Быстрый тест

    cd /root/ugv_rpi/yolo26/
    LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
    ./npu_server /dev/shm/yolo26s_6_pcq_a733.nb

## Как собрать заново

    cd /root/ugv_rpi/yolo26/
    ./build_native.sh

## Ключевые фиксы

1. SYS_ARCH=linux_aarch64 — принудительно.
2. OpenCV через pkg-config — не find_package.
3. Убрать _GLIBCXX_USE_CXX11_ABI=0.
4. MODEL_ZOO_HOME_DIR — абсолютный путь.
5. static у detect_yolo26_6_post — убран.
6. Исключить yolo11_6_*.cpp из сборки.
7. main.cpp — исключён из npu_server.

## Протокол

Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]

Клиент — npu_client.py в корне репозитория.
