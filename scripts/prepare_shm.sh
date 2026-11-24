#!/bin/bash
# Копирует модель в RAM-диск (/dev/shm) при старте сервиса
MODEL_SRC="/root/ugv_rpi/yolo11/yolo26s_6_pcq_a733.nb"
MODEL_DST="/dev/shm/yolo26s_6_pcq_a733.nb"

if [ ! -f "$MODEL_DST" ] || [ "$MODEL_SRC" -nt "$MODEL_DST" ]; then
    cp "$MODEL_SRC" "$MODEL_DST"
    echo "✅ Model copied to /dev/shm"
else
    echo "✅ Model already in /dev/shm"
fi

# Копирует asound.conf (маршрут звука на USB-спикер card 3) в системный /etc
ASOUND_SRC="/root/ugv_rpi/asound.conf"
ASOUND_DST="/etc/asound.conf"
if [ ! -f "$ASOUND_DST" ] || ! cmp -s "$ASOUND_SRC" "$ASOUND_DST"; then
    cp "$ASOUND_SRC" "$ASOUND_DST"
    echo "asound.conf installed to /etc/asound.conf"
else
    echo "asound.conf already up to date"
fi

# Громкость USB-спикера на максимум
amixer -c 3 sset Speaker 100% >/dev/null 2>&1 || true
