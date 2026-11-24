#!/bin/bash
# Открыть rviz на ПК через X11-форвардинг (когда SLAM-контейнер уже запущен).
# Запускать из MobaXterm (X11).
if [ -z "$DISPLAY" ]; then
  echo "Нет DISPLAY. Запусти из MobaXterm (X11-форвардинг)."
  exit 1
fi
echo "DISPLAY=$DISPLAY"
exec docker run --rm --network host \
  -e DISPLAY="$DISPLAY" \
  -e XAUTHORITY=/root/.Xauthority \
  -e LIBGL_ALWAYS_SOFTWARE=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v "$HOME/.Xauthority:/root/.Xauthority:ro" \
  -v /root/ugv_ws:/ws \
  ugv-ros-humble:malinka \
  bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec rviz2 -d /ws/install/ugv_slam/share/ugv_slam/rviz/view_slam_2d.rviz'
