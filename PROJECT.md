# PROJECT.md — Orange Pi 4 Pro UGV Robot

## 📋 Общая архитектура

**Проект:** Робот на базе Waveshare UGV (WAVE ROVER / General Driver for Robots) с заменой Raspberry Pi на Orange Pi 4 Pro.

**Аппаратная платформа:**
- **Orange Pi 4 Pro** — Allwinner A733, 8 ядер (4×A76 + 4×A55), 12 ГБ RAM, NPU 3 TOPS INT8.
- **General Driver for Robots** — Waveshare, ESP32-WROOM-32UE, 2×MX1919, INA219, OLED SSD1306 (0x3C).
- **2-Axis Pan-Tilt Camera Module** — сервоприводы ST3215, UART.
- **Шасси WAVE ROVER** — 4 мотора, 4 энкодера, 3S Li-Ion UPS.
- **USB-камера** — Logitech C920 HD Pro Webcam (навигационная).
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

## ⚠️ Важно: Отключение CQE (баг драйвера `sunxi-mmc`)

**Проблема:** драйвер `sunxi-mmc` на A733 имеет **баг с CQE** (Command Queue Engine). При **высокой частоте (HS400, 200 МГц)** CQE **сбоит** при записи, что **повреждает загрузчик** на eMMC.

**Симптомы:**
- `dmesg | grep cqhci` → `cqhci: Failed to halt`, `cmd 12, RTO`.
- Система **не загружается** с eMMC после **выключения**.

**Решение:** снизить частоту eMMC до **52 МГц** (HS-режим, без CQE).

```bash
# Снизить max-frequency в DTB
sudo fdtput -t i /boot/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb /soc@3000000/sdmmc@4022000 max-frequency 52000000

# Проверить
sudo dtc -I dtb -O dts /boot/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb 2>/dev/null | grep -A 25 "mmc@4022000" | grep "max-frequency"
# Должно быть: max-frequency = <0x3197500>;  (52 МГц)

# Перезагрузиться
sudo reboot
```
## После этого: dmesg | grep cqhci — ошибки исчезнут, eMMC не будет повреждаться.

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
### Model Zoo (YOLOv5s)
```bash
cd /root
wget https://dl.radxa.com/cubie/allwinner-model-zoo.tar.gz
tar -xzf allwinner-model-zoo.tar.gz

cd /root/awnpu_model_zoo-v0.9.0-*/3rdparty/opencv/
unzip opencv-4.9.0-aarch64-linux-sunxi-glibc.zip
```

### Сборка YOLOv5s демо
```bash
cd /root/awnpu_model_zoo-v0.9.0-*/examples/yolov5
mkdir -p build && cd build

sed -i 's|elseif(CMAKE_C_COMPILER MATCHES "aarch64")|elseif(TARGET_NAME STREQUAL "A733")|' ../CMakeLists.txt

cmake .. -DTARGET_NAME=A733 -DCMAKE_SYSTEM_NAME=Linux
make -j4
```

### Запуск YOLOv5s
```bash
cd /root/awnpu_model_zoo-v0.9.0-*/examples/yolov5/build
LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-*/common/npuruntime/lib_linux_aarch64/A733 \
./yolov5_demo_a733 -nb ../model/yolov5s_rt_uint8_a733.nb \
-i ../model/dog.jpg -l 1 -m 10
```

### Результат:
```text
detection num: 3
16:  91%, [ 135,  221,  311,  535], dog
 2:  67%, [ 470,   74,  688,  173], car
 1:  61%, [ 155,  118,  573,  424], bicycle
Скорость: ~25 мс на кадр (~39 FPS).
```

**Важно:** если LD_LIBRARY_PATH не указать — будет error while loading shared libraries: libNBGlinker.so.

## 🐍 Python-обёртка для NPU (`yolov5_npu.py`)

**Файл:** `~/ugv_rpi/yolov5_npu.py`

**Назначение:** Вызов YOLOv5s на NPU из Python через `subprocess`.

