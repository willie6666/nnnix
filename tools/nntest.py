#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""On-target test for nnnix (run as root: it creates a temporary network namespace).

Wiring: CP2102 on /dev/ttyUSB0 reads the kernel log; a spare USB Ethernet adapter is
cabled straight to the W5500. The adapter is moved into a private namespace as
198.18.0.1/30 for the test and moved back afterwards. It must have no IPv4 address
and must not carry the default route.

  sudo python3 tools/nntest.py --flash build/nnnix.bin
"""
import argparse
import json
import os
import re
import zlib
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
BANNER = r'nnnix \S+ NUC140VE3CN pdid'
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


class Telnet:
    """Minimal telnet client: drops option negotiation, reads up to the shell prompt."""
    PROMPT = 'nnnix$ '

    def __init__(self, ns, timeout=5):
        self.sock = ns.call(lambda: socket.create_connection((TARGET[0], 23), timeout=timeout))
        self.state = 0
        self.text = ''

    def _feed(self, data):
        out = bytearray()
        for b in data:
            if self.state == 0:
                if b == 255: self.state = 1
                else: out.append(b)
            elif self.state == 1:
                self.state = 2 if b in (251, 252, 253, 254) else (3 if b == 250 else 0)
                if b == 255: out.append(255); self.state = 0
            elif self.state == 2: self.state = 0
            elif self.state == 3: self.state = 4 if b == 255 else 3
            else: self.state = 0 if b == 240 else 3
        self.text += out.decode(errors='replace')

    def read_until(self, token, timeout=5):
        deadline = time.monotonic() + timeout
        while token not in self.text:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f'waiting for {token!r}, got {self.text[-80:]!r}')
            self.sock.settimeout(left)
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError(f'closed while waiting for {token!r}')
            self._feed(data)
        head, _, self.text = self.text.partition(token)
        return head

    def send(self, raw):
        self.sock.sendall(raw.encode() if isinstance(raw, str) else raw)

    def cmd(self, line, timeout=5):
        """Run a command; return its output without the echoed line."""
        self.send(line + '\r\n')
        out = self.read_until(self.PROMPT, timeout)
        return out.split('\r\n', 1)[1] if '\r\n' in out else out

    def wait_closed(self, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.sock.settimeout(deadline - time.monotonic())
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                break
            if not data:
                return True
            self._feed(data)
        return False

    def close(self):
        self.sock.close()


def console_tests(ns, report):
    """tty0 = LCD + keyboard. Keys are injected with diag 'I'; the screen is read back with 'V'."""
    def screen():
        return ns.call(request, b'V')[0].decode(errors='replace')

    def type_keys(keys, settle=0.6):
        ns.call(request, b'I', 4, len(keys), keys)
        time.sleep(settle)
        return screen()

    text = screen()
    report.add('console banner', 'nnnix$' in text and 'Type' in text, text.strip().replace('\n', ' | '))
    text = type_keys(b'echo lcd works\r')
    report.add('console echo', 'lcd works' in text.splitlines()[-3:-1] or 'lcd works' in text, '')
    text = type_keys(b'xx\x7f\x7funame\r')
    report.add('console line edit', 'NUC140' in text and 'xx' not in text, '')
    text = type_keys(b'clear\r')
    report.add('console clear', text.startswith('nnnix$') and text.count('nnnix$') == 1, repr(text[:30]))
    text = type_keys(b'help\r')
    report.add('console scroll', text.splitlines()[-1].startswith('nnnix$') and 'exit' in text,
               'help output scrolled the 8-line screen')
    type_keys(b'exit\r', 1.0)
    text = screen()
    report.add('console respawn', 'Type' in text and text.rstrip().endswith('nnnix$'),
               'getty started a new shell after exit')


def fs_tests(ns, serial, report):
    """File systems: littlefs on the SPI flash ("/"), the CH376S drive ("/usb"), /dev, /proc."""
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    for f in ('/hello.txt', '/home/big.bin', '/home/back.bin', '/usb/big.bin', '/usb/a.bin', '/usb/b.bin',
              '/usb/sub/x.txt'):
        t.cmd(f'rm {f}')                              # leftovers from an interrupted run
    t.cmd('rmdir /usb/sub')

    t.cmd('echo hello flash > /hello.txt')
    out = t.cmd('cat /hello.txt')
    report.add('flash write/read', out.strip() == 'hello flash', out.strip())

    def crc_of(text):
        m = re.match(r'([0-9a-f]{8}) (\d+) ', text.strip())
        return (int(m[1], 16), int(m[2])) if m else (None, None)

    expected = (zlib.crc32(pattern(32768)), 32768)
    out = t.cmd('mkfile /home/big.bin 32k', 30)
    rate = re.search(r'\((\d+) B/s\)', out)
    got = crc_of(t.cmd('sum /home/big.bin', 30))
    report.add('flash 32 KiB file', got == expected, f'write {int(rate[1]) // 1024 if rate else "?"} KiB/s, crc {got[0]:08x}' if got[0] else out)

    t.cmd('cp /home/big.bin /usb/big.bin', 30)
    got = crc_of(t.cmd('sum /usb/big.bin', 30))
    report.add('copy flash -> usb', got == expected, '')
    t.cmd('cp /usb/big.bin /home/back.bin', 30)
    got = crc_of(t.cmd('sum /home/back.bin', 30))
    report.add('copy usb -> flash', got == expected, '')

    t.cmd('mkdir /usb/sub')
    t.cmd('echo in sub > /usb/sub/x.txt')
    out = t.cmd('cat /usb/sub/x.txt') + t.cmd('ls /usb/sub')
    t.cmd('rm /usb/sub/x.txt')
    gone = t.cmd('rmdir /usb/sub') == '' and 'sub/' not in t.cmd('ls /usb')
    report.add('usb directories', 'in sub' in out and 'x.txt' in out and gone, '')

    t.cmd('echo more >> /hello.txt')
    out = t.cmd('cat < /hello.txt')
    report.add('redirection', out.split() == ['hello', 'flash', 'more'], repr(out))
    t.cmd('mv /home/back.bin /usb/back.bin', 30)
    out = t.cmd('ls /usb') + t.cmd('ls /home')
    report.add('mv across file systems', 'back.bin' in out.split('\r\n', 1)[0] or 'back.bin' in out, '')
    t.cmd('rm /usb/back.bin')

    df = t.cmd('df')
    proc = t.cmd('cat /proc/version') + t.cmd('ls /dev') + t.cmd('echo x > /dev/null')
    report.add('df, /proc, /dev', '/usb' in df and 'nnnix' in proc and 'null@' in proc, df.splitlines()[1].strip())

    # Two sessions read two different files on the single-file CH376S at the same time.
    t.cmd('mkfile /usb/a.bin 24k', 30)
    t.cmd('mkfile /usb/b.bin 40k', 30)
    t2 = Telnet(ns)
    t2.read_until(Telnet.PROMPT)
    t.send('sum /usb/a.bin\r\n')
    t2.send('sum /usb/b.bin\r\n')
    a = crc_of(t.read_until(Telnet.PROMPT, 30).split('\r\n', 1)[-1])
    b = crc_of(t2.read_until(Telnet.PROMPT, 30).split('\r\n', 1)[-1])
    report.add('concurrent usb reads', a == (zlib.crc32(pattern(24576)), 24576) and b == (zlib.crc32(pattern(40960)), 40960), '')
    t.cmd('rm /usb/a.bin /usb/b.bin /usb/big.bin')
    for s_ in (t, t2):
        s_.send('exit\r\n'); s_.wait_closed(); s_.close()

    # Survives a reboot.
    mark = len(serial.lines)
    ns.call(request, b'X')
    serial.wait_for(r'w5500: link up', 20, mark)
    time.sleep(0.5)
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT, 10)
    out = t.cmd('cat /hello.txt')
    got = crc_of(t.cmd('sum /home/big.bin', 30))
    report.add('persists across reboot', out.split() == ['hello', 'flash', 'more'] and got == expected, '')
    t.cmd('rm /hello.txt /home/big.bin')
    t.send('exit\r\n'); t.wait_closed(); t.close()


def telnet_tests(ns, report):
    time.sleep(0.3)                                   # let kd reap the previous diag worker
    t1 = Telnet(ns)
    t1.read_until(Telnet.PROMPT)
    free_before = t1.cmd('free')
    m = re.search(r'free (\d+) B', free_before)
    free_bytes = int(m[1]) if m else 0
    report.add('free ram', free_bytes >= 5500,
               f'{free_bytes} B heap free with the console shell and one telnet session (limit 5500)')
    out = t1.cmd('uname')
    report.add('telnet login', 'nnnix' in out, out.strip())

    t2 = Telnet(ns)
    t2.read_until(Telnet.PROMPT)
    out = t2.cmd('echo hello   "big world"')
    report.add('telnet 2nd session', out.strip() == 'hello big world', out.strip())
    ps = t1.cmd('ps')
    names = [line.split()[1] for line in ps.splitlines()[1:] if line.split() and line.split()[0].isdigit()]
    report.add('telnet ps', names.count('nsh') == 3 and 'kd' in names, ' '.join(names))

    t1.send('sleep 2\r\n')
    started = time.monotonic()
    out = t2.cmd('uptime')
    other = time.monotonic() - started
    t1.read_until(Telnet.PROMPT, 5)
    slept = time.monotonic() - started
    report.add('telnet concurrency', other < 0.5 and slept >= 1.9,
               f'session 2 answered in {other * 1000:.0f} ms while session 1 slept {slept:.2f} s')

    t2.send('ecxo\x7f\x7fho ed\x1b[Dited\r\n')       # backspaces and an arrow key
    out = t2.read_until(Telnet.PROMPT)
    report.add('line editing', out.splitlines()[-1].strip() == 'edited', repr(out.splitlines()[-1]))
    t1.send('abc\x03')
    out = t1.read_until(Telnet.PROMPT)
    report.add('ctrl-c', '^C' in out, repr(out))
    out = t1.cmd('nosuch')
    report.add('unknown command', 'command not found' in out, out.strip())
    out = t1.cmd('dmesg')
    report.add('dmesg', 'telnetd: session' in out, f'{len(out)} chars')

    t3 = Telnet(ns)
    t3.read_until(Telnet.PROMPT)
    out = t3.cmd('ps')
    report.add('three sessions', sum(line.split()[1:2] == ['nsh'] for line in out.splitlines()) == 4,
               'three telnet shells plus the console shell')
    for t in (t2, t3):
        t.send('exit\r\n')
    closed = all(t.wait_closed() for t in (t2, t3))
    report.add('logout', closed, 'both sessions closed by the server')
    for t in (t2, t3):
        t.close()

    time.sleep(0.3)                                   # telnetd reaps finished sessions every 50 ms
    t4 = Telnet(ns)
    t4.read_until(Telnet.PROMPT)
    t4.send('exit\r\n')
    report.add('reconnect', t4.wait_closed(), 'socket re-armed after logout')
    t4.close()
    time.sleep(0.3)
    free_after = t1.cmd('free')
    report.add('no leak', free_before == free_after, free_after.strip())
    t1.send('\x04')                                  # Ctrl-D on an empty line logs out
    report.add('ctrl-d logout', t1.wait_closed(), '')
    t1.close()


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
            banner = serial.wait_for(BANNER, 10, mark)
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
            data, seconds = ns.call(request, b'T', 1, args.net_bytes, b'', args.net_bytes)
            report.add('w5500 tx 12.5MHz', data == expected,
                       f'{len(data)} B {len(data) / seconds / 1024:.0f} KiB/s')
            reply, seconds = ns.call(request, b'R', 1, args.net_bytes, expected, 12)
            fields = struct.unpack('>III', reply) if len(reply) == 12 else (0, -1, 0)
            report.add('w5500 rx 12.5MHz', fields[0] == args.net_bytes and fields[1] == 0,
                       f'errors {fields[1]} {args.net_bytes / seconds / 1024:.0f} KiB/s')

            text, _ = ns.call(request, b'K')
            ps = text.decode(errors='replace')
            names = [line.split()[1] for line in ps.splitlines()[1:] if line[:5].strip().isdigit()]
            report.add('tasks (K)', {'idle', 'kd', 'diag', 'nsh'} <= set(names), ' '.join(names))

            text, _ = ns.call(request, b'D', 4, 1000, b'', None, 30)
            demo = text.decode(errors='replace')
            m = re.search(r'hog_a cpu (\d+) ms loops (\d+), hog_b cpu (\d+) ms loops (\d+)', demo)
            late = re.search(r'worst late (-?\d+) ms', demo)
            if m and late:
                a_ms, b_ms = int(m[1]), int(m[3])
                fair = min(a_ms, b_ms) / max(a_ms, b_ms, 1)
                report.add('preemption', int(late[1]) <= 2, f'10 ms ticker worst late {late[1]} ms while hogs spin')
                report.add('round robin', fair >= 0.8 and a_ms + b_ms >= 700,
                           f'equal-priority hogs {a_ms}/{b_ms} ms cpu (ratio {fair:.2f})')
            else:
                report.add('scheduler demo (D)', False, demo.strip())

            try:
                console_tests(ns, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                report.add('console', False, repr(exc))
            serial.wait_for(r'telnetd: listening', 5, 0)
            try:
                telnet_tests(ns, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                report.add('telnet', False, repr(exc))

            try:
                fs_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                report.add('file systems', False, repr(exc))

            if args.usb_bytes:
                text, _ = ns.call(request, b'U', 1, args.usb_bytes, b'', None, 180)
                line = text.decode(errors='replace').strip()
                report.add('ch376 file r/w', line.endswith('OK'), line)

            if not args.no_fault:
                mark = len(serial.lines)
                text, _ = ns.call(request, b'F')
                dump = serial.wait_for(r'\*\*\* HardFault pc=', 5, mark)
                rebooted = serial.wait_for(BANNER, 10, mark)
                cause = serial.wait_for(r'reset cause .*system', 5, rebooted or mark)
                crash = serial.wait_for(r'last crash #\d+: pc=', 5, rebooted or mark)
                report.add('fault capture', None not in (dump, rebooted, cause, crash),
                           serial.lines[crash] if crash is not None else 'missing crash report')
                back = serial.wait_for(r'w5500: link up', 15, rebooted or mark)
                text, _ = ns.call(request, b'S')
                report.add('crash report kept', back is not None and b'last crash: pc=' in text,
                           'diag S / /proc/crash after the boot log scrolled')

                mark = len(serial.lines)
                ns.call(request, b'O')
                rebooted = serial.wait_for(BANNER, 15, mark)
                crash = serial.wait_for(r'last crash #\d+: panic: stack overflow in ovf', 5, rebooted or mark)
                report.add('stack overflow', None not in (rebooted, crash),
                           serial.lines[crash] if crash is not None else 'no overflow panic reported')
                serial.wait_for(r'w5500: link up', 15, rebooted or mark)
    finally:
        log = REPO / 'build/nntest-serial.log'
        flasher.write_user_file(log, '\n'.join(serial.lines) + '\n')
        serial.close()
    passed = sum(ok for _, ok, _ in report.rows)
    print(f'\n{passed}/{len(report.rows)} passed; serial log: {log}')
    return 0 if report.ok else 1


if __name__ == '__main__':
    sys.exit(main())
