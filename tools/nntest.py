#!/usr/bin/env python3
"""On-target test for nnnix M0 (run as root: it creates a temporary network namespace).

Wiring: CP2102 on /dev/ttyUSB0 reads the kernel log; a spare USB Ethernet adapter is
cabled straight to the W5500. The adapter is moved into a private namespace as
198.18.0.1/30 for the test and moved back afterwards. It must have no IPv4 address
and must not carry the default route.

  sudo python3 tools/nntest.py --flash build/nnnix.bin
"""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True                  # runs under sudo; keep the tree user-owned
sys.path.insert(0, str(Path(__file__).resolve().parent))
from console import Serial                                    # noqa: E402
import flash as flasher                                       # noqa: E402

REPO = Path(__file__).resolve().parent.parent
TARGET = ('198.18.0.2', 5000)
PEER = '198.18.0.1/30'


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, capture_output=True)


def pattern(n):
    try:
        import numpy as np
        i = np.arange(n, dtype=np.uint32)
        return (((i * np.uint32(0x9e3779b1)) >> np.uint32(24)) ^ i).astype(np.uint8).tobytes()
    except ImportError:
        return bytes((((i * 0x9e3779b1) & 0xffffffff) >> 24 ^ i) & 0xff for i in range(n))


class Namespace:
    """Temporarily move one Ethernet adapter into a private netns."""

    def __init__(self, iface):
        self.iface, self.name, self.moved = iface, f'nnnix-test-{os.getpid()}', False

    def __enter__(self):
        info = json.loads(run('ip', '-j', 'addr', 'show', 'dev', self.iface).stdout)[0]
        if any(a['family'] == 'inet' for a in info.get('addr_info', [])):
            raise SystemExit(f'{self.iface} has an IPv4 address; refusing to move it')
        routes = json.loads(run('ip', '-j', 'route', 'show', 'default').stdout or '[]')
        if any(r.get('dev') == self.iface for r in routes):
            raise SystemExit(f'{self.iface} carries the default route; refusing to move it')
        self.was_up = 'UP' in info['flags']
        run('ip', 'netns', 'add', self.name)
        run('ip', 'link', 'set', self.iface, 'netns', self.name)
        self.moved = True
        run('ip', '-n', self.name, 'addr', 'add', PEER, 'dev', self.iface)
        run('ip', '-n', self.name, 'link', 'set', self.iface, 'up')
        run('ip', '-n', self.name, 'link', 'set', 'lo', 'up')
        deadline = time.monotonic() + 8          # USB adapters renegotiate after the move
        while time.monotonic() < deadline:
            link = json.loads(run('ip', '-n', self.name, '-j', 'link', 'show', self.iface).stdout)[0]
            if link.get('operstate') == 'UP':
                break
            time.sleep(.2)
        return self

    def __exit__(self, *exc):
        if self.moved:
            run('ip', '-n', self.name, 'addr', 'flush', 'dev', self.iface, check=False)
            run('ip', '-n', self.name, 'link', 'set', self.iface, 'netns', '1', check=False)
            run('ip', 'link', 'set', self.iface, 'up' if self.was_up else 'down', check=False)
        run('ip', 'netns', 'del', self.name, check=False)

    def call(self, fn, *args):
        """Run fn in a thread whose network namespace is ours (setns is per thread)."""
        box = {}

        def worker():
            try:
                with open(f'/run/netns/{self.name}') as ns:
                    os.setns(ns.fileno(), os.CLONE_NEWNET)
                box['value'] = fn(*args)
            except Exception as exc:          # reported by the caller
                box['error'] = exc
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        if 'error' in box:
            raise box['error']
        return box['value']


