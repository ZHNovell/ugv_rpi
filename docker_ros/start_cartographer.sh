#!/bin/bash
# Запуск SLAM как у малинки: ugv_slam cartographer.launch.py (bringup_lidar + rf2o + base_node + cartographer)
# + rosbridge/rosapi для Foxglove. Движение робота — app.py (хост), ROS здесь только SLAM.
set -e
NAME="ugv_slam"
docker rm -f "$NAME" 2>/dev/null || true
docker run -d --name "$NAME" \
  --device /dev/ttyACM1:/dev/ttyACM1 \
  -v /root/ugv_ws:/ws \
  -v /dev/shm:/dev/shm \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -e DISPLAY=:1 \
  --network host \
  ugv-ros-humble:malinka \
  bash -c 'export UGV_MODEL=ugv_rover; export LDLIDAR_MODEL=ld19; source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec ros2 launch /ws/bringup.launch.py'
echo "Запущен контейнер $NAME. Логи: docker logs -f $NAME"
echo "Foxglove: ws://192.168.91.3:9090"
