#!/bin/bash
# Открыть rviz навигации на ПК через X11-форвардинг (когда ugv_nav уже запущен).
# Запускать из MobaXterm (X11).
if [ -z "$DISPLAY" ]; then
  echo "Нет DISPLAY. Запусти из MobaXterm (X11-форвардинг)."
  exit 1
fi
docker rm -f ugv_rviz_nav 2>/dev/null || true
docker run -d --rm --name ugv_rviz_nav --network host \
  -e DISPLAY="$DISPLAY" \
  -e XAUTHORITY=/root/.Xauthority \
  -e LIBGL_ALWAYS_SOFTWARE=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v "$HOME/.Xauthority:/root/.Xauthority:ro" \
  -v /dev/shm:/dev/shm \
  -v /root/ugv_ws:/ws \
  ugv-ros-humble:malinka \
  bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; cp /ws/install/ugv_nav/share/ugv_nav/rviz/view_nav_2d.rviz /tmp/rviz_nav.rviz; exec rviz2 -d /tmp/rviz_nav.rviz'
