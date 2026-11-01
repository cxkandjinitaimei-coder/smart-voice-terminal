"""Reproduce wrong-NIC discovery using production selection and UDP reply code."""
from datetime import datetime, timedelta
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

import lan_discovery as D

INTERFACES = [('192.168.3.140', '255.255.255.0'),
              ('10.137.86.10', '255.255.255.0'),
              ('192.168.255.1', '255.255.255.0')]


class DiscoveryTests(unittest.TestCase):
    def test_wrong_default_route_reproduces_incident_and_is_corrected(self):
        self.assertEqual(D.select_address('10.137.86.176', '192.168.3.140', INTERFACES), '10.137.86.10')

    def test_route_within_matching_network_is_kept(self):
        self.assertEqual(D.select_address('192.168.3.60', '192.168.3.140', INTERFACES), '192.168.3.140')

    def test_real_mask_not_assumed_24(self):
        self.assertEqual(D.select_address('172.20.14.15', '192.168.3.140',
                         [('172.20.0.2', '255.255.240.0')]), '172.20.0.2')

    def test_longest_subnet_wins_over_default_route(self):
        rows = [('10.0.0.2', '255.0.0.0'), ('10.137.86.10', '255.255.255.0')]
        self.assertEqual(D.select_address('10.137.86.176', '10.0.0.2', rows), '10.137.86.10')

    def test_no_wifi_never_advertises_ethernet_or_vmware(self):
        self.assertEqual(D.select_address('10.137.86.176', '192.168.3.140', INTERFACES[:1]+INTERFACES[2:]), '')
        self.assertEqual(D.select_address('10.137.86.176', '192.168.3.140', []), '')

    def test_disconnected_and_invalid_interfaces_are_ignored(self):
        self.assertEqual(D.select_address('10.137.86.176', '192.168.3.140',
                         [('0.0.0.0', '0.0.0.0'), ('broken', 'invalid')]), '')
        self.assertEqual(D.select_address('invalid', '192.168.3.140', INTERFACES), '')

    def test_production_route_uses_interface_inventory(self):
        route = Mock()
        route.__enter__ = Mock(return_value=route)
        route.__exit__ = Mock(return_value=False)
        route.getsockname.return_value = ('192.168.3.140', 1234)
        with patch.object(D.socket, 'socket', return_value=route), patch.object(D, 'ipv4_interfaces', return_value=INTERFACES):
            self.assertEqual(D.local_ip_for('10.137.86.176'), '10.137.86.10')
        route.connect.assert_called_once_with(('10.137.86.176', 1))
        route.sendto.assert_not_called()

    def test_reply_source_is_bound_to_chosen_wifi(self):
        reply = Mock()
        reply.__enter__ = Mock(return_value=reply)
        reply.__exit__ = Mock(return_value=False)
        original = Mock()
        with patch.object(D.socket, 'socket', return_value=reply):
            D.send_reply(original, b'ARDP_IP:10.137.86.10:5000', ('10.137.86.176', 5001), '10.137.86.10')
        reply.bind.assert_called_once_with(('10.137.86.10', 0))
        reply.sendto.assert_called_once_with(b'ARDP_IP:10.137.86.10:5000', ('10.137.86.176', 5001))
        original.sendto.assert_not_called()

    def test_native_inventory_and_peer_selection(self):
        rows = D.ipv4_interfaces()
        self.assertIsInstance(rows, list)
        for ip, mask in rows:
            if ip != '0.0.0.0' and not ip.startswith(('127.', '169.254.')):
                self.assertEqual(D.select_address(ip, '203.0.113.1', rows), ip)

    def test_device_status_reports_five_second_freshness_without_changing_legacy_online(self):
        import app as A
        with patch.object(A, 'ensure_background'), patch.object(A, 'require_auth'), \
             patch.object(A, '_device_last_seen', {'test': datetime.now()-timedelta(seconds=20)}), \
             patch.object(A, '_pending_commands', {}), patch.object(A, '_device_last_ack', {}):
            with A.app.test_request_context('/api/devices'):
                d = A.api_devices().get_json()['devices'][0]
            self.assertTrue(d['online'])
            self.assertFalse(d['poll_fresh'])
            self.assertGreaterEqual(d['poll_age_sec'], 20)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    unittest.main()
