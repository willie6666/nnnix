#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real TCP sessions with stand-in capture/input children; no desktop needed."""
import os
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'tools' / 'nnremote.py'


class RemoteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name)
        shutil.copy(SCRIPT, self.folder)
        (self.folder / 'nnscreen.py').write_text('''
import os, signal, sys, time
from pathlib import Path
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
Path(os.environ['TEST_SCREEN']).write_text(os.environ['NNINPUT_STATE_FILE'] + '\\n' + repr(sys.argv))
sys.stdout.buffer.write(b'NNV1-test-frame')
sys.stdout.buffer.flush()
while True: time.sleep(0.05)
''')
        (self.folder / 'nninput.py').write_text('''
import os, signal, sys
from pathlib import Path
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
Path(os.environ['TEST_INPUT']).write_text(os.environ['NNINPUT_STATE_FILE'])
Path(os.environ['TEST_INPUT'] + '.args').write_text(repr(sys.argv))
try:
    with open(os.environ['TEST_EVENTS'], 'ab', buffering=0) as out:
        while True:
            data = sys.stdin.buffer.read1(4096)
            if not data: break
            out.write(data)
finally:
    Path(os.environ['TEST_RELEASE']).touch()
''')
        self.env = dict(os.environ, **{f'TEST_{name.upper()}': str(self.folder / name)
                                     for name in ('events', 'release', 'screen', 'input')})
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.proc = None

    def tearDown(self):
        if self.proc:
            if self.proc.poll() is None:
                self.proc.terminate()
            try:
                self.proc.wait(8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc.stderr.close()
        self.tmp.cleanup()

    def wait_line(self, word):
        # Unbuffered binary stderr avoids hiding another line in a Python buffer.
        with selectors.DefaultSelector() as sel:
            sel.register(self.proc.stderr, selectors.EVENT_READ)
            end = time.monotonic() + 8
            line = b''
            while time.monotonic() < end:
                if not sel.select(max(0, end - time.monotonic())):
                    break
                ch = self.proc.stderr.read(1)
                if not ch:
                    break
                line += ch
                if ch == b'\n':
                    if word in line:
                        return line
                    line = b''
        self.fail(f'server did not report {word!r}; last line: {line!r}')

    def start(self, *args):
        self.proc = subprocess.Popen([sys.executable, str(self.folder / 'nnremote.py'), str(self.port),
                                      '--bind', '127.0.0.1', *args], env=self.env,
                                     stderr=subprocess.PIPE, bufsize=0)
        self.wait_line(b'waiting on')

    def connect(self):
        conn = socket.create_connection(('127.0.0.1', self.port), 5)
        data = b''
        while len(data) < 15:
            part = conn.recv(15 - len(data))
            if not part:
                conn.close()
                self.fail('screen stream ended early')
            data += part
        self.assertEqual(data, b'NNV1-test-frame')
        return conn

    def wait_events(self, expected):
        end = time.monotonic() + 5
        path = self.folder / 'events'
        while time.monotonic() < end:
            if path.exists() and path.read_bytes() == expected:
                return
            time.sleep(0.01)
        self.fail('input events did not arrive intact')

    def test_duplex_reconnect_and_cleanup(self):
        self.start('--', '--fps', '15', '--invert')
        state_paths = []
        for i in range(2):
            with self.connect() as conn:
                # Binary input may be split anywhere in TCP, including inside an event.
                conn.sendall(b'\x01\x04\x01')
                conn.sendall(b'\x00\x00\x00\x12\x34')
                self.wait_events(b'\x01\x04\x01\x00\x00\x00\x12\x34' * (i + 1))
                screen = (self.folder / 'screen').read_text().splitlines()
                self.assertEqual(screen[0], (self.folder / 'input').read_text())
                self.assertIn("'--fps', '15', '--invert'", screen[1])
                state_paths.append(screen[0])
            self.wait_line(b'waiting on')
            self.assertTrue((self.folder / 'release').exists())
            self.assertFalse(Path(state_paths[-1]).parent.exists())
            (self.folder / 'release').unlink()
        self.assertNotEqual(*state_paths)

    def test_stop_releases_input_and_closes_connection(self):
        self.start()
        with self.connect() as conn:
            conn.sendall(b'held-key')
            self.wait_events(b'held-key')
            self.proc.terminate()
            self.assertEqual(self.proc.wait(8), 0)
            self.assertEqual(conn.recv(1), b'')
        self.assertTrue((self.folder / 'release').exists())

    def test_capture_failure_ends_once_server(self):
        (self.folder / 'nnscreen.py').write_text('raise SystemExit(1)\n')
        self.start('--once')
        with socket.create_connection(('127.0.0.1', self.port), 5) as conn:
            self.assertEqual(conn.recv(1), b'')
        self.assertEqual(self.proc.wait(8), 1)

    def test_input_failure_reports_error_status(self):
        (self.folder / 'nninput.py').write_text('raise SystemExit(1)\n')
        self.start('--once')
        with socket.create_connection(('127.0.0.1', self.port), 5) as conn:
            while conn.recv(4096):
                pass
        self.wait_line(b'disconnected (status 1)')
        self.assertEqual(self.proc.wait(8), 1)

    def test_once_exits_after_disconnect(self):
        self.start('--once', '--relative')
        with self.connect() as conn:
            conn.sendall(b'event')
            self.wait_events(b'event')
            self.assertIn("'--relative'", (self.folder / 'input.args').read_text())
        self.assertEqual(self.proc.wait(8), 0)
        self.assertTrue((self.folder / 'release').exists())

    def test_invalid_arguments(self):
        for args in (['0'], ['65536'], ['x'], ['1234', '--', '-o', 'video.nnv']):
            result = subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
