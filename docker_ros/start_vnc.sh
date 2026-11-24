#!/bin/bash
# Запуск VNC-десктопа на дисплее :1 (Xvnc + XFCE)
export HOME=/root
mkdir -p /tmp/.X11-unix /root/.vnc
pkill -f 'Xvnc :1' 2>/dev/null || true
sleep 1
rm -f /tmp/.X11-unix/X1 2>/dev/null || true
Xvnc :1 -geometry 1280x800 -depth 24 -localhost no -SecurityTypes None >/tmp/xvnc.log 2>&1 &
XVNC_PID=$!
sleep 3
DISPLAY=:1 startxfce4 >/tmp/xfce.log 2>&1 &
wait $XVNC_PID
