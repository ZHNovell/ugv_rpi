# YOLO11s на NPU A733 (Orange Pi 4 Pro)

Готовая сборка YOLO11s для Allwinner A733 (NPU VIP9000NANODI_PLUS).

## Что в папке

- `yolo11s_6_uint8_a733.nb` — модель (INT8, 6.6 МБ) для NPU A733.
- `yolo11_demo_a733` — собранный C++ демо-бинарник.
- `main.cpp`, `yolo11_6_post.cpp`, `yolo11_6_pre.cpp` — исходники.
- `model_config.h` — конфиг модели.
- `CMakeLists.txt.patched` — пропатченный CMakeLists (нативная сборка на A733).
- `CMakeLists.txt.orig` — оригинальный (кросс-компиляция).
- `build_native.sh` — скрипт нативной сборки.

## Быстрый тест

    cd /root/ugv_rpi/yolo11/
    LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
    ./yolo11_demo_a733 -nb yolo11s_6_uint8_a733.nb \
       -i /root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolo11/model/dog.jpg \
       -l 1 -m 10

Ожидаемый вывод:

    detection num: 3
     1:  94%, [ 126,  129,  568,  419], bicycle
    16:  92%, [ 132,  220,  311,  541], dog
     7:  50%, [ 465,   74,  692,  170], truck

Скорость: ~37 мс на инференс (~27 FPS).

## Как собрать заново

Требования:
- Model Zoo v0.9.0 распакован в `/root/awnpu_model_zoo-v0.9.0-*/`.
- `common/npuruntime/` скопирован из v1.1.0 (там `libNBGlinker.so`, `libVIPhal.so`).
- Системный OpenCV 4.x: `sudo apt install libopencv-dev pkg-config`.

Сборка:

    cd /root/ugv_rpi/yolo11/
    ./build_native.sh

## Ключевые фиксы для нативной сборки

1. `_GLIBCXX_USE_CXX11_ABI` — убрать `=0`, использовать **новый ABI** (иначе конфликт с системным OpenCV 4.10).
2. OpenCV — через **`pkg-config opencv4`**, не через `find_package`.
3. SYS_ARCH — принудительно **`linux_aarch64`** (gcc не содержит `aarch64` в имени).
