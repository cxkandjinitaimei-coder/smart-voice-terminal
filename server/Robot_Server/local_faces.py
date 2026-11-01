"""Adapter for the project's original, byte-identical YuNet/SFace engine."""
import threading
import math
from pathlib import Path

MODEL_DIR = Path(__file__).resolve().parent / 'face_local' / 'models'

ANGLE_DEFAULTS = dict(horizontal_fov_deg=60.0, vertical_fov_deg=45.0)


def angle_config(value):
    config = {}
    for key, default in ANGLE_DEFAULTS.items():
        number = value.get(key, default)
        if isinstance(number, bool) or not isinstance(number, (int, float)) or not math.isfinite(number) or not 1 <= number <= 175:
            raise ValueError(key + ' 必须为 1~175 度的有限数字')
        config[key] = float(number)
    return config


def add_targeting(result, frame_size, settings):
    """Project original engine centers to the saved frame and a pinhole camera ray.

    Pixels/normalized offsets: right/down positive. Angles: right/up positive.
    Pick only accepted matches; unknown candidates never become a named target.
    """
    config = angle_config(settings)
    width, height = frame_size
    model_w, model_h = result.get('size', frame_size)
    faces = []
    for original in result.get('faces', []):
        face = dict(original)
        cx, cy = face['center']
        nx, ny = 2 * cx / model_w - 1, 2 * cy / model_h - 1
        ray_x = nx * math.tan(math.radians(config['horizontal_fov_deg'] / 2))
        ray_y = -ny * math.tan(math.radians(config['vertical_fov_deg'] / 2))
        face.update(center_px=[round(cx / model_w * width, 2), round(cy / model_h * height, 2)],
                    offset_px=[round(nx * width / 2, 2), round(ny * height / 2, 2)],
                    offset_normalized=[round(nx, 6), round(ny, 6)],
                    angles_deg=dict(yaw=round(math.degrees(math.atan2(ray_x, 1)), 3),
                                    pitch=round(math.degrees(math.atan2(ray_y, math.hypot(1, ray_x))), 3)))
        faces.append(face)
    candidates = [f for f in faces if f.get('recognized') and isinstance(f.get('similarity'), (int, float))
                  and math.isfinite(f['similarity'])]
    # Ties stay in engine detection order; confidence is never used as similarity.
    target = max(candidates, key=lambda f: f['similarity']) if candidates else None
    boxes = [dict(box, selected=bool(target and box.get('index') == target['index']))
             for box in result.get('boxes', [])]
    return dict(result, faces=faces, boxes=boxes, frame_size=list(frame_size), target=target,
                target_policy='highest_similarity_accepted_match',
                target_reason='已选择通过匹配的人脸中相似度最高者' if target else '没有通过匹配的人脸；各人脸偏差仍可查看，请先登记参考照',
                angle_config=dict(config, model='pinhole', pixel_axes='right/down', angle_axes='right/up'))


class LocalFaces:
    def __init__(self, data_dir):
        self.data_dir = Path(data_dir)
        self.lock = threading.RLock()
        self.engine = None

    def get(self):
        with self.lock:
            if self.engine is None:
                try:
                    from face_local.face_engine import FaceEngine
                    self.engine = FaceEngine(data_dir=self.data_dir, model_dir=MODEL_DIR)
                except ImportError as error:
                    raise ValueError('本地人脸依赖未安装，请运行新版启动入口安装 OpenCV / NumPy') from error
                except (OSError, ValueError) as error:
                    raise ValueError('本地人脸模型缺失或校验失败，请使用含 face_local/models 的完整网页包') from error
            return self.engine

    def state(self):
        try:
            return dict(ready=True, **self.get().public_state())
        except ValueError as error:
            return dict(ready=False, error=str(error), engine='YuNet + SFace / CPU', people=[])

    def analyze(self, raw):
        result = self.get().recognize(raw)
        width, height = result['size']
        boxes = []
        for face in result['faces']:
            x, y, w, h = face['bbox']
            right, bottom = min(1, (x+w)/width), min(1, (y+h)/height)
            left, top = max(0, x/width), max(0, y/height)
            if right > left and bottom > top:
                boxes.append(dict(index=face['index'], x=left, y=top, w=right-left, h=bottom-top,
                                  name=face['name'], recognized=face['recognized'],
                                  similarity=face['similarity'], confidence=face['face_confidence']))
        # Keep raw frame coordinates and normalized overlays, but avoid storing
        # a second base64 image with every record.
        return dict(engine='YuNet + SFace / CPU', face_count=len(result['faces']),
                    faces=result['faces'], boxes=boxes, size=result['size'], elapsed_ms=result['elapsed_ms'],
                    summary=('检测到 %d 张人脸：' % len(result['faces']) + '、'.join(f['name'] for f in result['faces']))
                    if result['faces'] else '未检测到人脸。请使用清晰、较近的正面照片；这不代表画面中一定没有人。')
