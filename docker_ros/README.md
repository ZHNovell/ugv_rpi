# ROS: SLAM + навигация + мост (реплика малинки)

Стек ROS скопирован 1:1 с Raspberry Pi («малинки») — эталон, на котором карта получается чистой.
- Образ: `ugv-ros-humble:malinka`
- Workspace: `/root/ugv_ws` (на хосте) = `/ws` (в контейнере)
- Источники: `/root/ugv_ws/src/{ugv_else,ugv_main}` (полная копия малинки)
- ESP32-телеметрия и команды движения идут через хост-приложение `app.py` (владелец `/dev/ttyACM0`).

---

## Быстрый старт — все команды

| Действие | Команда |
|---|---|
| SLAM (карта) + rviz на ПК | `bash /root/start_cartographer.sh` |
| Только rviz SLAM (SLAM уже идёт) | `bash /root/start_rviz.sh` |
| Сохранить карту | `bash /root/save_map.sh` |
| Навигация (останавливает SLAM) | `bash /root/start_nav.sh` |
| rviz навигации | `bash /root/start_rviz_nav.sh` |
| Логи SLAM | `docker logs -f ugv_slam` |
| Логи навигации | `docker logs -f ugv_nav` |
| Остановить SLAM | `docker stop ugv_slam` |
| Остановить навигацию | `docker stop ugv_nav` |
| Авто-исследование (explore_lite) | `bash /root/start_explore.sh` |
| Остановить авто-исследование | `docker stop ugv_explore` |

---

## Режим 1 — SLAM (построение карты)

Управление роботом — вручную (джойстик Xbox 360 или веб-клавиатура), ROS строит карту.

```bash
bash /root/start_cartographer.sh
```

Что запускается: малинкин `ugv_slam cartographer.launch.py`
(`bringup_lidar.launch.py` → ldlidar LD19 + rf2o_laser_odometry + base_node +
robot_state_publisher + cartographer) + `rosbridge_websocket`(:9090) + `rosapi`.

Если `DISPLAY` задан (MobaXterm с X11) — дополнительно откроется rviz на ПК.

Остановить: `docker stop ugv_slam`.

## Режим 2 — Сохранение карты

При работающем SLAM (карта построена):

```bash
bash /root/save_map.sh
```

Пишет в `/root/ugv_ws/src/ugv_main/ugv_nav/maps/`: `map.pgm` + `map.yaml` + `map.pbstream`
(`nav2_map_server map_saver_cli` + сервис cartographer `/write_state`).

## Режим 3 — Навигация (автономное движение по цели)

Требует сохранённую карту (см. Режим 2).

```bash
bash /root/start_nav.sh        # останавливает SLAM, запускает ugv_nav
bash /root/start_rviz_nav.sh   # rviz на ПК (из MobaXterm с X11)
```

В rviz нужно: 1) **2D Pose Estimate** (зелёная стрелка) — кликнуть на карте, где робот
реально стоит, и потянуть в сторону «носа» робота; 2) **2D Nav Goal** (красный флажок) —
кликнуть цель и потянуть в сторону нужного направления. После этого робот едет сам.
`ros2 launch ugv_nav nav.launch.py use_localplan:=dwa` (AMCL + DWA + лидар +
robot_pose_publisher).

Остановить: `docker stop ugv_nav`.

## Режим 4 — rviz на ПК (MobaXterm X11)

rviz крутится в контейнере, окно открывается на ПК через X11-форвардинг MobaXterm
(ничего ставить не надо, окно открывается само).
- Подключение: MobaXterm → `root@192.168.91.3:22` (X11-форвардинг включён по умолчанию).
- Запускать `start_*rviz*` из MobaXterm (иначе нет `DISPLAY`).
- Флаги: `LIBGL_ALWAYS_SOFTWARE=1` (llvmpipe), контейнер монтирует `/dev/shm` (DDS shared memory).

## Режим 5 — Авто-исследование (explore_lite)

Робот сам объезжает помещение и исследует карту (как на малинке, раздел «Автоматическое
исследование»). Нужна запущенная навигация + сохранённая карта.

```bash
bash /root/start_nav.sh          # 1) навигация (если ещё не запущена)
bash /root/start_explore.sh      # 2) rviz на ПК + авто-исследование (одним скриптом)
```

`start_explore.sh` сам запускает rviz (в фоне, терминал не блокирует) и `explore_lite`
в отдельном контейнере. Если `DISPLAY` не задан (не из MobaXterm) — rviz пропускается.

Что происходит: `explore_lite` шлёт цели `navigate_to_pose` в nav2, nav2 строит путь и
публикует `/cmd_vel`, мост `/cmd_vel → ESP32` двигает робота. Важно: `use_sim_time:=false`
(реальный робот, а не Gazebo).

В rviz сначала задай позу робота: **2D Pose Estimate** (зелёная стрелка) → кликни, где робот
стоит, потяни по направлению «носа». Дальше ничего нажимать не надо — explore_lite сам
находит «фронтиры» (границы известного и неизвестного) и едет туда.

**Важно:** авто-исследование ищет СЕРЫЕ (неизведанные) зоны на карте. Если карта полностью
объезжена (серых зон нет, как у сохранённой карты) — в логах будет `No frontiers found`,
робот останется на месте. Для реального исследования нужна карта с серыми зонами
(частично построенная) или запуск SLAM в том же помещении.

