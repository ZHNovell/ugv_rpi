# YOLO26n-depth на NPU A733 (Orange Pi 4 Pro)

Готовая сборка YOLO26-depth для Allwinner A733 с NPU depth-сервером через UNIX-сокет.

Монокулярная оценка глубины по одному RGB-кадру. Выход — карта глубины 768×768 (метры). Дополняет лидар D500: depth даёт объём, лидар — точные расстояния в срезе.

## Что в папке

- `yolo26n-depth_pcq_a733.nb` — модель YOLO26n-depth (PCQ, 10.4 МБ) — **основная**.
- `yolo26n-depth_int16_a733.nb` — модель nano-int16 (9.7 МБ) — запас.
- `yolo26s-depth_int16_a733.nb` — модель small-int16 (21.3 МБ) — точная, но медленная.
- `npu_depth_server` — собранный C++ depth-сервер (UNIX-сокет).
- `npu_depth_server.cpp` — исходник сервера.
- `yolo26_depth_post.cpp` — постобработка (heatmap + JET colormap).
- `yolo26_depth_pre.cpp` — препроцессинг (letterbox 768×768).
- `model_config.h` — конфиг (INPUT=768, OUTPUT=768).
- `CMakeLists.txt.patched` — пропатченный CMakeLists (нативная сборка).
- `build_native.sh` — скрипт нативной сборки.
- `model/rgb_00285.jpg` — тестовое изображение (NYU Depth V2).

## Ключевые характеристики

- **Вход:** 768×768 RGB (mean=0, scale=1/255).
- **Выход:** 768×768 float32 (метры).
- **Квантизация:** PCQ (INT8) для nano, int16 для s.
- **Run time:** 65 мс (n-PCQ), 158 мс (n-int16), 244 мс (s-int16).
- **FPS:** ~15 (n-PCQ), ~6 (n-int16), ~4 (s-int16).

## Сравнение вариантов

| Модель | Квантизация | Размер .nb | Run time | FPS | Качество |
|---|---|---|---|---|---|
| **n-depth** | **PCQ** | **10.4 МБ** | **65 мс** | **~15** | приемлемое |
| n-depth | int16 | 9.7 МБ | 158 мс | ~6 | приемлемое |
| s-depth | int16 | 21.3 МБ | 244 мс | ~4 | высокое |
| s-depth | PCQ | — | — | — | шумное |

**Ключевой вывод:** nano + PCQ — лучший баланс (65 мс, ~15 FPS, приемлемое качество).

## Почему PCQ для nano работает, а для s — нет

YOLO26-depth имеет `Exp()` на выходе — малейшая ошибка в log-глубине экспоненциально усиливается при INT8-квантизации.

- **s-модель** (больше параметров) накапливает больше ошибок → PCQ шумит.
- **n-модель** (меньше параметров) — квантуется лучше → PCQ даёт приемлемое качество.

## Быстрый тест (демо)

```bash
cd /root/ugv_rpi/yolo26_depth/

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./yolo26_depth_demo \
  -nb model/yolo26n-depth_pcq_a733.nb \
  -i model/rgb_00285.jpg \
  -l 1 -m 10
```

Результат сохраняется в `output_depth_heatmap.jpg`.

## Сервер (UNIX-сокет)

```bash
cd /root/ugv_rpi/yolo26_depth/

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./npu_depth_server /dev/shm/yolo26n-depth_pcq_a733.nb
```

Сервер загрузит модель, создаст сокет `/tmp/npu_depth.sock` и будет ждать JPEG-кадры.

## Python-клиент

`npu_depth_client.py` (в корне репозитория):

- Подключается к `/tmp/npu_depth.sock`.
- `detect(frame)` возвращает dict с:
  - `grid` (16×16 значений глубины в метрах),
  - `near_rle` (RLE-маска пикселей ближе порога),
  - `near_threshold`, `near_mask_size`, `grid_size`.

## Как собрать заново

```bash
cd /root/ugv_rpi/yolo26_depth/
./build_native.sh
```

## Ключевые фиксы для нативной сборки

1. `SYS_ARCH=linux_aarch64` — принудительно.
2. OpenCV через `pkg-config` — не `find_package`.
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. Собирать только `yolo26_depth_demo` и `npu_depth_server`.

## Протокол

```
Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]
```

**JSON:**
```json
{
  "grid_size": [16, 16],
  "grid": [1.92, 4.51, ...256 значений в метрах],
  "near_threshold": 2.0,
  "near_mask_size": [640, 480],
  "near_rle": [[1, 120], [1, 80], ...]
}
```

## Автозапуск

Сервис `npu-depth-server.service`:

- Загружает модель один раз при старте.
- Слушает `/tmp/npu_depth.sock`.
- Автоматически перезапускается при падении.
- Сокет отдельный от других NPU-серверов — можно запускать все 4 одновременно.

## Интеграция с роботом

- Кнопка **DEPTH** в веб-интерфейсе → запуск depth-воркера.
- Переключение с другими режимами — мгновенное (без subprocess).
- Отрисовка: heatmap (16×16 сетка) + красная подсветка близких пикселей.
- CPU — средняя нагрузка.
- FPS — ~15.

## Конвертация модели

**Источник:** Model Zoo v1.1.0 от Allwinner (`examples/yolo26_depth/`).

**Пайплайн:**

1. Ultralytics: `yolo26n-depth.pt` → ONNX (opset 14, imgsz 768, simplify).
2. Docker `ubuntu-npu:v2.0.10.2`:
   - `./pegasus_import.sh yolo26n-depth`
   - `./pegasus_quantize.sh yolo26n-depth pcq 10`
   - `./pegasus_export_ovx_nbg.sh yolo26n-depth pcq a733`
3. **Результат:** `yolo26n-depth_pcq_a733.nb` (10.4 МБ).

**Важно:** `config_yml.py` — mean=[0,0,0], scale=[1/255,1/255,1/255], IMAGE_RGB. Для nano и s-версии — одинаковые параметры.
