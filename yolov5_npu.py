"""
Python-обёртка для YOLOv5s на NPU через subprocess.
Вызывает yolov5_demo_a733 и парсит результат.
"""
import subprocess
import re
import os

# Пути к демо и модели
DEMO_PATH = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/build/yolov5_demo_a733"
MODEL_PATH = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/model/yolov5s_rt_uint8_a733.nb"
LD_LIBRARY_PATH = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733"

# Классы COCO
COCO_CLASSES = [
    'person', 'bicycle', 'car', 'motorcycle', 'airplane', 'bus', 'train', 'truck',
    'boat', 'traffic light', 'fire hydrant', 'stop sign', 'parking meter', 'bench',
    'bird', 'cat', 'dog', 'horse', 'sheep', 'cow', 'elephant', 'bear', 'zebra',
    'giraffe', 'backpack', 'umbrella', 'handbag', 'tie', 'suitcase', 'frisbee',
    'skis', 'snowboard', 'sports ball', 'kite', 'baseball bat', 'baseball glove',
    'skateboard', 'surfboard', 'tennis racket', 'bottle', 'wine glass', 'cup',
    'fork', 'knife', 'spoon', 'bowl', 'banana', 'apple', 'sandwich', 'orange',
    'broccoli', 'carrot', 'hot dog', 'pizza', 'donut', 'cake', 'chair', 'couch',
    'potted plant', 'bed', 'dining table', 'toilet', 'tv', 'laptop', 'mouse',
    'remote', 'keyboard', 'cell phone', 'microwave', 'oven', 'toaster', 'sink',
    'refrigerator', 'book', 'clock', 'vase', 'scissors', 'teddy bear', 'hair drier',
    'toothbrush'
]

def detect(image_path):
    """
    Запускает YOLOv5s на NPU для указанного изображения.
    Возвращает список детекций: [{'class': 'dog', 'confidence': 0.91, 'bbox': [x0, y0, x1, y1]}, ...]
    """
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = LD_LIBRARY_PATH
    
    # Запускаем демо
    result = subprocess.run(
        [DEMO_PATH, '-nb', MODEL_PATH, '-i', image_path, '-l', '1', '-m', '10'],
        capture_output=True,
        text=True,
        env=env,
        timeout=30
    )
    
    # Парсим вывод
    detections = []
    lines = result.stderr.split('\n')
    for line in lines:
        # Ищем строки вида: "16:  91%, [ 135,  221,  311,  535], dog"
        match = re.match(r'\s*(\d+):\s+(\d+)%,\s+\[\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\],\s+(\w+)', line)
        if match:
            class_id = int(match.group(1))
            confidence = int(match.group(2)) / 100.0
            x0, y0, x1, y1 = map(int, match.groups()[2:6])
            class_name = match.group(7)
            detections.append({
                'class': class_name,
                'class_id': class_id,
                'confidence': confidence,
                'bbox': [x0, y0, x1, y1]
            })
    
    return detections


if __name__ == '__main__':
    # Тест на dog.jpg
    image = "/root/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/examples/yolov5/model/dog.jpg"
    print(f"Testing on {image}...")
    detections = detect(image)
    for det in detections:
        print(f"  {det['class']}: {det['confidence']*100:.0f}% at {det['bbox']}")
