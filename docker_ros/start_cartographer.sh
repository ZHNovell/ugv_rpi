#!/bin/bash
# Запуск SLAM на Cartographer + EKF: лидар + сырая одометрия + IMU + EKF + cartographer + rosbridge
set -e
NAME="ugv_slam"
docker rm -f "$NAME" 2>/dev/null || true
docker run -d --name "$NAME" \
  --device /dev/ttyACM1:/dev/ttyACM1 \
  -v /root/ugv_ws:/ws \
  -v /dev/shm:/dev/shm \
  --network host \
  ugv-ros-humble:ekf \
  bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; python3 /ws/wheel_odom_node.py & exec ros2 launch /ws/slam_cartographer.launch.py'
echo "Запущен контейнер $NAME. Логи: docker logs -f $NAME"
echo "Foxglove: ws://192.168.91.3:9090"