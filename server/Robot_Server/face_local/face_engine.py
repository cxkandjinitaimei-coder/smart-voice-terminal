"""face_engine.py：OpenCV YuNet 检测和 SFace 特征比对的本地 CPU 实现。

使用：先下载校验后的 ONNX，创建 FaceEngine；上传登记照片建立人员特征，再调用分析接口。
时序与边界：RLock 保护共享引擎与人员档案；模型输入先处理朝向，结果受像素、姿态、光照影响，不保证认出所有人脸。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
import base64
import hashlib
import json
import math
from pathlib import Path
import threading
import time
import uuid

import cv2
import numpy as np

from release_version import APP_VERSION as VERSION
HERE = Path(__file__).resolve().parent


class FaceEngine:
    def __init__(self, data_dir=None, model_dir=None):
        self.lock = threading.RLock()
        self.data_dir = Path(data_dir) if data_dir else HERE / 'data'
        self.model_dir = Path(model_dir) if model_dir else HERE / 'models'
        self.settings = {'threshold': 0.50, 'margin': 0.08, 'interval': 3.0}
        self.people = []
        if (self.data_dir / 'settings.json').exists():
            self.settings.update(json.loads((self.data_dir / 'settings.json').read_text(encoding='utf-8')))
        if (self.data_dir / 'people.json').exists():
            self.people = json.loads((self.data_dir / 'people.json').read_text(encoding='utf-8'))
        self._check_models()
        cv2.ocl.setUseOpenCL(False)
        cv2.setNumThreads(2)
        empty = np.empty(0, dtype=np.uint8)
        # Buffer overloads avoid Windows' non-ASCII model path limitations.
        self.detector = cv2.FaceDetectorYN.create('onnx',
            np.frombuffer((self.model_dir / 'face_detection_yunet_2023mar.onnx').read_bytes(), dtype=np.uint8),
            empty, (640, 640), 0.85, 0.3, 5000, cv2.dnn.DNN_BACKEND_OPENCV, cv2.dnn.DNN_TARGET_CPU)
        self.recognizer = cv2.FaceRecognizerSF.create('onnx',
            np.frombuffer((self.model_dir / 'face_recognition_sface_2021dec.onnx').read_bytes(), dtype=np.uint8),
            empty, cv2.dnn.DNN_BACKEND_OPENCV, cv2.dnn.DNN_TARGET_CPU)

    def _check_models(self):
        meta = json.loads((self.model_dir.parent / 'model_source.json').read_text(encoding='utf-8'))
        for entry in (meta, meta['recognition']):
            path = self.model_dir / entry['model']
            if hashlib.sha256(path.read_bytes()).hexdigest() != entry['model_sha256']:
                raise ValueError('模型校验失败：' + entry['model'])

    def _write(self, name, value):
        self.data_dir.mkdir(parents=True, exist_ok=True)
        path = self.data_dir / name
        temp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
        temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        temp.replace(path)

    def public_state(self):
        with self.lock:
            return {'version': VERSION, 'engine': 'YuNet + SFace / CPU', 'opencv': cv2.__version__,
                    'settings': dict(self.settings),
                    'people': [{'id': p['id'], 'name': p['name'], 'photos': len(p['samples'])} for p in self.people]}

    def save_settings(self, values):
        if not isinstance(values, dict):
            raise ValueError('参数须为对象')
        limits = {'threshold': (0.2, 0.95), 'margin': (0, 0.3), 'interval': (1, 30)}
        with self.lock:
            changed = dict(self.settings)
            for key, (lo, hi) in limits.items():
                value = values.get(key, changed[key])
                if isinstance(value, bool):
                    raise ValueError('参数须为数字')
                try:
                    value = float(value)
                except (TypeError, ValueError):
                    raise ValueError('参数须为数字')
                if not math.isfinite(value) or not lo <= value <= hi:
                    raise ValueError('%s 须在 %s～%s 内' % (key, lo, hi))
                changed[key] = value
            self._write('settings.json', changed)
            self.settings = changed
            return self.public_state()

    @staticmethod
    def decode(data):
        if not data or len(data) > 12 * 1024 * 1024:
            raise ValueError('请上传不超过12 MB的图片')
        image = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError('图片无法读取，请使用JPEG或PNG')
        if image.shape[0] * image.shape[1] > 16_000_000:
            raise ValueError('图片超过1600万像素，请先缩小')
        return image

    @staticmethod
    def _original_row(row, turns, width, height):
        """Map a CCW-rotated detection back to the unrotated input image."""
        restored = row.copy()
        x, y, w, h = (float(v) for v in row[:4])
        points = row[4:14].reshape(5, 2)
        # Boxes describe continuous edges; landmarks describe pixel centers.
        if turns == 1:
            restored[:4] = (width - y - h, x, h, w)
            restored[4:14:2] = width - 1 - points[:, 1]
            restored[5:14:2] = points[:, 0]
        elif turns == 2:
            restored[:4] = (width - x - w, height - y - h, w, h)
            restored[4:14:2] = width - 1 - points[:, 0]
            restored[5:14:2] = height - 1 - points[:, 1]
        elif turns == 3:
            restored[:4] = (y, height - x - w, h, w)
            restored[4:14:2] = points[:, 1]
            restored[5:14:2] = height - 1 - points[:, 0]
        return restored

    def extract(self, image):
        h, w = image.shape[:2]
        scale = min(1.0, 640 / max(h, w))
        if scale < 1:
            image = cv2.resize(image, (round(w * scale), round(h * scale)))
        h, w = image.shape[:2]
        self.detector.setInputSize((w, h))
        _, rows = self.detector.detect(image)
        turns = 0
        upright = image
        # Preserve the existing upright path and its threshold. Only retry
        # orientation when it found no face; use one direction to avoid duplicates.
        if rows is None or len(rows) == 0:
            best_score = -1.0
            for candidate in (1, 2, 3):
                rotated = np.ascontiguousarray(np.rot90(image, candidate))
                self.detector.setInputSize((rotated.shape[1], rotated.shape[0]))
                _, found = self.detector.detect(rotated)
                if found is not None and len(found):
                    score = float(np.max(found[:, -1]))
                    if score > best_score:
                        best_score = score
                        upright, rows, turns = rotated, found, candidate
        result = []
        for row in (() if rows is None else rows):
            aligned = self.recognizer.alignCrop(upright, row)
            vector = self.recognizer.feature(aligned).reshape(-1).astype(np.float32)
            norm = float(np.linalg.norm(vector))
            if norm <= 0 or not math.isfinite(norm):
                continue
            result.append({'row': self._original_row(row, turns, w, h), 'vector': vector / norm})
        return image, result

    def enroll(self, name, uploads, person_id=None):
        name = str(name or '').strip()
        if not 1 <= len(name) <= 40:
            raise ValueError('请输入1～40字的人员名称')
        if not 1 <= len(uploads) <= 10:
            raise ValueError('每次请上传1～10张照片')
        with self.lock:
            existing = next((p for p in self.people if p['id'] == person_id), None) if person_id else None
            if person_id and existing is None:
                raise ValueError('人员已删除，请刷新列表')
            if not existing and len(self.people) >= 50:
                raise ValueError('人脸库最多登记50人')
            if len(uploads) + (len(existing['samples']) if existing else 0) > 10:
                raise ValueError('每人最多保留10张参考照片')
            prepared = []
            for index, data in enumerate(uploads, 1):
                image, faces = self.extract(self.decode(data))
                if len(faces) != 1:
                    raise ValueError('第%d张照片检测到%d张人脸；登记照片须清晰且只有一个人' % (index, len(faces)))
                face = faces[0]
                if min(float(face['row'][2]), float(face['row'][3])) < 40:
                    raise ValueError('第%d张人脸过小，请使用近一点的清晰照片' % index)
                prepared.append((image, face['vector']))
            person = {'id': existing['id'] if existing else uuid.uuid4().hex, 'name': name,
                      'samples': list(existing['samples']) if existing else []}
            self.data_dir.mkdir(parents=True, exist_ok=True)
            paths = []
            try:
                for image, vector in prepared:
                    filename = person['id'] + '_' + uuid.uuid4().hex + '.jpg'
                    ok, jpg = cv2.imencode('.jpg', image, [cv2.IMWRITE_JPEG_QUALITY, 90])
                    if not ok:
                        raise ValueError('无法保存登记照片')
                    path = self.data_dir / filename
                    path.write_bytes(jpg.tobytes())
                    paths.append(path)
                    person['samples'].append({'image': filename, 'vector': vector.tolist()})
                people = [person if p['id'] == person['id'] else p for p in self.people]
                if existing is None:
                    people.append(person)
                self._write('people.json', people)
                self.people = people
            except Exception:
                for path in paths:
                    path.unlink(missing_ok=True)
                raise
            return self.public_state()

    def delete_person(self, person_id):
        with self.lock:
            person = next((p for p in self.people if p['id'] == person_id), None)
            if person is None:
                raise ValueError('人员不存在')
            people = [p for p in self.people if p['id'] != person_id]
            self._write('people.json', people)
            self.people = people
            for sample in person['samples']:
                path = (self.data_dir / sample['image']).resolve()
                if path.parent == self.data_dir.resolve():
                    path.unlink(missing_ok=True)
            return self.public_state()

    def recognize(self, data):
        started = time.perf_counter()
        with self.lock:
            image, extracted = self.extract(self.decode(data))
            h, w = image.shape[:2]
            output = []
            for index, face in enumerate(extracted, 1):
                scores = []
                for person in self.people:
                    score = max(float(np.dot(face['vector'], np.asarray(s['vector'], dtype=np.float32)))
                                for s in person['samples'])
                    scores.append((score, person))
                scores.sort(key=lambda item: item[0], reverse=True)
                best_score = scores[0][0] if scores else None
                gap = scores[0][0] - scores[1][0] if len(scores) > 1 else None
                accepted = bool(scores and best_score >= self.settings['threshold']
                                and (gap is None or gap >= self.settings['margin']))
                person = scores[0][1] if accepted else None
                x, y, bw, bh = (float(value) for value in face['row'][:4])
                cx, cy = x + bw / 2, y + bh / 2
                item = {'index': index, 'person_id': person['id'] if person else None,
                        'name': person['name'] if person else '未知人员', 'recognized': accepted,
                        'similarity': round(best_score, 4) if best_score is not None else None,
                        'candidate_name': scores[0][1]['name'] if scores else None,
                        'gap': round(gap, 4) if gap is not None else None,
                        'bbox': [round(v, 1) for v in (x, y, bw, bh)],
                        'center': [round(cx, 1), round(cy, 1)],
                        'offset': [round(cx / w * 2 - 1, 4), round(cy / h * 2 - 1, 4)],
                        'face_confidence': round(float(face['row'][-1]), 4)}
                output.append(item)
                color = (70, 210, 60) if accepted else (40, 150, 250)
                cv2.rectangle(image, (round(x), round(y)), (round(x + bw), round(y + bh)), color, 2)
                cv2.drawMarker(image, (round(cx), round(cy)), color, cv2.MARKER_CROSS, 16, 2)
                cv2.putText(image, 'ID%d %s' % (index, 'known' if accepted else 'unknown'),
                            (max(0, round(x)), max(18, round(y) - 6)), cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)
            _, jpg = cv2.imencode('.jpg', image, [cv2.IMWRITE_JPEG_QUALITY, 85])
            return {'ok': True, 'version': VERSION, 'faces': output, 'size': [w, h],
                    'elapsed_ms': round((time.perf_counter() - started) * 1000, 2),
                    'preview': 'data:image/jpeg;base64,' + base64.b64encode(jpg).decode('ascii')}
