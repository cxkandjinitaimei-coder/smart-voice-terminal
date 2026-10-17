"""test_face_coordinates.py：旋转后人脸框和五点坐标回映射检查。

使用：tools/run_checks.py；仅生成数值坐标，无照片、无权重、无联网。
范围：四种 90 度方向的坐标一致性，不测模型准确率。
"""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

import cv2
import numpy as np
from face_local.face_engine import FaceEngine


class Coordinates(unittest.TestCase):
    def test_boxes_and_all_five_landmarks_return_to_original_pixels(self):
        width, height = 320, 240
        base = np.array([13, 27, 55, 71, 24, 40, 52, 41, 39, 59,
                         27, 79, 50, 81, .97], dtype=np.float32)
        corners = np.array([[13, 27], [68, 27], [13, 98], [68, 98]], np.float32)
        for turns in (0, 1, 2, 3):
            def forward(points, pixel):
                x, y = points[:, 0], points[:, 1]
                if turns == 0:
                    return points.copy()
                if turns == 1:
                    return np.column_stack((y, width - pixel - x))
                if turns == 2:
                    return np.column_stack((width - pixel - x, height - pixel - y))
                return np.column_stack((height - pixel - y, x))
            rotated = base.copy()
            edges = forward(corners, 0)
            lo, hi = edges.min(axis=0), edges.max(axis=0)
            rotated[:4] = (*lo, *(hi-lo))
            rotated[4:14] = forward(base[4:14].reshape(5, 2), 1).reshape(-1)
            np.testing.assert_allclose(FaceEngine._original_row(rotated, turns, width, height), base)
            self.assertEqual(float(rotated[-1]), float(base[-1]))


if __name__ == '__main__':
    unittest.main(verbosity=2)
