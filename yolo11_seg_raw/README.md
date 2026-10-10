# YOLO11s-seg на NPU A733 (Orange Pi 4 Pro)

Готовая сборка YOLO11-seg для Allwinner A733 с NPU seg-сервером через UNIX-сокет.

Instance segmentation — модель выделяет пиксельные маски каждого объекта (не только рамки). Для каждого найденного объекта возвращается контур (полигон) + bbox + класс + confidence.

## Что в папке

- `yolo11s-seg_10_uint8_a733.nb` — модель YOLO11s-seg (uint8, 7.3 МБ).
- `npu_seg_server` — собранный C++ seg-сервер (UNIX-сокет).
- `npu_seg_server.cpp` — исходник сервера.
- `yolo11_seg_10_post.cpp` — постобработка (bbox + class + mask + protos).
- `yolo11_seg_10_pre.cpp` — препроцессинг (letterbox 640×640).
- `model_config.h` — конфиг (CLASS_NUM=80, 10 выходов, MASK_THRESHOLD=0.5).
- `CMakeLists.txt.patched` — пропатченный CMakeLists (нативная сборка).
- `build_native.sh` — скрипт нативной сборки.
- `model/dog.jpg` — тестовое изображение (COCO).

## Ключевые характеристики

- **10 выходов:** bbox (cv2) + class (cv3) + mask coeffs (cv4) × 3 уровня (80×80, 40×40, 20×20) + protos (160×160×32).
- **80 классов** (COCO).
- **Run time:** 45 мс на инференс (~22 FPS).
- **Post process:** 11 мс.
- **Клиент через сокет:** 64 мс (включая JPEG + RLE).

## Результаты теста (dog.jpg)

| Метрика | Значение |
|---|---|
| **run time (inference)** | **45.2 мс** (~22 FPS) |
| **post process** | 11 мс |
| **Размер .nb** | 7.3 МБ |

**Детекции:**
```
detection num: 3
 1:  95%, [ 127,  125,  568,  420], bicycle
16:  96%, [ 132,  221,  311,  541], dog
 2:  85%, [ 466,   75,  691,  172], car
```

**Качество:** маски чёткие, объекты выделены точно. Заметно лучше, чем PCQ у depth (у seg нет `exp()` на выходе).

## Сравнение со всеми моделями проекта

| Модель | Квантизация | Run time | FPS |
|---|---|---|---|
| YOLO26s (детекция) | PCQ | ~35 мс | 29 |
| YOLO11_pose | uint8 | ~62 мс | 16 |
| **YOLO11_seg** | **uint8** | **45 мс** | **~22** |
| YOLO26n_depth | PCQ | 65 мс | ~15 |

Seg работает **быстрее**, чем pose и depth — почти как детекция.

## Быстрый тест (демо)

```bash
cd /root/ugv_rpi/yolo11_seg/

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./yolo11_seg_demo \
  -nb model/yolo11s-seg_10_uint8_a733.nb \
  -i model/dog.jpg \
  -l 1 -m 10
```

Результат сохраняется в `out_yolo11_seg.png` (маски + bbox).

## Сервер (UNIX-сокет)

```bash
cd /root/ugv_rpi/yolo11_seg/

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./npu_seg_server /dev/shm/yolo11s-seg_10_uint8_a733.nb
```

Сервер загрузит модель, создаст сокет `/tmp/npu_seg.sock` и будет ждать JPEG-кадры.

## Python-клиент

`npu_seg_client.py` (в корне репозитория):

- Подключается к `/tmp/npu_seg.sock`.
- `detect(frame)` возвращает список детекций:
  - `class`, `class_id`, `confidence`,
  - `bbox` [x0,y0,x1,y1],
  - `mask_size` [w,h],
  - `mask_rle` [[v,count],...] (RLE-сжатие маски).

## Как собрать заново

```bash
cd /root/ugv_rpi/yolo11_seg/
./build_native.sh
```

## Ключевые фиксы для нативной сборки

1. `SYS_ARCH=linux_aarch64` — принудительно.
2. OpenCV через `pkg-config` — не `find_package`.
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. Собирать `yolo11_seg_demo` и `npu_seg_server`.

## Протокол

```
Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]
```

**JSON:**
```json
[
  {
    "class_id": 16,
    "class": "dog",
    "confidence": 0.96,
    "bbox": [132, 221, 311, 541],
    "mask_size": [179, 319],
    "mask_rle": [[0, 12], [1, 80], ...]
  }
]
```

## Автозапуск

Сервис `npu-seg-server.service`:

- Загружает модель один раз при старте.
- Слушает `/tmp/npu_seg.sock`.
- Автоматически перезапускается при падении.
- Сокет отдельный от других NPU-серверов.

## Интеграция с роботом

- Кнопка **SEG** в веб-интерфейсе → запуск seg-воркера.
- Переключение с другими режимами — мгновенное (без subprocess).
- Отрисовка: маски накладываются полупрозрачно + bbox + подписи.
- CPU — средняя нагрузка.
- FPS — ~22.

## Конвертация модели

**Источник:** Model Zoo v1.1.0 от Allwinner (`examples/yolo11_seg/`).

**Пайплайн:**

1. ONNX уже обрезан через `onnx_extract.py` (10 выходов: bbox + class + mask coeffs + protos).
2. Docker `ubuntu-npu:v2.0.10.2`:
   - `./pegasus_import.sh yolo11s-seg_10`
   - `./pegasus_quantize.sh yolo11s-seg_10 uint8 12`
   - `./pegasus_export_ovx_nbg.sh yolo11s-seg_10 uint8 a733`
3. **Результат:** `yolo11s-seg_10_uint8_a733.nb` (7.3 МБ).

**Важно:** постобработка seg при 8bit даёт потери точности, поэтому её вынесли на CPU (C++) — это и есть смысл обрезки через `onnx_extract.py`.
