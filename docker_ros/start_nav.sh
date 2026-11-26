#!/bin/bash
# Запуск навигации (AMCL + DWA + лидар). Останавливает SLAM-контейнер.
# Требуется сохранённая карта: ugv_nav/maps/map.yaml (создаётся: bash /root/save_map.sh при работающем SLAM).
set -e
NAME="ugv_nav"
docker stop ugv_slam 2>/dev/null || true
docker rm -f "$NAME" 2>/dev/null || true
docker run -d --name "$NAME" \
  --device /dev/ttyACM1:/dev/ttyACM1 \
  -v /root/ugv_ws:/ws \
  -v /dev/shm:/dev/shm \
  --network host \
  ugv-ros-humble:malinka \
  bash -c 'export UGV_MODEL=ugv_rover; export LDLIDAR_MODEL=ld19; source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec ros2 launch ugv_nav nav.launch.py use_localplan:=dwa use_rviz:=false'
echo "Навигация запущена ($NAME). Логи: docker logs -f $NAME"
echo "Остановить: docker stop $NAME"
echo "rviz навигации: bash /root/start_rviz_nav.sh"
