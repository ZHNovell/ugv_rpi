#!/bin/bash
# Авто-исследование карты (explore_lite) — как на малинке.
# Требует: запущенную навигацию (ugv_nav) + сохранённую карту (map.yaml).
# explore_lite сам шлёт цели navigate_to_pose в nav2, nav2 публикует /cmd_vel,
# мост (/cmd_vel -> ESP32) двигает робота. Робот сам объезжает помещение.
set -e
NAME="ugv_explore"

if ! docker ps --format '{{.Names}}' | grep -q '^ugv_nav$'; then
  echo "Навигация не запущена. Сначала: bash /root/start_nav.sh"
  exit 1
fi

docker rm -f "$NAME" 2>/dev/null || true
docker run -d --name "$NAME" \
  --network host \
  -v /dev/shm:/dev/shm \
  -v /root/ugv_ws:/ws \
  ugv-ros-humble:malinka \
  bash -c 'source /opt/ros/humble/setup.bash; source /ws/install/setup.bash; exec ros2 launch explore_lite explore.launch.py use_sim_time:=false'

echo "Авто-исследование запущено ($NAME)."
echo "Логи:        docker logs -f $NAME"
echo "Остановить:  docker stop $NAME"
echo "Следить на карте: bash /root/start_rviz_nav.sh"
