# ROS SLAM stack (реплика малинки)

Стек скопирован 1:1 с Raspberry Pi (малинки) — эталон, на котором карта чистая.

## Что запускается
`ros2 launch /ws/bringup.launch.py` → малинкин `ugv_slam cartographer.launch.py`
(bringup_lidar → ldlidar LD19 + rf2o_laser_odometry + base_node + robot_state_publisher + cartographer)
+ rosbridge(:9090) + rosapi для Foxglove.

## Отличия от малинки (только точечные адаптации)
1. `ugv_bringup.py` читает телеметрию из `/dev/shm/esp_telemetry.json` (пишет host app.py,
   владелец serial /dev/ttyACM0), а не из serial напрямую. (см. adaptations/ugv_bringup.py)
2. `bringup_lidar.launch.py` — убран `ugv_driver` (движением управляет app.py, ROS только SLAM).
3. `display.launch.py` — `ugv_nav` ищется лениво (навигация не установлена, будет на этапе nav).

## Сборка (в Docker-образе ugv-ros-humble:malinka)
```
docker run --rm -v /root/ugv_ws:/ws ugv-ros-humble:malinka bash -lc \
  'source /opt/ros/humble/setup.bash; cd /ws; colcon build --packages-select \
   cartographer ldlidar rf2o_laser_odometry robot_pose_publisher ugv_base_node ugv_bringup ugv_description ugv_slam'
```
Исходники пакетов: `/root/ugv_ws/src/{ugv_else,ugv_main}` (полная копия малинки).
