import unittest

import app


class CommandCapTests(unittest.TestCase):
    def setUp(self):
        app._pending_commands.clear()

    def tearDown(self):
        app._pending_commands.clear()

    def test_user_cap_and_presence_stop_does_not_block_next_user(self):
        for i in range(app.COMMAND_MAX_QUEUE - 1):
            app.enqueue_command('cap-dev', {
                'raw': '@MOVE:servo,2,%d' % (30 + i), 'device_id': 'cap-dev'})

        # 待机 STOP 是控制帧，不占用户指令名额。
        app._presence_yield('cap-dev', stop=True)

        # 最后一个用户名额仍然可用，不能被 STOP 顶成 409。
        app.enqueue_command('cap-dev', {
            'raw': '@MOVE:servo,2,60', 'device_id': 'cap-dev'})

        with self.assertRaises(ValueError):
            app.enqueue_command('cap-dev', {
                'raw': '@MOVE:servo,2,61', 'device_id': 'cap-dev'})

        self.assertLessEqual(len(app._pending_commands['cap-dev']),
                             app.COMMAND_MAX_QUEUE + 1)


if __name__ == '__main__':
    unittest.main()
