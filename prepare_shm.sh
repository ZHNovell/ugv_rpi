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
