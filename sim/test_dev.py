#!/usr/bin/env python3
"""Focused tests for the development-session lifecycle."""
import os
import tempfile
import unittest
from unittest import mock

from sim import dev


class DevSessionTests(unittest.TestCase):
    def test_pid_identity_requires_binary_and_agent_port(self):
        with mock.patch.object(dev, '_pid_argv', return_value=['/wrong/sim']):
            self.assertFalse(dev._pid_is_simulator(10))
        with mock.patch.object(dev, '_pid_argv',
                               return_value=[dev.BINARY, '--agent-port', '5001']):
            self.assertFalse(dev._pid_is_simulator(10))
        with mock.patch.object(dev, '_pid_argv',
                               return_value=[dev.BINARY, '--agent-port', '5002']):
            self.assertTrue(dev._pid_is_simulator(10))

    def test_stale_pid_file_is_removed(self):
        with mock.patch.object(dev, 'PID_FILE', '/tmp/mt-dev-stale.pid'):
            with open(dev.PID_FILE, 'w') as handle:
                handle.write('123')
            try:
                with mock.patch.object(dev, '_pid_alive', return_value=True), \
                        mock.patch.object(dev, '_pid_argv',
                                         return_value=['/usr/bin/python3']):
                    self.assertIsNone(dev.session_pid())
                self.assertFalse(os.path.exists(dev.PID_FILE))
            finally:
                try:
                    os.remove(dev.PID_FILE)
                except OSError:
                    pass

    def test_pid_identity_retries_while_exec_is_pending(self):
        pending = [[], [dev.BINARY, '--agent-port', '5002']]
        with mock.patch.object(dev, 'PID_FILE', '/tmp/mt-dev-exec.pid'):
            with open(dev.PID_FILE, 'w') as handle:
                handle.write('123')
            try:
                with mock.patch.object(dev, '_pid_alive', return_value=True), \
                        mock.patch.object(dev, '_pid_argv', side_effect=pending), \
                        mock.patch.object(dev, 'time') as fake_time:
                    self.assertEqual(dev.session_pid(), 123)
                self.assertGreater(fake_time.sleep.call_count, 0)
                self.assertTrue(os.path.exists(dev.PID_FILE))
            finally:
                try:
                    os.remove(dev.PID_FILE)
                except OSError:
                    pass

    def test_display_environment_match_and_mismatch(self):
        process_env = {
            'DISPLAY': ':0',
            'WAYLAND_DISPLAY': 'wayland-0',
            'XDG_RUNTIME_DIR': '/run/user/1000',
            'SDL_VIDEODRIVER': 'x11',
        }
        with mock.patch.dict(os.environ, process_env, clear=False):
            with mock.patch.object(dev, '_process_environment',
                                   return_value=dict(process_env)):
                self.assertTrue(dev._session_display_matches(10))
            changed = dict(process_env)
            changed['DISPLAY'] = ':1'
            with mock.patch.object(dev, '_process_environment',
                                   return_value=changed):
                self.assertFalse(dev._session_display_matches(10))

    def test_display_mismatch_stops_verified_old_session(self):
        with mock.patch.object(dev, 'session_pid', return_value=321), \
                mock.patch.object(dev, '_session_display_matches', return_value=False), \
                mock.patch.object(dev, 'stop_session') as stop_session, \
                mock.patch.object(dev, 'agent_ready', return_value=False), \
                mock.patch.object(dev, 'build', return_value=False):
            self.assertFalse(dev.start_session())
        stop_session.assert_called_once_with()

    def test_matching_display_reuses_ready_session(self):
        with mock.patch.object(dev, 'session_pid', return_value=321), \
                mock.patch.object(dev, '_session_display_matches', return_value=True), \
                mock.patch.object(dev, 'agent_ready', return_value=True), \
                mock.patch.object(dev, 'build') as build:
            self.assertFalse(dev.start_session())
        build.assert_not_called()

    def test_start_session_uses_default_nvs_dir(self):
        proc = mock.Mock()
        proc.pid = 123
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(dev, 'session_pid', return_value=None), \
                    mock.patch.object(dev, 'agent_ready', return_value=False), \
                    mock.patch.object(dev, 'build', return_value=True), \
                    mock.patch.object(dev, 'OUT_DIR',
                                      os.path.join(tmp, 'shots')), \
                    mock.patch.object(dev, 'LOG_FILE',
                                      os.path.join(tmp, 'session.log')), \
                    mock.patch.object(dev, 'PID_FILE',
                                      os.path.join(tmp, 'dev.pid')), \
                    mock.patch.object(dev.subprocess, 'Popen',
                                      return_value=proc) as popen, \
                    mock.patch.object(dev, '_wait_socket', return_value=True), \
                    mock.patch.object(dev, '_connect'), \
                    mock.patch.object(dev, 'rpc', return_value={'ok': True}):
                self.assertTrue(dev._start_session(headless=True))
        cmd = popen.call_args[0][0]
        self.assertEqual(cmd[cmd.index('--nvs-dir') + 1], dev.NVS_DIR)

    def test_agent_ready_rejects_non_object_reply(self):
        sock = mock.Mock()
        with mock.patch.object(dev, '_connect', return_value=sock), \
                mock.patch.object(dev, 'rpc', return_value=[]):
            self.assertFalse(dev.agent_ready())
        sock.close.assert_called_once_with()

    def test_wait_socket_stops_when_process_exits(self):
        proc = mock.Mock()
        proc.poll.return_value = 1
        with mock.patch.object(dev, 'agent_ready') as ready:
            self.assertFalse(dev._wait_socket(60, proc))
        ready.assert_not_called()

    def test_wait_socket_ignores_exec_pending_pid_file(self):
        proc = mock.Mock()
        proc.poll.return_value = None
        with mock.patch.object(dev, 'session_pid') as session_pid, \
                mock.patch.object(dev, 'agent_ready', return_value=True):
            self.assertTrue(dev._wait_socket(60, proc))
        session_pid.assert_not_called()

    def test_stop_session_cleans_up_after_rpc_failure(self):
        with mock.patch.object(dev, 'session_pid', return_value=123), \
                mock.patch.object(dev, '_connect', side_effect=RuntimeError('closed')), \
                mock.patch.object(dev, '_terminate_process') as terminate, \
                mock.patch.object(dev, '_clear_pid_file') as clear:
            dev.stop_session()
        terminate.assert_called_once_with(123)
        clear.assert_called_once_with()

    def test_navigation_rejects_malformed_rpc_reply(self):
        sock = mock.Mock()
        with mock.patch.object(dev, '_connect', return_value=sock), \
                mock.patch.object(dev, 'rpc', return_value=[]):
            self.assertFalse(dev._navigate('home'))
        sock.close.assert_called_once_with()

    def test_temporary_session_is_stopped_after_action_exception(self):
        sock = mock.Mock()
        with mock.patch.object(dev, 'running', return_value=False), \
                mock.patch.object(dev, 'start_session', return_value=True), \
                mock.patch.object(dev, 'session_pid', return_value=456), \
                mock.patch.object(dev, '_connect', return_value=sock), \
                mock.patch.object(dev, 'rpc', return_value={'ok': True}), \
                mock.patch.object(dev, '_pid_alive', return_value=True), \
                mock.patch.object(dev, 'stop_session') as stop_session:
            def failing_action(_sock):
                raise RuntimeError('screenshot connection closed')

            self.assertFalse(dev.do_on_session(None, failing_action))
        stop_session.assert_called_once_with()


if __name__ == '__main__':
    unittest.main()
