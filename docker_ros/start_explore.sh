#!/bin/bash
# Авто-исследование карты (explore_lite) — как на малинке.
# Один скрипт: запускает rviz на ПК (в фоне) + explore_lite (отдельный контейнер).
# Терминал остаётся свободным — ничего не блокирует.
# Требует: запущенную навигацию (ugv_nav) + сохранённую карту (map.yaml).
set -e
NAME="ugv_explore"
RVIZ="ugv_rviz_nav"

if ! docker ps --format '{{.Names}}' | grep -q '^ugv_nav$'; then
  echo "Навигация не запущена. Сначала: bash /root/start_nav.sh"
  exit 1
fi

# 1) rviz на ПК — в фоне, чтобы терминал не блокировался
if [ -n "$DISPLAY" ]; then
  docker rm -f "$RVIZ" 2>/dev/null || true
  docker run -d --rm --name "$RVIZ" \
    --network host \
    -e DISPLAY="$DISPLAY" \
    -e XAUTHORITY=/root/.Xauthority \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v "$HOME/.Xauthority:/root/.Xauthority:ro" \
    -v /dev/shm:/dev/shm \
    -v /root/ugv_ws:/ws \
    ugv-ros-humble:malinka \
    bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; cp /ws/install/ugv_nav/share/ugv_nav/rviz/view_nav_2d.rviz /tmp/rviz_nav.rviz; exec rviz2 -d /tmp/rviz_nav.rviz'
  echo "rviz запущен на ПК ($RVIZ)."
else
  echo "DISPLAY не задан — rviz пропущен (запусти из MobaXterm с X11)."
fi

# 2) авто-исследование (explore_lite) — отдельный контейнер
docker rm -f "$NAME" 2>/dev/null || true
docker run -d --name "$NAME" \
  --network host \
  -v /dev/shm:/dev/shm \
  -v /root/ugv_ws:/ws \
  ugv-ros-humble:malinka \
  bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec ros2 launch explore_lite explore.launch.py use_sim_time:=false'

echo "Авто-исследование запущено ($NAME)."
echo "Логи:        docker logs -f $NAME"
echo "Остановить:  docker stop $NAME $RVIZ"
