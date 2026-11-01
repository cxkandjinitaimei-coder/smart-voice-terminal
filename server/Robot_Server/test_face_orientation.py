"""Orientation geometry and real-model regression; never writes live data."""
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


@unittest.skipUnless(os.environ.get('FACE_ROTATION_FIXTURE_DIR'), 'set saved capture directory for real-model tests')
class RealPhotos(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = Path(os.environ['FACE_ROTATION_FIXTURE_DIR'])
        cls.temporary = tempfile.TemporaryDirectory()
        cls.engine = FaceEngine(data_dir=Path(cls.temporary.name)/'unused')
        old_file = cls.fixtures.parent/'face_local/face_engine.py'
        spec = importlib.util.spec_from_file_location('original_face_engine', old_file)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        cls.old = module.FaceEngine(data_dir=Path(cls.temporary.name)/'old-unused', model_dir=old_file.parent/'models')

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def read(self, name):
        return cv2.imdecode(np.frombuffer((self.fixtures/(name+'.jpg')).read_bytes(),np.uint8),cv2.IMREAD_COLOR)

    def test_three_previous_misses_now_detected_in_original_coordinate_frame(self):
        for name in ('72c8a54d0d5242dc8ba4be2b5c2e9870', 'b04564bc492b4e64a345fd7482334898',
                     'c462433608c4488a8a380a95081fcc65'):
            image = self.read(name)
            self.assertEqual(len(self.old.extract(image.copy())[1]), 0)
            restored, faces = self.engine.extract(image.copy())
            self.assertEqual(len(faces), 1, name)
            np.testing.assert_array_equal(restored, image)
            x,y,w,h = faces[0]['row'][:4]
            self.assertTrue(0 <= x+w/2 <= image.shape[1])
            self.assertTrue(0 <= y+h/2 <= image.shape[0])

    def test_upright_features_unchanged_and_rotation_preserves_identity(self):
        image = np.ascontiguousarray(np.rot90(self.read('b04564bc492b4e64a345fd7482334898'),3))
        _, reference = self.old.extract(image.copy())
        self.assertEqual(len(reference),1)
        for turns in (0,1,2,3):
            rotated = np.ascontiguousarray(np.rot90(image,turns))
            _, current = self.engine.extract(rotated)
            self.assertEqual(len(current),1)
            self.assertGreater(float(np.dot(reference[0]['vector'],current[0]['vector'])),.9999)
            if turns == 0:
                np.testing.assert_allclose(reference[0]['row'],current[0]['row'])

    def test_adapter_box_and_targeting_refer_to_saved_image(self):
        from local_faces import LocalFaces, add_targeting
        adapter=LocalFaces(Path(self.temporary.name)/'adapter-unused')
        adapter.engine=self.engine
        raw=(self.fixtures/'b04564bc492b4e64a345fd7482334898.jpg').read_bytes()
        result=adapter.analyze(raw)
        self.assertEqual(result['size'],[320,240])
        self.assertEqual(result['face_count'],1)
        box=result['boxes'][0];face=result['faces'][0]
        self.assertAlmostEqual(box['x']+box['w']/2,face['center'][0]/320,places=3)
        self.assertAlmostEqual(box['y']+box['h']/2,face['center'][1]/240,places=3)
        target=add_targeting(result,(320,240),{})
        self.assertEqual(target['faces'][0]['center_px'],face['center'])

    def test_blank_image_stays_empty_and_settings_unchanged(self):
        image=np.zeros((240,320,3),np.uint8)
        self.assertEqual(self.engine.extract(image)[1],[])
        self.assertEqual(self.engine.settings,{'threshold':.50,'margin':.08,'interval':3.0})
        self.assertFalse(self.engine.data_dir.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
