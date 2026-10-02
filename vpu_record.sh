#!/bin/bash
# VPU H.264 record script with proper signal handling
OUTPUT="$1"
DURATION="${2:-0}"

if [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <output_file> [duration_seconds]"
    exit 1
fi

# Обработка SIGTERM/SIGINT: корректно завершаем FFmpeg
cleanup() {
    echo "[vpu_record] Stopping FFmpeg..."
    pkill -INT -P $$ ffmpeg 2>/dev/null
    wait
    exit 0
}
trap cleanup SIGTERM SIGINT

# FFmpeg: capture from camera, convert to NV12, pipe to GStreamer
ffmpeg -f v4l2 -input_format mjpeg -video_size 1280x720 -i /dev/video0 \
    -pix_fmt nv12 -f rawvideo -t "$DURATION" - 2>/dev/null | \
gst-launch-1.0 -e fdsrc blocksize=1382400 ! \
    videoparse width=1280 height=720 format=nv12 framerate=30/1 ! \
    omxh264videoenc ! h264parse ! \
    mp4mux ! filesink location="$OUTPUT" 2>/dev/null

wait
