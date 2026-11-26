# ROS SLAM + навигация (реплика малинки)

Стек скопирован 1:1 с Raspberry Pi (малинки) — эталон, на котором карта чистая.

## Что запускается (SLAM)
`ros2 launch /ws/bringup.launch.py` → малинкин `ugv_slam cartographer.launch.py`
(bringup_lidar → ldlidar LD19 + rf2o_laser_odometry + base_node + robot_state_publisher + cartographer)
+ rosbridge(:9090) + rosapi для Foxglove.

## Отличия от малинки (только точечные адаптации)
1. `ugv_bringup.py` читает телеметрию из `/dev/shm/esp_telemetry.json` (пишет host app.py,
   владелец serial /dev/ttyACM0), а не из serial напрямую — см. `adaptations/ugv_bringup.py`.
2. `bringup_lidar.launch.py` — убран `ugv_driver` (движением управляет app.py, ROS только SLAM).
3. `display.launch.py` — `ugv_nav` ищется лениво.
4. Навигация использует **DWA** (стандартный nav2-контроллер) вместо **TEB** — в образе нет `libg2o`.

## Сборка (в Docker-образе ugv-ros-humble:malinka)
```bash
docker run --rm -v /root/ugv_ws:/ws ugv-ros-humble:malinka bash -lc \
  'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; cd /ws; colcon build --packages-select \
   cartographer ldlidar rf2o_laser_odometry robot_pose_publisher ugv_base_node ugv_bringup ugv_description ugv_slam ugv_nav emcl2 explore_lite'
```
Исходники пакетов: `/root/ugv_ws/src/{ugv_else,ugv_main}` (полная копия малинки).

## SLAM: запуск и просмотр карты (rviz на ПК через MobaXterm)
rviz крутится в контейнере, окно открывается на ПК через X11-форвардинг MobaXterm.

- Подключение: MobaXterm → `root@192.168.91.3:22` (X11-форвардинг по умолчанию).
- **Запуск SLAM + rviz:** `bash /root/start_cartographer.sh`
- **Только rviz (SLAM уже идёт):** `bash /root/start_rviz.sh`
- **Остановить SLAM:** `docker stop ugv_slam`
- rviz запускается с `LIBGL_ALWAYS_SOFTWARE=1`; rviz-контейнер монтирует `/dev/shm` (DDS shared memory).

## Сохранение карты
При работающем SLAM (карта построена):
- **Сохранить:** `bash /root/save_map.sh`
- Пишет в `ugv_nav/maps/`: `map.pgm` + `map.yaml` + `map.pbstream`
  (`nav2_map_server map_saver_cli` + сервис cartographer `/write_state`).

## Навигация (AMCL + DWA)
Требует сохранённую карту (см. выше).

- **Запуск навигации (останавливает SLAM):** `bash /root/start_nav.sh`
  → `ros2 launch ugv_nav nav.launch.py use_localplan:=dwa` (AMCL + DWB + лидар).
- **rviz навигации (на ПК):** `bash /root/start_rviz_nav.sh`
- **Остановить:** `docker stop ugv_nav`
- В rviz: инструмент «2D Goal Pose» (задать цель) + «2D Pose Estimate» (задать стартовую позу).

## Авто-исследование (explore_lite)
- Пакет собран. Запуск (при работающей навигации): `ros2 launch explore_lite explore.launch.py` (требует теста).

## Статус
Готово:
- [x] SLAM (Cartographer, стек малинки 1:1) + rviz на ПК.
- [x] Звук (голосовые сообщения, громкость 100%).
- [x] Сохранение карты (`save_map.sh`).
- [x] Навигация (AMCL + DWA + nav2, `start_nav.sh`).

В плане:
- [ ] TEB local planner (нужен `libg2o`; пока DWA).
- [ ] Авто-исследование (explore_lite — собран, тестировать).
- [ ] emcl2 (альтернативная локализация, собран).
