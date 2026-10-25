# Orange Pi 4 Pro UGV Robot

![GitHub top language](https://img.shields.io/github/languages/top/ZHNovell/ugv_rpi)
![GitHub language count](https://img.shields.io/github/languages/count/ZHNovell/ugv_rpi)
![GitHub code size in bytes](https://img.shields.io/github/languages/code-size/ZHNovell/ugv_rpi)
![GitHub repo size](https://img.shields.io/github/repo-size/ZHNovell/ugv_rpi)
![GitHub](https://img.shields.io/github/license/ZHNovell/ugv_rpi)
![GitHub last commit](https://img.shields.io/github/last-commit/ZHNovell/ugv_rpi)

<img width="1088" height="721" alt="UGV Robot" src="https://github.com/user-attachments/assets/368fbf34-2087-4e6b-b819-b5fca5f83acd" />

**UGV-робот** на **Orange Pi 4 Pro** (Allwinner A733, NPU 3 TOPS) с **4 NPU-моделями**: детекция, позы, глубина, сегментация. Работает **автономно** (SPI + NVMe), управляется через **веб-интерфейс**.

## 📋 Содержание

1. [Аппаратная платформа](#-аппаратная-платформа)
2. [Архитектура](#-архитектура)
3. [Сборка Armbian](#-сборка-armbian)
4. [Установка ОС](#-установка-ос)
5. [Носители: eMMC и NVMe](#-носители-emmc-и-nvme)
6. [CQE-баг eMMC](#-cqe-баг-emmc)
7. [Настройка интерфейсов](#-настройка-интерфейсов)
8. [Зависимости](#-зависимости)
9. [NPU-серверы](#-npu-серверы)
10. [Адаптация кода](#-адаптация-кода)
11. [Веб-интерфейс](#-веб-интерфейс)
12. [Автозапуск](#-автозапуск)
13. [JSON-команды](#-json-команды)
14. [USB-камера](#-usb-камера)
15. [Запись видео](#-запись-видео)
16. [Стресс-тесты](#-стресс-тесты)
17. [История изменений](#-история-изменений)
18. [Ссылки](#-ссылки)
## 🔧 Аппаратная платформа

- **Orange Pi 4 Pro** — Allwinner A733, 8 ядер (4×A76 + 4×A55), 12 ГБ RAM, NPU 3 TOPS INT8.
- **General Driver for Robots** — Waveshare, ESP32-WROOM-32UE, 2×MX1919, INA219, OLED SSD1306 (0x3C).
- **2-Axis Pan-Tilt Camera Module** — сервоприводы ST3215, UART.
- **Шасси WAVE ROVER** — 4 мотора, 4 энкодера, 3S Li-Ion UPS.
- **USB-камера** — навигационная (UVC).
- **CSI-камера** — опционально (обзорная).

**Распиновка (40-pin):**

| Пин Orange Pi | Функция | Пин на General Driver |
|---|---|---|
| 8 | UART7 TX | RX (ESP32) |
| 10 | UART7 RX | TX (ESP32) |
| 1 | 3.3V | 3.3V |
| 2, 4 | 5V | 5V |
| 6, 9, 14... | GND | GND |

**Важно:** OLED, INA219, сервы управляются **ESP32** через **внутренние шины**. Orange Pi их **не трогает**.

## 📋 Архитектура

```
┌─────────────────────────────────────────────────────────┐
│                     Orange Pi 4 Pro                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │  app.py     │  │ cv_ctrl.py  │  │   os_info.py    │  │
│  │  (Flask)    │──│ (CV-контр.) │──│  (мониторинг)   │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────┘  │
│         │                │                              │
│         │       ┌────────┴────────┐                     │
│         │       │  4 NPU-клиента  │                     │
│         │       │  (UNIX-сокеты)  │                     │
│         │       └────────┬────────┘                     │
│         │                │                              │
│  ┌──────┴──────┐  ┌──────┴──────┐  ┌─────────────────┐  │
│  │ gst_stream  │  │ npu_server  │  │ npu_pose_server │  │
│  │ (GStreamer) │  │ (YOLO26s)   │  │ (YOLO11_pose)   │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │ npu_seg_    │  │ npu_depth_  │  │ aruco_client    │  │
│  │ server      │  │ server      │  │ (OpenCV CPU)    │  │
│  │ (YOLO11_seg)│  │ (YOLO26n_   │  │                 │  │
│  │             │  │  depth)     │  │                 │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
└─────────────────────────────────────────────────────────┘
                           │ UART7 (JSON)
                 ┌─────────┴─────────┐
                 │      ESP32        │
                 │  (General Driver) │
                 │  моторы/сервы/IMU │
                 └───────────────────┘
```

**Ключевые решения:**
- **4 NPU-сервера** через UNIX-сокеты (RAW BGR).
- **ArUco** — через OpenCV (CPU), не NPU.
- Видео через GStreamer `tee` — превью и запись одновременно.
- CV-режимы переключаются JSON-командами (`{"T":10304}` = OBJECTS, `{"T":10309}` = MP POSE, `{"T":10310}` = DEPTH, `{"T":10311}` = SEG, `{"T":10312}` = ARUCO).
## 🛠️ Сборка Armbian

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

**Результат:** `Armbian-unofficial_26.11.0-trunk_Orangepi4pro_trixie_vendor_6.6.98_minimal.img`

**Что включено:**
- Ядро 6.6.98-vendor-sun60iw2.
- DTB: sun60i-a733-orangepi-4-pro.dtb.
- GPU PowerVR BXM-4-64 (EGL/GLES/GBM/Vulkan/OpenCL).
- NPU VIPLite (`/dev/vipcore`, `libVIPhal.so`, `libNBGlinker.so`).
- VPU CedarC (`/dev/cedar_dev`, `/dev/cedar_dev_ve2`).
- ISP (libAWIspApi) — для CSI-камер.
- GStreamer OMX (`gstreamer1.0-omx-allwinner`).

## 💾 Установка ОС

**Запись на SD-карту:**

```bash
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
## 💾 Носители: eMMC и NVMe

**Выбор носителя:**
- **eMMC** — встроенный модуль (32 ГБ). Скорость ~205 МБ/с. Проще в установке.
- **NVMe SSD** — M.2 M-Key (128 ГБ). Скорость ~570 МБ/с. Требует SPI Flash + патч U-Boot.

**Общий порядок загрузки:**
```
BootROM: SD -> eMMC -> SPI -> USB
U-Boot (boot_targets): SD -> eMMC -> NVMe -> USB
```

### Вариант A: Установка на eMMC (ручное копирование)

**Важно:** `armbian-install` багованный на Orange Pi 4 Pro — неправильно копирует `boot_package`. Использовать **ручное** копирование.

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

**После первой загрузки — расширить раздел до 29 ГБ:**

```bash
sudo parted /dev/mmcblk0 resizepart 1 100%
sudo resize2fs /dev/mmcblk0p1
df -h /
```
### Вариант B: Установка на NVMe (SPI + патч U-Boot)

**Требуется:** SPI Flash + U-Boot с патчем `dc1sw1` (см. ниже).

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

**Результат:**

```
$ findmnt -no SOURCE,UUID /
/dev/nvme0n1p1 e4384603-9cf2-4342-ba68-bb64e3e921e4

$ cat /sys/bus/pci/devices/0000:01:00.0/current_link_speed
8.0 GT/s PCIe

$ df -h /
/dev/nvme0n1p1 117G 11G 101G 10% /
```

**Скорость:** 570 МБ/с (NVMe Gen3 x1) vs 205 МБ/с (eMMC).

**Причина, почему U-Boot не видел NVMe:** U-Boot включал не тот ключ 3.3 В (`dc1sw2` вместо `dc1sw1`). Патч: `pcie3v3_supply = "dc1sw1"` в `board-uboot.dts`.
## ⚠️ CQE-баг eMMC

**Проблема:** драйвер `sunxi-mmc` на A733 имеет баг с CQE (Command Queue Engine). При высокой частоте (200 МГц) CQE не может остановиться (`Failed to halt`), драйвер отключает CQE и сбрасывает частоту до 50 МГц.

**Решение:** использовать **HS400 @ 150 МГц**.

```bash
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb
NODE=/soc@3000000/sdmmc@4022000

# Проверить
sudo dtc -I dtb -O dts $DTB 2>/dev/null | grep -A 30 "mmc@4022000" | \
    grep -E "compatible|max-frequency|sunxi-dly"

# Должно быть:
#   compatible = "allwinner,sunxi-mmc-v5p3x";
#   max-frequency = <0x8f0d180>;   (150 МГц)
#   sunxi-dly-208M = <0xff 0x01 0xff 0xff 0xff 0xff>;

# Если 200 МГц — снизить до 150
sudo fdtput -t i $DTB $NODE max-frequency 150000000
sudo reboot
```

**Результат:** 150 МГц (HS400), скорость ~205 МБ/с, CQE-ошибок нет.

## ⚙️ Настройка интерфейсов

### UART7 (пины 8/10)

```bash
sudo armbian-config
# System -> Kernel -> Manage device tree overlays
# Включить: uart7
# Сохранить, выйти, перезагрузиться

ls -la /dev/ttyS7
# crw-rw---- 1 root dialout 241, 7 ... /dev/ttyS7
```

**Loopback-тест (замкнуть пины 8 и 10):**

```bash
picocom -b 115200 /dev/ttyS7
# Печатать символы -> должны эхо-возвращаться
# Выход: Ctrl+A, Ctrl+Q
```

### I2C2 (пины 19/23)

```bash
sudo armbian-config
# System -> Kernel -> Manage device tree overlays
# Включить: i2c2
# Сохранить, выйти, перезагрузиться

ls -la /dev/i2c-2
# crw------- 1 root root 89, 2 ... /dev/i2c-2

apt install -y i2c-tools
i2cdetect -y 2
```

**armbianEnv.txt (после активации):**

```
overlays=i2c2 uart7
```
## 📦 Зависимости

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

**requirements.txt (Python 3.13):**

```
Flask==3.0.3
Flask-SocketIO==5.3.6
Werkzeug==3.0.3
Jinja2==3.1.4
itsdangerous==2.2.0
blinker==1.8.2
python-socketio==5.11.2
python-engineio==4.9.1
aiortc==1.15.0
av==17.1.0
simplejpeg==1.9.0
aioice==0.10.2
pyee==14.0.0
pylibsrtp==1.0.0
cryptography==50.0.1
numpy==1.26.4
PyYAML==6.0.1
requests==2.32.3
psutil==5.9.8
pyserial==3.5
opencv-python-headless==4.10.0.84
Pillow==11.3.0
imutils==0.5.4
```

```bash
pip install -r requirements.txt
pip install imageio pygame-ce pyttsx3 netifaces
```

**Ключевые моменты:**
- **Pillow 11.3.0** (не 10.3.0 — не работает с Python 3.13).
- **pygame-ce** (не pygame — нет wheel для Python 3.13 + ARM64).
- **av 17.1.0** (не 12.3.0).
- **aiortc 1.15.0** (не 1.8.0).

## 🖐️ MediaPipe (HAND GS)

Wheel для Orange Pi 4 Pro (aarch64, Python 3.13):
https://github.com/ZHNovell/ugv_rpi/releases/download/mediapipe-0.10.14-aarch64/mediapipe-0.10.14.dev0-cp313-cp313-linux_aarch64.whl

Установка:
```
pip3 install --break-system-packages --ignore-installed --force-reinstall https://github.com/ZHNovell/ugv_rpi/releases/download/mediapipe-0.10.14-aarch64/mediapipe-0.10.14.dev0-cp313-cp313-linux_aarch64.whl
```

Проверка:
```
python3 -c "import mediapipe as mp; print(mp.__version__)"
```

Модель hand landmarker:
```
wget -O /tmp/hand_landmarker.task https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task
```
### 🎯 Motion (Object Detection через YOLO26s NPU)

Режим **Motion** (Simple Detection Type) теперь использует **YOLO26s на NPU** (`/tmp/npu11.sock`), а не `absdiff` или `efficientdet`. Это даёт:

- **Точность:** находит **людей** (класс `person`), а не абстрактное движение.
- **Скорость:** ~53 мс на кадр (NPU), боксы обновляются быстро.
- **Устойчивость:** не «сходит с ума», когда камера двигается (в отличие от `absdiff`).
- **Dead Zone:** если объект в центре (60 пикселей), серва не дёргается.
- **Подсветка:** при обнаружении включает прожектор (IO5) в режиме AUTO.

**Код:** `cv_detect_movition` в `cv_ctrl.py` (вызов `self.npu_client.detect(img)`).

### 📷 WebRTC Resolution Switcher

Кнопка **WebRTC** заменена на выпадающий список **1080P / 720P**. Переключение — мгновенное (через `set_camera_mode()` и `GstStream.switch_mode()`).

**Код:** `set_camera_mode()` в `cv_ctrl.py`, команды `cam_1080` (10108) и `cam_720` (10109) в `config.yaml`.

## 🧠 NPU-серверы

**4 NPU-сервера** через UNIX-сокеты. Все работают параллельно.

| Сервис | Модель | Сокет | Время | Квантизация |
|---|---|---|---|---|
| `npu-server.service` | YOLO26s (детекция) | `/tmp/npu11_raw.sock` | ~53 мс | PCQ |
| `npu-pose-server.service` | YOLO11_pose (позы) | `/tmp/npu_pose_raw.sock` | ~42 мс | uint8 |
| `npu-seg-server.service` | YOLO11_seg (маски) | `/tmp/npu_seg_raw.sock` | ~70 мс | uint8 |
| `npu-depth-server.service` | YOLO26n_depth (глубина) | `/tmp/npu_depth_raw.sock` | ~77 мс | PCQ |

**Python-клиенты (в корне):**
- `npu_client.py` — YOLO26s
- `npu_pose_client.py` — YOLO11_pose
- `npu_seg_client.py` — YOLO11_seg
- `npu_depth_client.py` — YOLO26n_depth

**Протокол (RAW BGR):**
```
Клиент -> Сервер: [4 байта BE: width] [4 байта BE: height] [4 байта BE: channels] [raw BGR]
Сервер -> Клиент: [4 байта BE: длина JSON] [JSON bytes]
```

### YOLO26s (детекция объектов)

**Пайплайн конвертации:**

1. Скачать `yolo26s.pt` (Ultralytics).
2. Экспорт ONNX: `yolo export model=yolo26s.pt format=onnx simplify=True dynamic=False opset=16`.
3. Вырезать 6 выходов через `onnx_extract.py`:
   - `/model.23/Reshape_output_0` (`[1, 4, 6400]`)
   - `/model.23/Reshape_1_output_0` (`[1, 4, 1600]`)
   - `/model.23/Reshape_2_output_0` (`[1, 4, 400]`)
   - `/model.23/Reshape_3_output_0` (`[1, 80, 6400]`)
   - `/model.23/Reshape_4_output_0` (`[1, 80, 1600]`)
   - `/model.23/Reshape_5_output_0` (`[1, 80, 400]`)
4. Docker ACUITY (`ubuntu-npu:v2.0.10.2`):
   - `./pegasus_import.sh yolo26s_6`
   - `./pegasus_quantize.sh yolo26s_6 pcq 12`
   - `./pegasus_export_ovx_nbg.sh yolo26s_6 pcq a733`
5. Результат: `yolo26s_6_pcq_a733.nb` (9 МБ).

**Нативная сборка:**

```bash
cd /root/ugv_rpi/yolo26_raw/
./build_native.sh
```

**Ключевые фиксы `CMakeLists.txt.patched`:**
1. `SYS_ARCH=linux_aarch64`.
2. OpenCV через `pkg-config` (не `find_package`).
3. Убрать `_GLIBCXX_USE_CXX11_ABI=0`.
4. `MODEL_ZOO_HOME_DIR` — абсолютный путь.
5. Исключить лишние `.cpp` (main, yolo11_6_*).
### YOLO11_pose (позы)

**Пайплайн:**
1. `yolo11s-pose.pt` -> ONNX (opset 15).
2. `onnx_extract.py` — вырезать 9 выходов (cv2/cv3/cv4 × 3 уровня).
3. Docker ACUITY:
   - `./pegasus_import.sh yolo11s-pose_9`
   - `./pegasus_quantize.sh yolo11s-pose_9 uint8 12`
   - `./pegasus_export_ovx_nbg.sh yolo11s-pose_9 uint8 a733`
4. Результат: `yolo11s-pose_9_uint8_a733.nb` (7 МБ).

**Модель:**
- 9 выходов: bbox (cv2) + class (cv3) + keypoints (cv4) × 3 уровня.
- 1 класс — person.
- 17 keypoints (COCO): нос, глаза, уши, плечи, локти, запястья, бёдра, колени, лодыжки.

**Нативная сборка:**

```bash
cd /root/ugv_rpi/yolo11_pose_raw/
./build_native.sh
```

### YOLO11_seg (сегментация)

**Пайплайн:**
1. Скачать `yolo11s-seg_10.onnx` (Allwinner, 40 МБ) — уже обрезан.
2. Docker ACUITY:
   - `./pegasus_import.sh yolo11s-seg_10`
   - `./pegasus_quantize.sh yolo11s-seg_10 uint8 12`
   - `./pegasus_export_ovx_nbg.sh yolo11s-seg_10 uint8 a733`
3. Результат: `yolo11s-seg_10_uint8_a733.nb` (7.3 МБ).

**Модель:**
- 10 выходов: bbox + class + mask coeffs + protos × 3 уровня.
- 80 классов (COCO).
- Instance segmentation (пиксельные маски).

**Нативная сборка:**

```bash
cd /root/ugv_rpi/yolo11_seg_raw/
./build_native.sh
```

### YOLO26n_depth (карта глубины)

**Пайплайн:**
1. Скачать `yolo26n-depth.pt` (Ultralytics, 12.4 МБ).
2. Экспорт ONNX: `imgsz=768, opset=14, simplify=True`.
3. Docker ACUITY:
   - `./pegasus_import.sh yolo26n-depth`
   - `./pegasus_quantize.sh yolo26n-depth pcq 10`
   - `./pegasus_export_ovx_nbg.sh yolo26n-depth pcq a733`
4. Результат: `yolo26n-depth_pcq_a733.nb` (10.4 МБ).

**Модель:**
- Вход: 768×768 RGB (mean=0, scale=1/255).
- Выход: 768×768 float32 (метры).

**Важно:** nano + PCQ — лучший баланс (65 мс, ~11 FPS). s + PCQ шумит из-за `Exp()` на выходе.

**Нативная сборка:**

```bash
cd /root/ugv_rpi/yolo26_depth_raw/
./build_native.sh
```
### Процесс перевода сервера на RAW BGR

Для каждого из 4 серверов:

**1. Копируем папку сервера в `_raw`:**

```bash
cp -r yolo26 yolo26_raw
cd yolo26_raw
```

**2. Правим `npu_server.cpp` — меняем приём JPEG на raw BGR.**

Было (JPEG):
```cpp
uint32_t jpeg_len_be;
if (!read_n(cli, &jpeg_len_be, 4)) break;
uint32_t jpeg_len = ntohl(jpeg_len_be);
vector<unsigned char> jpeg_buf(jpeg_len);
if (!read_n(cli, jpeg_buf.data(), jpeg_len)) break;
cv::Mat frame = cv::imdecode(jpeg_buf, cv::IMREAD_COLOR);
```

Стало (RAW):
```cpp
uint32_t width_be, height_be, channels_be;
if (!read_n(cli, &width_be, 4)) break;
if (!read_n(cli, &height_be, 4)) break;
if (!read_n(cli, &channels_be, 4)) break;

uint32_t width = ntohl(width_be);
uint32_t height = ntohl(height_be);
uint32_t channels = ntohl(channels_be);

if (width == 0 || height == 0 || channels != 3) break;
if (width > 4096 || height > 4096) break;

uint32_t raw_size = width * height * channels;
if (raw_size > 20 * 1024 * 1024) break;

vector<unsigned char> raw_buf(raw_size);
if (!read_n(cli, raw_buf.data(), raw_size)) break;

cv::Mat frame = cv::Mat(height, width, CV_8UC3, raw_buf.data()).clone();
```

**Важно:** `.clone()` — обязательно, иначе данные `raw_buf` уничтожаются после выхода из блока.

**3. Меняем SOCKET_PATH на `_raw`:**

```cpp
#define SOCKET_PATH "/tmp/npu11_raw.sock"
```

**4. Правим `build_native.sh`:**

```bash
SRC_DIR=${REPO_DIR}
mkdir -p "${SRC_DIR}"
rm -f "${SRC_DIR}/yolov26_6_post.cpp" "${SRC_DIR}/yolov26_6_pre.cpp"
```

**5. Собираем:**

```bash
./build_native.sh
cp -f build/npu_server ./npu_server
```

**6. Правим `npu_client.py` — отправка raw BGR.**

Было (JPEG):
```python
ok, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
jpeg_bytes = jpeg.tobytes()
self.sock.sendall(struct.pack('>I', len(jpeg_bytes)))
self.sock.sendall(jpeg_bytes)
```

Стало (RAW):
```python
h, w = frame.shape[:2]
self.sock.sendall(struct.pack('>I', w))
self.sock.sendall(struct.pack('>I', h))
self.sock.sendall(struct.pack('>I', 3))
self.sock.sendall(frame.tobytes())
```

**7. Меняем SOCKET_PATH в клиенте:**

```python
SOCKET_PATH = "/tmp/npu11_raw.sock"
```

**8. Добавляем таймаут в `_connect` (10 сек):**

```python
attempts = 0
while attempts < 20:
    try:
        ...
        return
    except (FileNotFoundError, ConnectionRefusedError) as e:
        attempts += 1
        time.sleep(0.5)
raise ConnectionError(f"[NPUClient] Server {self.socket_path} not available after 10s")
```

**9. Обновляем systemd-сервис:**

```bash
sudo sed -i 's|/root/ugv_rpi/yolo26/|/root/ugv_rpi/yolo26_raw/|g' /etc/systemd/system/npu-server.service
sudo systemctl daemon-reload
sudo systemctl restart npu-server.service
```

**10. Проверяем:**

```bash
ls -la /tmp/npu11_raw.sock
ps aux | grep npu_server | grep -v grep
```
## 🔧 Адаптация кода

### `base_ctrl.py`

**Изменение:** UART-порт.

```python
# Было (Raspberry Pi):
base = BaseController('/dev/ttyAMA0', 115200)

# Стало (Orange Pi 4 Pro):
base = BaseController('/dev/ttyS7', 115200)
```

```bash
sed -i "s|/dev/ttyAMA0|/dev/ttyS7|g" base_ctrl.py
```

### `app.py`

**Изменение 1:** UART-порт + удаление блока проверки Raspberry Pi.

```python
# Было:
def is_raspberry_pi5():
    ...

if is_raspberry_pi5():
    base = BaseController('/dev/ttyAMA0', 115200)
else:
    base = BaseController('/dev/serial0', 115200)

# Стало:
base = BaseController('/dev/ttyS7', 115200)
```

**Изменение 2:** `eth0_ip` -> `end0_ip`.

```bash
sed -i 's/si\.eth0_ip/si.end0_ip/g' app.py
```

**Изменение 3:** Добавлен обработчик `SIGTERM`.

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

**Изменение 2:** Блоки `mediapipe` обёрнуты в `if MEDIAPIPE_AVAILABLE:`.

**Изменение 3:** Блоки CSI/OAK — проверки `CSI_CAMERA_AVAILABLE`, `OAK_CAMERA_AVAILABLE`.

**Изменение 4:** Инициализация NPU (вместо `cv2.dnn`).

```python
from npu_client import NPUClient
from npu_pose_client import NPUPoseClient
from npu_seg_client import NPUSegClient
from npu_depth_client import NPUDepthClient

def _try_client(cls, name):
    try:
        client = cls()
        print(f"[cv_ctrl] {name} connected", flush=True)
        return client
    except Exception as e:
        print(f"[cv_ctrl] {name} not available: {e}", flush=True)
        return None

self.npu_client = _try_client(NPUClient, 'NPUClient')
self.npu_pose_client = _try_client(NPUPoseClient, 'NPUPoseClient')
self.npu_seg_client = _try_client(NPUSegClient, 'NPUSegClient')
self.npu_depth_client = _try_client(NPUDepthClient, 'NPUDepthClient')
```

### `os_info.py`

**Изменение 1:** `vcgencmd` -> `sysfs`.

```python
with open('/sys/class/thermal/thermal_zone0/temp', 'r') as f:
    temperature_str = f.read().strip()
temperature = float(temperature_str) / 1000.0
return round(temperature, 1)
```

**Изменение 2:** `iwconfig` -> `iw`.

```python
result = subprocess.check_output(['/usr/sbin/iw', 'dev', 'wlan0', 'info'],
                                 encoding='utf-8', stderr=subprocess.DEVNULL)

output = subprocess.check_output(["/usr/sbin/iw", "dev", interface, "link"],
                                 encoding="utf-8", stderr=subprocess.DEVNULL)

signal_strength = re.search(r"signal:\s*(-\d+)", output)
if signal_strength:
    return int(signal_strength.group(1))
return 0
```

**Изменение 3:** `eth0` -> `end0`.

```bash
sed -i 's/'"'"'eth0'"'"'/'"'"'end0'"'"'/g' os_info.py
sed -i 's/self\.eth0_ip/self.end0_ip/g' os_info.py
```
## 🌐 Веб-интерфейс

### `templates/index.html`

**Кнопки `OBJECTS`, `COLOR`, `HAND GS`:**

```html
<div><button onclick="sendCmdObjs();" class="ctl_btn">OBJECTS</button></div>
<div><button onclick="sendCmdClor();" class="ctl_btn">COLOR</button></div>
<div><button onclick="sendCmdHand();" class="ctl_btn">HAND GS</button></div>
```

**Блок NPU Depth/Seg:**

```html
<div>
    <h2 class="sc_title feed_ct_tt">NPU Depth/Seg</h2>
    <div id="npu_depth_seg_btn">
        <div><button onclick="sendCmdDepth();" class="ctl_btn">DEPTH</button></div>
        <div><button onclick="sendCmdSeg();" class="ctl_btn">SEG</button></div>
        <div><button onclick="sendCmdAruco();" class="ctl_btn">ARUCO</button></div>
    </div>
</div>
```

**Версионирование `control.js`:**

```html
<script src="./control.js?v=8"></script>
```

### `templates/control.js`

**Функции:**

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
function sendCmdDepth() {
    sendCommand('base -c {"T":' + cv_depth + '}');
}
function sendCmdSeg() {
    sendCommand('base -c {"T":' + cv_seg + '}');
}
function sendCmdAruco() {
    sendCommand('base -c {"T":' + cv_aruco + '}');
}

function sendCommand(command) {
    fetch('/send_command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'command=' + encodeURIComponent(command)
    })
    .then(response => response.json())
    .then(data => console.log('Command sent:', command, data))
    .catch(error => console.error('Error:', error));
}
```

**Обработчики кнопок (в `socket.on('update')`):**

```javascript
FButtons[0].onclick = function() { sendCommand('base -c {"T":' + cv_objs + '}'); };
FButtons[1].onclick = function() { sendCommand('base -c {"T":' + cv_clor + '}'); };
FButtons[2].onclick = function() { sendCommand('base -c {"T":' + mp_hand + '}'); };

var npuDSBtn = document.getElementById("npu_depth_seg_btn");
if (npuDSBtn) {
    var DSButtons = npuDSBtn.getElementsByTagName("button");
    removeButtonsClass(DSButtons);
    DSButtons[0].onclick = function() { sendCommand('base -c {"T":' + cv_depth + '}'); };
    DSButtons[1].onclick = function() { sendCommand('base -c {"T":' + cv_seg + '}'); };
    if (DSButtons[2]) DSButtons[2].onclick = function() { sendCommand('base -c {"T":' + cv_aruco + '}'); };
}
```

### `templates/style.css`

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
## 🛑 Emergency Stop (Аварийная остановка)

В веб-интерфейсе добавлена красная кнопка **STOP**.
Она отправляет команду `{"T":0}` напрямую на ESP32 (через `/api/stop`), минуя джойстик и heartbeat.

**Использование:**
1. Нажмите **STOP** в веб-интерфейсе.
2. Робот мгновенно остановится (даже если вы продолжаете удерживать клавишу движения).

**Примечание:** Для работы кнопки ESP32 должен быть прошит с поддержкой команды `{"T":0}` (см. репозиторий `ugv_base_general`).

## 🎭 MP FACE (Кивок / Поворот)

Новый режим **MP FACE** (MediaPipe Face Landmarker):
*   **Кивок (вверх-вниз):** Робот моргает IO5 дважды (подтверждение) и **кивает в ответ** (PT вниз-вверх).
*   **Поворот (влево-вправо):** Робот моргает IO5 дважды и **зеркально поворачивается** (PT).

**Реализация:** `mp_detect_face` в `cv_ctrl.py`, модель `face_landmarker.task`.

## 🚀 Автозапуск

### `npu-server.service`

```ini
[Unit]
Description=NPU Server for YOLO26s (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo26_raw
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/root/ugv_rpi/prepare_shm.sh
ExecStart=/root/ugv_rpi/yolo26_raw/npu_server /dev/shm/yolo26s_6_pcq_a733.nb
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `npu-pose-server.service`

```ini
[Unit]
Description=NPU Pose Server for YOLO11_pose (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo11_pose_raw
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/bin/sh -c 'cp /root/ugv_rpi/yolo11_pose_raw/yolo11s-pose_9_uint8_a733.nb /dev/shm/ 2>/dev/null || true'
ExecStart=/root/ugv_rpi/yolo11_pose_raw/npu_pose_server /dev/shm/yolo11s-pose_9_uint8_a733.nb
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `npu-seg-server.service`

```ini
[Unit]
Description=NPU Seg Server for YOLO11_seg (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo11_seg_raw
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/bin/sh -c 'cp /root/ugv_rpi/yolo11_seg_raw/model/yolo11s-seg_10_uint8_a733.nb /dev/shm/ 2>/dev/null || true'
ExecStart=/root/ugv_rpi/yolo11_seg_raw/npu_seg_server /dev/shm/yolo11s-seg_10_uint8_a733.nb
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `npu-depth-server.service`

```ini
[Unit]
Description=NPU Depth Server for YOLO26n_depth (A733)
After=network.target
Before=ugv.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi/yolo26_depth_raw
Environment=LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733
ExecStartPre=/bin/sh -c 'cp /root/ugv_rpi/yolo26_depth_raw/model/yolo26n-depth_pcq_a733.nb /dev/shm/ 2>/dev/null || true'
ExecStart=/root/ugv_rpi/yolo26_depth_raw/npu_depth_server /dev/shm/yolo26n-depth_pcq_a733.nb
Restart=always
RestartSec=3
TimeoutStopSec=5
KillMode=control-group
SendSIGKILL=yes

[Install]
WantedBy=multi-user.target
```

### `ugv.service`

```ini
[Unit]
Description=UGV Robot App
After=network.target npu-server.service npu-pose-server.service npu-seg-server.service npu-depth-server.service
Requires=npu-server.service npu-pose-server.service npu-seg-server.service npu-depth-server.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ugv_rpi
ExecStartPre=/root/ugv_rpi/prepare_shm.sh
ExecStartPre=/bin/sh -c "v4l2-ctl -d /dev/video0 -c focus_automatic_continuous=0 && v4l2-ctl -d /dev/video0 -c focus_absolute=671"
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
sudo systemctl enable npu-server.service npu-pose-server.service npu-seg-server.service npu-depth-server.service ugv.service
sudo systemctl start npu-server.service npu-pose-server.service npu-seg-server.service npu-depth-server.service ugv.service
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

### CV-команды

| Команда | T | Описание |
|---|---|---|
| None | 10301 | Отключить детекцию |
| Motion | 10302 | Детекция движения |
| Faces | 10303 | Детекция лиц |
| Objects | 10304 | NPU YOLO26s |
| Color | 10305 | Детекция цвета |
| Hand GS | 10306 | Жесты рук |
| Auto | 10307 | Авто-режим |
| MP Face | 10308 | MediaPipe Face (не работает) |
| MP Pose | 10309 | NPU YOLO11_pose |
| Depth | 10310 | NPU YOLO26n_depth |
| Seg | 10311 | NPU YOLO11_seg |
| ArUco | 10312 | ArUco-маркеры |
## 📷 USB-камера (UVC)

**Подключение:**
- Камера подключается в USB-порт Orange Pi 4 Pro.
- Определяется как `/dev/video0` и `/dev/video1`.
- Модуль `uvcvideo` загружается автоматически.

**Проверка:**

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
fswebcam -d /dev/video0 --no-banner -r 1920x1080 -S 5 ./test.jpg
```

**Универсальная детекция:**

```python
def usb_camera_detection(self):
    import glob
    video_devices = glob.glob('/dev/video*')
    if not video_devices:
        print("USB Camera not connected (no /dev/video*)")
        return False
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

### Универсальный auto-detect FPS

**Проблема:** разные камеры поддерживают разные FPS для разных разрешений.

**Решение:** при переключении режима камера сама определяет максимальный FPS для выбранного разрешения через `v4l2-ctl --list-formats-ext`.

**`GstStream.detect_max_fps(width, height, device)`:**
- Парсит вывод `v4l2-ctl --list-formats-ext`.
- Находит секцию MJPG.
- Ищет нужное разрешение.
- Возвращает максимальный FPS.
- **Ограничение:** 1080p — максимум 30 fps (USB 2.0 не тянет 60).

**`GstStream.switch_mode(width, height, fps=None)`:**
- Если `fps` не задан — вызывает `detect_max_fps`.
- Пересоздаёт пайплайн с найденным FPS.

**`cv_ctrl.toggle_camera_mode()`:**
- Определяет целевое разрешение.
- Вызывает `switch_mode(new_w, new_h)` без fps.
- Формирует `self.camera_mode` динамически: `f"{height}p{real_fps}"`.
## 🎥 Запись видео

**Требование:** во время записи живой поток не должен прерываться.

**Архитектура (GStreamer `tee`):**

```
v4l2src -> image/jpeg (MJPG, 1920x1080@30) -> jpegdec -> tee
├─ queue (leaky) -> videoconvert -> BGR -> appsink (живой поток)
└─ queue -> videoconvert -> videoscale -> 1280x720 I420
   -> avenc_mjpeg -> matroskamux -> filesink (запись)
```

**Ключевые решения:**

1. **`tee` после `jpegdec`** — камера читается один раз. Живой поток не прерывается.
2. **Контейнер `.mkv` (matroskamux), не `.avi`** — камера даёт VFR (в темноте FPS падает). AVI не хранит VFR корректно. Matroska сохраняет реальные PTS.
3. **Энкодер `avenc_mjpeg`** (libav), не `jpegenc`.
4. **Правильный teardown ветки записи:**
   - Отлинковать `rec_tee_pad` от `rec_queue`.
   - Отправить EOS через sink pad `rec_queue`.
   - `set_state(NULL)` -> `pipeline.remove()`.
5. **`v4l2src do-timestamp=true`** — PTS из системных часов.

**Параметры:**
- Разрешение записи: **1280x720**.
- Битрейт MJPEG: **8 Мбит/с**, `qmin=3`, `qmax=10`.
- Размер файла: ~10 МБ на 10 секунд.

**Ограничение:** `.mkv` не проигрывается в браузере. Для просмотра — VLC / MPC-HC / ffmpeg.
## 🎯 ArUco-маркеры

**Итог:** детекция ArUco через OpenCV (CPU). Точность — **±1 см на 1.35 м** (~0.7%). Кнопка **ARUCO** в веб-интерфейсе.

**Параметры:**

| Параметр | Значение |
|---|---|
| Словарь | `DICT_4X4_50` |
| Размер маркера | 14.6 см (чёрный квадрат) |
| Разрешённые ID | 0, 1, 2, 3 |
| Разрешение | 1280×720 |
| Калибровка | `camera_calib.npz` (RMS=0.3475) |
| Точность | ±1 см на 1.35 м (~0.7%) |

**Файлы:**
- `aruco_client.py` — клиент (детекция + поза + отрисовка).
- `aruco_detect.py` — демо-скрипт.
- `camera_calibrate_v2.py` — калибровка без GUI.
- `camera_calib.npz` — сохранённая калибровка.

**Калибровка камеры:**

```bash
python3 camera_calibrate_v2.py
```

Собрать 25 кадров с шахматной доской (5×8 внутренних углов, 30 мм). Результат: `camera_calib.npz` (RMS=0.3475).

**Автомасштабирование под разрешение:**

```python
if (self.calib_w != frame_w) or (self.calib_h != frame_h):
    scale_x = frame_w / self.calib_w
    scale_y = frame_h / self.calib_h
    self.camera_matrix[0, 0] *= scale_x  # fx
    self.camera_matrix[1, 1] *= scale_y  # fy
    self.camera_matrix[0, 2] *= scale_x  # cx
    self.camera_matrix[1, 2] *= scale_y  # cy
```
## 📊 Стресс-тесты

**Условия:** 4 NPU-модели, реальный видеопоток 720p60, 70 сек на модель + 70 сек параллельно. Мониторинг CPU/RAM/TEMP каждые 2 сек.

### RAW BGR (актуальные)

**По очереди (70 сек на модель):**

| Модель | avg | FPS | Кадров |
|---|---|---|---|
| YOLO26s | 67.3 мс | **14.9** | 703 |
| YOLO11_pose | 53.3 мс | **18.8** | 720 |
| YOLO11_seg | 90.0 мс | **11.1** | 632 |
| YOLO26n_depth | 94.4 мс | **10.6** | 580 |

**Параллельно (все 4):**

| Метрика | Значение |
|---|---|
| Общий FPS камеры | **12.5** |
| Каждая модель | **5.5 FPS** |
| CPU avg | **14.3%** |
| CPU max | **26.4%** |
| RAM | 5.3% |
| TEMP max | **36.3 °C** |

**Примечание:** FPS в таблице — это FPS инференса модели (сколько кадров обрабатывает NPU). FPS камеры на экране (в OSD) — может отличаться из-за просадки при параллельной работе.

### Сравнение JPEG vs RAW

| Модель | JPEG (avg / FPS) | RAW (avg / FPS) | Прирост |
|---|---|---|---|
| YOLO26s | 90.9 мс / 11.0 | **67.3 мс / 14.9** | +35% |
| YOLO11_pose | 83.7 мс / 12.0 | **53.3 мс / 18.8** | +57% |
| YOLO11_seg | 113.8 мс / 8.8 | **90.0 мс / 11.1** | +26% |
| YOLO26n_depth | 117.3 мс / 8.5 | **94.4 мс / 10.6** | +25% |

**CPU max:** 100% -> 26.4% (-74%).

**GIL:** в параллельном режиме RAW даёт 12.5 FPS камеры (JPEG — 15.0). Причина — GIL в Python (копирование больших буферов). Решение (на будущее): `multiprocessing` или C++ раскидыватель.

## 🚧 Что осталось

### Сделано

- [x] **Пересборка Armbian с PR #10835** — GPU, VPU, H.264, CSI.
- [x] **CQE-баг eMMC** — HS400 @ 150 МГц (205 МБ/с).
- [x] **Расширение eMMC** до 29 ГБ.
- [x] **`TimeoutStopSec=5`**, **`KillMode=control-group`**, **`SendSIGKILL=yes`**.
- [x] **Обработчик `SIGTERM`** в `app.py`.
- [x] **Запись видео с параллельным потоком** — GStreamer `tee` + `avenc_mjpeg` + `.mkv`.
- [x] **NVMe SSD + SPI Flash** — загрузка с NVMe (570 МБ/с) без eMMC.
- [x] **Универсальный auto-detect FPS** для камеры.
- [x] **YOLO26s** — детекция (PCQ, RAW, ~53 мс).
- [x] **YOLO11_pose** — 17 keypoints (uint8, RAW, ~42 мс).
- [x] **YOLO11_seg** — сегментация (uint8, RAW, ~70 мс).
- [x] **YOLO26n_depth** — карта глубины (PCQ, RAW, ~77 мс).
- [x] **4 NPU-сервиса** + 4 Python-клиента (RAW BGR).
- [x] **Кнопки DEPTH/SEG/ARUCO** в веб-интерфейсе.
- [x] **ArUco-маркеры** — детекция (OpenCV CPU, ±1 см на 1.35 м).
- [x] **Калибровка камеры** — RMS=0.3475.
- [x] **Фиксация фокуса** камеры (671).
- [x] **Стресс-тест** — 4 модели, 720p60, CPU 14.3% avg, TEMP 36.3 °C.



### Ближайшее

- [ ] **GIL** — узкое место параллельного RAW
- [ ] **Мульти-клиент для NPU-сервера** (сейчас 1 клиент за раз).
- [ ] **CSI-камера** — вторая камера (обзорная, на PT).
- [ ] **GPU** — ускорение OpenCV (OpenCL).
- [ ] **Переключатель камер** — USB / CSI в веб-интерфейсе.
- [ ] **ArUco-маркеры** — логика парковки.
- [ ] **Лидар D500** — SLAM + навигация.
- [ ] **ESP32 (ИК, сонары)** — код для прошивки.

### Долгосрочное (из ROADMAP)

- [x] **MediaPipe Pose -> YOLO11_pose (NPU)** — выполнено.
- [ ] **MediaPipe Face** — заменить на NPU (по аналогии с pose).
- [ ] **Лидар D500** — драйвер напрямую в `cv_ctrl.py` (без ROS2).
- [ ] **Sensor Fusion** — лидар + depth в Python-воркере.
- [ ] **SLAM** — Hector SLAM (лёгкий порт, без ROS2).
- [ ] **Навигация** — VFH или Bug-алгоритм (без Nav2).
- [ ] **Лидарная защита** — стоп < 0.4 м.
- [ ] **Автопилот** — визуальная одометрия.
- [ ] **Камера глубины** — Orbbec Gemini 335 (когда проект поедет).
- [ ] **Голосовое управление** — через `pyttsx3` + распознавание.

**Подробный план:** [ROADMAP.md](https://github.com/ZHNovell/ugv_rpi/blob/main/ROADMAP.md)





## 📅 История изменений

> **Полная история проекта:** [PROJECT.md](https://github.com/ZHNovell/ugv_rpi/blob/main/PROJECT.md)

- **2026-09-28:** Первый запуск Orange Pi 4 Pro, UART7, I2C2, NPU.
- **2026-10-01:** Ручное копирование на eMMC, CQE-фикс (150 МГц).
- **2026-10-02:** GStreamer `tee` — запись без прерывания.
- **2026-10-03:** Переключение 1080p30/720p60.
- **2026-10-05:** YOLO11s INT8 на NPU.
- **2026-10-06:** YOLO26s (PCQ), YOLO11_pose.
- **2026-10-07:** CQE-баг побеждён (150 МГц, 205 МБ/с).
- **2026-10-08:** Auto-detect FPS камеры, YOLO26_depth, YOLO11_seg.
- **2026-10-09:** 4 NPU-сервиса, NVMe + SPI Flash.
- **2026-10-10:** Стресс-тест, калибровка камеры, RAW BGR для 4 серверов.

## 🔗 Ссылки

**GitHub:**
- Форк: [ZHNovell/ugv_rpi](https://github.com/ZHNovell/ugv_rpi)
- Оригинал: [waveshareteam/ugv_rpi](https://github.com/waveshareteam/ugv_rpi)

**Аппаратура:**
- [Orange Pi 4 Pro](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-4-Pro.html)
- [Waveshare WAVE ROVER](https://www.waveshare.com/wiki/WAVE_ROVER)
- [General Driver for Robots](https://www.waveshare.com/wiki/General_Driver_for_Robots)
- [2-Axis Pan-Tilt](https://www.waveshare.com/wiki/2-Axis_Pan-Tilt_Camera_Module)

**Armbian:**
- [PR #10835 (GPU/VPU/NPU/ISP)](https://github.com/armbian/build/pull/10835)
- [Ветка ijiki16](https://github.com/ijiki16/build/tree/sun60iw2-4pro-gpu-desktop-upstream)

**NPU / Ultralytics:**
- [Allwinner Model Zoo v1.1.0](https://www.aw-ol.com/)
- [Radxa YOLO26 Docs](https://docs.radxa.com/en/cubie/a7a/app-dev/npu-dev/model-zoo/yolo26)
- [Ultralytics YOLO26](https://docs.ultralytics.com/models/yolo26/)
- [COCO Keypoints](https://cocodataset.org/#keypoints-2020)

**JSON-команды:**
- [Waveshare JSON Instruction Set](https://www.waveshare.com/wiki/08_Slave_Device_JSON_Instruction_Set)

## 👥 Авторы

- **ZHNovell** — адаптация под Orange Pi 4 Pro, NPU, веб-интерфейс.
- **Waveshare** — оригинальный `ugv_rpi`.
- **deece** — поддержка A733 в Armbian.
- **ijiki16** — GPU модули на A733.
- **Allwinner** — Model Zoo v1.1.0, ACUITY Toolkit.

---

**Последнее обновление:** 2026-10-10

**PROJECT:** [PROJECT.md](https://github.com/ZHNovell/ugv_rpi/blob/main/PROJECT.md)
**ROADMAP:** [ROADMAP.md](https://github.com/ZHNovell/ugv_rpi/blob/main/ROADMAP.md)

Copyright (C) 2024 [Waveshare](https://www.waveshare.com/)

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