```python
"""
Python-обёртка для YOLOv5s на NPU через subprocess.
Вызывает yolov5_demo_a733 и парсит результат.
"""
import subprocess
import re
import os

# Актуальные пути (после распаковки Model Zoo)
DEMO_PATH = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/build/yolov5_demo_a733"
MODEL_PATH = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/model/yolov5s_rt_uint8_a733.nb"
LD_LIBRARY_PATH = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733"


def detect(image_path):
    """
    Запускает YOLOv5s на NPU для указанного изображения.
    Возвращает список детекций.
    """
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = LD_LIBRARY_PATH

    result = subprocess.run(
        [DEMO_PATH, '-nb', MODEL_PATH, '-i', image_path, '-l', '1', '-m', '10'],
        capture_output=True,
        text=True,
        env=env,
        timeout=30
    )

    detections = []
    lines = result.stderr.split('\n')  # ВАЖНО: stderr, а не stdout!
    for line in lines:
        match = re.match(r'\s*(\d+):\s+(\d+)%,\s+\[\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\],\s+(\w+)', line)
        if match:
            class_id = int(match.group(1))
            confidence = int(match.group(2)) / 100.0
            x0, y0, x1, y1 = map(int, match.groups()[2:6])
            class_name = match.group(7)
            detections.append({
                'class': class_name,
                'class_id': class_id,
                'confidence': confidence,
                'bbox': [x0, y0, x1, y1]
            })

    return detections


if __name__ == '__main__':
    image = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/model/dog.jpg"
    print(f"Testing on {image}...")
    detections = detect(image)
    for det in detections:
        print(f"  {det['class']}: {det['confidence']*100:.0f}% at {det['bbox']}")
```

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
import yolov5_npu
self.yolov5_npu = yolov5_npu
self.npu_temp_path = "/tmp/yolo_input.jpg"
```

**Изменение 5:** Функция `cv_detect_objects` — использует NPU.

```python
def cv_detect_objects(self, img):
    overlay_buffer = np.zeros_like(img)
    cv2.putText(overlay_buffer, 'NPU YOLOv5s', (50, 50), cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2)

    cv2.imwrite(self.npu_temp_path, img)

    try:
        detections = self.yolov5_npu.detect(self.npu_temp_path)
    except Exception as e:
        print(f"[cv_detect_objects] NPU error: {e}")
        self.overlay = overlay_buffer
        return

    for det in detections:
        x0, y0, x1, y1 = det['bbox']
        label = f"{det['class']}: {det['confidence']*100:.0f}%"
        cv2.rectangle(overlay_buffer, (x0, y0), (x1, y1), (0, 255, 0), 2)
        y = y0 - 10 if y0 - 10 > 10 else y0 + 20
        cv2.putText(overlay_buffer, label, (x0, y), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

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
.video img{
    width: 960px;
    height: 540px;
    border-radius: 4px;
    object-fit: contain;
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

## 🚀 Автозапуск `app.py` (systemd)

**Файл:** `/etc/systemd/system/ugv.service`

```ini
[Unit]
Description=UGV Robot App
After=network.target

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
| Objects | 10304 | Детекция объектов (NPU YOLOv5s) |
| Color | 10305 | Детекция цвета |
| Hand GS | 10306 | Жесты рук |
| Auto | 10307 | Авто-режим |
| MP Face | 10308 | MediaPipe Face |
| MP Pose | 10309 | MediaPipe Pose |

### Формат отправки (через `base_ctrl.py`)

```python
# В app.py (обработчик /send_command)
cmdline_ctrl('base -c {"T":10304}')

# cmdline_ctrl парсит:
# args = ['base', '-c', '{"T":10304}']
# base.base_json_ctrl(json.loads(args[2]))
# → UART7: {"T":10304}\n
```

## 🆕 Обновления (2026-09-29)

### 📷 USB-камера (Logitech C920 HD Pro)

**Подключение:**
- Камера подключена в USB 2.0 порт Orange Pi 4 Pro.
- Определяется как `/dev/video0` и `/dev/video1`.
- Модуль `uvcvideo` загружается автоматически.

**Проверка:**

```bash
# Список устройств
v4l2-ctl --list-devices
# HD Pro Webcam C920 (usb-sunxi-ehci-1.1):
#         /dev/video0
#         /dev/video1

# Поддерживаемые форматы
v4l2-ctl -d /dev/video0 --list-formats-ext
# YUYV (4:2:2): до 1920x1080 @ 5 FPS
# MJPG (Motion-JPEG): до 1920x1080 @ 30 FPS

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

### 🧠 NPU YOLOv5s на видео

**Результат:** **39 FPS**, bounding boxes в реальном времени.

**Что работает:**
- YOLOv5s обнаруживает объекты (`person`, `tv`, `chair` и т.д.).
- Отображает bounding boxes с уверенностью.
- Работает на видео с USB-камеры.

**Скорость:** ~25 мс на кадр.

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

**`templates/style.css`:**

```css
.video img{
    width: 960px;
    height: 540px;
    border-radius: 4px;
    object-fit: contain;
}
```
## 🆕 Обновления (2026-10-01)

### 💾 Ручное копирование на eMMC (обход бага `armbian-install`)

**Проблема:** `armbian-install` **неправильно копирует** `boot_package` на eMMC. После **выключения** система **не загружается** (`bad magic`, `Loading boot-pkg fail`).

**Решение:** **ручное копирование** системы с SD на eMMC (см. **раздел «Установка ОС»**).

**Важно:** после ручного копирования **обязательно** скопировать **20 МБ** загрузчика:
```bash
sudo dd if=/dev/mmcblk1 of=/dev/mmcblk0 bs=1M count=20 conv=notrunc
```
### ⚠️ Отключение CQE (баг драйвера sunxi-mmc)
**Проблема:** драйвер sunxi-mmc на A733 имеет баг с CQE. При HS400 (200 МГц) CQE сбоит при записи, что повреждает загрузчик на eMMC.

**Симптомы:**

```dmesg | grep cqhci → cqhci: Failed to halt, cmd 12, RTO.```

Система не загружается с eMMC после выключения.

**Решение:** снизить частоту eMMC до 52 МГц (HS-режим, без CQE):

```bash
sudo fdtput -t i /boot/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb /soc@3000000/sdmmc@4022000 max-frequency 52000000
```
**Результат:** dmesg | grep cqhci — ошибки исчезают, eMMC не повреждается.

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
### 🎥 VPU-энкодер (H.264, аппаратное кодирование)
> ⚠️ **Устарело.** Этот путь через FFmpeg+pipe **прерывал живой поток**. Заменён на GStreamer `tee` + `avenc_mjpeg` + `matroskamux` (см. «Запись видео — финальное решение»).

**Дата:** 2026-10-02

**Проблема:** `libvencoder.so` на A733 **не генерирует SPS/PPS** (возвращает филлер `0xFF`). Без SPS/PPS видео **не воспроизводится**.

**Решение:** **обходной путь** через **FFmpeg + GStreamer**:
- **FFmpeg** захватывает камеру (`/dev/video0`), конвертирует в **NV12**.
- **Pipe** передаёт данные в **GStreamer**.
- **GStreamer** использует `omxh264videoenc` (VPU) + `h264parse` + `mp4mux` + `filesink`.
- **SPS/PPS** генерируются **GStreamer** автоматически.

**Скрипт** `/root/ugv_rpi/vpu_record.sh`:
```bash
#!/bin/bash
OUTPUT="$1"
DURATION="${2:-0}"
if [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <output_file> [duration_seconds]"
    exit 1
fi
ffmpeg -f v4l2 -input_format mjpeg -video_size 1280x720 -i /dev/video0 \
    -pix_fmt nv12 -f rawvideo -t "$DURATION" - 2>/dev/null | \
gst-launch-1.0 -e fdsrc blocksize=1382400 ! \
    videoparse width=1280 height=720 format=nv12 framerate=30/1 ! \
    omxh264videoenc ! h264parse ! \
    mp4mux ! filesink location="$OUTPUT" 2>/dev/null
```
**Использование:**

```bash
# Запись 10 секунд
sudo /root/ugv_rpi/vpu_record.sh /tmp/test.mp4 10

# Проверка
ffprobe -v error -show_format /tmp/test.mp4
# format_name=mov,mp4,m4a,3gp,3g2,mj2
# duration=10.000000
# size=2623745
```
**Результат:** валидный MP4 с H.264 (аппаратное кодирование через VPU).

**Ограничение:** при записи через app.py — поток прерывается (камера освобождается для FFmpeg). 
**Решение** — Этап 2 (GStreamer для всего).

.........


### 📡 GstStream: поток без прерывания (2026-10-02)

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

### 🆕 Обновления 

- **2026-10-02:** GStreamer `tee` — запись без прерывания живого потока. Контейнер `.mkv` (matroskamux) — VFR сохранён. `avenc_mjpeg` вместо `jpegenc`. Фикс счётчика FPS (двойной инкремент → один). Лимит `generate_frames` 30 FPS → задержка 0.1 сек, CPU 17%. UI галереи: фильтр `.mp4`/`.avi`/`.mkv`, скачивание вместо открытия в браузере.
## 🆕 Обновления (2026-10-03)

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

## 🆕 Обновления (2026-10-0): Конвертация моделей YOLOv9 для NPU Allwinner A733 (FP16)

**Проблема:** При стандартной INT8-квантовании через Acuity Toolkit модель выдавала некорректные результаты (68+ детекций с вероятностью ~50% для всего подряд). 
**Диагностика:** Анализ гистограммы выходных данных показал, что 82% значений застряли в диапазоне `[0..15]` (INT8). Это произошло из-за того, что конвертер автоматически склеивал два выхода ONNX (`bbox` и `scores`) в один тензор и применял к ним общую, некорректную математику масштабирования (scale/zero_point) без калибровочного датасета.
**Решение:** Переход на формат **FP16** (полуточность). Это полностью исключает искажения INT8-квантования, сохраняя исходную точность ONNX-модели. Размер модели увеличивается незначительно (до ~15-25 МБ), а скорость на NPU A733 остается высокой.

#### 🛠 Алгоритм конвертации (отлаженный пайплайн)

**1. Подготовка ONNX (Гостевая ОС Ubuntu VirtualBox):**
Скачиваем официальные репараметризованные веса и экспортируем в ONNX с упрощением графа:
```bash
cd /home/user/yolov9
source yolovenv/bin/activate

# Для YOLOv9-tiny (320x320 или 640x640)
wget -O yolov9-t-converted.pt https://github.com/WongKinYiu/yolov9/releases/download/v0.1/yolov9-t-converted.pt
python3 export.py --weights ./yolov9-t-converted.pt --img-size 640 640 --batch-size 1 --include onnx --simplify

# Для YOLOv9-small (640x640)
wget -O yolov9-s-converted.pt https://github.com/WongKinYiu/yolov9/releases/download/v0.1/yolov9-s-converted.pt
python3 export.py --weights ./yolov9-s-converted.pt --img-size 640 640 --batch-size 1 --include onnx --simplify
```
**2. Конвертация в Docker (Образ ubuntu-npu:v2.0.10.2):**
Важно: Образ содержит баг в путях к библиотекам для финальной компиляции C-кода. Перед запуском pegasus.py необходимо создать символические ссылки (workaround):
```bash
# Внутри контейнера Docker:
# 1. Ссылка на папку с .so библиотеками
ln -s /root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/vsimulator/lib /root/Vivante_IDE/VivanteIDE5.11.0/prebuilt-sdk/x86_64_linux/lib

# 2. Ссылки на отсутствующие библиотеки линковщика
mkdir -p /root/Vivante_IDE/VivanteIDE5.11.0/prebuilt-sdk/common/lib
ln -s /root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/common/lib/libjpeg.a /root/Vivante_IDE/VivanteIDE5.11.0/prebuilt-sdk/common/lib/libjpeg.a
ln -s /root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/common/lib/libvdtproxy.so /root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/vsimulator/lib/libvdtproxy.so
```
**3. Импорт и Экспорт (внутри Docker):**
Создаем файл метаданных входа (input_meta_640.yaml для 640x640 или input_meta_320.yaml для 320x320):
```yaml
images:
  shape: [1, 3, 640, 640] # или [1, 3, 320, 320]
  format: "rgb"
  mean_value: [0, 0, 0]
  std_value: [1.0, 1.0, 1.0]
```
Запускаем конвертацию (пример для YOLOv9-s):
```bash
# Импорт
python3 /root/acuity-toolkit-whl-6.30.22/bin/pegasus.py import onnx \
    --model yolov9-s-converted.onnx \
    --output-model yolov9s_acuity.json \
    --output-data yolov9s_acuity.data

# Экспорт в NBG (FP16) для чипа A733 (VIP9000NANODI_PLUS)
python3 /root/acuity-toolkit-whl-6.30.22/bin/pegasus.py export ovxlib \
    --model yolov9s_acuity.json \
    --model-data yolov9s_acuity.data \
    --output-path yolov9s_fp16.nb \
    --dtype float16 \
    --optimize VIP9000NANODI_PLUS_PID0X1000003B \
    --pack-nbg-unify \
    --viv-sdk /root/Vivante_IDE/VivanteIDE5.11.0 \
    --with-input-meta input_meta_640.yaml
```
**4. Извлечение результата:**
Готовый файл network_binary.nb появляется в папке /workspace/yolov9_nbg_unify/. Копируем его в примонтированную папку для передачи на хост-машину:
```bash
cp /workspace/yolov9_nbg_unify/network_binary.nb /workspace/yolov9/yolov9s_fp16.nb
# Затем в гостевой Ubuntu: cp /home/user/yolov9/yolov9s_fp16.nb /media/sf_orangepi-build/
```
**4. Запускаем диагностику для самой быстрой модели (Tiny 320x320):**

```bash
cd /root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov8/build

LD_LIBRARY_PATH=/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733 \
./yolov8_demo_a733 -nb /root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/model/yolov9t_320_fp16.nb \
-i /root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov8/model/dog.jpg \
-l 1 -m 10
```
**Результат:**
```text
=== FP16 МОДЕЛЬ ДИАГНОСТИКА (YOLOv9-tiny 320x320) ===
Total elements: 176400
Min: -48, Max: 372, Mean: 4.70434
Гистограмма распределения значений:
  [-10.0 .. -5.0): 0
  [ -5.0 .. -1.0): 3  <-- Logits (отрицательные)
  [ -1.0 ..  0.0): 1
  [  0.0 ..  0.5): 166942  <-- Вероятности (низкие)
  [  0.5 ..  1.0): 37  <-- Вероятности (высокие)
  [  1.0 ..  5.0): 1128  <-- Координаты (малые)
  [  5.0 .. 50.0): 3806  <-- Координаты (средние)
  [ 50.0 .. +inf): 4480  <-- Координаты (большие)

=== ПЕРВЫЕ 10 БОКСОВ (cx, cy, w, h) ===
Box 0: cx=8, cy=11.5781, w=24, h=28.2344
Box 1: cx=40, cy=40, w=60, h=60
Box 2: cx=72, cy=104, w=88, h=92
Box 3: cx=128, cy=108, w=112, h=124
Box 4: cx=164, cy=144, w=148, h=156
Box 5: cx=164, cy=168, w=184, h=188
Box 6: cx=192, cy=236, w=220, h=220
Box 7: cx=228, cy=236, w=244, h=248
Box 8: cx=256, cy=268, w=272, h=284
Box 9: cx=288.5, cy=352, w=340, h=312

=== ПЕРВЫЕ 10 ВЕРОЯТНОСТЕЙ (первые 3 класса) ===
Box 0: cls0=24, cls1=44, cls2=60
Box 1: cls0=72, cls1=80, cls2=116
Box 2: cls0=100, cls1=104, cls2=112
Box 3: cls0=128, cls1=140, cls2=148
Box 4: cls0=168, cls1=172, cls2=176
Box 5: cls0=230.75, cls1=232, cls2=212
Box 6: cls0=228, cls1=236, cls2=272
Box 7: cls0=256, cls1=279.25, cls2=272
Box 8: cls0=288, cls1=336, cls2=312
Box 9: cls0=8, cls1=-4, cls2=20
destory npu finished.
~NpuUint.
```
## 🚧 Что осталось

### Ближайшее

- [x] **Пересборка Armbian с PR #10835** — GPU, VPU, H.264, CSI.
- [x] **NPU YOLOv5s** — работает (39 FPS).
- [x] **Отключение CQE** — eMMC стабильна.
- [x] **Расширение eMMC** до 29 ГБ.
- [x] **`TimeoutStopSec=5`**, **`KillMode=control-group`**, **`SendSIGKILL=yes`**.
- [x] **Обработчик `SIGTERM`** в `app.py`.
- [x] **Запись видео с параллельным потоком** — GStreamer `tee` + `avenc_mjpeg` + `.mkv`.
- [ ] **CSI-камера** — вторая камера (обзорная, на PT).
- [ ] **GPU** — ускорение OpenCV (OpenCL).
- [ ] **Переключатель камер** — USB / CSI в веб-интерфейсе.
- [ ] **ArUco-маркеры** — логика парковки.
- [ ] **ESP32 (ИК, сонары)** — код для прошивки.

### Долгосрочное

- [ ] **MediaPipe на NPU** — если получится портировать.
- [ ] **YOLOv8** — более точная модель.
- [ ] **Автопилот** — SLAM или визуальная одометрия.
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

## 👥 Авторы

- **ZHNovell** — адаптация под Orange Pi 4 Pro, NPU, веб-интерфейс.
- **Waveshare** — оригинальный `ugv_rpi`.
- **deece** — поддержка A733 в Armbian.
- **ijiki16** - GPU модули на A733.

## 📅 История изменений

- **2026-09-28:** Первый запуск Orange Pi 4 Pro, UART7, I2C2, NPU, YOLOv5s, кнопки веб-интерфейса.
- **2026-09-29:** USB-камера, запись видео, NPU на видео, OSD, локальный `cmd_action`.
- **2026-10-01:** Ручное копирование на eMMC (обход бага `armbian-install`), отключение CQE (`max-frequency` 52 МГц), расширение eMMC до 29 ГБ, `TimeoutStopSec=5`, обработчик `SIGTERM` в `app.py`.
- **2026-10-02:** GStreamer `tee` — запись без прерывания живого потока. Контейнер `.mkv` (matroskamux) — VFR сохранён.
- **2026-10-03:** Переключение режима камеры (1080p30 ↔ 720p60) по кнопке WebRTC. RES-индикатор в OSD. Динамический FPS-лимит в `generate_frames`. Проверено с NPU и записью.
---

**Последнее обновление:** 2026-10-03
