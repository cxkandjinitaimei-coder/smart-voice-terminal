import unittest
import presence


class PresenceTests(unittest.TestCase):
    def setUp(self):
        self.now = [0.0]
        self.p = presence.Tracker(lambda: self.now[0])

    def test_look_then_sleep_then_wake(self):
        self.assertIsNone(self.p.tick('d', 60, 120, False))
        self.now[0] = 61
        self.assertEqual(self.p.tick('d', 60, 120, False), 'look')
        self.assertEqual(self.p.status('d')['stage'], 1)
        self.now[0] = 121
        self.assertEqual(self.p.tick('d', 60, 120, False), 'sleep')
        self.assertEqual(self.p.status('d')['stage'], 2)
        self.now[0] = 130
        self.assertTrue(self.p.note('d', sound=True))
        self.assertEqual(self.p.status('d')['stage'], 0)
        self.now[0] = 191
        self.assertEqual(self.p.tick('d', 60, 120, False), 'look')

    def test_busy_does_not_shift_look_stage(self):
        self.now[0] = 61
        self.assertIsNone(self.p.tick('d', 60, 120, True))
        self.assertEqual(self.p.status('d')['stage'], 0)
        self.now[0] = 122
        self.assertEqual(self.p.tick('d', 60, 120, False), 'look')
        # 观望执行中被 busy 顶到，不会把 120 秒休眠推后。
        self.now[0] = 132
        self.assertIsNone(self.p.tick('d', 60, 120, True))
        self.now[0] = 181
        self.assertEqual(self.p.tick('d', 60, 120, False), 'sleep')

    def test_manual_activity_resets(self):
        self.assertIsNone(self.p.tick('d', 60, 120, False))
        self.now[0] = 61
        self.assertEqual(self.p.tick('d', 60, 120, False), 'look')
        self.now[0] = 70
        self.assertFalse(self.p.note('d', sound=False))
        self.assertEqual(self.p.status('d')['stage'], 0)
        self.now[0] = 125
        self.assertIsNone(self.p.tick('d', 60, 120, False))


if __name__ == '__main__':
    unittest.main()
