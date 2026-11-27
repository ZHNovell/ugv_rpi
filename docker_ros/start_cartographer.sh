#!/bin/bash
# Запуск SLAM (малинка-стек) + rviz на ПК через X11 (MobaXterm).
NAME="ugv_slam"

# SLAM и навигация не могут работать одновременно: оба читают лидар /dev/ttyACM1.
# Останавливаем навигацию/автоисследование, иначе карта размазывается.
docker stop ugv_nav 2>/dev/null || true
docker rm -f ugv_nav 2>/dev/null || true
docker stop ugv_explore 2>/dev/null || true
docker rm -f ugv_explore 2>/dev/null || true

# 1. SLAM-контейнер (headless, без rviz внутри)
if ! docker ps --format '{{.Names}}' | grep -q "^${NAME}$"; then
  docker rm -f "$NAME" 2>/dev/null || true
  docker run -d --name "$NAME" \
    --device /dev/ttyACM1:/dev/ttyACM1 \
    -v /root/ugv_ws:/ws \
    -v /dev/shm:/dev/shm \
    --network host \
    ugv-ros-humble:malinka \
    bash -c 'export UGV_MODEL=ugv_rover; export LDLIDAR_MODEL=ld19; source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec ros2 launch /ws/bringup.launch.py'
  echo "SLAM-контейнер запущен"
else
  echo "SLAM-контейнер уже работает"
fi

# 2. rviz на ПК (через X11-форвардинг MobaXterm)
if [ -z "$DISPLAY" ]; then
  echo "DISPLAY не задан. Запусти из MobaXterm (X11), чтобы открылось окно rviz."
  exit 0
fi

echo "Жду запуска SLAM..."; sleep 6
echo "Открываю rviz (DISPLAY=$DISPLAY)..."
exec docker run --rm --network host \
  -e DISPLAY="$DISPLAY" \
  -e XAUTHORITY=/root/.Xauthority \
  -e LIBGL_ALWAYS_SOFTWARE=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v "$HOME/.Xauthority:/root/.Xauthority:ro" \
  -v /dev/shm:/dev/shm \
  -v /root/ugv_ws:/ws \
  ugv-ros-humble:malinka \
  bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; cp /ws/install/ugv_slam/share/ugv_slam/rviz/view_slam_2d.rviz /tmp/rviz_slam.rviz; exec rviz2 -d /tmp/rviz_slam.rviz'