def request(cmd, div=4, length=0, payload=b'', expect=None, timeout=60):
    """One diag session. expect: number of bytes to read, or None to read until close."""
    deadline = time.monotonic() + 15
    while True:
        try:
            s = socket.create_connection(TARGET, timeout=timeout)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(.5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    started = time.perf_counter()
    s.sendall(struct.pack('>cBI', cmd, div, length) + payload)
    data = bytearray()
    while expect is None or len(data) < expect:
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        data.extend(chunk)
    elapsed = time.perf_counter() - started
    s.close()
    return bytes(data), elapsed


class Report:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail=''):
        self.rows.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name:<18} {detail}", flush=True)

    @property
    def ok(self):
        return all(ok for _, ok, _ in self.rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--flash', type=Path, metavar='IMAGE', help='program this image first')
    parser.add_argument('--iface', default='eth1', help='USB Ethernet adapter cabled to the W5500')
    parser.add_argument('--serial', default='/dev/ttyUSB0')
    parser.add_argument('--net-bytes', type=int, default=512 * 1024)
    parser.add_argument('--usb-bytes', type=int, default=256 * 1024, help='0 skips the CH376S test')
    parser.add_argument('--no-fault', action='store_true', help='skip the HardFault capture test')
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit('run as root (network namespace setup)')

    report = Report()
    serial = Serial(args.serial)
    try:
        if args.flash:
            mark = len(serial.lines)
            ok = flasher.flash(args.flash, REPO / 'build/flash.log')
            report.add('flash', ok, str(args.flash))
            if not ok:
                return 1
            banner = serial.wait_for(r'nnnix \S+ \(M0\)', 10, mark)
            report.add('boot banner', banner is not None, serial.lines[banner] if banner is not None else 'no banner')
        listening = serial.wait_for(r'diag: listening', 15, 0)
        report.add('diag listening', listening is not None)
        serial.wait_for(r'w5500: link up', 10, listening or 0)

        with Namespace(args.iface) as ns:
            ping = run('ip', 'netns', 'exec', ns.name, 'ping', '-n', '-c', '3', '-w', '10',
                       TARGET[0], check=False)
            report.add('ping', ping.returncode == 0, ping.stdout.strip().splitlines()[-1] if ping.stdout else '')

            text, _ = ns.call(request, b'P')
            report.add('banner (P)', b'nnnix' in text, text.decode(errors='replace').strip())
            text, _ = ns.call(request, b'S')
            report.add('status (S)', b'uptime' in text, text.decode(errors='replace').strip().replace('\n', ' | '))

            expected = pattern(args.net_bytes)
            data, seconds = ns.call(request, b'T', 0, args.net_bytes, b'', args.net_bytes)
            report.add('w5500 tx 25MHz', data == expected,
                       f'{len(data)} B {len(data) / seconds / 1024:.0f} KiB/s')
            reply, seconds = ns.call(request, b'R', 0, args.net_bytes, expected, 12)
            fields = struct.unpack('>III', reply) if len(reply) == 12 else (0, -1, 0)
            report.add('w5500 rx 25MHz', fields[0] == args.net_bytes and fields[1] == 0,
                       f'errors {fields[1]} {args.net_bytes / seconds / 1024:.0f} KiB/s')

            if args.usb_bytes:
                text, _ = ns.call(request, b'U', 1, args.usb_bytes, b'', None, 180)
                line = text.decode(errors='replace').strip()
                report.add('ch376 file r/w', line.endswith('OK'), line)

            if not args.no_fault:
                mark = len(serial.lines)
                text, _ = ns.call(request, b'F')
                dump = serial.wait_for(r'\*\*\* HardFault pc=', 5, mark)
                rebooted = serial.wait_for(r'nnnix \S+ \(M0\)', 10, mark)
                cause = serial.wait_for(r'reset cause .*system', 5, rebooted or mark)
                crash = serial.wait_for(r'last crash #\d+: pc=', 5, rebooted or mark)
                report.add('fault capture', None not in (dump, rebooted, cause, crash),
                           serial.lines[crash] if crash is not None else 'missing crash report')
                back = serial.wait_for(r'w5500: link up', 15, rebooted or mark)
                text, _ = ns.call(request, b'L')
                report.add('klog after reboot', back is not None and b'last crash' in text,
                           f'{len(text)} B log ring')
    finally:
        log = REPO / 'build/nntest-serial.log'
        flasher.write_user_file(log, '\n'.join(serial.lines) + '\n')
        serial.close()
    passed = sum(ok for _, ok, _ in report.rows)
    print(f'\n{passed}/{len(report.rows)} passed; serial log: {log}')
    return 0 if report.ok else 1


if __name__ == '__main__':
    sys.exit(main())
