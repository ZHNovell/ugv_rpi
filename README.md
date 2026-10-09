

![GitHub top language](https://img.shields.io/github/languages/top/ZHNovell/ugv_rpi)
![GitHub language count](https://img.shields.io/github/languages/count/ZHNovell/ugv_rpi)
![GitHub code size in bytes](https://img.shields.io/github/languages/code-size/ZHNovell/ugv_rpi)
![GitHub repo size](https://img.shields.io/github/repo-size/ZHNovell/ugv_rpi)
![GitHub](https://img.shields.io/github/license/ZHNovell/ugv_rpi)
![GitHub last commit](https://img.shields.io/github/last-commit/ZHNovell/ugv_rpi)
<img width="1088" height="721" alt="261006200253" src="https://github.com/user-attachments/assets/368fbf34-2087-4e6b-b819-b5fca5f83acd" />

# Orange Pi 4 Pro UGV Robot

## 📖 О проекте

**UGV-робот** на **Orange Pi 4 Pro** (Allwinner A733, NPU 3 TOPS) с **двумя NPU-серверами**:
- **YOLO26s** — детекция объектов (29 FPS).
- **YOLO11_pose** — позы людей (17 keypoints, 28 FPS).

**Возможности:**
- **Живой MJPEG-поток** с камеры (1080p30 / 720p60).
- **Запись видео** (`.mkv`, без прерывания потока).
- **NPU-инференс** через **UNIX-сокет** (отставание <0.3 сек).
- **Веб-интерфейс** (Flask + Socket.IO).
- **Управление** через **ESP32** (UART7).

**Ключевые особенности:**
- **Отставание рамок** — **<0.3 сек** (в 3 раза **быстрее** YOLO11s через subprocess).
- **CPU** — **36-38%** (вместо 55%).
- **Автозапуск** — `npu-server.service`, `npu-pose-server.service`, `ugv.service`.

## 📋 Общая архитектура

**Проект:** Робот на базе Waveshare UGV (WAVE ROVER / General Driver for Robots) с заменой Raspberry Pi на Orange Pi 4 Pro.

**Аппаратная платформа:**
- **Orange Pi 4 Pro** — Allwinner A733, 8 ядер (4×A76 + 4×A55), 12 ГБ RAM, NPU 3 TOPS INT8.
- **General Driver for Robots** — Waveshare, ESP32-WROOM-32UE, 2×MX1919, INA219, OLED SSD1306 (0x3C).
- **2-Axis Pan-Tilt Camera Module** — сервоприводы ST3215, UART.
- **Шасси WAVE ROVER** — 4 мотора, 4 энкодера, 3S Li-Ion UPS.
- **USB-камера** — (навигационная).
- **CSI-камера** — (опционально, обзорная).

**Архитектура управления:**
- **Orange Pi 4 Pro** — «верхний мозг»: видео, NPU, веб-интерфейс, стратегия.
- **ESP32** (на General Driver) — «нижний мозг»: моторы, сервы, INA219, OLED.
- **Связь:** UART7 (пины 8/10) между Orange Pi и ESP32.
- **Протокол:** JSON-команды (`{"T":1,"L":0.5,"R":0.5}`).

## 🔌 Распиновка (40-pin)

| Пин Orange Pi 4 Pro | Функция | Пин на General Driver |
|---|---|---|
| **8** | UART7 TX | RX (ESP32) |
| **10** | UART7 RX | TX (ESP32) |
| **3** | I2C0 SDA | (не используется) |
| **5** | I2C0 SCL | (не используется) |
| **19** | I2C2 SDA | (резерв) |
| **23** | I2C2 SCL | (резерв) |
| **1** | 3.3V | 3.3V |
| **2, 4** | 5V | 5V |
| **6, 9, 14...** | GND | GND |

**Важно:** OLED, INA219, сервы управляются **ESP32** через **внутренние шины**. Orange Pi их **не трогает**.

## 🛠️ Сборка ОС (Armbian)

**Репозиторий для сборки:**
- [PR #10835 (GPU/VPU/NPU/ISP)](https://github.com/armbian/build/pull/10835)
- [Ветка ijiki16](https://github.com/ijiki16/build/tree/sun60iw2-4pro-gpu-desktop-upstream)

**Команды сборки:**

```bash
git clone https://github.com/armbian/build.git
cd armbian-build
git fetch origin pull/10835/head:pr-10835
git checkout pr-10835

./compile.sh BOARD=orangepi4pro BRANCH=vendor RELEASE=trixie \
  BUILD_DESKTOP=no BUILD_MINIMAL=yes \
  KERNEL_CONFIGURE=no KERNEL_BTF=no KERNEL_GIT=shallow
```

**Результат:**
- `Armbian-unofficial_26.11.0-trunk_Orangepi4pro_trixie_vendor_6.6.98_minimal.img (vendor, Linux 6.6.98)


**Что включено (PR #10835):**
- Ядро 6.6.98-vendor-sun60iw2.
- DTB: sun60i-a733-orangepi-4-pro.dtb.`.
- GPU PowerVR BXM-4-64 — `pvrsrvkm.ko` (автозагрузка), EGL/GLES/GBM/Vulkan/OpenCL.
- NPU VIPLite — `/dev/vipcore`, `libVIPhal.so`, `libNBGlinker.so`.
- VPU CedarC — `/dev/cedar_dev`, `/dev/cedar_dev_ve2`.
- ISP (libAWIspApi) — для CSI-камер.
- Утилиты NPU: `/usr/bin/lenet`, `/usr/bin/vpm_run`.
- GStreamer OMX — `gstreamer1.0-omx-allwinner` (плагин для VE).
- Модели NPU: `/etc/npu/lenet/`, `/etc/npu/vpm_run/`.
- udev-правила — `99-sun60iw2-permissions.rules`.

**Важно:** Vendor-сборка (6.6.98) **не содержит** NPU-утилит.

## 💾 Установка ОС

**Запись на SD-карту:**

```bash
# На Ubuntu (VirtualBox)
unxz Armbian-...-minimal.img.xz
sudo dd if=Armbian-unofficial_26.11.0-trunk_Orangepi4pro_trixie_vendor_6.6.98_minimal.img of=/dev/sdX bs=4M status=progress
sync
```

**Первый запуск:**
1. Вставить SD-карту в Orange Pi 4 Pro.
2. Подключить Ethernet + HDMI.
3. Загрузиться.
4. Пройти `armbian-firstlogin` (через HDMI + клавиатуру).
5. Сменить пароль root, создать пользователя.

**Перенос на eMMC (РУЧНОЙ, НЕ ЧЕРЕЗ armbian-install!):**

**Важно:** armbian-install багованный на Orange Pi 4 Pro — он неправильно копирует boot_package на eMMC. 
Использовать только ручное копирование.

```bash
# 1. Размонтировать eMMC
sudo umount /mnt/emmc 2>/dev/null

# 2. Разметить eMMC (таблица разделов как на SD)
sudo sfdisk -d /dev/mmcblk1 | sudo sfdisk /dev/mmcblk0

# 3. Форматировать eMMC
sudo mkfs.ext4 -F /dev/mmcblk0p1

# 4. Смонтировать eMMC
sudo mkdir -p /mnt/emmc
sudo mount /dev/mmcblk0p1 /mnt/emmc

# 5. Скопировать систему с SD на eMMC (5-10 минут)
sudo rsync -aAXHv --exclude={/dev/*,/proc/*,/sys/*,/tmp/*,/run/*,/mnt/*,/media/*,/lost+found,/var/log.hdd/*} / /mnt/emmc/

# 6. Настроить armbianEnv.txt (новый UUID eMMC)
sudo blkid /dev/mmcblk0p1  # запомнить UUID
sudo sed -i 's/^rootdev=.*/rootdev=UUID=<НОВЫЙ_UUID>/' /mnt/emmc/boot/armbianEnv.txt

# 7. Настроить fstab (новый UUID eMMC)
sudo sed -i 's/^UUID=.* \/ ext4/UUID=<НОВЫЙ_UUID> \/ ext4/' /mnt/emmc/etc/fstab

# 8. Отключить авто-resize на eMMC
sudo mkdir -p /mnt/emmc/etc/systemd/system
sudo ln -sf /dev/null /mnt/emmc/etc/systemd/system/armbian-resize-filesystem.service

# 9. Скопировать загрузчик (U-Boot + boot_package) — 20 МБ
sudo dd if=/dev/mmcblk1 of=/dev/mmcblk0 bs=1M count=20 conv=notrunc

# 10. Синхронизировать и размонтировать
sudo sync
sudo umount /mnt/emmc

# 11. Выключить, вытащить SD, загрузиться с eMMC
sudo shutdown -h now
```
##После первой загрузки с eMMC — расширить раздел до 29 ГБ:##
```bash
# 1. Расширить раздел
sudo parted /dev/mmcblk0 resizepart 1 100%
# (на вопрос "Yes/No?" — Yes)

# 2. Расширить файловую систему
sudo resize2fs /dev/mmcblk0p1

# 3. Проверить
df -h /
# Должно быть ~29 ГБ, ~21 ГБ свободно
```
**Важно:** eMMC-модуль (32 ГБ) подключается в штатный разъём платы.

## ⚠️ Важно: CQE-баг на A733 и решение через HS400 @ 150 МГц (2026-10-07)

**Проблема:** драйвер `sunxi-mmc` на A733 имеет **баг с CQE** (Command Queue Engine). При **высокой частоте (200 МГц)** CQE **не может остановиться** (`Failed to halt`), драйвер **отключает CQE** и **сбрасывает частоту** до **50 МГц**.

**Симптомы:**
- `dmesg | grep cqhci` → `cqhci: Failed to halt`, `running CQE recovery`, `recovery to disabled cqe`.
- Частота eMMC — **50 МГц** вместо ожидаемых 200 МГц.
- Скорость чтения — **~80 МБ/с**.

**Причина:** баг CQE — **пограничный** (аппаратно-программный). Спецификация eMMC **допускает** такое поведение («в некоторых случаях halt может не произойти, software должен продолжить после таймаута»). В **mainline Linux** уже есть **патч** для **Rockchip**, который решает проблему через бит `SW_ERR_HALR_REQ_DISABLE (CQHCI_CTL[1])` — **если кремний поддерживает**.

**Решение (текущее):** использовать **HS400 @ 150 МГц** — **стабильная** частота, **CQE-ошибок нет**.

```bash
# Убедиться, что в DTB: v5p3x + 150 МГц + sunxi-dly-208M
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb
NODE=/soc@3000000/sdmmc@4022000

# Проверить текущие настройки
sudo dtc -I dtb -O dts $DTB 2>/dev/null | grep -A 30 "mmc@4022000" | \
    grep -E "compatible|max-frequency|sunxi-dly"

# Должно быть:
#   compatible = "allwinner,sunxi-mmc-v5p3x";
#   max-frequency = <0x8f0d180>;   (150 МГц)
#   sunxi-dly-208M = <0xff 0x01 0xff 0xff 0xff 0xff>;

# Если max-frequency = 200 МГц — снизить до 150
sudo fdtput -t i $DTB $NODE max-frequency 150000000

# Перезагрузиться
sudo reboot
```

**Результат:**
- **Частота:** 150 МГц (HS400).
- **Скорость чтения:** **~239 МБ/с** (замер `dd iflag=direct`).
- **CQE-ошибок нет.**
- **Система стабильна.**

**Почему не 200 МГц:**
- При 200 МГц CQE **падает** (`Failed to halt`), драйвер **отключает CQE** и **сбрасывает частоту** до 50 МГц.
- **150 МГц** — максимальная **стабильная** частота для текущего драйвера.

**Будущее решение (когда mainline подтянется):**
- Патч `SW_ERR_HALR_REQ_DISABLE` для `cqhci-core.c` — если **бит 1** поддерживается кремнием A733, **200 МГц** заработает **без CQE-ошибок**.
- Или **M.2 NVMe** через PCIe Gen1 (~250 МБ/с, не зависит от eMMC).

**Дополнительно:** в ядро добавлен патч **очистки `CQHCI_CTL`** при `Failed to halt` (из mainline). Патч **в** `drivers/mmc/host/cqhci-core.c` — функция `cqhci_halt`.

```c
if (!ret) {
    pr_warn("%s: cqhci: Failed to halt\n", mmc_hostname(mmc));
    /* Clear CQHCI_CTL to recover from failed halt */
    cqhci_writel(cq_host, 0, CQHCI_CTL);
}
```
## ⚙️ Настройка интерфейсов
### UART7 (пины 8/10)
```bash
# Активация через armbian-config
sudo armbian-config
# System → Kernel → Manage device tree overlays
# Включить: uart7
# Сохранить, выйти, перезагрузиться

ls -la /dev/ttyS7
# Должно быть: crw-rw---- 1 root dialout 241, 7 ... /dev/ttyS7
```

# Loopback-тест (замкнуть пины 8 и 10)
```bash
picocom -b 115200 /dev/ttyS7
# Печатать символы → должны эхо-возвращаться
# Выход: Ctrl+A, Ctrl+Q
```
### I2C2 (пины 19/23)
```bash
# Активация через armbian-config
sudo armbian-config
# System → Kernel → Manage device tree overlays
# Включить: i2c2
# Сохранить, выйти, перезагрузиться

ls -la /dev/i2c-2
# Должно быть: crw------- 1 root root 89, 2 ... /dev/i2c-2
```
# Сканирование шины
```bash
apt install -y i2c-tools
i2cdetect -y 2
# Должно быть пусто (если ничего не подключено)
```

armbianEnv.txt (после активации):

```text
overlays=i2c2 uart7
```

## 📦 Установка зависимостей

### Системные пакеты

```bash
sudo apt update
sudo apt install -y \
    git python3-pip python3-venv python3-dev \
    cmake build-essential \
    libopenblas-dev liblapack-dev libhdf5-dev \
    libjpeg-dev libtiff-dev libpng-dev \
    libavcodec-dev libavformat-dev libswscale-dev \
    libv4l-dev libxvidcore-dev libx264-dev \
    libgtk-3-dev libcanberra-gtk3-module \
    gfortran libfreetype-dev libharfbuzz-dev \
    libfribidi-dev libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    i2c-tools picocom unzip espeak-ng libespeak1
```
### Python-зависимости (venv)
```bash
cd ~/ugv_rpi
python3 -m venv ugv-env
source ugv-env/bin/activate
pip install --upgrade pip
```
### requirements.txt (Python 3.13)
```text
# Веб-сервер
Flask==3.0.3
Flask-SocketIO==5.3.6
Werkzeug==3.0.3
Jinja2==3.1.4
itsdangerous==2.2.0
blinker==1.8.2

# WebSocket / WebRTC
python-socketio==5.11.2
python-engineio==4.9.1
aiortc==1.15.0
av==17.1.0
simplejpeg==1.9.0
aioice==0.10.2
pyee==14.0.0
pylibsrtp==1.0.0
cryptography==50.0.1

# Работа с данными
numpy==1.26.4
PyYAML==6.0.1
requests==2.32.3
psutil==5.9.8
pyserial==3.5

# Обработка изображений
opencv-python-headless==4.10.0.84
Pillow==11.3.0
imutils==0.5.4
```
### Установка:

```bash
pip install -r requirements.txt
pip install imageio pygame-ce pyttsx3 netifaces
```
**Ключевые моменты:**
- **Pillow 11.3.0** (не 10.3.0 — та не работает с Python 3.13).
- **pygame-ce** (не pygame — у pygame нет wheel для Python 3.13 + ARM64).
- **av 17.1.0** (не 12.3.0 — та не работает).
- **aiortc 1.15.0** (не 1.8.0).
- **i2c-tools**, **picocom**, **espeak-ng** — установлены **отдельно** (для UART, I2C, TTS).

## 🧠 NPU: Установка и настройка

### Драйвер NPU (уже в ядре)

```bash
ls -la /dev/vipcore
# crw-rw-rw- 1 root root 199, 0 ... /dev/vipcore

lsmod | grep vipcore
# vipcore 270336 0
```
### YOLO26s + устранение отставания через NPU-сервер

**Итог:** YOLO26s INT8 (PCQ) работает на NPU A733. **Отставание рамок < 0.3 сек** (было ~1 сек). Архитектура — **постоянный C++ NPU-сервер через UNIX-сокет** (вместо `subprocess.run` на каждый кадр).

### Почему YOLO26s лучше YOLO11s

Три ключевых улучшения:
1. **DFL-free** (убран Distribution Focal Loss) — голова проще, лучше квантизуется.
2. **NMS-free** — постобработка ~6 мс вместо ~11 мс.
3. **ProgLoss + STAL** — лучше детектит мелкие и удалённые объекты.

### Проблема с `subprocess` (решение от 2026-10-05)

Первая версия (YOLO11s) вызывала демо через `subprocess.run()` на каждый кадр:
- **fork + exec** — ~20-50 мс.
- **Загрузка `.nb` (6.6 МБ)** с eMMC 52 МГц — ~100-130 мс.
- **`cv2.imwrite` + `cv2.imread`** — ~20-30 мс.
- **Итого ~200-300 мс** на цикл → **отставание ~0.7-1 сек**.

### Решение: NPU-сервер через UNIX-сокет

**C++ сервер `npu_server`** (`yolo26/npu_server.cpp`):
- Загружает модель **ОДИН РАЗ** при старте.
- Слушает UNIX-сокет `/tmp/npu11.sock`.
- Принимает JPEG-кадры от Python (4 байта длины BE + JPEG).
- Декодирует, letterbox 640×640, запускает NPU.
- Постобработка (`detect_yolo26_6_post`).
- Возвращает JSON (4 байта длины BE + JSON).

**Python-клиент `npu_client.py`:**
- Подключается к сокету (с автопереподключением).
- `detect(frame)` — отправляет JPEG, получает список детекций.
- Асинхронный воркер `_npu_worker` в `cv_ctrl.py` — крутится в отдельном потоке, обновляет `self.overlay`.

### Результаты YOLO26s vs YOLO11s

| Метрика | YOLO11s | YOLO26s |
|---|---|---|
| FPS 1080p30 | 26 | **29** |
| FPS 720p60 | 57 | **60** |
| CPU 1080p30 | 39% | **36-38%** |
| CPU 720p60 | 35% | **36%** |
| Удалённые объекты | часто терялись | **лучше** |
| Мигание (статика) | иногда | **нет** |
| Отставание | <0.3 сек | **<0.3 сек** |

**Температура** — ниже (меньше CPU → меньше тепла).

### Протокол

```
Клиент -> Сервер: [4 байта BE: длина JPEG] [JPEG bytes]
Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]

JSON: [{"class_id": 16, "class": "dog", "confidence": 0.92, "bbox": [x0,y0,x1,y1]}, ...]
```

### Автозапуск

**`/etc/systemd/system/npu-server.service`:**
```ini
[Unit]
Description=NPU Server for YOLO26s (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo26
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/root/ugv_rpi/prepare_shm.sh
ExecStart=/root/ugv_rpi/yolo26/npu_server /dev/shm/yolo26s_6_pcq_a733.nb
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

**`ugv.service`** — `After=network.target npu-server.service` + `Requires=npu-server.service`.

**`prepare_shm.sh`** — копирует YOLO26 модель в `/dev/shm/` (RAM-диск) при старте.

### Пайплайн конвертации YOLO26s (PCQ)

1. **Скачать** `yolo26s.pt` (Ultralytics).
2. **Экспорт ONNX** (opset 16): `yolo export model=yolo26s.pt format=onnx simplify=True dynamic=False opset=16`.
3. **Вырезать 6 выходов** через `onnx_extract.py`:
   - `/model.23/Reshape_output_0` (`[1, 4, 6400]` — bbox, stride 8)
   - `/model.23/Reshape_1_output_0` (`[1, 4, 1600]`)
   - `/model.23/Reshape_2_output_0` (`[1, 4, 400]`)
   - `/model.23/Reshape_3_output_0` (`[1, 80, 6400]` — classes)
   - `/model.23/Reshape_4_output_0` (`[1, 80, 1600]`)
   - `/model.23/Reshape_5_output_0` (`[1, 80, 400]`)
4. **Docker ACUITY** (`ubuntu-npu:v2.0.10.2`):
   - `./pegasus_import.sh yolo26s_6`
   - `./pegasus_quantize.sh yolo26s_6 pcq 12` ← **PCQ**, не uint8.
   - `./pegasus_export_ovx_nbg.sh yolo26s_6 pcq a733`
5. **Результат:** `yolo26s_6_pcq_a733.nb` (9 МБ).

### Нативная сборка NPU-сервера на Orange Pi

```bash
cd /root/ugv_rpi/yolo26/
./build_native.sh
```

**Ключевые фиксы `CMakeLists.txt.patched`:**
1. `SYS_ARCH=linux_aarch64` — принудительно.
2. OpenCV через `pkg-config` — не `find_package`.
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0` — иначе конфликт с системным OpenCV 4.10.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. `static` у `detect_yolo26_6_post` — убран (для экспорта).
6. Исключить `yolo11_6_*.cpp` из сборки (конфликтуют с YOLO26).
7. `main.cpp` — исключён из `npu_server` (у него свой `main`).

### Файлы в репозитории

- `yolo26/` — папка YOLO26s (модель, сервер, скрипты, README).
- `yolo11/` — папка YOLO11s (остаётся как история).
- `npu_client.py` — Python-клиент (общий для YOLO11/YOLO26).
- `prepare_shm.sh` — копирование `.nb` в RAM.
- `/etc/systemd/system/npu-server.service` — автозапуск.

### YOLO11_pose на NPU — скелет в реальном времени

**Итог:** YOLO11_pose работает на NPU A733 через **второй NPU-сервер** (`npu_pose_server`, сокет `/tmp/npu_pose.sock`). **Заменяет MediaPipe Pose** (который не работает на A733). Кнопка **MP POSE** в веб-интерфейсе теперь использует **NPU-инференс**.

### Что нового

1. **YOLO11_pose INT8 (uint8)** сконвертирована в `.nb` (7 МБ).
2. **Второй NPU-сервер** — `npu_pose_server` (по аналогии с YOLO26).
3. **Python-клиент** — `npu_pose_client.py` (подключается к `/tmp/npu_pose.sock`).
4. **Асинхронный pose-воркер** в `cv_ctrl.py` — `_npu_pose_worker`.
5. **Отрисовка скелета** — 17 keypoints + 19 соединений (COCO skeleton).
6. **Автозапуск** — `npu-pose-server.service`.

### Архитектура

```
Клиент (cv_ctrl.py)          Сервер (npu_pose_server)
        |                              |
        |--- JPEG (4 байта + данные) ->|
        |                              | decode + letterbox 640×640
        |                              | NPU inference (YOLO11_pose)
        |                              | detect_yolo11_pose_9_post
        |<- JSON (4 байта + данные) ---|
        |                              |
   рисуем bbox + скелет                |
```

**Сокеты:**
- `/tmp/npu11.sock` — YOLO26s (детекция объектов).
- `/tmp/npu_pose.sock` — YOLO11_pose (17 keypoints).

**Оба сервера работают параллельно**, переключение — **мгновенное** (без subprocess).

### Модель

- **9 выходов**: bbox (cv2) + class (cv3) + keypoints (cv4) × 3 уровня (80×80, 40×40, 20×20).
- **1 класс** — person.
- **17 keypoints** COCO: нос, глаза, уши, плечи, локти, запястья, бёдра, колени, лодыжки.
- **Detection time**: ~62 мс.
- **FPS**: ~28 (с отрисовкой скелета).
- **CPU**: минимальная нагрузка.

### Пайплайн конвертации

1. **`yolo11s-pose.pt`** (Ultralytics) → ONNX (opset 15).
2. **`onnx_extract.py`** — вырезать 9 выходов (cv2/cv3/cv4 × 3 уровня).
3. **Docker ACUITY** (`ubuntu-npu:v2.0.10.2`):
   - `./pegasus_import.sh yolo11s-pose_9`
   - `./pegasus_quantize.sh yolo11s-pose_9 uint8 12`
   - `./pegasus_export_ovx_nbg.sh yolo11s-pose_9 uint8 a733`
4. **Результат:** `yolo11s-pose_9_uint8_a733.nb` (7 МБ).

### Нативная сборка

```bash
cd /root/ugv_rpi/yolo11_pose/
./build_native.sh
```
**Ключевые фиксы** (аналогично YOLO26):
1. `SYS_ARCH=linux_aarch64`.
2. OpenCV через `pkg-config`.
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. Исключить `main.cpp`, `npu_server.cpp`, `npu_pose_server.cpp`, `yolo11_6_*.cpp` из сборки.
6. Собирать только `npu_pose_server`.

### Автозапуск

**`/etc/systemd/system/npu-pose-server.service`:**
```ini
[Unit]
Description=NPU Pose Server for YOLO11_pose (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo11_pose
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/bin/sh -c 'cp /root/ugv_rpi/yolo11_pose/yolo11s-pose_9_uint8_a733.nb /dev/shm/ 2>/dev/null || true'
ExecStart=/root/ugv_rpi/yolo11_pose/npu_pose_server /dev/shm/yolo11s-pose_9_uint8_a733.nb
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

### Интеграция с роботом

- **Кнопка MP POSE** (T=10309) → `set_cv_mode(mp_pose)` → **запуск pose-воркера**.
- **Переключение OBJECTS ↔ MP POSE** — **мгновенное**.
- **Отрисовка:** bbox + label + 17 keypoints (красные кружки) + 19 соединений (жёлтые линии).
- **Фильтр по confidence** — keypoints с `prob > 0.3`.

### Файлы в репозитории

- `yolo11_pose/` — папка YOLO11_pose (модель, сервер, скрипты, README).
- `npu_pose_client.py` — Python-клиент (в корне репозитория).
- `/etc/systemd/system/npu-pose-server.service` — автозапуск.

## 🐍 NPU-сервер (YOLO26s + YOLO11_pose)

- npu_server (YOLO26s) — сокет /tmp/npu11.sock.

- npu_pose_server (YOLO11_pose) — сокет /tmp/npu_pose.sock.

- Python-клиенты — npu_client.py, npu_pose_client.py.

- Автозапуск — npu-server.service, npu-pose-server.service.

- .nb в /dev/shm — prepare_shm.sh.

## 🔧 Адаптация кода `ugv_rpi`

### `base_ctrl.py`

**Изменение:** UART-порт.

```python
# Было (Raspberry Pi):
base = BaseController('/dev/ttyAMA0', 115200)

# Стало (Orange Pi 4 Pro):
base = BaseController('/dev/ttyS7', 115200)
```

**Команда замены:**

```bash
sed -i "s|/dev/ttyAMA0|/dev/ttyS7|g" base_ctrl.py
```

### `app.py`

**Изменение 1:** UART-порт + удаление блока проверки Raspberry Pi.

```python
# Было:
def is_raspberry_pi5():
    with open('/proc/cpuinfo', 'r') as file:
        for line in file:
            if 'Model' in line:
                if 'Raspberry Pi 5' in line:
                    return True
                else:
                    return False

if is_raspberry_pi5():
    base = BaseController('/dev/ttyAMA0', 115200)
else:
    base = BaseController('/dev/serial0', 115200)

# Стало:
# Orange Pi 4 Pro — UART7
base = BaseController('/dev/ttyS7', 115200)
```

**Изменение 2:** `eth0_ip` → `end0_ip`.

```bash
sed -i 's/si\.eth0_ip/si.end0_ip/g' app.py
```

**Изменение 3:** Добавлен обработчик `SIGTERM` (для корректного завершения при `shutdown`).

```python
import signal
import sys

def cleanup_handler(signum, frame):
    print(f"[app] Received signal {signum}, cleaning up...", flush=True)
    try:
        if hasattr(cvf, "camera") and cvf.camera:
            cvf.camera.release()
            print("[app] Camera released", flush=True)
    except Exception as e:
        print(f"[app] Camera release error: {e}", flush=True)
    try:
        if hasattr(cvf, "writer") and cvf.writer:
            cvf.writer.release()
            print("[app] Video writer released", flush=True)
    except Exception as e:
        print(f"[app] Writer release error: {e}", flush=True)
    try:
        cvf.cv_event.set()
    except Exception:
        pass
    print("[app] Cleanup done, exiting", flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, cleanup_handler)
signal.signal(signal.SIGINT, cleanup_handler)
```

### `cv_ctrl.py`

**Изменение 1:** Оборачиваем импорты в `try/except`.

```python
try:
    import mediapipe as mp
    MEDIAPIPE_AVAILABLE = True
except ImportError:
    MEDIAPIPE_AVAILABLE = False
    print("mediapipe not available — face/hand/pose detection disabled")
    mp = None

try:
    from picamera2 import Picamera2
    from picamera2.encoders import H264Encoder, Encoder
    from picamera2.outputs import FfmpegOutput
    CSI_CAMERA_AVAILABLE = True
except ImportError:
    CSI_CAMERA_AVAILABLE = False
    print("picamera2 not available — CSI camera disabled")

try:
    import depthai as dai
    OAK_CAMERA_AVAILABLE = True
except ImportError:
    OAK_CAMERA_AVAILABLE = False
    print("depthai not available — OAK camera disabled")
```

**Изменение 2:** Блоки `mediapipe` в `__init__` — обёрнуты в `if MEDIAPIPE_AVAILABLE:`.

**Изменение 3:** Блоки CSI/OAK — добавлены проверки `CSI_CAMERA_AVAILABLE`, `OAK_CAMERA_AVAILABLE`.

**Изменение 4:** Инициализация NPU (вместо `cv2.dnn`).

```python
# Было:
self.net = cv2.dnn.readNetFromCaffe(thisPath + '/models/deploy.prototxt', ...)
self.class_names = [...]

# Стало:
from npu_client import NPUClient
self.npu_client = NPUClient()
from npu_pose_client import NPUPoseClient
self.npu_pose_client = NPUPoseClient()
```

**Изменение 5:** Функция `cv_detect_objects` — использует NPU.

```python
def cv_detect_objects(self, img):
    """
    Legacy-функция. Реально cv_objs обрабатывается в _npu_worker через сокет.
    Эта функция вызывается только если что-то сломалось в воркере.
    """
    overlay_buffer = np.zeros_like(img)
    cv2.putText(overlay_buffer, 'NPU YOLO26s', (50, 50),
                cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2)

    try:
        detections = self.npu_client.detect(img)   # ← сокет, не subprocess
    except Exception as e:
        print(f"[cv_detect_objects] NPU error: {e}")
        self.overlay = overlay_buffer
        return

    for det in detections:
        x0, y0, x1, y1 = det['bbox']
        x0, y0, x1, y1 = int(round(x0)), int(round(y0)), int(round(x1)), int(round(y1))
        label = f"{det['class']}: {det['confidence']*100:.0f}%"
        cv2.rectangle(overlay_buffer, (x0, y0), (x1, y1), (0, 255, 0), 2)
        y = y0 - 10 if y0 - 10 > 10 else y0 + 20
        cv2.putText(overlay_buffer, label, (x0, y),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

    self.overlay = overlay_buffer
```

### `os_info.py`

**Изменение 1:** `vcgencmd` → `sysfs`.

```python
# Было:
temperature_str = os.popen('vcgencmd measure_temp').readline()

# Стало:
with open('/sys/class/thermal/thermal_zone0/temp', 'r') as f:
    temperature_str = f.read()
```

**Изменение 2:** `iwconfig` → `iw`.

```python
# get_wifi_mode
result = subprocess.check_output(['/usr/sbin/iw', 'dev', 'wlan0', 'info'],
                                 encoding='utf-8',
                                 stderr=subprocess.DEVNULL)

# get_signal_strength
output = subprocess.check_output(["/usr/sbin/iw", "dev", interface, "link"],
                                 encoding="utf-8",
                                 stderr=subprocess.DEVNULL)
```

**Изменение 3:** `eth0` → `end0`.

```bash
sed -i 's/'"'"'eth0'"'"'/'"'"'end0'"'"'/g' os_info.py
sed -i 's/self\.eth0_ip/self.end0_ip/g' os_info.py
```

## 🌐 Веб-интерфейс

### `templates/index.html`

**Проблема:** Кнопки `OBJECTS`, `COLOR`, `HAND GS` использовали `onclick="cmdSend(cv_objs,0,0);"`, который отправлял `{A,B,C}` через WebSocket (требует ESP32).

**Решение:** Заменить на `onclick="sendCmdObjs();"`.

```html
<div><button onclick="sendCmdObjs();" class="ctl_btn">OBJECTS</button></div>
<div><button onclick="sendCmdClor();" class="ctl_btn">COLOR</button></div>
<div><button onclick="sendCmdHand();" class="ctl_btn">HAND GS</button></div>
```

### `templates/control.js`

**Добавлены функции:**

```javascript
function sendCmdObjs() {
    sendCommand('base -c {"T":' + cv_objs + '}');
}

function sendCmdClor() {
    sendCommand('base -c {"T":' + cv_clor + '}');
}

function sendCmdHand() {
    sendCommand('base -c {"T":' + mp_hand + '}');
}

function sendCommand(command) {
    fetch('/send_command', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/x-www-form-urlencoded',
        },
        body: 'command=' + encodeURIComponent(command)
    })
    .then(response => response.json())
    .then(data => {
        console.log('Command sent:', command, data);
    })
    .catch(error => {
        console.error('Error:', error);
    });
}
```

**Исправлена `speedCtrl`:**

```javascript
function speedCtrl(inputSpd){
    speed_rate = inputSpd;
    defaultSpeed = speed_rate;
    var spdCtrlBtn = document.getElementById("speed_ctrl_btn");
    var spdbuttons = spdCtrlBtn.getElementsByTagName("button");
    removeButtonsClass(spdbuttons);
    if (speed_rate <= 0.30) {
        spdbuttons[0].classList.add("ctl_btn_active");
    } else if (speed_rate > 0.30 && speed_rate < 0.70) {
        spdbuttons[1].classList.add("ctl_btn_active");
    } else if (speed_rate >= 0.70) {
        spdbuttons[2].classList.add("ctl_btn_active");
    }
    sendCommand('base -c {"T":138,"L":' + speed_rate + ',"R":' + speed_rate + '}');
}
```

**Версионирование `control.js` в `index.html`:**

```html
<script src="./control.js?v=2"></script>
```

**Важно:** Параметр `?v=2` нужен, чтобы браузер **не кэшировал** старую версию.

### `templates/style.css`

```css
.video{
    width: 960px;
    height: 540px;
    overflow: hidden;
}
.video img{
    width: 100%;
    height: 100%;
    border-radius: 4px;
    object-fit: contain;
}

/* Родительский контейнер страницы */
main {
    width: 1600px;
    margin: auto;
}

/* Секция с видео */
.box1 .section_video{
    width: 1000px;
    margin-right: 10px;
}
```

**Дополнительно:** В `socket.on('update', ...)` **убрана** проверка `base_voltage` (она **блокировала** обновление OSD без ESP32).

```javascript
socket.on('update', function(data) {
    // Убрана проверка base_voltage — она блокировала OSD без ESP32
    try {
        ...
    }
});
```

## 🚀 Автозапуск `app.py` (systemd) + смотреть выше (npu-server.service, npu-pose-server.service)

**Файл:** `/etc/systemd/system/ugv.service`

```ini
[Unit]
Description=UGV Robot App
After=network.target npu-server.service npu-pose-server.service
Requires=npu-server.service npu-pose-server.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi
ExecStart=/root/ugv_rpi/ugv-env/bin/python /root/ugv_rpi/app.py
Restart=on-failure
RestartSec=5
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

**Активация:**

```bash
sudo systemctl daemon-reload
sudo systemctl enable ugv.service
sudo systemctl start ugv.service
sudo systemctl status ugv.service
```

**Логи:**

```bash
journalctl -u ugv.service -f
```
## 📡 JSON-команды (ESP32)

### Команды управления (через UART7)

| Команда | JSON | Описание |
|---|---|---|
| Движение | `{"T":1,"L":0.5,"R":0.5}` | L/R: -1.0 до 1.0 |
| PWM напрямую | `{"T":11,"L":164,"R":164}` | L/R: -255 до 255 |
| Скорость (rate) | `{"T":138,"L":0.3,"R":0.3}` | Масштаб скорости |
| OLED | `{"T":3,"lineNum":0,"Text":"..."}` | Вывод текста |
| Модуль | `{"T":4,"cmd":0}` | 0=Null, 1=RoArm, 2=PT |
| PT сервы | `{"T":133,"X":0,"Y":0,"SPD":0,"ACC":0}` | Пан-тилт |
| Свет | `{"T":132,"IO4":255,"IO5":255}` | IO4/IO5: 0-255 |
| Телеметрия | `{"T":130}` | Запрос данных |
| Поток телеметрии | `{"T":131,"cmd":1}` | Вкл/выкл |
| Интервал телеметрии | `{"T":142,"cmd":0}` | мс |

### CV-команды (детекция)

| Команда | T | Описание |
|---|---|---|
| None | 10301 | Отключить детекцию |
| Motion | 10302 | Детекция движения |
| Faces | 10303 | Детекция лиц |
| Objects | 10304 | Детекция объектов (NPU YOLO26s) |
| Color | 10305 | Детекция цвета |
| Hand GS | 10306 | Жесты рук |
| Auto | 10307 | Авто-режим |
| MP Face | 10308 | MediaPipe Face (не работает — нет mediapipe) |
| MP Pose | 10309 | NPU YOLO11_pose (17 keypoints) |

### Формат отправки (через `base_ctrl.py`)

```python
# В app.py (обработчик /send_command)
cmdline_ctrl('base -c {"T":10304}')

# cmdline_ctrl парсит:
# args = ['base', '-c', '{"T":10304}']
# base.base_json_ctrl(json.loads(args[2]))
# → UART7: {"T":10304}\n
```

### 📷 USB-камера (универсальная, UVC)

**Подключение:**
- Камера подключается в USB-порт Orange Pi 4 Pro.
- Определяется как `/dev/video0` и `/dev/video1`.
- Модуль `uvcvideo` загружается автоматически.

**Проверка:**

```bash
# Список устройств
v4l2-ctl --list-devices

# Поддерживаемые форматы
v4l2-ctl -d /dev/video0 --list-formats-ext

# Тест захвата кадра
fswebcam -d /dev/video0 --no-banner -r 1920x1080 -S 5 ./test.jpg
```

**Универсальная детекция в `cv_ctrl.py`:**

```python
def usb_camera_detection(self):
    import glob
    # 1. Быстрая проверка: есть ли /dev/video*
    video_devices = glob.glob('/dev/video*')
    if not video_devices:
        print("USB Camera not connected (no /dev/video*)")
        return False

    # 2. Надёжная проверка: пробуем открыть через OpenCV
    try:
        cap = cv2.VideoCapture(0)
        if cap.isOpened():
            ret, frame = cap.read()
            cap.release()
            if ret and frame is not None:
                print(f"USB Camera connected: {video_devices}, frame: {frame.shape}")
                return True
        cap.release()
    except Exception as e:
        print(f"USB Camera detection error: {e}")

    print("USB Camera not connected (OpenCV failed)")
    return False
```

**Преимущества:** работает для **любой** UVC-камеры, независимо от имени в `lsusb`.

## 🎥 Универсальный auto-detect FPS (2026-10-08)
**Проблема:** разные камеры поддерживают разные FPS для разных разрешений. Например:
- UGREEN SM831: 1080p30, 4K30 (нет 60 fps).
- Vitade (дешёвая): 1080p30, 720p30 (нет 60 fps).
- WSD-p8002: 1080p60, 4K30.

Если жёстко задать 720p60 — на камерах без 60 fps пайплайн GStreamer **не запускается**, видео пропадает.
**Решение:** при переключении режима камера **сама определяет максимальный FPS** для выбранного разрешения через `v4l2-ctl --list-formats-ext`.

**`GstStream.detect_max_fps(width, height, device)`:**
- Парсит вывод `v4l2-ctl --list-formats-ext` (построчно, без регулярок).
- Находит секцию **MJPG**.
- Ищет нужное разрешение (`1920x1080`, `1280x720`).
- Возвращает **максимальный** FPS для него.
- Fallback — 30, если не найдено.
**`GstStream.switch_mode(width, height, fps=None)`:**
- Если `fps` не задан — вызывает `detect_max_fps`.
- Пересоздаёт пайплайн с найденным FPS.
- Сохраняет `self.fps` — реальный FPS.

**`cv_ctrl.toggle_camera_mode()`:**
- Определяет целевое разрешение по текущему `self.gst_stream.width` (1920 → 1280, иначе → 1920).
- Вызывает `switch_mode(new_w, new_h)` **без fps**.
- Формирует `self.camera_mode` динамически: `f"{height}p{real_fps}"` (например, `1080p30`, `720p30`, `720p60`).
**`app.py` (generate_frames):**
- FPS-лимит берётся из `cvf.gst_stream.fps` (реальный), а не из хардкода.

**OSD:**
- `camera_mode` передаётся в websocket **как строка** (`'720p30'`).
- `control.js` выводит её напрямую в `res_mode` (без маппинга `'1' → '720p60'`).
**Результат:**
- Любая UVC-камера работает с максимальным FPS для каждого разрешения.
- Нет пропадания видео при переключении.
- OSD показывает **реальный** режим (например, `720p30`, а не `720p60`).

**Файлы:**
- `gst_stream.py` — `detect_max_fps`, `switch_mode(width, height, fps=None)`.
- `cv_ctrl.py` — `toggle_camera_mode` без хардкода.
- `app.py` — FPS-лимит из `gst_stream.fps`.
- `templates/control.js` — `res_mode` = `data[camera_mode]` напрямую.

### 🎥 Запись видео — финальное решение (2026-10-02)

**Требование:** во время записи **живой поток не должен прерываться** (оператор смотрит обстановку, а запись — для фиксации событий).

**Архитектура (GStreamer `tee`):**

v4l2src → image/jpeg (MJPG, 1920x1080@30) → jpegdec → tee
├─ queue (leaky) → videoconvert → BGR → appsink (живой поток — ВСЕГДА)
└─ queue → videoconvert → videoscale → 1280x720 I420
→ avenc_mjpeg → matroskamux → filesink (ветка записи, добавляется/убирается на лету)


**Ключевые решения:**

1. **`tee` после `jpegdec`** — камера читается один раз, поток идёт в две ветки. Живой поток **не прерывается** во время записи.
2. **Контейнер `.mkv` (matroskamux), не `.avi`** — потому что камера даёт VFR (в темноте FPS падает до 15). AVI не хранит VFR корректно → видео ускорялось (10 сек → 7 сек). Matroska сохраняет реальные PTS → 10 секунд реального времени = 10 секунд в файле.
3. **Энкодер `avenc_mjpeg`** (libav), не `jpegenc` — корректно ставит PTS.
4. **`omxh264videoenc` и `omxmjpegvideoenc` НЕ используются** — в этой сборке они требуют `video/x-raw(memory:DMABuf)`, а `videoconvert` не может выдать DMABuf. Падает с `Failed to configure the buffer pool`.
5. **Правильный teardown ветки записи:**
   - **Отлинковать `rec_tee_pad` от `rec_queue`** (иначе EOS уйдёт вверх и убьёт живой поток).
   - **Отправить EOS через sink pad `rec_queue`** — закроет `matroskamux`, запишет индекс.
   - `set_state(NULL)` → `pipeline.remove()`.
6. **`v4l2src do-timestamp=true`** (по умолчанию) — PTS берутся из системных часов, что даёт **корректную длительность при VFR**.

**Параметры:**
- Разрешение записи: **1280x720** (масштабируется через `videoscale`).
- Битрейт MJPEG: **8 Мбит/с**, `qmin=3`, `qmax=10`.
- Размер файла: ~10 МБ на 10 секунд.

**Ограничение:** `.mkv` не проигрывается в браузере и Windows Media Player. Для просмотра — скачать и открыть в VLC / MPC-HC / ffmpeg-совместимом плеере.

### 🐛 Фикс счётчика FPS (2026-10-02)

**Проблема:** OSD показывал **удвоенный FPS** (60 вместо 30).

**Причина:** счётчик `fps_count += 1` вызывался **дважды за кадр** — в `_process_frame_opencv` и в `frame_process`. Плюс `generate_frames` в `app.py` крутился в **свободном цикле** без ограничения.

**Решение:**
- **Один** счётчик — в начале `frame_process`.
- В `generate_frames` добавлен **`time.sleep`** для лимита **30 FPS** (совпадает с реальным FPS камеры).
- Побочный эффект: **задержка видео упала** с ~0.9 сек до ~0.1 сек, **CPU** — с 24% до 17%. Потому что мы перестали кормить браузер кадрами, которые он не успевал рисовать, и не гоняли `cv2.imencode` вхолостую.

### 📁 UI галереи видео (2026-10-02)

- `get_video_names` в `app.py` — фильтр расширений: `.mp4`, `.avi`, `.mkv`.
- `control.js` — `strippedname` убирает любое из этих расширений; ссылка скачивает файл (`download`), имя без расширения.


### 🎛️ NPU по кнопке без ESP32

**Проблема:** Кнопка `OBJECTS` отправляла `{"T":10304}` в UART, но **без ESP32** — `cv_mode` не обновлялся, NPU не включался.

**Решение:** Локальный `cmd_action` в `handle_command` (`app.py`):

```python
def handle_command():
    command = request.form['command']
    print("Received command:", command)
    cvf.info_update("CMD:" + command, (0,255,255), 0.36)

    # Локально устанавливаем CV-режим (для NPU без ESP32)
    try:
        if command.startswith('base -c '):
            import json
            cmd_json = json.loads(command[8:])  # "base -c " = 8 символов
            t_value = cmd_json.get('T')
            if t_value in cmd_actions:
                cmd_actions[t_value]()
                print(f"[handle_command] Local cmd_action executed for T={t_value}")
    except Exception as e:
        print(f"[handle_command] Local cmd_action error: {e}")

    try:
        cmdline_ctrl(command)
    except Exception as e:
        print(f"[app.handle_command] error: {e}")
    return jsonify({"status": "success", "message": "Command received"})
```

**Результат:** Кнопка `OBJECTS` **локально** включает NPU (без ESP32) **и одновременно** отправляет команду в UART (для ESP32).
- Кнопка OBJECTS → YOLO26s.
- Кнопка MP POSE → YOLO11_pose (не MediaPipe).

### 📊 OSD (On-Screen Display)

**Проблема:** OSD не обновлялся — `CPU: 0, RAM: 0, FPS: 0, TEMP: 0, RSSI: 0`.

**Причина 1:** В `control.js` была проверка `if (data[base_voltage] != 0)`, которая **блокировала** обновление OSD без ESP32.

**Решение 1:** Убрана проверка `base_voltage`:

```javascript
socket.on('update', function(data) {
    // Убрана проверка base_voltage — она блокировала OSD без ESP32
    try {
        ...
    }
});
```

**Причина 2:** Температура в `sysfs` — **millidegrees** (24242 = 24.242 °C).

**Решение 2:** Деление на 1000:

```python
# os_info.py — get_cpu_temperature
with open('/sys/class/thermal/thermal_zone0/temp', 'r') as f:
    temperature_str = f.read().strip()
temperature = float(temperature_str) / 1000.0
return round(temperature, 1)
```

**Причина 3:** RSSI — `iw dev wlan0 link` отдаёт `signal: -59 dBm`, а код искал `Signal level=-59` (старый формат `iwconfig`).

**Решение 3:** Новое регулярное выражение:

```python
# os_info.py — get_signal_strength
signal_strength = re.search(r"signal:\s*(-\d+)", output)
if signal_strength:
    return int(signal_strength.group(1))
return 0
```

**Результат:** OSD полностью работает:
- **CPU: 8.2%**
- **RAM: 3.7%**
- **FPS: 15.0**
- **TEMP: 23.7 °C**
- **RSSI: -59 dBm**
- **Photos: 0.45 MB**
- **Videos: 2.76 MB**

### 🐛 Убрана `NoneType` ошибка

**Проблема:** `base.base_data['v']` — `base_data = None` (ESP32 не подключён).

**Решение:** `base.base_data['v'] if base.base_data else 0`:

```python
# app.py — update_data_websocket_single
f['fb'][f'base_voltage']:base.base_data['v'] if base.base_data else 0,
```

### 📝 Обновления в CSS

**`templates/style.css`** — актуальный вид (см. также раздел «🌐 Веб-интерфейс»):

```css
.video {
    width: 960px;
    height: 540px;
    overflow: hidden;
}
.video img {
    width: 100%;
    height: 100%;
    border-radius: 4px;
    object-fit: contain;
}
main { width: 1600px; margin: auto; }
.box1 .section_video { width: 1000px; margin-right: 10px; }
```
## 🆕 Обновления (2026-10-01)

### 💾 Ручное копирование на eMMC (обход бага `armbian-install`)

**Проблема:** `armbian-install` **неправильно копирует** `boot_package` на eMMC. После **выключения** система **не загружается** (`bad magic`, `Loading boot-pkg fail`).

**Решение:** **ручное копирование** системы с SD на eMMC (см. **раздел «Установка ОС»**).

**Важно:** после ручного копирования **обязательно** скопировать **20 МБ** загрузчика:
```bash
sudo dd if=/dev/mmcblk1 of=/dev/mmcblk0 bs=1M count=20 conv=notrunc
```
### 📏 Расширение eMMC до 29 ГБ

**Проблема:** после ручного копирования раздел eMMC — **7.2 ГБ** (как SD).

**Решение:**
```bash
sudo parted /dev/mmcblk0 resizepart 1 100%
sudo resize2fs /dev/mmcblk0p1
```
### 🛑 `TimeoutStopSec=5`, `KillMode=control-group`, `SendSIGKILL=yes`

**Проблема:** `systemctl stop ugv.service` и `shutdown -h now` **занимали 90 секунд** (systemd **ждал** завершения `app.py`), что **повреждало eMMC**.

**Решение:** добавлены параметры в `ugv.service`:
```ini
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes
```
### 🐍 Обработчик `SIGTERM` в `app.py`

**Проблема:** `app.py` **не завершался** корректно при `SIGTERM` (от systemd), что **блокировало shutdown**.

**Решение:** добавлен **обработчик `SIGTERM`** (закрывает камеру, writer, выходит):
```python
import signal
import sys

def cleanup_handler(signum, frame):
    print(f"[app] Received signal {signum}, cleaning up...", flush=True)
    try:
        if hasattr(cvf, "camera") and cvf.camera:
            cvf.camera.release()
    except Exception as e:
        print(f"[app] Camera release error: {e}", flush=True)
    try:
        if hasattr(cvf, "writer") and cvf.writer:
            cvf.writer.release()
    except Exception as e:
        print(f"[app] Writer release error: {e}", flush=True)
    try:
        cvf.cv_event.set()
    except Exception:
        pass
    print("[app] Cleanup done, exiting", flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, cleanup_handler)
signal.signal(signal.SIGINT, cleanup_handler)
```

### 📡 GstStream: поток без прерывания

**Проблема:** при записи через `vpu_record.sh` (FFmpeg + GStreamer) **поток** **прерывался** (камера **освобождалась**).

**Решение:** **GStreamer** `tee`:
- **Ветка 1:** `jpegdec → videoconvert → BGR → appsink` (для Flask).
- **Ветка 2:** **динамически** **добавляется** **при** **записи** (`avenc_mjpeg → matroskamux → .mkv `).

**Ключевое:** **поток** **не** **прерывается** — **камера** **читается** **один** **раз**.

**Файл:** `gst_stream.py`.

**Интеграция:**
- `cv_ctrl.py` — `GstStream` **вместо** `cv2.VideoCapture`.
- `frame_process` — **берёт** **BGR** **из** `GstStream.get_frame()`.
- `video_record(True)` — **вызывает** `GstStream.start_recording()`.

**Результат:** **поток** **работает** **параллельно** **с** **записью**.


- **GStreamer `tee`**  — запись без прерывания живого потока. Контейнер `.mkv` (matroskamux) — VFR сохранён. `avenc_mjpeg` вместо `jpegenc`. Фикс счётчика FPS (двойной инкремент → один). Лимит `generate_frames` 30 FPS → задержка 0.1 сек, CPU 17%. UI галереи: фильтр `.mp4`/`.avi`/`.mkv`, скачивание вместо открытия в браузере.

### 🎥 Переключение режима камеры (1080p30 ↔ 720p60)

**Задача:** дать оператору выбор между **детализацией (1080p30)** и **плавностью (720p60)** прямо из веб-интерфейса.

**Решение:**
- Кнопка **WebRTC** (была бесполезной) **перепрофилирована** в переключатель режима камеры.
- При нажатии отправляется команда `{"T":10104}` → `cvf.toggle_camera_mode()`.
- `GstStream.switch_mode(width, height, fps)` — пересоздаёт пайплайн с новыми параметрами (видео моргает ~1 сек, поток не падает).
- Режимы:
  - **1080p30** — детализация, CPU ~28%, задержка минимальна.
  - **720p60** — плавность, CPU ~26%, USB 2.0 тянет стабильно 57-60 fps.

**Индикатор в OSD:**
- В правом верхнем углу, **под `TEMP:`** добавлена строка **`RES: 1080p30`** / **`RES: 720p60`**.
- Обновляется через websocket в реальном времени.
- HTML: `<p><span>RES: </span><span id="res_mode">1080p30</span></p>`.
- `config.yaml`: `fb.camera_mode: 116`.
- `app.py` → `update_data_websocket_single()`: `f['fb']['camera_mode']: '1' if camera_mode == '720p60' else '0'`.
- `control.js` в `socket.on('update')`: обновляет `res_mode` по `data[camera_mode]`.

**Динамический FPS-лимит:**
- `generate_frames` в `app.py` подстраивает `target_dt` под текущий режим:
  - 1080p30 → 30 FPS cap,
  - 720p60 → 60 FPS cap.
- Это позволяет счётчику FPS в OSD **отражать реальную частоту камеры**.

**Команды config.yaml:**
- `cam_toggle: 10104` (в `code`),
- `camera_mode: 116` (в `fb`).

**Проверено:**
- ✅ Переключение **на горячую** (без перезагрузки).
- ✅ **NPU (OBJECTS)** работает в обоих режимах, не падает при переключении.
- ✅ **Запись видео** работает в обоих режимах (файл .mkv, 720p, 60 fps реальных).
- ✅ **FPS в OSD** меняется (30 ↔ 57-60).

## 🆕 Обновления (2026-10-05): YOLO11s на NPU Allwinner A733 (INT8, 27 FPS)
> ⚠️ **Не актуально** — заменено на YOLO26s (см. выше). Оставлено как история.

**Итог:** YOLO11s успешно сконвертирована в INT8, собирается и работает на NPU A733 (Orange Pi 4 Pro).
**Скорость:** ~37 мс на инференс (~27 FPS) + ~5 мс постобработка на CPU.
**Точность:** правильная детекция (`dog: 92%`, `bicycle: 94%`, `truck: 50%` на тестовом `dog.jpg`).

### Почему YOLO11s, а не YOLOv9

YOLOv9 **не поддерживается** официально Allwinner для A733 (нет в Model Zoo v0.9.0 / v1.1.0).
YOLO11s **официально поддержан** — есть `examples/yolo11/` с **готовыми скриптами конвертации**, **C++ постобработкой**, **датасетом** для калибровки.
YOLO11s — **новее, легче, точнее** YOLOv9 при том же размере.

### Пайплайн конвертации

**Источник ONNX:** `https://netstorage.allwinnertech.com:5001/sharing/SwiiD4rGh` — готовый `yolo11s_6.onnx` от Allwinner (37 МБ, **уже обрезан через `onnx_extract.py`**: постобработка вынесена на CPU).

**Model Zoo v1.1.0:** скачивается с [Allwinner Customer Service Platform](https://www.aw-ol.com/) — регистрация → **Resource Download** → **AI Development SDK** → **AWNPU_ModelZoo v1.1.0** (303 МБ).

**Docker-образ:** `ubuntu-npu:v2.0.10.2` (ACUITY Toolkit 6.30.22, IDE 5.11.0).

**Скрипты конвертации** (в `examples/yolo11/convert_model/`):

```bash
# 1. Симлинки на общие скрипты
./convert_model_env.sh

# 2. Импорт ONNX → ACUITY JSON+DATA
./pegasus_import.sh yolo11s_6

# 3. Квантизация (uint8, 12 калибровочных изображений из coco_12)
./pegasus_quantize.sh yolo11s_6 uint8 12

# 4. Экспорт в .nb для A733
./pegasus_export_ovx_nbg.sh yolo11s_6 uint8 a733
```

**Результат:** `yolo11s_6_uint8_a733.nb` — **6.6 МБ** (INT8, в 5.5 раз меньше FP32 ONNX).
**Копируется** в `examples/yolo11/model/`.

### Нативная сборка C++ демо на Orange Pi

**Проблема:** стандартный `build_linux.sh` рассчитан на **кросс-компиляцию** (x86_64 → aarch64) с **toolchain 10.3** и **bundled OpenCV**. На Orange Pi это **не работает** — нужен **нативный gcc** и **системный OpenCV**.

**Решение — патч `CMakeLists.txt`:**

1. **`SYS_ARCH`** — принудительно `linux_aarch64` (gcc не содержит `aarch64` в имени, CMake не угадает).
2. **OpenCV** — через **`pkg-config opencv4`** (не `find_package`, которое на Ubuntu 24.04 / OpenCV 4.10 не заполняет `OpenCV_LIBS`).
3. **`_GLIBCXX_USE_CXX11_ABI`** — **убрать `=0`** (новый ABI обязателен для совместимости с системным OpenCV 4.10; старый ABI даёт `undefined reference to cv::imread`).

**Сборка:**

```bash
cd /root/ugv_rpi/yolo11/
./build_native.sh
```

**Готовый бинарник:** `yolo11_demo_a733` (145 КБ).

### Тест

```bash
cd /root/ugv_rpi/yolo11/
LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./yolo11_demo_a733 -nb yolo11s_6_uint8_a733.nb \
   -i /root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolo11/model/dog.jpg \
   -l 1 -m 10
```

**Вывод:**

```
input  0 dim 3 640 640 1, ... none-quant
output 0 dim 80 80 64 1, ... none-quant
output 1 dim 80 80 80 1, ... none-quant
output 2 dim 40 40 64 1, ...
output 3 dim 40 40 80 1, ...
output 4 dim 20 20 64 1, ...
output 5 dim 20 20 80 1, ...
run time for this network 0: 36783 us.
post process time : 5 ms
detection num: 3
 1:  94%, [ 126,  129,  568,  419], bicycle
16:  92%, [ 132,  220,  311,  541], dog
 7:  50%, [ 465,   74,  692,  170], truck
```

### Файлы в репозитории

Папка **`yolo11/`**:
- `yolo11s_6_uint8_a733.nb` — модель (INT8, 6.6 МБ).
- `yolo11_demo_a733` — бинарник.
- `main.cpp`, `yolo11_6_post.cpp`, `yolo11_6_pre.cpp` — исходники.
- `model_config.h` — конфиг.
- `CMakeLists.txt.patched` — патч для нативной сборки.
- `CMakeLists.txt.orig` — оригинал.
- `build_native.sh` — скрипт сборки.
- `README.md` — инструкция.

### Что это даёт проекту

- **NPU-детекция** — теперь на **YOLO11s** (быстрее, точнее стандартной собранной модели YOLOv5s в allwinner-model-zoo).
- **Готовый пайплайн** — можно пересобрать модель под **другие задачи** (pose, seg, depth — есть в `examples/yolo11_pose/`, `yolo11_seg/`, `yolo26_depth/`).
- **Официальная поддержка** Allwinner — не самодельная конвертация.

## 🧠 YOLO26 Depth на NPU (карта глубины) (2026-10-08)

**Итог:** модель YOLO26 depth (nano) сконвертирована в NBG, работает на NPU A733. **65 мс** на инференс (~11 FPS) с квантизацией PCQ. Заменяет монокулярную оценку глубины через тяжёлые CPU-модели. Дополняет лидар D500 — depth даёт объём, лидар — точные расстояния в срезе.
### Что такое YOLO26 depth

Монокулярная оценка глубины по **одному RGB-кадру**. Выход — **карта глубины** (heatmap) 768×768, значения в метрах. Позволяет:
- видеть препятствия выше/ниже среза лидара,
- дополнять SLAM (стекло, прозрачные поверхности),
- давать роботу «объёмное» зрение для объезда.
### Пайплайн конвертации

1. **Скачать** `yolo26n-depth.pt` (Ultralytics, 12.4 МБ) или `yolo26s-depth.pt` (s-версия).
2. **Экспорт ONNX** через Ultralytics (venv в VirtualBox):
   - nano: `imgsz=768, opset=14, simplify=True`.
   - результат: `yolo26n-depth.onnx` (~20 МБ).
3. **Docker ACUITY** (`ubuntu-npu:v2.0.10.2`, Model Zoo v1.1.0):
   - `./pegasus_import.sh yolo26n-depth`
   - `./pegasus_quantize.sh yolo26n-depth pcq 10` ← **PCQ (INT8)**, не int16.
   - `./pegasus_export_ovx_nbg.sh yolo26n-depth pcq a733`
4. **Результат:** `yolo26n-depth_pcq_a733.nb` (10.4 МБ).

**Важно:** `config_yml.py` из `examples/yolo26_depth/convert_model/` универсальный — mean=[0,0,0], scale=[1/255,1/255,1/255], IMAGE_RGB. Для nano и s-версии — одинаковые параметры (обе обучались на NYU Depth V2).
### Сравнение вариантов (тесты на `rgb_00285.jpg`, A733)

| Модель | Квантизация | Размер .nb | Run time | FPS | Качество |
|---|---|---|---|---|---|
| **yolo26s-depth** | int16 | 21.3 МБ | 244 мс | 4.1 | 🟢 высокое |
| **yolo26n-depth** | int16 | 9.7 МБ | 158 мс | 5.3 | 🟢 приемлемое |
| **yolo26n-depth** | **PCQ** | **10.4 МБ** | **65 мс** | **~11** | 🟢 **приемлемое** |
| yolo26s-depth | PCQ | — | — | — | 🔴 шумное (отпадает) |

**Ключевой вывод:** nano + PCQ — **лучший баланс** (65 мс, ~11 FPS, приемлемое качество). s + PCQ даёт шум из-за `Exp()` в конце модели (INT8 усиливает ошибку экспоненциально).
### Почему PCQ для nano работает, а для s — нет

YOLO26-depth имеет **`Exp()` на выходе** — малейшая ошибка в log-глубине экспоненциально усиливается при INT8-квантизации.
- **s-модель** (больше параметров) накапливает больше ошибок → PCQ шумит.
- **n-модель** (меньше параметров) — квантуется лучше → PCQ даёт приемлемое качество.

**Отсюда:** int16 для точности (медленно), PCQ для скорости (для nano — с приемлемым качеством).
### Стратегия использования

**Основной режим:** `yolo26n-depth_pcq_a733.nb` — 65 мс, ~11 FPS.

**Резерв (если понадобится точность):**
- `yolo26n-depth_int16_a733.nb` — 158 мс, запас.
- `yolo26s-depth_int16_a733.nb` — 244 мс, для редкого уточнения.

**Не использовать:** `yolo26s-depth_pcq` — шумно.

**Комбо-режим (резерв на будущее):** nano-PCQ постоянно + s-int16 раз в 1-2 сек для уточнения. Если реальные испытания покажут, что одного nano не хватает — включим.
### Демо на Orange Pi

```bash
cd /root/ugv_rpi/yolo26_depth/

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./yolo26_depth_demo \
  -nb model/yolo26n-depth_pcq_a733.nb \
  -i model/rgb_00285.jpg \
  -l 1 -m 10
```
**Вывод:**
```text
input  0 dim 3 768 768 1
output 0 dim 768 768 1 1
create network 0: 10405 us
prepare network: 5349 us
run time for this network 0: 65368 us  ← 65 мс
depth postprocess time: 3.33 ms
saved heatmap: output_depth_heatmap.jpg
```
### Нативная сборка

```bash
cd /root/ugv_rpi/yolo26_depth/
./build_native.sh
```
**Ключевые фиксы:**
1. `SYS_ARCH=linux_aarch64` — принудительно.
2. OpenCV через `pkg-config opencv4` — не `find_package`.
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. Собирать только `yolo26_depth_demo` из 3 файлов (`main.cpp`, `yolo26_depth_pre.cpp`, `yolo26_depth_post.cpp`).
6. `build_native.sh` — собирает в своей папке (не копирует в Model Zoo).
### Файлы в репозитории

- `yolo26_depth/main.cpp` — демо.
- `yolo26_depth/yolo26_depth_pre.cpp` — letterbox 768×768.
- `yolo26_depth/yolo26_depth_post.cpp` — heatmap + JET colormap.
- `yolo26_depth/model_config.h` — INPUT=768, OUTPUT=768.
- `yolo26_depth/CMakeLists.txt.patched` — нативная сборка.
- `yolo26_depth/build_native.sh` — скрипт сборки.
- `yolo26_depth/model/yolo26n-depth_pcq_a733.nb` — рабочая модель.
- `yolo26_depth/model/yolo26n-depth_int16_a733.nb` — запас.
- `yolo26_depth/model/yolo26s-depth_int16_a733.nb` — точная (редко).
- `yolo26_depth/model/rgb_00285.jpg` — тестовое изображение.

**🚧 Что дальше:**
- [ ] Комбо-режим (nano-PCQ + s-int16) — если понадобится.


## 🎨 YOLO11 Segmentation на NPU (маски объектов)

**Итог:** модель YOLO11s-seg сконвертирована в NBG, работает на NPU A733. **45 мс** на инференс (~22 FPS) с квантизацией uint8. Выделяет пиксельные маски каждого объекта (instance segmentation).
### Что такое YOLO11-seg

**Instance segmentation** — модель не просто рисует рамку, а **выделяет пиксели каждого объекта** (маска-полигон). Для каждого найденного объекта — контур.

**Зачем нам:**
- Точные границы объектов (не «где-то тут человек», а «вот эти пиксели»).
- Форма объекта (площадь, ориентация).
- Для демонстраций — красиво и наглядно.
- Для взаимодействия — знать, где именно объект.
### Пайплайн конвертации

1. **Скачать** `yolo11s-seg_10.onnx` (Allwinner, 40 МБ) — **уже обрезанный через `onnx_extract.py`** (10 выходов: bbox + class + mask coeffs + protos × 3 уровня + protos).
2. **Docker ACUITY** (`ubuntu-npu:v2.0.10.2`, Model Zoo v1.1.0):
   - `./pegasus_import.sh yolo11s-seg_10`
   - `./pegasus_quantize.sh yolo11s-seg_10 uint8 12` ← **uint8**, не int16/pcq.
   - `./pegasus_export_ovx_nbg.sh yolo11s-seg_10 uint8 a733`
3. **Результат:** `yolo11s-seg_10_uint8_a733.nb` (7.3 МБ).

**Важно:** в README Allwinner указано, что **постобработка seg при 8bit даёт потери точности**, поэтому её вынесли на CPU (C++). Это и есть смысл обрезки через `onnx_extract.py`.
### Результаты теста (dog.jpg)
```
| Метрика | Значение |
|---|---|
| **run time (inference)** | **45.2 мс** (~22 FPS) |
| **post process** | 11 мс |
| **Размер .nb** | 7.3 МБ |
```

**Детекции:**

```
detection num: 3
1: 95%, [ 127, 125, 568, 420], bicycle
16: 96%, [ 132, 221, 311, 541], dog
2: 85%, [ 466, 75, 691, 172], car
```
```text
**Качество:** маски чёткие, объекты выделены точно. Заметно лучше, чем PCQ у depth (у seg нет `exp()` на выходе — квантизация не убивает точность).
```
### Сравнение со всеми моделями проекта

| Модель | Квантизация | Run time | FPS | Качество |
|---|---|---|---|---|
| YOLO26s (детекция) | PCQ | ~35 мс | 29 | 🟢 |
| YOLO11_pose | uint8 | ~62 мс | 16 | 🟢 |
| **YOLO11_seg** | **uint8** | **45 мс** | **~22** | 🟢 |
| YOLO26n_depth | PCQ | 65 мс | ~11 | 🟢 |

Seg работает **быстрее**, чем pose и depth — почти как детекция.

### Демо на Orange Pi

```bash
cd /root/ugv_rpi/yolo11_seg/

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./yolo11_seg_demo \
  -nb model/yolo11s-seg_10_uint8_a733.nb \
  -i model/dog.jpg \
  -l 1 -m 10
```
```text
input  0 dim 3 640 640 1
output 0 dim 80 80 64 1   (bbox)
output 1 dim 80 80 80 1   (class)
output 2 dim 80 80 32 1   (mask coeffs)
...
output 9 dim 160 160 32 1 (protos)
run time for this network 0: 45220 us  ← 45 мс
post process time : 11 ms
detection num: 3
```
### Нативная сборка

```bash
cd /root/ugv_rpi/yolo11_seg/
./build_native.sh
```
**Ключевые фиксы:**
1. `SYS_ARCH=linux_aarch64` — принудительно.
2. OpenCV через `pkg-config opencv4`.
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. Собирать только `yolo11_seg_demo` из 3 файлов (`main.cpp`, `yolo11_seg_10_pre.cpp`, `yolo11_seg_10_post.cpp`).
6. `build_native.sh` — собирает в своей папке.

### Файлы в репозитории

- `yolo11_seg/main.cpp` — демо.
- `yolo11_seg/yolo11_seg_10_pre.cpp` — letterbox 640×640.
- `yolo11_seg/yolo11_seg_10_post.cpp` — маски + bbox.
- `yolo11_seg/model_config.h` — INPUT=640, 10 выходов.
- `yolo11_seg/CMakeLists.txt.patched` — нативная сборка.
- `yolo11_seg/build_native.sh` — скрипт сборки.
- `yolo11_seg/model/yolo11s-seg_10_uint8_a733.nb` — модель.
- `yolo11_seg/model/dog.jpg` — тестовое изображение.

## 🎛️ NPU-сервисы (4 одновременно)

В проекте работают **4 NPU-сервера** через UNIX-сокеты. Все запускаются автоматически при старте системы.
| Сервис | Модель | Сокет | Время |
|---|---|---|---|
| `npu-server.service` | YOLO26s (детекция) | `/tmp/npu11.sock` | ~35 мс |
| `npu-pose-server.service` | YOLO11_pose (позы) | `/tmp/npu_pose.sock` | ~62 мс |
| `npu-seg-server.service` | YOLO11_seg (маски) | `/tmp/npu_seg.sock` | ~45 мс |
| `npu-depth-server.service` | YOLO26n_depth (глубина) | `/tmp/npu_depth.sock` | ~65 мс |
**Python-клиенты (в корне репозитория):**
- `npu_client.py` — YOLO26s.
- `npu_pose_client.py` — YOLO11_pose.
- `npu_seg_client.py` — YOLO11_seg.
- `npu_depth_client.py` — YOLO26n_depth.

**Особенности:**
- Все 4 сервиса работают параллельно — NPU справляется.
- Переключение между режимами в вебе — мгновенное ~ 0.2 сек (без subprocess).
- Модели копируются в `/dev/shm` (RAM-диск) через `ExecStartPre`.

## 🎛️ Кнопки CV в веб-интерфейсе

| Кнопка | Режим | Что делает |
|---|---|---|
| **OBJECTS** | YOLO26s | Детекция объектов (bbox + класс + confidence) |
| **DEPTH** | YOLO26n_depth | Карта глубины (16×16 сетка + красная подсветка близких пикселей) |
| **SEG** | YOLO11_seg | Маски объектов (RLE-сжатие) |
| **COLOR** | OpenCV | Трекинг цвета |
| **HAND GS** | MediaPipe | Жесты (не работает без MediaPipe) |
| **MP FACE** | MediaPipe | Лица (не работает без MediaPipe) |
| **MP POSE** | YOLO11_pose | Скелет (17 keypoints, COCO) |
| **None** (Simple Detection Type) | — | Отключить CV |

**Расположение в интерфейсе:**
- **Advance CV Funcs** — OBJECTS, COLOR, HAND GS (3 кнопки).
- **NPU Depth/Seg** — DEPTH, SEG (2 кнопки).
- **MediaPipe Funcs** — MP FACE, MP POSE.

**Подсветка активной кнопки:** автоматически через Socket.IO (событие `update`).


## 💾 NVMe SSD + SPI Flash (2026-10-09)

**Итог:** система **загружается с NVMe** (570 МБ/с) **без eMMC и без SD-карты**. U-Boot прошит в **SPI Flash** с патчем питания (`dc1sw1`). eMMC — **бэкап** (лежит на полке).
### Что сделано

1. **SPI Flash прошит** — U-Boot с патчем `pcie3v3_supply = "dc1sw1"` (вместо `dc1sw2`).
2. **Система перенесена на NVMe** — `rsync` + правка `armbianEnv.txt` (UUID NVMe) + `/etc/fstab`.
3. **NVMe работает на Gen3** — 8.0 GT/s PCIe, **570 МБ/с** (было 205 МБ/с на eMMC).
4. **eMMC вынута** — загрузка идёт **SPI → NVMe**.
### Порядок загрузки
```text
BootROM: SD → eMMC → SPI → USB
U-Boot (boot_targets): SD → eMMC → NVMe → USB
```
### Причина, почему U-Boot не видел NVMe

**Питание M.2 слота.** U-Boot включал **не тот ключ** 3.3 В: `dc1sw2` вместо `dc1sw1`. Слот был **обесточен** → PCIe-линк **не тренировался** → NVMe **не определялся**.

**Linux не замечал** — ядро помечает **оба** ключа (`dc1sw1`, `dc1sw2`) как `regulator-always-on`, поэтому включает **оба**. NVMe «оживал» при загрузке ядра — и казалось, что нужна SD-карта.

**Патч:** `pcie3v3_supply = "dc1sw1"` (в `board-uboot.dts`).
### Как повторить

**1. Сборка U-Boot с патчем (VirtualBox):**
```bash
cd ~/orangepi-build
git clone https://github.com/TblP/orangepi-uboot-fix.git /tmp/uboot-fix
mkdir -p userpatches/u-boot/u-boot-sunxi/
cp /tmp/uboot-fix/patches/0001-orangepi4pro-pcie-3v3-dc1sw1.patch userpatches/u-boot/u-boot-sunxi/
sudo ./build.sh BOARD=orangepi4pro BRANCH=current RELEASE=jammy BUILD_OPT=u-boot KERNEL_CONFIGURE=no CLEAN_LEVEL=make
```
**2. Установка на Orange Pi:**
```bash
sudo dpkg -r linux-u-boot-orangepi4pro-vendor
sudo dpkg -i linux-u-boot-current-orangepi4pro_1.1.0_arm64.deb
sudo dpkg -r linux-u-boot-orangepi4pro-vendor
sudo dpkg -i linux-u-boot-current-orangepi4pro_1.1.0_arm64.deb
```
**3. Прошивка SPI:**
```bash
sudo dd if=/dev/mtd0 of=/root/spi_backup_$(date +%Y%m%d_%H%M%S).bin bs=1M count=16
sudo flash_erase /dev/mtd0 0 50
sudo mtd_debug write /dev/mtd0 0 $(stat --format="%s" /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot0_spinor_a733.fex) /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot0_spinor_a733.fex
sudo mtd_debug write /dev/mtd0 262144 $(stat --format="%s" /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot_package.fex) /usr/lib/linux-u-boot-current-orangepi4pro_1.1.0_arm64/boot_package.fex
sudo sync
```
**4. Перенос системы на NVMe:**
```bash
sudo parted /dev/nvme0n1 mklabel gpt
sudo parted /dev/nvme0n1 mkpart primary ext4 0% 100%
sudo mkfs.ext4 -F /dev/nvme0n1p1
sudo mkdir -p /mnt/nvme && sudo mount /dev/nvme0n1p1 /mnt/nvme
sudo rsync -aAXHv --exclude={/dev/*,/proc/*,/sys/*,/tmp/*,/run/*,/mnt/*,/media/*,/lost+found} / /mnt/nvme/
sudo sed -i 's|^rootdev=.*|rootdev=UUID=<UUID_NVME> rootdelay=5|' /mnt/nvme/boot/armbianEnv.txt
sudo sed -i 's|<UUID_EMMC>|<UUID_NVME>|g' /mnt/nvme/etc/fstab
sudo sync && sudo umount /mnt/nvme
```
**5. Выключить, вынуть eMMC, включить.**
### Результат
```text
$ findmnt -no SOURCE,UUID /
/dev/nvme0n1p1 e4384603-9cf2-4342-ba68-bb64e3e921e4

$ cat /sys/bus/pci/devices/0000:01:00.0/current_link_speed
8.0 GT/s PCIe

$ df -h /
/dev/nvme0n1p1 117G 11G 101G 10% /
```
**Скорость:** 570 МБ/с (NVMe Gen3 x1) vs 205 МБ/с (eMMC).

## 🎯 ArUco-маркеры (2026-10-09)

**Итог:** детекция ArUco-маркеров через OpenCV (CPU). Точность — **±2 см на 1.35 м** (~1.5%). Кнопка **ARUCO** в веб-интерфейсе.

### Что такое ArUco

**ArUco-маркер** — квадратный чёрно-белый код. Камера видит маркер, OpenCV вычисляет **позицию** (X, Y, Z в метрах) и **ориентацию** (углы). Используется для **точной парковки** и **навигации**.

**Преимущества:**
- Точность **±2 см** на 1.35 м.
- Работает **на CPU** (не грузит NPU).
- **15 FPS** при 1080p.
- Дёшево (нужна только бумага + принтер).
### Параметры

| Параметр | Значение |
|---|---|
| **Словарь** | `DICT_4X4_50` |
| **Размер маркера** | 14.6 см (чёрный квадрат) |
| **Focal length** | 1041.7 (откалибровано) |
| **Разрешённые ID** | 0, 1, 2, 3 |
| **Точность** | ±2 см на 1.35 м (~1.5%) |
| **FPS** | 15 (при 1080p) |
### Файлы

- `aruco_client.py` — клиент (детекция + поза + отрисовка).
- `aruco_detect.py` — демо-скрипт (синтетика + камера).
- `aruco_marker_0..3.png` — сгенерированные маркеры (14.6 см).

### Использование

**1. Сгенерировать маркеры:**
```bash
python3 -c "
import cv2, cv2.aruco as aruco
d = aruco.getPredefinedDictionary(aruco.DICT_4X4_50)
for i in range(4):
    img = aruco.generateImageMarker(d, i, 400)
    img = cv2.copyMakeBorder(img, 40, 40, 40, 40, cv2.BORDER_CONSTANT, value=255)
    cv2.imwrite(f'aruco_marker_{i}.png', img)
"
```
**2. Напечатать** (или показать на телефоне). **Измерить** размер чёрного квадрата (14.6 см).
**3. В веб-интерфейсе** — нажать **ARUCO** → показать маркер камере.
### Калибровка

**Проблема:** `camera_matrix` без калибровки даёт ошибку **+23%**.

**Решение:** вычисление `focal_length` по **известному расстоянию**:
```python
focal_real = focal_current × (Z_real / Z_measured)
           = 1280 × (1.355 / 1.665)
           = 1041.7
```
**Результат: ошибка +1.9% (было +23.7%).**
### Интеграция в веб

- **`config.yaml`:** `cv_aruco: 10312`.
- **`app.py`:** `cmd_actions` для `T=10312`.
- **`cv_ctrl.py`:** воркер `_aruco_worker`, `set_cv_mode`, `cv_mode_list`.
- **`templates/index.html`:** кнопка **ARUCO** в блоке **NPU Depth/Seg**.
- **`templates/control.js`:** `sendCmdAruco()`, `DSButtons[2]`, подсветка активной кнопки.

**Переключение:** ARUCO → воркер стартует; None → воркер останавливается. **Мгновенно.**

  
## 🚧 Что осталось

### Ближайшее

- [x] **Пересборка Armbian с PR #10835** — GPU, VPU, H.264, CSI.
- [x] **Отключение CQE** — eMMC стабильна.
- [x] **Расширение eMMC** до 29 ГБ.
- [x] **`TimeoutStopSec=5`**, **`KillMode=control-group`**, **`SendSIGKILL=yes`**.
- [x] **Обработчик `SIGTERM`** в `app.py`.
- [x] **Запись видео с параллельным потоком** — GStreamer `tee` + `avenc_mjpeg` + `.mkv`.
- [x] **YOLO26s** — более точная модель, INT8 (PCQ), 29 FPS.
- [x] **YOLO11_pose** — 17 keypoints, скелет, 28 FPS.
- [x] **YOLO26_depth** — карта глубины (n-PCQ, 65 мс, 15 FPS).
- [x] **YOLO11_seg** — сегментация (uint8, 45 мс, 22 FPS).
- [x] **4 NPU-сервиса** + 4 Python-клиента (детекция, поза, глубина, сегментация).
- [x] **Кнопки DEPTH/SEG** в веб-интерфейсе + отдельный блок NPU Depth/Seg.
- [ ] **eMMC 200 МГц** — вернуть скорость (с бэкапом).
- [ ] **Передача raw RGB в NPU-сервер** (вместо JPEG) — уменьшит отставание.
- [ ] **Мульти-клиент для NPU-сервера** (сейчас 1 клиент за раз).
- [ ] **CSI-камера** — вторая камера (обзорная, на PT).
- [ ] **GPU** — ускорение OpenCV (OpenCL).
- [ ] **Переключатель камер** — USB / CSI в веб-интерфейсе.
- [ ] **ArUco-маркеры** — логика парковки.
- [ ] **Лидар D500** — SLAM + навигация.
- [ ] **ESP32 (ИК, сонары)** — код для прошивки.

### Долгосрочное

- [x] **MediaPipe Pose → YOLO11_pose (NPU)** — выполнено.
- [ ] **MediaPipe Face** — заменить на NPU (по аналогии с pose).
- [ ] **SLAM** — Cartographer / slam_toolbox + лидар D500.
- [ ] **Автопилот** — визуальная одометрия / навигация.
- [ ] **Камера глубины** — Orbbec Gemini 335 или Intel RealSense D435i.
- [ ] **Голосовое управление** — через `pyttsx3` + распознавание.


## 🔗 GitHub-репозиторий

- **Форк:** [ZHNovell/ugv_rpi](https://github.com/ZHNovell/ugv_rpi)
- **Оригинал:** [waveshareteam/ugv_rpi](https://github.com/waveshareteam/ugv_rpi)

## 📚 Полезные ссылки

- [Orange Pi 4 Pro](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-4-Pro.html)
- [Waveshare WAVE ROVER](https://www.waveshare.com/wiki/WAVE_ROVER)
- [General Driver for Robots](https://www.waveshare.com/wiki/General_Driver_for_Robots)
- [2-Axis Pan-Tilt](https://www.waveshare.com/wiki/2-Axis_Pan-Tilt_Camera_Module)
- [ugv_rpi (GitHub)](https://github.com/waveshareteam/ugv_rpi)
- [ugv_base_general (GitHub)](https://github.com/waveshareteam/ugv_base_general)
- [Armbian PR #10712 (A733)](https://github.com/armbian/build/pull/10712)
- [Allwinner Model Zoo](https://dl.radxa.com/cubie/allwinner-model-zoo.tar.gz)
- [PR #10835 (GPU/VPU)](https://github.com/armbian/build/pull/10835)
- [Ветка ijiki16](https://github.com/ijiki16/build/tree/sun60iw2-4pro-gpu-desktop-upstream)
- [JSON-команды (Waveshare)](https://www.waveshare.com/wiki/08_Slave_Device_JSON_Instruction_Set)
- [Jetson-документация](https://www.waveshare.com/wiki/Jetson_03_Pan-Tilt_Control_and_LED_Light_Control)
- [Allwinner Model Zoo v1.1.0](https://www.aw-ol.com/) — официальный SDK.
- [Radxa YOLO26 Docs](https://docs.radxa.com/en/cubie/a7a/app-dev/npu-dev/model-zoo/yolo26) — примеры конвертации.
- [Ultralytics YOLO26](https://docs.ultralytics.com/models/yolo26/) — документация.
- [COCO Keypoints](https://cocodataset.org/#keypoints-2020) — 17 точек скелета.



## 👥 Авторы

- **ZHNovell** — адаптация под Orange Pi 4 Pro, NPU, веб-интерфейс.
- **Waveshare** — оригинальный `ugv_rpi`.
- **deece** — поддержка A733 в Armbian.
- **ijiki16** - GPU модули на A733.
- **Allwinner** — Model Zoo v1.1.0, ACUITY Toolkit.
- 
## 📅 История изменений

- **2026-09-28:** Первый запуск Orange Pi 4 Pro, UART7, I2C2, NPU, YOLOv5s, кнопки веб-интерфейса.
- **2026-09-29:** USB-камера, запись видео, NPU на видео, OSD, локальный `cmd_action`.
- **2026-10-01:** Ручное копирование на eMMC (обход бага `armbian-install`), отключение CQE (`max-frequency` 52 МГц), расширение eMMC до 29 ГБ, `TimeoutStopSec=5`, обработчик `SIGTERM` в `app.py`.
- **2026-10-02:** GStreamer `tee` — запись без прерывания живого потока. Контейнер `.mkv` (matroskamux) — VFR сохранён.
- **2026-10-03:** Переключение режима камеры (1080p30 ↔ 720p60) по кнопке WebRTC. RES-индикатор в OSD. Динамический FPS-лимит в `generate_frames`. Проверено с NPU и записью.
- **2026-10-05:** YOLO11s сконвертирована в INT8 (`.nb` 6.6 МБ), C++ демо собрано нативно на Orange Pi, тест пройден (27 FPS, `dog: 92%`). Пайплайн: Allwinner Model Zoo v1.1.0 + ACUITY Toolkit 6.30.22 + Docker `ubuntu-npu:v2.0.10.2`. Папка `yolo11/` добавлена в репозиторий.
- **2026-10-06:** YOLO26s INT8 (PCQ) сконвертирована, `npu_server` пересобран под YOLO26. FPS 29 (1080p30) / 60 (720p60). CPU ~36-38%. Удалённые объекты — лучше. Отставание <0.3 сек через NPU-сервер (UNIX-сокет). Папка `yolo26/` в репозитории. Commit `b83d513`.
- **2026-10-06:** YOLO11_pose на NPU (17 keypoints, скелет). Второй NPU-сервер (`npu-pose-server.service`). Python-клиент. Асинхронный pose-воркер. Отрисовка скелета (17 keypoints). FPS ~28. Переключение OBJECTS ↔ MP POSE мгновенное. Commit `f8d8da8`.
- **2026-10-07:** CQE-баг побеждён. Патч `cqhci_halt` (polling + очистка `CQHCI_CTL` + `ret = true`). eMMC HS400 @ **150 МГц**, скорость чтения **205 МБ/с** (было 80 на 50 МГц). Патчи в `patches/kernel/` (0001-cqhci, 0002-sun60iw2p1). Commit `064afa2`.
- **2026-10-08:** Универсальный **auto-detect FPS** для камеры (`GstStream.detect_max_fps` — парсит `v4l2-ctl --list-formats-ext`, выбирает максимальный FPS для каждого разрешения). Кнопка WebRTC → переключение 1080p/720p с автоопределением. OSD показывает реальный режим (`720p30`, `720p60`). Commit `7eb80c0`.
- **2026-10-08:** YOLO26_depth сконвертирован (n-PCQ 65 мс, ~15 FPS; n-int16 158 мс; s-int16 244 мс). `npu_depth_server` + `npu_depth_client.py`. Формат JSON: сетка 16×16 + near-RLE. Папка `yolo26_depth/` в репозитории.
- **2026-10-08:** YOLO11_seg сконвертирован (uint8, 45 мс, ~22 FPS). `npu_seg_server` + `npu_seg_client.py`. RLE-сжатие масок. Папка `yolo11_seg/` в репозитории. Commit `99edae6`.
- **2026-10-09:** Интеграция depth + seg в веб-интерфейс.
  - `cv_ctrl.py`: воркеры `_npu_seg_worker`, `_npu_depth_worker` + отрисовка (heatmap, маски).
  - `app.py`: `cmd_actions` для `T=10310` (DEPTH), `T=10311` (SEG).
  - `templates/index.html`: новый блок **NPU Depth/Seg** (2 кнопки: DEPTH, SEG).
  - `templates/control.js`: `sendCmdDepth()`, `sendCmdSeg()`, отдельная обработка `DSButtons`.
  - `config.yaml`: `cv_depth: 10310`, `cv_seg: 10311`.
  - `npu-seg-server.service` + `npu-depth-server.service` (systemd, автозапуск).
  - Итог: **4 NPU-сервиса** работают параллельно (детекция, поза, глубина, сегментация).
- **2026-10-09:** NVMe SSD + SPI Flash. U-Boot с патчем `dc1sw1` прошит в SPI. Система перенесена на NVMe (570 МБ/с, Gen3). eMMC вынута (бэкап). Загрузка: SPI → NVMe.
- **2026-10-09:** ArUco-маркеры. Детекция через OpenCV (CPU), точность ±2 см на 1.35 м. Кнопка ARUCO в веб. Клиент `aruco_client.py` с калибровкой (`focal=1041.7`). Commit `ce36e56`.


---

**Последнее обновление:** 2026-10-09

**PROJECT:** [PROJECT.md](https://github.com/ZHNovell/ugv_rpi/blob/main/PROJECT.md) 
**ROADMAP:** [PROJECT.md](https://github.com/ZHNovell/ugv_rpi/blob/main/ROADMAP.md) 

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.
