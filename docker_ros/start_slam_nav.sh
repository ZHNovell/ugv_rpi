#!/bin/bash
# SLAM + Навигация ВМЕСТЕ (как малинка: slam_nav.launch.py) — для автоисследования.
# SLAM (slam_toolbox) строит карту на лету, Nav2 едет по ней, explore_lite ищет фронтиры.
# Сохранять карту заранее НЕ нужно.
set -e
NAME="ugv_slam_nav"

# SLAM и навигация не могут работать одновременно (общий лидар /dev/ttyACM1)
for c in ugv_slam ugv_nav ugv_explore; do
  docker stop "$c" 2>/dev/null || true
  docker rm -f "$c" 2>/dev/null || true
done
docker rm -f "$NAME" 2>/dev/null || true

docker run -d --name "$NAME" \
  --device /dev/ttyACM1:/dev/ttyACM1 \
  -v /root/ugv_ws:/ws \
  -v /dev/shm:/dev/shm \
  --network host \
  ugv-ros-humble:malinka \
  bash -c 'export UGV_MODEL=ugv_rover; export LDLIDAR_MODEL=ld19; source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec ros2 launch ugv_nav slam_nav.launch.py use_rviz:=false'

echo "SLAM+Навигация запущены ($NAME). Логи: docker logs -f $NAME"
