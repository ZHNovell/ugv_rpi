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
        ...
        print(f"  {det['class']}: {det['confidence']*100:.0f}% at {det['bbox']}")
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

### 🎥 Запись видео

**Проблема:** `imageio` с `libx264` не работает (ошибки `quality`, `broadcast`).

**Решение:** `cv2.VideoWriter` с кодеком `mp4v`:

```python
# В cv_ctrl.py (frame_process)
if self.set_video_record_flag and not self.video_record_status_flag:
    current_time = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    video_filename = f'{self.video_path}video_{current_time}.mp4'
    h, w = input_frame.shape[:2]
    self.writer = cv2.VideoWriter(
        video_filename,
        cv2.VideoWriter_fourcc(*'mp4v'),
        30,
        (w, h)
    )
    self.video_record_status_flag = True
elif self.set_video_record_flag and self.video_record_status_flag:
    cv2.circle(input_frame, (15, 15), 5, (64, 64, 255), -1)
    frame_to_write = cv2.cvtColor(input_frame, cv2.COLOR_BGRA2BGR)
    self.writer.write(frame_to_write)
elif not self.set_video_record_flag and self.video_record_status_flag:
    self.video_record_status_flag = False
    self.writer.release()
```

**Важно:** `mp4v` (MPEG-4 Part 2) **не воспроизводится** в браузерах. Для H.264 нужен **VPU** (CedarC) — пересборка Armbian с PR #10835.

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

## 🚧 Что осталось

### Ближайшее

- [ ] **H.264** — аппаратная запись видео (нужен VPU, пересборка Armbian).
- [ ] **CSI-камера** — вторая камера (обзорная, на PT).
- [ ] **GPU** — ускорение OpenCV (пересборка Armbian).
- [ ] **Переключатель камер** — USB / CSI в веб-интерфейсе.
- [ ] **ArUco-маркеры** — логика парковки.
- [ ] **ESP32 (ИК, сонары)** — код для прошивки.
- [ ] **Пересборка Armbian с PR #10835** — GPU, VPU, H.264, CSI.

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

## 📅 История изменений

- **2026-09-28:** Первый запуск Orange Pi 4 Pro, UART7, I2C2, NPU, YOLOv5s, кнопки веб-интерфейса.
- **2026-09-29:** USB-камера, запись видео, NPU на видео, OSD, локальный `cmd_action`.

---

**Последнее обновление:** 2026-09-30
