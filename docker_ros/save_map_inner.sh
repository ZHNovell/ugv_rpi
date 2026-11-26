#!/bin/bash
# Сохранение карты cartographer: map.pgm + map.yaml + map.pbstream
# Запускается ВНУТРИ SLAM-контейнера (docker exec ugv_slam bash /ws/save_map.sh)
set -e
source /opt/ros/humble/setup.bash
source /ws/install/setup.bash
MAPDIR="/ws/src/ugv_main/ugv_nav/maps"
mkdir -p "$MAPDIR"
cd "$MAPDIR"
ros2 run nav2_map_server map_saver_cli -f ./map
ros2 service call /write_state cartographer_ros_msgs/srv/WriteState "{filename: '$MAPDIR/map.pbstream'}"
echo "Карта сохранена в $MAPDIR (map.pgm, map.yaml, map.pbstream)"
