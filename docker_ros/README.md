# ROS SLAM stack (реплика малинки)

Стек скопирован 1:1 с Raspberry Pi (малинки) — эталон, на котором карта чистая.

## Что запускается
`ros2 launch /ws/bringup.launch.py` → малинкин `ugv_slam cartographer.launch.py`
(bringup_lidar → ldlidar LD19 + rf2o_laser_odometry + base_node + robot_state_publisher + cartographer)
+ rosbridge(:9090) + rosapi для Foxglove.

## Отличия от малинки (только точечные адаптации)
1. `ugv_bringup.py` читает телеметрию из `/dev/shm/esp_telemetry.json` (пишет host app.py,
   владелец serial /dev/ttyACM0), а не из serial напрямую — см. `adaptations/ugv_bringup.py`.
2. `bringup_lidar.launch.py` — убран `ugv_driver` (движением управляет app.py, ROS только SLAM).
3. `display.launch.py` — `ugv_nav` ищется лениво (навигация не установлена, будет на этапе nav).

## Сборка (в Docker-образе ugv-ros-humble:malinka)
```bash
docker run --rm -v /root/ugv_ws:/ws ugv-ros-humble:malinka bash -lc \
  'source /opt/ros/humble/setup.bash; cd /ws; colcon build --packages-select \
   cartographer ldlidar rf2o_laser_odometry robot_pose_publisher ugv_base_node ugv_bringup ugv_description ugv_slam'
```
Исходники пакетов: `/root/ugv_ws/src/{ugv_else,ugv_main}` (полная копия малинки).

## Запуск и просмотр карты (rviz на ПК через MobaXterm)
rviz крутится в контейнере, окно открывается на ПК через X11-форвардинг MobaXterm
(на ПК ничего устанавливать не надо — MobaXterm уже содержит X-сервер).

- Подключение: MobaXterm → `root@192.168.91.3:22` (X11-форвардинг включён по умолчанию).
- **Полный запуск (SLAM + окно rviz на ПК):** `bash /root/start_cartographer.sh`
- **Только rviz (SLAM уже запущен):** `bash /root/start_rviz.sh`
- **Остановить SLAM (кататься вручную):** `docker stop ugv_slam`
- **Снова запустить SLAM:** `bash /root/start_cartographer.sh`
- rviz запускается с `LIBGL_ALWAYS_SOFTWARE=1` (рендер через llvmpipe).
- rviz-контейнер монтирует `/dev/shm` (без этого DDS-данные не идут между контейнерами).

Примечание: закрытие окна rviz НЕ останавливает SLAM — картограф продолжает строить карту
в фоне (контейнер `ugv_slam`). Чтобы освободить ресурсы — `docker stop ugv_slam`.

## Статус и план
Готово:
- [x] SLAM (Cartographer, стек малинки 1:1) + rviz на ПК через MobaXterm.
- [x] Звук (голосовые сообщения) — ALSA → USB-спикер (card 3), громкость 100%.

Планируется (требуются nav2-пакеты, по запросу):
- [ ] **Сохранение карты** — `nav2_map_server map_saver_cli` + сервис `/write_state`
      (аналог `save_2d_cartographer_map.sh` → `map.pgm`/`map.yaml`/`map.pbstream` в `ugv_nav/maps`).
- [ ] **Навигация** — `ros2 launch ugv_nav nav.launch.py use_rviz:=true` (AMCL/emcl2 + TEB).
- [ ] **Авто-исследование** — `ros2 launch explore_lite explore.launch.py`.
