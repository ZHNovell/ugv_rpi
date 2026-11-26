#!/bin/bash
# Сохранение карты (вызывать с хоста при работающем SLAM-контейнере)
if ! docker ps --format '{{.Names}}' | grep -q '^ugv_slam$'; then
  echo "SLAM-контейнер не запущен. Сначала: bash /root/start_cartographer.sh"
  exit 1
fi
docker exec ugv_slam bash /ws/save_map.sh