Остановить: `docker stop ugv_explore ugv_rviz_nav`. Логи: `docker logs -f ugv_explore`.
Примечание (как на малинке): авто-исследование — для небольшого закрытого помещения.

---

## Мост /cmd_vel → ESP32 (автономное движение)

Чтобы nav2 реально двигал робота, его `/cmd_vel` нужно довести до ESP32:

1. **ROS-сторона** (`ugv_bringup.py` в контейнере): подписка на `/cmd_vel`
   (geometry_msgs/Twist) → атомарная запись `X`/`Z` + метка времени в
   `/dev/shm/cmd_vel.json` (см. `adaptations/ugv_bringup.py`).
2. **Хост-сторона** (`app.py`): поток `cmd_vel_bridge_loop` читает
   `/dev/shm/cmd_vel.json` и шлёт в ESP32 `{"T":13,"X":...,"Z":...}`.

Правила:
- Мост работает только пока файл **свежий** (nav2 реально публикует `/cmd_vel`) —
  во время SLAM с ручным управлением он не мешает.
- **Ручное управление в приоритете**: ненулевое движение с джойстика/клавиатуры
  паузит мост на 1 с.

## Джойстик + web-heartbeat (фикс рывков)

Web-фронтенд каждые 2 с шлёт heartbeat `{"T":1,"L":0,"R":0}`. Клавиатура «хитро»
обновляет `heartbeat_left/right`, поэтому heartbeat пересылает текущую скорость, а не ноль.
Физический джойстик (pygame) шёл в обход браузера, из-за чего heartbeat каждые 2 с
останавливал робота (рывки). Исправлено: `app.py` шлёт текущие `L/R` джойстика в браузер
(событие `joy_vel`), `control.js` подставляет их в heartbeat. Итог — heartbeat пересылает
текущую скорость джойстика (и `0`, когда он отпущен — страховка остаётся).

---

## Отличия от малинки (только точечные адаптации)

1. `ugv_bringup.py` читает телеметрию из `/dev/shm/esp_telemetry.json` (пишет host `app.py`,
   владелец serial `/dev/ttyACM0`), а не из serial напрямую.
2. `ugv_bringup.py` дополнительно подписан на `/cmd_vel` → `/dev/shm/cmd_vel.json` (мост, см. выше).
3. `bringup_lidar.launch.py` — убран `ugv_driver` (движением управляет app.py, ROS только SLAM).
4. `display.launch.py` — `ugv_nav` ищется лениво.
5. Навигация использует **DWA** (стандартный nav2-контроллер) вместо **TEB** — в образе нет `libg2o`.
6. `explore_lite` подписан на `/map` с `transient_local` QoS (иначе map_server публикует карту один раз и explore её не получает) — см. `adaptations/costmap_client.cpp`.

## Сборка (в Docker-образе ugv-ros-humble:malinka)

```bash
docker run --rm -v /root/ugv_ws:/ws ugv-ros-humble:malinka bash -lc \
  'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; cd /ws; colcon build --packages-select \
   cartographer ldlidar rf2o_laser_odometry robot_pose_publisher ugv_base_node ugv_bringup ugv_description ugv_slam ugv_nav emcl2 explore_lite'
```

Пересборка одного пакета (например, после правки `ugv_bringup.py`):

```bash
docker run --rm -v /root/ugv_ws:/ws ugv-ros-humble:malinka bash -lc \
  'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; cd /ws; colcon build --packages-select ugv_bringup'
```

## Известные особенности

- **xpad (джойстик Xbox 360)** — модуль DKMS, после обновления ядра пересобрать:
  `dkms remove xpad/0.4 -k $(uname -r); dkms install xpad/0.4 -k $(uname -r); modprobe xpad`
  (иначе `Exec format error` из-за рассинхрона vermagic, джойстик `/dev/input/js0` не появится).
- rviz при закрытии просит сохранить `.rviz` — загружается одноразовый конфиг из `/tmp`, можно игнорировать.
- Ошибка GLSL `active samplers ... same texture image unit` в rviz — безвредный артефакт llvmpipe, карта рендерится.
- GPU-ускорение 3D на Orange Pi нет (`/dev/dri/card0` — только дисплей), для 2D rviz не нужно.

## Статус

Готово:
- [x] SLAM (Cartographer, стек малинки 1:1) + rviz на ПК.
- [x] Звук (голосовые сообщения, громкость 100%).
- [x] Сохранение карты (`save_map.sh`).
- [x] Навигация (AMCL + DWA + nav2, `start_nav.sh`).
- [x] Джойстик Xbox 360 (xpad/DKMS) + фикс рывков (heartbeat-мост).
- [x] Мост `/cmd_vel → ESP32` (автономное движение).

В плане:
- [ ] Авто-исследование (explore_lite — собран, включить мост, тестировать).
- [ ] emcl2 (альтернативная локализация, собран).
- [ ] TEB local planner (нужен `libg2o`; пока DWA).
- [ ] Тюнинг Cartographer (submaps.num_range_data, loop closure) против удвоения стен.
- [ ] Лидар STL-27L (`product_name:=LDLiDAR_STL27L`, `port_baudrate:=921600`, `LDLIDAR_MODEL=stl27l`).
