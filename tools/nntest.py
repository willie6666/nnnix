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
import http.client
import http.server
import json
import os
import re
import zlib
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import tempfile
import time
import traceback

sys.dont_write_bytecode = True                  # runs under sudo; keep the tree user-owned
sys.path.insert(0, str(Path(__file__).resolve().parent))
from console import Serial                                    # noqa: E402
import flash as flasher                                       # noqa: E402
import nnshare                                                # noqa: E402

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
    """Temporarily move one Ethernet adapter into a private netns.

    If the adapter already carries exactly the test peer address (someone set it up
    by hand to use telnet), it is left alone and the tests run in the host namespace."""

    def __init__(self, iface):
        self.iface, self.name, self.moved, self.direct = iface, f'nnnix-test-{os.getpid()}', False, False
        self.created, self.was_up = False, False

    def __enter__(self):
        info = json.loads(run('ip', '-j', 'addr', 'show', 'dev', self.iface).stdout)[0]
        inet = [f"{a['local']}/{a['prefixlen']}" for a in info.get('addr_info', []) if a['family'] == 'inet']
        if inet == [PEER]:
            self.direct = True
            print(f'note: {self.iface} already has {PEER}; testing without a namespace', flush=True)
            return self
        if inet:
            raise SystemExit(f'{self.iface} has an IPv4 address; refusing to move it')
        routes = json.loads(run('ip', '-j', 'route', 'show', 'default').stdout or '[]')
        if any(r.get('dev') == self.iface for r in routes):
            raise SystemExit(f'{self.iface} carries the default route; refusing to move it')
        self.was_up = 'UP' in info['flags']
        # __exit__ does not run when __enter__ fails: undo a partial setup here.
        try:
            self._setup()
        except BaseException:
            self._cleanup()
            raise
        return self

    def _setup(self):
        run('ip', 'netns', 'add', self.name)
        self.created = True
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

    def exec_prefix(self):
        """Command prefix that runs a program on the test network."""
        return [] if self.direct else ['ip', 'netns', 'exec', self.name]

    def __exit__(self, *exc):
        if not self.direct:
            self._cleanup()

    def _cleanup(self):
        """Every step runs even if an earlier one fails; failures are reported, not raised."""
        steps = []
        if self.moved:
            steps += [('ip', '-n', self.name, 'addr', 'flush', 'dev', self.iface),
                      ('ip', '-n', self.name, 'link', 'set', self.iface, 'netns', '1'),
                      ('ip', 'link', 'set', self.iface, 'up' if self.was_up else 'down')]
        if self.created:
            steps.append(('ip', 'netns', 'del', self.name))
        for step in steps:
            try:
                result = run(*step, check=False)
                if result.returncode:
                    print(f'warning: cleanup step failed: {" ".join(step)}: {result.stderr.strip()}', file=sys.stderr)
            except Exception as exc:             # keep going: the adapter must get back
                print(f'warning: cleanup step failed: {" ".join(step)}: {exc}', file=sys.stderr)
        self.moved = self.created = False

    def call(self, fn, *args):
        """Run fn in a thread whose network namespace is ours (setns is per thread)."""
        box = {}

        def worker():
            try:
                if not self.direct:
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


    def background(self, fn, *args):
        """Like call, but return the started thread instead of waiting for it."""
        def worker():
            if not self.direct:
                with open(f'/run/netns/{self.name}') as ns:
                    os.setns(ns.fileno(), os.CLONE_NEWNET)
            fn(*args)
        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        return thread


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

    def __init__(self, ns, timeout=5, connect_within=20, host=TARGET[0]):
        def connect():
            # telnetd only listens once /etc/rc has finished (or after 10 s): retry refusals.
            deadline = time.monotonic() + connect_within
            while True:
                try:
                    return socket.create_connection((host, 23), timeout=timeout)
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.3)
        self.sock = ns.call(connect)
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
    if 'nnnix$' not in text:                          # console=ask: no shell until Enter
        report.add('console ask', 'Press Enter to\nlog in.' in text, text.strip().replace('\n', ' | '))
        text = type_keys(b'\r', 1.0)
    report.add('console banner', 'nnnix$' in text and 'Type' in text, text.strip().replace('\n', ' | '))
    text = type_keys(b'echo lcd works\r')
    report.add('console echo', 'lcd works' in text.splitlines()[-3:-1] or 'lcd works' in text, '')
    text = type_keys(b'xx\x7f\x7funame\r')
    flat = text.replace('\n', '')                    # output wraps at 21 columns
    report.add('console line edit', 'NUC140' in flat and 'xx' not in text, '')
    text = type_keys(b'clear\r')
    report.add('console clear', text.startswith('nnnix$') and text.count('nnnix$') == 1, repr(text[:30]))
    text = type_keys(b'help\r', 2.0)          # 50 lines through the LCD
    # The end of the list: the programs in /bin, by name (wc is the last of them).
    report.add('console scroll', text.splitlines()[-1].startswith('nnnix$') and ' wc' in text
               and 'list commands' not in text,
               'help output scrolled the 8-line screen')
    console_view_tests(ns, report, type_keys, screen)
    # A program printing to the LCD: the screen is drawn after the terminal's
    # calls return, not under them (wget's usage overflowed its stack here).
    text = type_keys(b'wget\r', 2.0) + type_keys(b'host\r', 1.5)
    report.add('program on the console', 'usage: wget' in text and 'usage: host NAME' in text
               and text.rstrip().endswith('nnnix$'), text.strip().split('\n')[-1])
    text = type_keys(b'exit\r', 1.0)
    tasks = ns.call(request, b'K')[0].decode(errors='replace')
    report.add('console logout', 'Press Enter to\nlog in.' in text and ' nsh ' not in tasks,
               'notice shown, shell task gone')
    text = type_keys(b'\r', 1.0)
    report.add('console respawn', 'Type' in text and text.rstrip().endswith('nnnix$'),
               'Enter started a new shell')


def text_frame(text, small):
    """The LCD frame (/dev/lcd layout) that the console should draw for this
    screen text, from the fonts' sources: 6x8 cells, or 4x6 when small."""
    big = [[int(v, 16) for v in m] for m in
           re.findall(r'\{ (0x..), (0x..), (0x..), (0x..), (0x..) \}', (REPO / 'drivers/font5x7.c').read_text())]
    tiny = [int(v, 16) for v in re.findall(r'0x([0-9a-f]{4})', (REPO / 'drivers/font3x5.c').read_text())]
    w, h, cols, rows = (4, 6, 32, 10) if small else (6, 8, 21, 8)
    frame = bytearray(1024)
    for r, line in enumerate(text.split('\n')[:rows]):
        for c, ch in enumerate(line[:cols]):
            code = ord(ch) - 32 if 32 <= ord(ch) < 128 else ord('?') - 32
            for i in range(w):
                bits = (big[code][i] if i < 5 else 0) if not small else ((tiny[code] >> 5 * i) & 31 if i < 3 else 0)
                for y in range(h):
                    if bits >> y & 1:
                        frame[(r * h + y) // 8 * 128 + c * w + i] |= 1 << (r * h + y) % 8
    return bytes(frame)


def frame_matches(ns, text, small):
    """The console's pixels are the text's, apart from the cursor: the
    underline of one cell."""
    got = ns.call(request, b'G', 4, 0, b'', 1024)[0]
    want = text_frame(text, small)
    if len(got) != 1024:
        return False, f'{len(got)} bytes'
    w, h = (4, 6) if small else (6, 8)
    off = [(x, p * 8 + b) for p in range(8) for x in range(128) for b in range(8)
           if (got[p * 128 + x] ^ want[p * 128 + x]) >> b & 1]
    cells = {(x // w, y // h) for x, y in off}
    ok = all(y % h == h - 1 for _, y in off) and len(cells) <= 1
    return ok, f'{len(off)} pixels differ (the cursor)' if ok else f'{len(off)} pixels differ in {len(cells)} cells'


def console_view_tests(ns, report, type_keys, screen):
    """Small text (Ctrl+-, Ctrl+=) and the scrollback (PageUp/PageDown,
    Shift+Up/Down): keys are injected as the console bytes kbd.c makes."""
    live = screen()
    ok, detail = frame_matches(ns, live, False)
    report.add('console pixels', ok, f'21x8 text drawn as the 5x7 font gives it; {detail}')
    small = type_keys(bytes([0x84]), 1.0)               # KBD_TEXT_SMALL
    lines = small.rstrip('\n').split('\n')
    ok, detail = frame_matches(ns, small, True)
    report.add('console small text', ok and len(lines) == 10 and lines[-1].startswith('nnnix$')
               and lines[-2] == live.rstrip('\n').split('\n')[-2],
               f'32x10, rows from the scrollback above the old ones; {detail}')
    up = type_keys(bytes([0x80]), 1.0)                  # KBD_PAGE_UP: 9 lines back
    up_lines = up.rstrip('\n').split('\n')
    line = type_keys(bytes([0x83]), 1.0)                # KBD_LINE_DOWN
    down = type_keys(bytes([0x81]), 1.0)                # KBD_PAGE_DOWN: the terminal again
    ok, detail = frame_matches(ns, up, True)
    report.add('console scrollback', up != small and up_lines[-1] == lines[0] and down == small
               and line.rstrip('\n').split('\n')[-2] == lines[0],
               'PageUp shows the 9 lines above, Shift+Down one less, PageDown back')
    type_keys(bytes([0x80]), 0.5)
    typed = type_keys(b'x', 0.8)                        # any key returns to the terminal first
    type_keys(b'\x7f', 0.5)
    last = typed.rstrip('\n').split('\n')[-1]
    report.add('console scrollback ends', last.startswith('nnnix$') and last.endswith(' x') and typed != up,
               'a key while scrolled back: the terminal, with the key typed')
    normal = type_keys(bytes([0x85]), 1.0)              # KBD_TEXT_LARGE
    ok, detail = frame_matches(ns, normal, False)
    report.add('console normal text', ok and len(normal.rstrip('\n').split('\n')) <= 8
               and normal.rstrip('\n').split('\n')[-1].startswith('nnnix$'), f'back to 21x8; {detail}')


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
    out = ''.join(t.cmd(f'mkfile /home/big.bin {bad}') for bad in ('1M', 'junk', '4294967296', '4194304k', ''))
    report.add('mkfile bad sizes', out.count('usage: mkfile') == 5 and crc_of(t.cmd('sum /home/big.bin', 30)) == expected,
               'refused before the file is opened: it keeps its 32 KiB')

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
    for _ in range(20):                               # the last diag worker may still be closing
        if ' diag ' not in t1.cmd('ps'):
            break
        time.sleep(0.2)
    free_before = t1.cmd('free')
    m = re.search(r'free (\d+) B', free_before)
    free_bytes = int(m[1]) if m else 0
    # The console shell has its 512-byte scrollback meanwhile (sysctl console_history).
    report.add('free ram', free_bytes >= 5200,
               f'{free_bytes} B heap free with the console shell and one telnet session (limit 5200)')
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


def http_request(method, path, data=None, headers=None):
    c = http.client.HTTPConnection(TARGET[0], 8080, timeout=10)
    try:
        c.request(method, path, body=data, headers=headers or {})
        r = c.getresponse()
        return r.status, r.read()
    finally:
        c.close()


def m5_tests(ns, serial, report):
    """Shell scripts, text tools and streamed, atomic HTTP file transfers."""
    time.sleep(.3)
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    folder = f'/home/nntest-{os.getpid()}'
    t.cmd(f'mkdir {folder}')
    other = None
    rc_saved = False
    rc_changed = False
    backup = f'/etc/.rc-nntest-{os.getpid()}'
    files = []

    def upload(name, data):
        path = f'{folder}/{name}'
        status, _ = ns.call(http_request, 'PUT', path, data)
        if status not in (201, 204):
            raise RuntimeError(f'PUT {path}: HTTP {status}')
        files.append(path)
        return path

    try:
        before = t.cmd('free')
        t.cmd('set MSG "two words"')
        out = t.cmd('echo "${MSG}" \'$MSG\' ">literal"')
        report.add('shell quoting/env', out.strip() == 'two words $MSG >literal', repr(out))
        other = Telnet(ns)
        other.read_until(Telnet.PROMPT)
        isolated = other.cmd('echo "x${MSG}x"').strip() == 'xx'
        t.cmd('unset MSG')
        report.add('session variables', isolated and t.cmd('echo "x${MSG}x"').strip() == 'xx', '')
        other.send('exit\r\n'); other.wait_closed(); other.close(); other = None
        time.sleep(.2)

        out = t.cmd('echo "unterminated')
        report.add('shell syntax errors', 'unclosed quote' in out and t.cmd('echo $?').strip() == '2', '')
        out = t.cmd('echo a b c d e f g h i j k')
        report.add('argument overflow', 'too many arguments' in out, '')

        data = b'alpha one\nbeta two\nalpha three\ntail'
        textfile = upload('text.txt', data)
        got = t.cmd(f'wc {textfile}').split()
        report.add('wc', got[:3] == ['3', '7', str(len(data))], ' '.join(got))
        out = t.cmd(f'head -n 2 {textfile}')
        report.add('head', out.splitlines() == ['alpha one', 'beta two'], repr(out))
        out = t.cmd(f'tail -n 2 {textfile}')
        report.add('tail', out.replace('\r\n', '\n') == 'alpha three\ntail', repr(out))
        report.add('tail zero', t.cmd(f'tail -n 0 {textfile}') == '', '')
        # Input that cannot seek: the last 2 KiB of it are kept.
        out = (t.cmd(f'cat {textfile} | tail -n 2') + t.cmd(f'tail -n 1 < {textfile}')
               + t.cmd('hexdump /dev/zero | head -n 300 | tail -n 1', 20))
        last = '000012b0  ' + '00 ' * 15 + '00  |................|\r\n'
        report.add('tail from a pipe', out == 'alpha three\r\ntailtail' + last, repr(out[-90:]))   # no final newline
        out = t.cmd(f'grep -n alpha {textfile}')
        report.add('grep', out.splitlines() == ['1:alpha one', '3:alpha three'], repr(out))
        t.cmd(f'grep missing {textfile}')
        report.add('grep exit status', t.cmd('echo $?').strip() == '1', '')
        out = t.cmd(f'hexdump {textfile}')
        report.add('hexdump', '00000000  61 6c 70 68 61 20 6f 6e 65 0a' in out and '|ail|' in out, '')

        final = f'{folder}/result.txt'
        files.append(final)
        inner = upload('inner.sh', b'# nested script\r\necho inner\r\nexit\r\necho unreachable\r\n')
        script = upload('main.sh', (f'# script\nset DEST {final}\n'
                                  f'echo "hello script">"$DEST"\nsh {inner}\n'
                                  f'echo done>>"$DEST"\ncat "$DEST"').encode())
        out = t.cmd(f'sh {script}')
        report.add('scripts/nesting', out.splitlines() == ['inner', 'hello script', 'done'], repr(out))
        t.cmd('unset DEST')
        longscript = upload('long.sh', (f'echo bad > {final} ' + 'x' * 192 + '\n').encode())
        out = t.cmd(f'sh {longscript}')
        report.add('long script line', 'script read failed' in out and t.cmd(f'cat {final}').splitlines() == ['hello script', 'done'], '')

        payload = pattern(32768)
        binary = upload('upload.bin', payload)
        status, body = ns.call(http_request, 'GET', binary)
        summed = t.cmd(f'sum {binary}')
        report.add('http binary roundtrip', status == 200 and body == payload and
                   summed.startswith(f'{zlib.crc32(payload):08x} 32768 '), '32 KiB, CRC-32 verified')
        with tempfile.TemporaryDirectory(prefix='nnnix-copy-') as tmp:
            local, copied = Path(tmp) / 'source.bin', Path(tmp) / 'copy.bin'
            local.write_bytes(payload)
            remote = f'{folder}/nncp.bin'
            files.append(remote)
            put = run(*ns.exec_prefix(), sys.executable, str(REPO / 'tools/nncp.py'),
                      'put', str(local), remote, check=False)
            get = run(*ns.exec_prefix(), sys.executable, str(REPO / 'tools/nncp.py'),
                      'get', remote, str(copied), check=False)
            report.add('nncp host tool', put.returncode == get.returncode == 0 and copied.read_bytes() == payload,
                       put.stdout.strip() + '; ' + get.stdout.strip())
        status, _ = ns.call(http_request, 'PUT', binary, b'new')
        report.add('http atomic replace', status == 204 and ns.call(http_request, 'GET', binary) == (200, b'new'), '')
        empty = upload('empty', b'')
        report.add('http empty file', ns.call(http_request, 'GET', empty) == (200, b''), '')

        def raw(message, half_close=False):
            s = socket.create_connection((TARGET[0], 8080), timeout=8)
            try:
                s.sendall(message)
                if half_close:
                    s.shutdown(socket.SHUT_WR)
                reply = bytearray()
                while chunk := s.recv(4096):
                    reply.extend(chunk)
                return bytes(reply)
            finally:
                s.close()

        reply = ns.call(raw, (f'PUT {binary} HTTP/1.0\r\nContent-Length: 4096\r\n\r\nshort').encode(), True)
        kept = ns.call(http_request, 'GET', binary)
        report.add('http aborted upload', b' 408 ' in reply and kept == (200, b'new'),
                   f'original preserved: {kept == (200, b"new")}; {reply.splitlines()[:1]!r}')
        reply = ns.call(raw, f'GET {textfile} HTTP/1.0\r\n\r\n'.encode(), True)
        report.add('http TCP half-close', b' 200 ' in reply and reply.partition(b'\r\n\r\n')[2] == data,
                   'reply arrives after the client closes its send direction')
        statuses = [ns.call(http_request, 'GET', p)[0]
                    for p in ('/etc/rc', '/home/../etc/rc', '/home/%2e%2e/etc/rc')]
        report.add('http path boundary', statuses == [403, 403, 403], str(statuses))
        reply = ns.call(raw, f'PUT {binary} HTTP/1.0\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx'.encode())
        report.add('http bad framing', b' 400 ' in reply and ns.call(http_request, 'GET', binary) == (200, b'new'), '')
        reply = ns.call(raw, f'PUT {binary} HTTP/1.1\r\nHost: nnnix\r\nContent-Length: 0\r\nExpect: 100-continue\r\n\r\n'.encode())
        report.add('http expect continue', b' 100 Continue' in reply and b' 204 ' in reply, '')

        slow = ns.call(lambda: socket.create_connection((TARGET[0], 8080), timeout=8))
        try:
            slow.sendall(b'GET /home/incomplete HTTP/1.0\r\n')
            started = time.monotonic()
            out = t.cmd('uptime')
            elapsed = time.monotonic() - started
            report.add('http shell concurrency', 'up ' in out and elapsed < .5, f'shell answered in {elapsed * 1000:.0f} ms')
        finally:
            slow.close()
        time.sleep(.3)

        # Keep an existing rc safe across the test, including its error path.
        etc = t.cmd('ls /etc')
        rc_saved = any(line.split()[:1] == ['rc'] for line in etc.splitlines())
        if rc_saved:
            out = t.cmd(f'mv /etc/rc {backup}')
            if out:
                raise RuntimeError(f'cannot preserve /etc/rc: {out}')
        marker = f'{folder}/boot.txt'
        files.append(marker)
        rc_changed = True
        t.cmd(f'echo "echo boot-script-ok>{marker}" > /etc/rc')
        t.send('exit\r\n'); t.wait_closed(); t.close(); t = None
        mark = len(serial.lines)
        ns.call(request, b'X')
        serial.wait_for(r'w5500: link up', 20, mark)
        time.sleep(.5)
        t = Telnet(ns)
        t.read_until(Telnet.PROMPT, 10)
        report.add('boot /etc/rc', t.cmd(f'cat {marker}').strip() == 'boot-script-ok', 'ran before interactive shells')
        t.cmd('rm /etc/rc')
        if rc_saved:
            t.cmd(f'mv {backup} /etc/rc')
        rc_changed = False

        # Temporary files may consume flash, but should not retain any heap.
        t.cmd(f'cat {textfile} > /dev/null')
        time.sleep(.3)
        after = t.cmd('free')
        report.add('m5 heap reclaimed', before == after, after.strip())
    finally:
        if other:
            other.close()
        if t is None:
            t = Telnet(ns)
            t.read_until(Telnet.PROMPT, 10)
        if rc_changed:
            t.cmd('rm /etc/rc')
            if rc_saved:
                t.cmd(f'mv {backup} /etc/rc')
        for path in files:
            t.cmd(f'rm {path}')
        t.cmd(f'rmdir {folder}')
        t.send('exit\r\n'); t.wait_closed(); t.close()


def job_tests(ns, serial, report):
    """Pipelines, background jobs, kill, Ctrl-C, hang-up at logout, /etc/rc time limit."""
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    out = t.cmd('echo hello pipe world | wc')
    report.add('pipe', out.strip() == '1 3 17', out.strip())
    out = t.cmd('help | grep kill | wc')
    report.add('three-stage pipe', out.split()[:1] == ['1'], out.strip())
    out = t.cmd('hexdump /dev/zero | head -n 2', 10)
    report.add('broken pipe ends writer', out.count('00000') == 2, 'hexdump stops when head exits')
    out = t.cmd('cd / | wc')
    report.add('shell builtin rejected', 'cannot run in a pipeline' in out, out.strip())

    before = t.cmd('free')
    out = t.cmd('sleep 2 &')
    jobs = t.cmd('jobs')
    t.cmd('wait', 10)
    done = t.cmd('jobs')
    report.add('background job', out.startswith('[1] ') and 'running  sleep' in jobs and 'running' not in done,
               out.strip())
    t.cmd('sleep 30 &')
    t.cmd('kill %1')
    time.sleep(0.3)
    out = t.cmd('jobs')
    report.add('kill job', 'done  sleep' in out or 'running' not in out, out.strip() or 'job gone')

    results = []
    for cmd in ('sleep 30', 'hexdump /dev/zero | grep zzz', 'cat'):
        t.send(cmd + '\r\n')
        time.sleep(0.6)
        started = time.monotonic()
        t.send('\x03')
        t.read_until(Telnet.PROMPT, 5)
        results.append(time.monotonic() - started)
    report.add('ctrl-c stops commands', max(results) < 1.0,
               'sleep, pipeline, cat: ' + ', '.join(f'{r * 1000:.0f} ms' for r in results))
    time.sleep(0.3)
    after = t.cmd('free')
    report.add('jobs heap reclaimed', before == after, after.strip())

    t.send('sleep 60 &\r\n')
    t.read_until(Telnet.PROMPT)
    t.send('exit\r\n')
    t.wait_closed()
    t.close()
    time.sleep(0.5)
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    ps = t.cmd('ps')
    report.add('logout hangs up jobs', ' sleep ' not in ps, 'no orphaned sleep after logout')

    # A boot script that never finishes must not lock out logins.
    keep = 'rc' in t.cmd('ls /etc').split()
    if keep:
        t.cmd('mv /etc/rc /etc/rc.keep')
    try:
        t.cmd('echo "sleep 14" > /etc/rc')
        t.send('exit\r\n'); t.wait_closed(); t.close()
        mark = len(serial.lines)
        ns.call(request, b'X')
        serial.wait_for(r'w5500: link up', 20, mark)
        started = time.monotonic()
        t = Telnet(ns)
        t.read_until(Telnet.PROMPT, 20)
        waited = time.monotonic() - started
        forced = serial.wait_for(r'rc still running after 10 s', 2, mark) is not None
        report.add('rc time limit', forced and waited < 15, f'login {waited:.1f} s after the link came up')
    finally:
        t.cmd('rm /etc/rc')
        for _ in range(40):                       # let the boot script finish before later tests
            if ' init ' not in t.cmd('ps'):
                break
            time.sleep(0.5)
        if keep:
            t.cmd('mv /etc/rc.keep /etc/rc')
        t.send('exit\r\n'); t.wait_closed(); t.close()


def settings_tests(ns, report):
    """sysctl: console modes, idle logout and the telnet session limit, and the RAM they give back."""
    def screen():
        return ns.call(request, b'V')[0].decode(errors='replace')

    def heap_free(t):
        m = re.search(r'free (\d+) B', t.cmd('free'))
        return int(m[1]) if m else 0

    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    out = t.cmd('sysctl')
    report.add('sysctl list', all(k in out for k in ('console=', 'idle_logout=0', 'telnet_max=3', 'telnetd=on',
                                                       'httpd=on')), out.strip().replace('\r\n', ' '))
    bad = t.cmd('sysctl console=maybe') + t.cmd('sysctl telnet_max=4') + t.cmd('sysctl nosuch=1')
    report.add('sysctl checks', bad.count('invalid value') == 2 and 'unknown setting' in bad, '')
    # The LCD's text size and scrollback (also Ctrl+-/Ctrl+= and PageUp on the keyboard).
    out = (t.cmd('sysctl console_font=small') + t.cmd('sysctl console_font') + t.cmd('sysctl console_font=big')
           + t.cmd('sysctl console_history=9000') + t.cmd('sysctl console_history=1024') + t.cmd('sysctl console_history'))
    rows = screen().count('\n')                         # a line for every row, blank or not
    out += t.cmd('sysctl console_font=normal') + t.cmd('sysctl console_history=512')
    report.add('sysctl console text', 'console_font=small' in out and out.count('invalid value') == 2
               and 'console_history=1024' in out and rows == 10, out.strip().replace('\r\n', ' '))
    out = t.cmd('free')
    report.add('free breakdown', re.search(r'used: \d+ B task stacks \(\d+ tasks\), \d+ B other', out) is not None,
               out.strip().replace('\r\n', ' | '))

    # The console: on starts a shell at once, off logs it out and frees its RAM.
    t.cmd('sysctl console=on')
    time.sleep(0.5)
    with_shell = heap_free(t)
    on_screen = screen()
    # 8 KiB of scrollback does not fit beside the shells: the 512-byte ring stays.
    out = t.cmd('sysctl console_history=8192') + t.cmd('sysctl console_history')
    report.add('scrollback no memory', 'console_history: out of memory' in out and 'console_history=512' in out
               and heap_free(t) == with_shell, out.strip().replace('\r\n', ' | '))
    t.cmd('sysctl console=off')
    time.sleep(0.6)
    without = heap_free(t)
    off_screen = screen()
    ns.call(request, b'I', 4, 1, b'\r')
    time.sleep(0.4)
    still_off = 'The console is off.' in screen()
    report.add('console off', 'nnnix$' in on_screen and 'The console is off.' in off_screen and still_off
               and without - with_shell >= 1400, f'{without - with_shell} B freed by logging the console out')
    t.cmd('sysctl console=ask')
    time.sleep(0.4)
    ask = screen()
    ns.call(request, b'I', 4, 1, b'\r')
    time.sleep(0.8)
    report.add('console ask again', 'Press Enter to\nlog in.' in ask and 'nnnix$' in screen(), 'Enter starts a shell')

    # Idle logout: a quiet session is logged out; a busy shell is not.
    t2 = Telnet(ns)
    t2.read_until(Telnet.PROMPT)
    t.cmd('sysctl idle_logout=2')
    started = time.monotonic()
    t.cmd('sleep 4', 8)                               # busy, so this shell does not time out
    t.cmd('sysctl idle_logout=0')
    closed = t2.wait_closed(1)
    report.add('idle logout', closed and 'idle timeout' in t2.text, f'quiet session closed, {time.monotonic() - started:.1f} s')
    t2.close()
    time.sleep(0.5)
    console_gone = 'Press Enter to\nlog in.' in screen()

    # At most one telnet session: a second connection is not accepted.
    t.cmd('sysctl telnet_max=1')

    def try_connect():
        try:
            socket.create_connection((TARGET[0], 23), timeout=2).close()
            return True
        except OSError:
            return False
    time.sleep(0.3)
    refused = not ns.call(try_connect)
    t.cmd('sysctl telnet_max=3')
    time.sleep(0.3)
    t3 = Telnet(ns, connect_within=3)
    t3.read_until(Telnet.PROMPT)
    t3.send('exit\r\n'); t3.wait_closed(); t3.close()
    report.add('telnet_max', refused and console_gone, 'second login refused at 1; console also timed out')
    t.send('exit\r\n'); t.wait_closed(); t.close()
    time.sleep(0.5)


def review_tests(ns, serial, report, iface):
    """Regression tests for the 2026-10-16 code review (R1, R3, R5, R6)."""
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    folder = f'/home/rv-{os.getpid()}'
    t.cmd(f'mkdir {folder}')
    try:
        # R1: a background command with four redirections (plus /dev/null as its input).
        t.cmd(f'echo four > {folder}/a > {folder}/b > {folder}/c > {folder}/d &')
        t.cmd('wait', 5)
        out = t.cmd(f'cat {folder}/d') + t.cmd(f'wc {folder}/a')
        report.add('bg redirections', out.startswith('four') and ' 0 ' in out, out.strip().replace('\r\n', ' | '))

        # R3: a killed program's allocations come back.
        t2 = Telnet(ns)
        t2.read_until(Telnet.PROMPT)
        status, _ = ns.call(http_request, 'PUT', f'{folder}/text', b'line\n' * 100)
        for _ in range(20):                           # the HTTP worker may still be closing
            if ' httpd ' not in t2.cmd('ps'):
                break
            time.sleep(0.2)
        before = t2.cmd('free')
        t.send(f'ed -s {folder}/text | cat\r\n')
        time.sleep(0.8)
        ps = t2.cmd('ps')
        pid = next((line.split()[0] for line in ps.splitlines() if line.split()[1:2] == ['ed']), None)
        killed = t2.cmd(f'kill {pid}') if pid else 'no ed task'
        t.read_until(Telnet.PROMPT, 5)
        time.sleep(0.3)
        after = t2.cmd('free')
        time.sleep(1.2)                               # the kernel daemon logs it within a second
        log = t2.cmd('dmesg | grep freed')
        report.add('kill frees memory', status in (201, 204) and pid and before == after
                   and f'pid {pid}: freed 2 blocks' in log, f'{killed.strip() or "killed"}; {after.strip()}')
        t2.send('exit\r\n'); t2.wait_closed(); t2.close()

        # R5: a background job and the shell write to one telnet session at once; nothing is lost.
        status, _ = ns.call(http_request, 'PUT', f'{folder}/lines', b'0123456789abcdef\n' * 200)
        t.send(f'cat {folder}/lines &\r\n')
        t.send(f'cat {folder}/lines\r\n')
        text = t.read_until(Telnet.PROMPT, 10)            # the job starts printing at once
        text += t.read_until(Telnet.PROMPT, 10)
        t.send('wait\r\n')
        text += t.read_until(Telnet.PROMPT, 10)
        report.add('shared terminal', text.count('b') == 400 and text.count('f') == 400,
                   f"'b' {text.count('b')}, 'f' {text.count('f')} of 400 each")

        # R10: cp and mv of a file onto itself, under other names, keep it.
        t.cmd(f'echo keep me > {folder}/same')
        t.cmd('echo keep usb > /usb/r10.txt')
        out = t.cmd(f'cp {folder}/same {folder}/./same') + t.cmd('cp /usb/r10.txt /usb/R10.TXT')
        out += t.cmd(f'cd {folder}') + t.cmd(f'mv same ../{folder.split("/")[-1]}/same') + t.cmd('cd /')
        out += t.cmd('mv /usb/R10.TXT /usb/') + t.cmd('cp /usb/r10.txt /usb')
        kept = t.cmd(f'cat {folder}/same') + t.cmd('cat /usb/r10.txt')
        t.cmd('rm /usb/r10.txt')
        report.add('same-file cp/mv', out.count('are the same file') == 3 and kept == 'keep me\r\nkeep usb\r\n',
                   kept.strip().replace('\r\n', ' | '))

        # R15: nc with a pipe as input sends and receives at once: 32 KiB through an echo
        # server that stops reading while its send buffer is full. All the buffers on the
        # way (W5500 2+2 KiB, the server's small socket buffers) hold far less than that.
        def echo_server(port, total, got):
            lsock = socket.socket()
            lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            lsock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)   # inherited by the connection
            lsock.bind((PEER.split('/')[0], port))
            lsock.listen(1)
            lsock.settimeout(15)
            with lsock:
                c, _ = lsock.accept()
                with c:
                    c.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
                    c.settimeout(20)
                    while got[0] < total:
                        chunk = c.recv(512)
                        if not chunk:
                            break
                        got[0] += len(chunk)
                        c.sendall(chunk)
                    time.sleep(0.5)
        t.cmd(f'mkfile {folder}/big 32k', 10)
        got = [0]
        server = ns.background(echo_server, 7400, 32768, got)
        time.sleep(0.3)
        out = t.cmd(f'cat {folder}/big | nc 198.18.0.1 7400 > {folder}/echo', 60)
        server.join()
        sums = t.cmd(f'sum {folder}/big {folder}/echo', 10).split('\r\n')
        crc = [line.split()[:2] for line in sums if line.strip()]
        report.add('nc both ways', got[0] == 32768 and len(crc) == 2 and crc[0] == crc[1],
                   f'{got[0]} B echoed, {" / ".join(" ".join(c) for c in crc)} {out.strip()}')
    finally:
        for name in ('a', 'b', 'c', 'd', 'text', 'lines', 'same', 'big', 'echo'):
            t.cmd(f'rm {folder}/{name}')
        t.cmd(f'rmdir {folder}')

    # R6: when a DHCP lease ends without renewal, the address is removed until a new lease.
    def reachable():
        try:
            socket.create_connection((TARGET[0], 23), timeout=1).close()
            return True
        except OSError:
            return False
    stop, log = threading.Event(), []
    server = ns.background(dhcp_server, stop, iface, log, 20)
    time.sleep(0.3)
    out = t.cmd('dhcp', 25)
    stop.set(); server.join()
    t.close()
    mark = len(serial.lines)
    removed = serial.wait_for(r'dhcp: lease expired, address removed', 40, mark)
    time.sleep(0.5)
    gone = not ns.call(reachable)
    stop, log = threading.Event(), []
    server = ns.background(dhcp_server, stop, iface, log, 120)
    try:
        back = serial.wait_for(r'dhcp: lease 120 s', 45, mark)
        report.add('dhcp lease end', 'lease 20 s' in out and removed is not None and gone and back is not None,
                   'address removed at expiry, new lease 30 s later at most')
        t = Telnet(ns, connect_within=10)
        t.read_until(Telnet.PROMPT)
        t.cmd('ifconfig eth0 198.18.0.2/30')              # back to the static test address
        t.send('exit\r\n'); t.wait_closed(); t.close()
    finally:
        stop.set(); server.join()


def install_programs(ns, report):
    """The programs for /bin (build/bin, `make programs`): play, ed, grep... are
    programs, not built-in commands; later sections use them."""
    files = sorted((REPO / 'build/bin').glob('*.nxe'))
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    t.cmd('mkdir /bin')
    t.send('exit\r\n'); t.wait_closed(); t.close()
    statuses = [ns.call(http_request, 'PUT', f'/bin/{f.stem}', f.read_bytes())[0] for f in files]
    report.add('programs installed', len(files) >= 7 and all(s in (201, 204) for s in statuses),
               ' '.join(f.stem for f in files))


NXE = struct.Struct('<IHHIIIIHHIII')


def nxe_seal(f, data_part):
    """The header for fields f, data_crc (f[11]) taken over the rest of it and data_part."""
    f = list(f)
    f[11] = zlib.crc32(NXE.pack(*f)[:NXE.size - 4] + data_part)
    return NXE.pack(*f)


def nxe(text, data=b'', bss=0, stack=0, trel=(), drel=(), entry=1):
    """An NXE file (lib/nnlib.h) from its parts; the entry is the start of text."""
    t = b''.join(struct.pack('<I', r) for r in trel)
    d = b''.join(struct.pack('<I', r) for r in drel)
    f = (0x3145584e, 3, 1, len(text), len(data), bss, entry, len(trel), len(drel), stack, zlib.crc32(text + t), 0)
    return nxe_seal(f, data + d) + text + t + data + d


def nxe_text_crc(data):
    return NXE.unpack_from(data)[10]


def retag(data, tag):
    """relocs.nxe with another 8-character tag: other text, so another CRC."""
    f = list(NXE.unpack_from(data))
    text, trel = NXE.size, NXE.size + f[3]
    out = bytearray(data)
    at = out.index(b'TAG:00000000', text, trel)
    out[at + 4:at + 12] = tag.encode()
    f[10] = zlib.crc32(bytes(out[text:trel + 4 * f[7]]))
    out[:NXE.size] = nxe_seal(f, bytes(out[trel + 4 * f[7]:]))
    return bytes(out)


def padded(pages, code, mark):
    """A program whose text and slot header fill exactly `pages` APROM pages."""
    size = pages * 512 - 32
    return nxe(code + mark + bytes(size - len(code) - len(mark)), stack=768)


QUIT = struct.pack('<HH', 0x2000, 0x4770)                 # movs r0, #0; bx lr
SLEEPER = struct.pack('<HHHH', 0x2064, 0xdf02, 0xe7fc, 0)  # movs r0, #100; svc SYS_SLEEP; b .-4


def xip_area(t):
    """/proc/xip: (area bytes, slots written since boot, {crc: (address, users)})."""
    out = t.cmd('cat /proc/xip')
    size = re.search(r'area 0x[0-9a-f]+ (\d+)', out)
    written = re.search(r'written (\d+)', out)
    slots = {int(m[3], 16): (int(m[1], 16), int(m[4])) for m in re.finditer(r'0x([0-9a-f]+) (\d+) ([0-9a-f]{8}) (\d+)', out)}
    return (int(size[1]) if size else 0), (int(written[1]) if written else -1), slots


def idle_httpd(t):
    for _ in range(20):                               # an HTTP worker may still be closing
        if ' httpd ' not in t.cmd('ps'):
            break
        time.sleep(0.2)
    time.sleep(0.3)


def xip_tests(ns, serial, report):
    """Program text in APROM (kernel/xip.c): written once, run in place by every
    run, kept across a reboot, giving way oldest first but never while a
    program runs from it; with no room left, the text goes to RAM."""
    base = (REPO / 'sdk/build/relocs.nxe').read_bytes()
    tags = [f'{int(time.time()) % 100000000 + i:08d}'[-8:] for i in range(3)]
    copies = {tag: retag(base, tag) for tag in tags}
    statuses = [ns.call(http_request, 'PUT', f'/bin/r{i}', data)[0] for i, data in enumerate(copies.values())]
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    mark = len(serial.lines)
    sleepers = []
    try:
        size, written, slots = xip_area(t)
        pages = size // 512
        report.add('xip area', size >= 24 * 1024 and written >= 0, f'{size} B of APROM ({pages} pages) for programs')
        crc0 = nxe_text_crc(copies[tags[0]])
        idle_httpd(t)
        before = t.cmd('free')
        out = t.cmd('r0') + t.cmd('r0')
        _, written1, slots = xip_area(t)
        report.add('program in APROM', out.count(f'relocs {tags[0]}: ok (0)') == 2 and written1 == written + 1
                   and crc0 in slots and slots[crc0][1] == 0,
                   f'two runs, one slot written ({written} -> {written1}); text, data and bss relocations ok')
        t.cmd('r0 2 &')
        time.sleep(0.5)
        _, _, running = xip_area(t)
        out = t.cmd('r0') + t.cmd('wait', 10)
        report.add('one text, two runs', out.count(f'relocs {tags[0]}: ok (0)') == 2 and running.get(crc0, (0, 0))[1] == 1,
                   'a second run while the first sleeps: each its own data')

        # The whole area for one program: everything else gives way.
        fill = padded(pages, QUIT, b'FILL' + tags[0].encode())
        statuses.append(ns.call(http_request, 'PUT', '/bin/xfill', fill, None)[0])
        started = time.monotonic()
        out = t.cmd('xfill', 30) + t.cmd('echo $?')
        took = time.monotonic() - started
        _, _, slots = xip_area(t)
        report.add('APROM full', out.strip().endswith('0') and list(slots) == [nxe_text_crc(fill)],
                   f'a {size // 1024} KiB program written in {took:.1f} s; the other slots gave way')

        # Programs running from every page: a new one goes to RAM, and none of
        # theirs is touched. Three fill the area exactly, starting at its base
        # (the next slot wraps around after xfill).
        third = pages // 3
        parts = [third, third, pages - 2 * third]
        for i, n in enumerate(parts):
            data = padded(n, SLEEPER, b'SLEEP' + tags[0].encode() + bytes([48 + i]))
            sleepers.append(nxe_text_crc(data))
            statuses.append(ns.call(http_request, 'PUT', f'/bin/xs{i}', data)[0])
            t.cmd(f'xs{i} &', 30)
        _, w2, slots = xip_area(t)
        busy = all(slots.get(c, (0, 0))[1] == 1 for c in sleepers)
        out = t.cmd('r1') + t.cmd('echo $?')
        _, w3, after = xip_area(t)
        crc1 = nxe_text_crc(copies[tags[1]])
        report.add('no room: text in RAM', busy and f'relocs {tags[1]}: ok (0)' in out and w3 == w2 and crc1 not in after
                   and all(after.get(c, (0, 0))[1] == 1 for c in sleepers),
                   f'{len(parts)} running programs fill APROM; r1 ran from RAM, theirs untouched')
        alive = t.cmd('jobs')
        for i in range(len(parts)):
            t.cmd(f'kill %{i + 1}')
        t.cmd('wait', 10)
        out = t.cmd('r1')
        _, w4, after = xip_area(t)
        report.add('room again', alive.count('running') == len(parts) and f'relocs {tags[1]}: ok (0)' in out
                   and w4 == w3 + 1 and crc1 in after, 'after they ended, r1 got a slot')
        idle_httpd(t)
        report.add('xip memory freed', t.cmd('free') == before, 'heap as before')

        # Kept across a reboot: running r1 again writes nothing.
        t.send('reboot\r\n')
        t.wait_closed(10)
        t.close()
        serial.wait_for(r'telnetd: listening', 30, mark)
        t = Telnet(ns, connect_within=30)
        t.read_until(Telnet.PROMPT, 10)
        out = t.cmd('r1')
        _, w5, after = xip_area(t)
        boot = serial.wait_for(r'xip: APROM 0x[0-9a-f]+-0x20000 \(\d+ KiB\) for programs, \d+ kept', 1, mark)
        report.add('kept across reboot', f'relocs {tags[1]}: ok (0)' in out and w5 == 0 and crc1 in after,
                   serial.lines[boot] if boot is not None else 'no xip line in the log')
        fault = serial.wait_for(r'HardFault|PANIC|panic', 1, mark)
        report.add('xip without faults', fault is None, serial.lines[fault] if fault is not None else '')
    finally:
        for name in ['r0', 'r1', 'r2', 'xfill'] + [f'xs{i}' for i in range(len(sleepers))]:
            t.cmd(f'rm /bin/{name}')
        t.send('exit\r\n'); t.wait_closed(); t.close()
    report.add('xip uploads', all(s in (201, 204) for s in statuses), str(statuses))


def m6_tests(ns, serial, report):
    """Loadable programs: SDK-built NXE files uploaded to /bin and run by the shell."""
    sdk = REPO / 'sdk/build'
    programs = {name: (sdk / f'{name}.nxe').read_bytes() for name in ('hello', 'upper', 'primes', 'spawn', 'memtest')}
    statuses = [ns.call(http_request, 'PUT', f'/bin/{name}', data)[0] for name, data in programs.items()]
    bad = bytearray(programs['hello'])
    bad[40] ^= 0xff                                   # the text, and its CRC: no copy in APROM has it,
    bad[32] ^= 0x01                                   # and the text no longer matches it
    statuses.append(ns.call(http_request, 'PUT', '/bin/damaged', bytes(bad))[0])
    moved = bytearray(programs['hello'])
    moved[20] ^= 0x02                                 # the entry only: the text is hello's, in APROM already
    statuses.append(ns.call(http_request, 'PUT', '/bin/badentry', bytes(moved))[0])
    statuses.append(ns.call(http_request, 'PUT', '/bin/short', programs['hello'][:100])[0])
    statuses.append(ns.call(http_request, 'PUT', '/bin/text', b'echo this is not a program\n')[0])
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    try:
        for _ in range(20):                           # the HTTP worker may still be closing
            if ' httpd ' not in t.cmd('ps'):
                break
            time.sleep(0.2)
        before = t.cmd('free')
        out = t.cmd('hello one two') + t.cmd('echo $?')
        report.add('run /bin/hello', all(s in (201, 204) for s in statuses)
                   and re.match(r'hello from a loaded program, pid \d+\r\nargv\[1\] = one\r\nargv\[2\] = two\r\n2\r\n$', out)
                   is not None, out.strip().replace('\r\n', ' | '))
        out = t.cmd('/bin/hello') + t.cmd('echo shout please | upper') + t.cmd('hello x | upper | wc')
        report.add('programs in pipes', 'SHOUT PLEASE' in out and re.search(r'\b2 10 4\d\b', out) is not None, out.strip().replace('\r\n', ' | '))
        # A 3 KiB sieve fits; 900000 (56 KiB) never does.
        out = t.cmd('primes', 10) + t.cmd('primes 50000', 10) + t.cmd('primes 900000', 10)
        report.add('program library', '1229 primes up to 10000, the largest 9973' in out and 'out of memory' in out
                   and '5133 primes up to 50000, the largest 49999' in out,
                   out.strip().replace('\r\n', ' | '))
        out = t.cmd('primes 20000 &') + t.cmd('wait', 10)
        report.add('program in background', '2262 primes up to 20000' in out, out.strip().replace('\r\n', ' | '))
        t.send('upper\r\n')
        time.sleep(0.5)
        t.send('typed\r\n')
        typed = t.read_until('\r\nTYPED', 5)
        started = time.monotonic()
        t.send('\x03')
        t.read_until(Telnet.PROMPT, 5)
        report.add('ctrl-c program', time.monotonic() - started < 1.0, 'upper echoed a line, ^C ended it')
        out = t.cmd('damaged') + t.cmd('badentry') + t.cmd('short') + t.cmd('text') + t.cmd('nosuch')
        report.add('bad programs', out.count('not an executable') == 4 and 'command not found' in out,
                   out.strip().replace('\r\n', ' | '))
        # A worker spawned by a loaded program keeps the program's image: the
        # program returns at once, primes then allocates while the worker computes.
        out = t.cmd('spawn wait', 10)
        mark = len(serial.lines)
        out += t.cmd('spawn') + t.cmd('primes 20000', 10)
        time.sleep(3)
        # The worker keeps writing into the program's malloc'd buffer after the
        # program has ended: that buffer must not go to primes' sieve meanwhile.
        # memtest's blocks would take that buffer if it were freed early, and the
        # worker's 0xff writes would then show up in them.
        shared = t.cmd('spawn share') + t.cmd('memtest 3000', 10)
        out += shared
        tested = re.search(r'memtest: (\d+) blocks of 256 B, intact', shared)
        time.sleep(6)                                 # the worker ends (killed at its printf)
        alive = t.cmd('uname')
        fault = serial.wait_for(r'HardFault|PANIC', 1, mark)
        report.add('program children', 'worker done' in out and 'nnnix' in alive and fault is None
                   and '2262 primes' in out and re.match(r'worker \d+ started', shared) is not None
                   and tested is not None and int(tested[1]) > 0, out.strip().replace('\r\n', ' | '))
        out = t.cmd('cp /bin/hello /usb/hello.nxe', 10) + t.cmd('/usb/hello.nxe from usb', 10)
        t.cmd('rm /usb/hello.nxe')
        report.add('program from usb', 'argv[1] = from' in out, out.strip().replace('\r\n', ' | '))
        time.sleep(0.3)
        after = t.cmd('free')
        report.add('program memory freed', before == after, after.strip().replace('\r\n', ' | '))
    finally:
        for name in ('damaged', 'badentry', 'short', 'text'):
            t.cmd(f'rm /bin/{name}')
        t.send('exit\r\n'); t.wait_closed(); t.close()


def nnv_tests(ns, serial, report):
    """/dev/lcd and the NNV player (bin/play.c, tools/mknnv.py)."""
    sys.path.insert(0, str(REPO / 'tools'))
    import mknnv
    files = {}
    for name in ('pattern', 'bounce', 'mixed'):
        frames, frame_ms, flags = mknnv.DEMOS[name]()
        files[name] = mknnv.encode(frames, frame_ms, flags)
    # A first record whose skips run past the end of the frame (16 x 64 bytes fit).
    files['damaged'] = files['mixed'][:16] + bytes((mknnv.DELTA, 20, 0)) + bytes((0xbf,) * 20)
    files['notnnv'] = b'just some text\n'
    # The tone demo's pictures with silent sound: samples of 0 keep the buzzer
    # off (PWM output high), so the sound path is tested without a noise.
    frames, frame_ms, flags, audio = mknnv.DEMOS['tone']()
    files['tone'] = mknnv.encode(frames, frame_ms, flags, [-32768] * len(audio))
    # R25: cut after the first whole frame record, and after the header.
    first = 16 + 3 + struct.unpack_from('<BH', files['mixed'], 16)[1]
    files['cut'] = files['mixed'][:first]
    files['headonly'] = files['mixed'][:16]
    # R26: two frames of a second each; the last must stay up its second too.
    blank, full = bytes(mknnv.FRAME), b'\xff' * mknnv.FRAME
    files['two'] = mknnv.encode([full, blank], 1000)
    statuses = [ns.call(http_request, 'PUT', '/bin/audioshare', (REPO / 'sdk/build/audioshare.nxe').read_bytes())[0]]
    statuses += [ns.call(http_request, 'PUT', f'/home/{n}.nnv', d)[0] for n, d in files.items()]
    # 2 s of raw sound, all 0: the buzzer stays off.
    statuses.append(ns.call(http_request, 'PUT', '/home/quiet.u8', bytes(16000))[0])
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    mark = len(serial.lines)
    try:
        for _ in range(20):
            if ' httpd ' not in t.cmd('ps'):
                break
            time.sleep(0.2)
        before = t.cmd('free')
        started = time.monotonic()
        out = t.cmd('play /home/mixed.nnv', 15) + t.cmd('echo $?')
        took = time.monotonic() - started
        # 60 frames of 40 ms, a sixth of them raw (1 KiB each from the flash).
        report.add('play video', all(s in (201, 204) for s in statuses) and out.strip() == '0' and 2.3 < took < 3.5,
                   f'{took:.2f} s for 60 frames at 40 ms; ' + out.strip().replace('\r\n', ' | '))
        # Streamed: nc -l | play, the file sent over TCP as fast as the board takes it.
        def stream(data, port=1234):
            for _ in range(50):
                try:
                    c = socket.create_connection((TARGET[0], port), timeout=5)
                    break
                except OSError:
                    time.sleep(0.2)
            else:
                return False
            with c:
                c.settimeout(30)
                c.sendall(data)
                c.shutdown(socket.SHUT_WR)
                time.sleep(0.5)
            return True
        out = ''
        for name, flags in (('mixed', '-v'), ('tone', '-s -v'), ('cut', '')):
            t.send(f'nc -l 1234 | play {flags}\r\n')
            time.sleep(0.5)
            sent = ns.call(stream, files[name])
            out += t.read_until(Telnet.PROMPT, 15) + t.cmd('echo $?')
        report.add('play from nc', re.search(r'play: 60 frames in 2\.\d+ s, 0 late', out) is not None
                   and out.count(' 0 late') == 2 and 'standard input: cut short' in out and sent,
                   out.strip().replace('\r\n', ' | '))

        # One writer at a time; the busy device is free again once its holder ends.
        # The holder is a background `sleep` with its output on the LCD (two
        # players at once would not fit in RAM with this shell logged in).
        t.cmd('sleep 20 > /dev/lcd &')
        time.sleep(0.5)
        busy = t.cmd('play /home/pattern.nnv') + t.cmd('echo x > /dev/lcd') + t.cmd('cat /dev/lcd') + t.cmd('ls /dev')
        t.cmd('kill %1')
        time.sleep(0.5)
        after_kill = t.cmd('jobs') + t.cmd('echo x > /dev/lcd') + t.cmd('echo $?')
        report.add('lcd one writer', 'play: /dev/lcd: busy' in busy and busy.count('busy') == 2
                   and 'lcd' in busy and after_kill.strip().endswith('0') and 'running' not in after_kill,
                   (busy + after_kill).strip().replace('\r\n', ' | '))
        # A picture waits for Enter; ^C ends a looping video at once.
        t.send('play /home/pattern.nnv\r\n')
        time.sleep(1)
        t.send('\r\n')
        pic = t.read_until(Telnet.PROMPT, 5)
        t.send('play -l /home/bounce.nnv\r\n')
        time.sleep(1.5)
        started = time.monotonic()
        t.send('\x03')
        t.read_until(Telnet.PROMPT, 5)
        stopped = time.monotonic() - started
        out = t.cmd('echo x > /dev/lcd') + t.cmd('echo $?')
        report.add('picture and ctrl-c', stopped < 1.0 and out.strip().endswith('0') and 'play' not in pic.split('\r\n', 1)[-1],
                   f'^C took {stopped:.2f} s; ' + out.strip().replace('\r\n', ' | '))
        # Sound: 16000 samples take 2 s at 8 kHz; one writer; the close plays out the buffer.
        mark = len(serial.lines)
        started = time.monotonic()
        out = t.cmd('cat /home/quiet.u8 > /dev/audio', 10) + t.cmd('echo $?')
        took = time.monotonic() - started
        played = serial.wait_for(r'audio: 16000 samples played, 0 short', 2, mark)
        t.cmd('sleep 20 > /dev/audio &')
        time.sleep(0.5)
        busy = t.cmd('echo x > /dev/audio') + t.cmd('echo x > /dev/adpcm')   # one writer of the two
        t.cmd('kill %1')
        time.sleep(0.5)
        free = t.cmd('cat /dev/null > /dev/audio') + t.cmd('echo $?')
        # /dev/adpcm takes only NNV sound records: raw samples fail the write.
        notnnv = t.cmd('cat /home/quiet.u8 > /dev/adpcm') + t.cmd('echo $?')
        report.add('audio device', out.strip() == '0' and 1.95 < took < 2.6 and played is not None
                   and busy.count('busy') == 2 and free.strip().endswith('0') and notnnv.strip().endswith('1'),
                   f'2 s of samples in {took:.2f} s; ' + (busy + free + notnnv).strip().replace('\r\n', ' | '))
        # A video with sound: silent unless -s; with it, frames on time, sound
        # without gaps; ^C silences it at once.
        mark = len(serial.lines)
        out = t.cmd('play /home/tone.nnv', 15)
        silent = serial.wait_for(r'audio: [1-9]', 1, mark) is None   # no sound played (a late 0-sample close may show)
        mark = len(serial.lines)
        out += t.cmd('play -s -v /home/tone.nnv', 15)
        gapless = serial.wait_for(r'audio: 32000 samples played, 0 short', 2, mark)
        t.send('play -l -s /home/tone.nnv\r\n')
        time.sleep(1.5)
        started = time.monotonic()
        t.send('\x03')
        t.read_until(Telnet.PROMPT, 5)
        stopped = time.monotonic() - started
        free = t.cmd('cat /dev/null > /dev/audio') + t.cmd('echo $?')
        report.add('play with sound', re.search(r'120 frames in (3\.9[5-9]|4\.0[0-5])\d s, 0 late', out) is not None
                   and silent and gapless is not None and stopped < 1.0 and free.strip().endswith('0'),
                   f'^C took {stopped:.2f} s; ' + (out + free).strip().replace('\r\n', ' | '))
        # R24: two tasks writing to one open /dev/audio at once lose and double nothing.
        mark = len(serial.lines)
        out = t.cmd('audioshare', 15)
        shared = serial.wait_for(r'audio: 16000 samples played', 2, mark)
        crash = serial.wait_for(r'HardFault|PANIC|panic', 0.5, mark)
        report.add('audio shared file', '16000 samples written' in out and shared is not None and crash is None,
                   out.strip().replace('\r\n', ' | ') + (f' | {serial.lines[shared]}' if shared is not None else ''))
        # R25: fewer frames than the header says is an error, also when looping.
        out = (t.cmd('play /home/cut.nnv') + t.cmd('echo $?') + t.cmd('play -l /home/cut.nnv', 5)
               + t.cmd('play /home/headonly.nnv'))
        report.add('play cut short', out.count('cut short: 1 of 60 frames') == 2 and 'cut short: 0 of 60 frames' in out
                   and '\r\n1\r\n' in '\r\n' + out, out.strip().replace('\r\n', ' | '))
        # R26: the last frame gets its whole time before the text comes back.
        started = time.monotonic()
        out = t.cmd('play -v /home/two.nnv', 10)
        took = time.monotonic() - started
        m = re.search(r'2 frames in (\d+\.\d+) s', out)
        report.add('play last frame', m is not None and 1.95 <= float(m[1]) < 2.15 and took >= 1.95,
                   f'{took:.2f} s; ' + out.strip().replace('\r\n', ' | '))
        out = (t.cmd('play /home/damaged.nnv') + t.cmd('echo $?') + t.cmd('play /home/notnnv.nnv')
               + t.cmd('play /home/nosuch.nnv') + t.cmd('play'))
        report.add('play bad files', 'damaged after 0 frames' in out and 'not an NNV file' in out
                   and 'nosuch.nnv: no such file' in out and 'usage: play' in out,
                   out.strip().replace('\r\n', ' | '))
        time.sleep(0.3)
        after = t.cmd('free')
        fault = serial.wait_for(r'HardFault|PANIC', 0.5, mark)
        report.add('play memory freed', before == after and fault is None, after.strip().replace('\r\n', ' | '))
    finally:
        for name in ('damaged', 'notnnv', 'cut', 'headonly', 'two'):
            t.cmd(f'rm /home/{name}.nnv')
        t.cmd('rm /home/quiet.u8')
        t.send('exit\r\n'); t.wait_closed(); t.close()


def audit_tests(ns, serial, report):
    """Fixes from the full audit of 2026-10-17 (R27-R35)."""
    statuses = [ns.call(http_request, 'PUT', '/bin/readzero', (REPO / 'sdk/build/readzero.nxe').read_bytes())[0],
                # R32: a last line without '\n', shorter than the line before it.
                ns.call(http_request, 'PUT', '/home/edscript', b'f /home/edlongfilename\nw /home/edx')[0],
                ns.call(http_request, 'PUT', '/home/r34.bin', pattern(32768))[0]]
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    try:
        # R27: mv into a directory on the same file system.
        out = (t.cmd('mkdir /home/mvd') + t.cmd('echo moved > /home/mva') + t.cmd('mv /home/mva /home/mvd')
               + t.cmd('echo $?') + t.cmd('cat /home/mvd/mva') + t.cmd('cat /home/mva'))
        report.add('mv into a directory', 'moved' in out and 'no such file' in out and '\r\n0\r\n' in '\r\n' + out,
                   out.strip().replace('\r\n', ' | '))
        # R28: a PID beyond 16 bits never reaches another task.
        bg = t.cmd('sleep 5 &')
        m = re.search(r'\[\d+\] (\d+)', bg)
        pid = int(m[1]) if m else 0
        out = t.cmd(f'kill {pid + 65536}') + t.cmd('kill 99999999999') + t.cmd('jobs')
        t.cmd('kill %1')
        report.add('kill out of range', pid > 0 and 'no such task' in out and 'bad argument' in out
                   and 'running  sleep' in out, out.strip().replace('\r\n', ' | '))
        # R31: reads of 0 bytes return at once.
        out = t.cmd('readzero', 5)
        report.add('read of 0 bytes', re.search(r'readzero: ok in \d+ ms', out) is not None, out.strip())
        # R32: ed's last line without '\n' is the command as written.
        out = t.cmd('ed < /home/edscript', 10) + t.cmd('ls /home')
        report.add('ed last line', ' edx ' in out.replace('\r\n', ' ') and 'edxongfilename' not in out,
                   ' '.join(l.split()[0] for l in out.strip().split('\r\n') if l.startswith('ed')))
        # R34: flash dumps through diag while files are copied: the copies stay intact.
        stop = threading.Event()
        dumps = []

        def dumper():
            while not stop.is_set():
                data, _ = request(b'M', 4, len(dumps) % 4, b'', None, 30)
                dumps.append(len(data))
        thread = ns.background(dumper)
        sums = [t.cmd('sum /home/r34.bin')]
        for i in range(3):
            sums.append(t.cmd(f'cp /home/r34.bin /home/r34c.bin', 20) + t.cmd('sum /home/r34c.bin'))
        stop.set()
        thread.join(30)
        first = sums[0].split()[0] if sums[0].split() else '?'
        report.add('flash dump during writes', all(first in x for x in sums[1:]) and len(dumps) >= 2
                   and all(n == 65536 for n in dumps), f'{len(dumps)} dumps of 64 KiB, sum {first} kept')
        # R35: the CH376 bench while a /usb file is open with unwritten data.
        t.send('cat > /usb/r35.txt\r\n')
        time.sleep(0.5)
        t.send('hello\r\n')
        time.sleep(1)
        bench, _ = ns.call(request, b'U', 4, 16384, b'', None, 60)
        t.send('world\r\n')
        time.sleep(0.5)
        t.send('\x04')
        t.read_until(Telnet.PROMPT, 10)
        out = t.cmd('cat /usb/r35.txt', 10)
        report.add('usb file across the bench', out.strip().split('\r\n') == ['hello', 'world'] and b'OK' in bench,
                   out.strip().replace('\r\n', ' | ') + ' / ' + bench.decode(errors='replace').strip())
    finally:
        for f in ('/home/mvd/mva', '/home/mvd', '/home/edscript', '/home/edx', '/home/r34.bin', '/home/r34c.bin',
                  '/usb/r35.txt'):
            t.cmd(f'rm {f}' if f != '/home/mvd' else f'rmdir {f}')
        t.send('exit\r\n'); t.wait_closed(); t.close()
    report.add('audit uploads', all(s in (201, 204) for s in statuses), str(statuses))


def efficiency_tests(ns, serial, report):
    """O7/O8 and the exit stack: work that waits sleeps, interrupts are held off
    only briefly, and a program killed deep in a system call exits cleanly."""
    status = ns.call(http_request, 'PUT', '/home/eff.bin', pattern(196608))[0]   # a copy that lasts a few seconds
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    try:
        def fsd_cpu():
            m = re.search(r'\d+ fsd\s+\d+ \S+\s+\d+/\d+\s+(\d+)', t.cmd('ps'))
            return int(m[1]) if m else -1
        before = t.cmd('free')
        # O7: a USB copy while a video plays: fsd sleeps while the CH376 works.
        c0, t0 = fsd_cpu(), time.monotonic()
        t.cmd('cp /home/eff.bin /usb/eff.bin &')
        out = t.cmd('play -v /home/tone.nnv', 30)
        while 'running' in t.cmd('jobs'):
            time.sleep(0.2)
        took, cpu = time.monotonic() - t0, fsd_cpu() - c0
        m = re.search(r'120 frames in (\d+)\.\d+ s, (\d+) late', out)
        report.add('play during a usb copy', status in (201, 204) and m is not None and int(m[1]) <= 4 and int(m[2]) <= 30 and cpu < took * 1000 * 0.6,
                   f'{out.strip()}; fsd {cpu} ms of cpu in {took:.1f} s')
        # The exit runs on a fresh stack: a program killed while deep in its
        # calls (here a player with sound during a copy) ends cleanly.
        mark = len(serial.lines)
        t.cmd('cp /home/eff.bin /usb/eff.bin &')
        t.cmd('play -l -s /home/tone.nnv &')
        time.sleep(3)
        t.cmd('kill %2')
        while 'running' in t.cmd('jobs'):
            time.sleep(0.2)
        crash = serial.wait_for(r'PANIC|HardFault|stack overflow', 1, mark)
        # O8: the audio interrupt's latency while ps and /proc scan the task stacks.
        t.cmd('play -l -s /home/tone.nnv &')
        time.sleep(0.5)
        for _ in range(3):
            t.cmd('ps'); t.cmd('cat /proc/tasks'); t.cmd('cat /proc/stacks')
        mark2 = len(serial.lines)
        t.cmd('kill %1')
        t.cmd('wait', 10)
        i = serial.wait_for(r'audio: \d+ samples played, \d+ short, latency up to \d+ us', 3, mark2)
        lat = int(re.search(r'latency up to (\d+) us', serial.lines[i])[1]) if i is not None else -1
        t.cmd('rm /usb/eff.bin'); t.cmd('rm /home/eff.bin')
        time.sleep(0.3)
        after = t.cmd('free')
        report.add('killed program exits cleanly', crash is None and before == after, after.strip().replace('\r\n', ' | '))
        report.add('interrupt latency', 0 <= lat <= 40, f'audio interrupt held off at most {lat} us during ps and /proc')
    finally:
        t.send('exit\r\n'); t.wait_closed(); t.close()


def m7_tests(ns, serial, report, image):
    """Firmware update through the LDROM bootloader (CONFIG0 set to start in LDROM)."""
    # The running build plus a word after its end: never executed, but a CRC of
    # its own, so the bootloader really installs it (an image identical to APROM
    # is only marked installed).
    data = image.read_bytes()
    data += b'\xff' * (-len(data) % 4) + struct.pack('<I', int(time.time()))
    padded = data
    crc = f'{zlib.crc32(padded) & 0xffffffff:08x}'
    statuses = [ns.call(http_request, 'PUT', '/home/nnnix.bin', data)[0],
                ns.call(http_request, 'PUT', '/home/notfw.bin', b'this is not a firmware image\n' * 4)[0]]
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    info = t.cmd('fwupdate')
    report.add('fwupdate status', 'bootloader: in LDROM' in info and 'power-on boot: LDROM' in info
               and all(s in (201, 204) for s in statuses), info.strip().replace('\r\n', ' | '))
    # One update operation at a time: a cancel and a second staging during a staging are refused.
    t2 = Telnet(ns)
    t2.read_until(Telnet.PROMPT)
    t.send('fwupdate -n /home/nnnix.bin\r\n')
    time.sleep(1)
    busy = t2.cmd('fwupdate -c') + t2.cmd('fwupdate -n /home/nnnix.bin')
    staged = t.read_until(Telnet.PROMPT, 30)
    cancelled = t2.cmd('fwupdate -c') + t2.cmd('fwupdate')
    t2.send('exit\r\n'); t2.wait_closed(); t2.close()
    report.add('fwupdate one at a time', busy.count('another update operation is running') == 2 and 'staged; it is installed' in staged
               and 'nothing staged' in cancelled and 'update area: empty' in cancelled, busy.strip().replace('\r\n', ' | '))
    out = t.cmd('fwupdate -n /home/notfw.bin', 20) + t.cmd('fwupdate -n /home/missing.bin')
    report.add('fwupdate refuses', 'not an APROM image' in out and 'no such file' in out, out.strip().replace('\r\n', ' | '))

    def update():
        t = Telnet(ns)
        t.read_until(Telnet.PROMPT)
        mark = len(serial.lines)
        started = time.monotonic()
        t.send('fwupdate /home/nnnix.bin\r\n')
        t.read_until('restarting through the bootloader', 30)
        t.close()
        installing = serial.wait_for(r'boot: installing update', 60, mark)
        done = serial.wait_for(r'boot: APROM written and verified', 60, mark)
        up = serial.wait_for(r'telnetd: listening', 30, done or mark)
        return None not in (installing, done, up), time.monotonic() - started

    ok, took = update()
    t = Telnet(ns, connect_within=20)
    t.read_until(Telnet.PROMPT)
    info = t.cmd('fwupdate')
    report.add('fwupdate install', ok and f'installed, {len(padded)} bytes, crc {crc}' in info,
               f'staged, installed by the bootloader, back in {took:.0f} s')

    # Interrupted: stage only, then (connecting halts the chip at the bootloader's
    # entry) let the bootloader work for 1 s and reset it in the middle.
    status = ns.call(http_request, 'PUT', '/home/nnnix.bin', data[:-4] + struct.pack('<I', int(time.time()) + 1))[0]
    out = t.cmd('fwupdate -n /home/nnnix.bin', 30)
    t.send('exit\r\n'); t.wait_closed(); t.close()
    mark = len(serial.lines)
    flasher.run_openocd(['resume', 'sleep 1000', 'reset run', 'shutdown'])
    first = serial.wait_for(r'boot: installing update', 30, mark)
    again = serial.wait_for(r'boot: installing update', 30, first + 1) if first is not None else None
    finished = serial.lines[mark:first + 1 if again is None else again] if first is not None else []
    done = serial.wait_for(r'boot: APROM written and verified', 60, again or mark)
    up = serial.wait_for(r'telnetd: listening', 30, done or mark)
    cut = not any('written and verified' in line for line in finished)
    report.add('fwupdate interrupted', status in (201, 204) and 'staged' in out
               and None not in (first, again, done, up) and cut,
               f'upload {status}; reset 1 s into the installation: started again and finished')

    t = Telnet(ns, connect_within=20)
    t.read_until(Telnet.PROMPT)
    mark = len(serial.lines)
    t.send('reboot -p\r\n')
    t.wait_closed(10)
    t.close()
    through = serial.wait_for(r'boot: starting APROM', 20, mark)
    cause = serial.wait_for(r'reset cause .*power-on', 10, through or mark)
    serial.wait_for(r'telnetd: listening', 30, cause or mark)
    report.add('power-on path', None not in (through, cause), 'chip reset: bootloader, then APROM')
    t = Telnet(ns, connect_within=20)
    t.read_until(Telnet.PROMPT)
    t.cmd('rm /home/nnnix.bin'); t.cmd('rm /home/notfw.bin')
    t.send('exit\r\n'); t.wait_closed(); t.close()


# ---- M5c: clock, editor and network tools ----

NTP_TIME = 1893553445                 # 2030-01-02 03:04:05 UTC


def ntp_server(stop, served):
    """Answers SNTP requests with NTP_TIME, after two forged answers a year
    later: one from another port, one not naming the request."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind((PEER.split('/')[0], 123))
    u.settimeout(0.2)
    other = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    other.bind((PEER.split('/')[0], 0))
    with u, other:
        while not stop.is_set():
            try:
                d, a = u.recvfrom(100)
            except socket.timeout:
                continue

            def answer(when, originate):
                r = bytearray(48)
                r[0], r[1] = 0x24, 2                   # version 4, server, stratum 2
                struct.pack_into('>II', r, 40, when + 2208988800, 0)
                r[24:32] = originate
                return bytes(r)
            later = NTP_TIME + 365 * 86400
            other.sendto(answer(later, d[40:48]), a)
            u.sendto(answer(later, bytes(8)), a)
            time.sleep(0.1)
            u.sendto(answer(NTP_TIME, d[40:48]), a)
            served.append(a)


WGET_BIG = bytes((i * 7 + i // 256) & 0xff for i in range(100000))


def web_server(stop):
    """HTTP on the test peer, port 8000: plain, chunked, redirects (absolute and
    relative), no length, malformed lengths and chunks, 404."""
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def reply(self, code, body, extra=()):
            self.send_response(code)
            for k, v in extra:
                self.send_header(k, v)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def raw(self, head, body):
            """A reply written as it is (also the malformed ones), then the close."""
            self.wfile.write(b'HTTP/1.1 200 OK\r\n' + head + b'Connection: close\r\n\r\n' + body)
            self.close_connection = True

        def do_GET(self):
            p = self.path
            relative = {'/rel': 'small.txt', '/dir/up': '../small.txt', '/dir/dots': './../dir/../small.txt',
                        '/q': '?x=1', '/scheme': '//web.test:8000/small.txt'}
            broken = {'/badlen': b'Content-Length: 12abc\r\n', '/twolen': b'Content-Length: 5\r\nContent-Length: 6\r\n',
                      '/gzip': b'Transfer-Encoding: gzip, chunked\r\n', '/hugelen': b'Content-Length: 99999999999\r\n'}
            if p in relative:
                self.reply(302, b'', [('Location', relative[p])])
            elif p == '/q?x=1':
                self.reply(200, b'query kept\n')
            elif p in broken:
                self.raw(broken[p], b'hello')
            elif p == '/badchunk':
                self.raw(b'Transfer-Encoding: chunked\r\n', b'5\r\nhello\r\nzz\r\nmore\r\n0\r\n\r\n')
            elif p == '/nosep':
                self.raw(b'Transfer-Encoding: chunked\r\n', b'5\r\nhelloXX6\r\n world\r\n0\r\n\r\n')
            elif p == '/chunklen':
                self.raw(b'Transfer-Encoding: chunked\r\nContent-Length: 3\r\n', b'6\r\nchunks\r\n4\r\n win\r\n0\r\n\r\n')
            elif p == '/small.txt':
                self.reply(200, b'hello from the peer\n')
            elif p == '/big.bin':
                self.reply(200, WGET_BIG)
            elif p == '/redirect':
                self.reply(302, b'', [('Location', '/small.txt')])
            elif p == '/abs':
                self.reply(301, b'', [('Location', 'http://web.test:8000/small.txt')])
            elif p == '/loop':
                self.reply(302, b'', [('Location', '/loop')])
            elif p == '/chunked':
                self.send_response(200)
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                for part in (b'chunk one\n', b'x' * 300 + b'\n'):
                    self.wfile.write(b'%x;ext=1\r\n%s\r\n' % (len(part), part))
                self.wfile.write(b'0\r\n\r\n')
            elif p == '/noclen':
                self.send_response(200)
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(b'no length, ends at close\n')
                self.close_connection = True
            else:
                self.reply(404, b'not here\n')

        def log_message(self, *args):
            pass

    class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        allow_reuse_address = True
    server = Server((PEER.split('/')[0], 8000), Handler)
    server.timeout = 0.2
    with server:
        while not stop.is_set():
            server.handle_request()


def dns_server(stop, asked):
    """A names for web.test, alias.test (a CNAME to web.test), forged.test (after
    a forged answer from another port); NXDOMAIN otherwise."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind((PEER.split('/')[0], 53))
    u.settimeout(0.2)
    other = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    other.bind((PEER.split('/')[0], 0))
    peer = bytes(int(x) for x in PEER.split('/')[0].split('.'))
    with u, other:
        while not stop.is_set():
            try:
                q, a = u.recvfrom(512)
            except socket.timeout:
                continue
            i, labels = 12, []
            while q[i]:
                labels.append(q[i + 1:i + 1 + q[i]].decode().lower())
                i += q[i] + 1
            name, question = '.'.join(labels), q[12:i + 5]
            asked.append(name)
            answers, rcode = b'', 3
            a_record = struct.pack('>HHIH', 1, 1, 60, 4) + peer
            if name == 'forged.test':
                head = struct.pack('>HHHHHH', q[0] << 8 | q[1], 0x8180, 1, 1, 0, 0) + question
                other.sendto(head + b'\xc0\x0c' + struct.pack('>HHIH', 1, 1, 60, 4) + bytes((10, 9, 9, 9)), a)
                time.sleep(0.1)
            if name in ('web.test', 'forged.test'):
                answers, rcode, count = b'\xc0\x0c' + a_record, 0, 1
            elif name == 'alias.test':
                target = b'\x03web\x04test\x00'
                answers = (b'\xc0\x0c' + struct.pack('>HHIH', 5, 1, 60, len(target)) + target
                           + bytes((0xc0, 12 + len(question) + 12)) + a_record)
                rcode, count = 0, 2
            else:
                count = 0
            u.sendto(struct.pack('>HHHHHH', q[0] << 8 | q[1], 0x8180 | rcode, 1, count, 0, 0) + question + answers, a)


def ch9350_keys(*keys, mods=0):
    """A CH9350 keyboard frame: a boot report with these key usages held."""
    return bytes([0x57, 0xab, 0x01, mods, 0] + list(keys) + [0] * (6 - len(keys)))


def input_tests(ns, serial, report):
    """/dev/input: keyboard and mouse frames fed through the CH9350 parser (diag
    J), the board keypad and SW_INT pressed by diag; one holder at a time; the
    SW_INT button ends a program that never makes a system call, and a game
    holding /dev/lcd too; the games of bin/ start and end."""
    def feed(data):
        return ns.call(request, b'J', 4, len(data), data)[0].decode(errors='replace')

    def board(mask, hold=0.15):
        ns.call(request, b'J', 4, 0x10000 | mask)
        time.sleep(hold)

    statuses = [ns.call(http_request, 'PUT', '/bin/keys', (REPO / 'sdk/build/keys.nxe').read_bytes())[0]]
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    mark = len(serial.lines)
    try:
        idle_httpd(t)
        before = t.cmd('free')
        t.send('keys 9\r\n')
        time.sleep(0.8)
        fed = [feed(f) for f in (ch9350_keys(0x04), ch9350_keys(), ch9350_keys(0x04, mods=0x02), ch9350_keys(),
                                 bytes([0x57, 0xab, 0x02, 1, 5, 0xfd, 0]))]
        board(0x010)
        board(0x110)
        board(0)
        out = t.read_until(Telnet.PROMPT, 5)
        report.add('input events', all('fed' in f for f in fed) and
                   re.search(r"key 0x04 down 'a'\r\nkey 0x04 up \r\nkey 0xe1 down \r\nkey 0x04 down 'A'\r\n"
                             r"key 0xe1 up \r\nkey 0x04 up \r\nmouse buttons 1 dx 5 dy -3 wheel 0\r\n"
                             r"pad 5 down\r\npad 9 down\r\n", out) is not None,
                   out.strip().replace('\r\n', ' | '))
        # remote: a live stream (a header with no time per frame) shown, and the
        # events fed through the CH9350 parser and the keypad sent back on the
        # same connection, as for tools/nnscreen.py | nc | nninput.py.
        def remote():
            for _ in range(50):
                try:
                    c = socket.create_connection((TARGET[0], 1234), timeout=5)
                    break
                except OSError:
                    time.sleep(0.2)
            else:
                return b''
            got = b''
            with c:
                c.settimeout(5)
                frame = struct.pack('<BHBB', 1, 2, 0xff, 0xaa)       # a delta: 64 bytes of 0xaa
                c.sendall(b'NNV1' + struct.pack('<HHHHBBH', 128, 64, 0, 0, 0, 0, 0) + frame + frame)
                for f in (ch9350_keys(0x04), ch9350_keys(), bytes([0x57, 0xab, 0x02, 1, 5, 0xfd, 0]),
                          bytes([0x57, 0xab, 0x02, 0, 0, 0, 0])):
                    request(b'J', 4, len(f), f)
                request(b'J', 4, 0x10000 | 0x002)                    # keypad 2
                time.sleep(0.15)
                request(b'J', 4, 0x10000)
                limit = time.monotonic() + 5
                while len(got) < 48 and time.monotonic() < limit:
                    try:
                        d = c.recv(64)
                    except socket.timeout:
                        break
                    if not d:
                        break
                    got += d
                c.shutdown(socket.SHUT_WR)
                time.sleep(0.5)
            return got

        t.send('remote -v\r\n')
        time.sleep(0.8)
        got = ns.call(remote)
        out = t.read_until(Telnet.PROMPT, 10)
        events = [struct.unpack_from('<BBBbbb', got, i) for i in range(0, len(got) - 7, 8)]
        report.add('remote', events == [(1, 0x04, 1, ord('a'), 0, 0), (1, 0x04, 0, 0, 0, 0), (2, 1, 0, 0, 5, -3),
                                        (2, 0, 0, 0, 0, 0), (3, 2, 1, 0, 0, 0), (3, 2, 0, 0, 0, 0)]
                   and 'remote: 2 frames, 26 bytes in' in out and '6 events sent' in out,
                   f'events {events}; ' + out.strip().replace('\r\n', ' | '))
        t.cmd('keys spin &')
        time.sleep(0.8)
        busy = t.cmd('keys 1')
        board(0x200, 0.2)
        board(0, 0.6)
        jobs = t.cmd('jobs') + t.cmd('ps')
        line = serial.wait_for(r'input: SW_INT: ending pid \d+ \(/dev/input\)', 2, mark)
        report.add('input SW_INT', 'busy' in busy and 'done' in jobs and ' keys ' not in jobs and line is not None,
                   'a second reader gets EBUSY; SW_INT ended `keys spin` (no system calls) '
                   + (serial.lines[line] if line is not None else 'no log line'))
        out = ''
        for game in ('snake', 'tetris', 'flappy'):            # bin/: started, played a moment, Esc
            t.send(f'{game}\r\n')
            time.sleep(1.5)
            board(0x080)
            board(0x020)
            board(0)
            feed(ch9350_keys(0x29))
            feed(ch9350_keys())
            out += t.read_until(Telnet.PROMPT, 5)
        t.send('snake\r\n')
        time.sleep(1.0)
        board(0x200, 0.2)
        board(0, 0.5)
        out2 = t.read_until(Telnet.PROMPT, 5)
        idle_httpd(t)
        after = t.cmd('free')
        fault = serial.wait_for(r'PANIC|HardFault|stack overflow', 1, mark)
        report.add('input games', all(f'{g}: best ' in out for g in ('snake', 'tetris', 'flappy'))
                   and 'snake: best' not in out2 and before == after and fault is None
                   and all(s in (201, 204) for s in statuses),
                   'snake, tetris and flappy end with Esc; SW_INT ends snake while it holds /dev/lcd and '
                   '/dev/input; heap as before')
    finally:
        t.cmd('rm /bin/keys')
        t.send('exit\r\n'); t.close()


def wget_tests(ns, serial, report):
    """wget and host: HTTP and DNS against servers on the test peer."""
    stop, asked = threading.Event(), []
    servers = [ns.background(web_server, stop), ns.background(dns_server, stop, asked)]
    time.sleep(0.5)
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    mark = len(serial.lines)
    try:
        m = re.search(r'dns (\d+\.\d+\.\d+\.\d+)', t.cmd('ifconfig'))
        old_dns = m[1] if m else '0.0.0.0'
        t.cmd('ifconfig eth0 198.18.0.2/30 dns 0.0.0.0')
        before = t.cmd('free')
        none = t.cmd('host web.test')
        t.cmd('ifconfig eth0 198.18.0.2/30 dns 198.18.0.1')
        out = (t.cmd('host web.test', 10) + t.cmd('host alias.test', 10) + t.cmd('host nosuch.test', 10)
               + t.cmd('host forged.test', 10))
        report.add('host', 'no DNS server' in none and 'web.test has address 198.18.0.1' in out
                   and 'alias.test has address 198.18.0.1' in out and 'nosuch.test: not found' in out
                   and 'forged.test has address 198.18.0.1' in out,
                   out.strip().replace('\r\n', ' | '))
        out = t.cmd('wget -O /home/w.txt http://web.test:8000/small.txt', 20) + t.cmd('cat /home/w.txt')
        report.add('wget', 'HTTP/1.1 200 OK' in out and 'saved /home/w.txt: 20 bytes' in out
                   and out.endswith('hello from the peer\r\n'), out.strip().replace('\r\n', ' | '))
        started = time.monotonic()
        out = t.cmd('wget -q -O /home/w.bin http://alias.test:8000/big.bin', 60) + t.cmd('sum /home/w.bin', 20)
        took = time.monotonic() - started
        report.add('wget 100 KB', f'{zlib.crc32(WGET_BIG):08x} 100000' in out, f'{took:.1f} s with the sum; ' + out.strip())
        out = (t.cmd('wget -O - http://web.test:8000/redirect', 20) + t.cmd('wget -O - http://web.test:8000/abs', 20)
               + t.cmd('wget -O - http://web.test:8000/chunked | wc', 20) + t.cmd('wget -O - http://198.18.0.1:8000/noclen', 20))
        report.add('wget replies', out.count('hello from the peer') == 2 and re.search(r'\b2 3 311\b', out) is not None
                   and 'no length, ends at close' in out, out.strip().replace('\r\n', ' | '))
        out = ''.join(t.cmd(f'wget -O - http://web.test:8000/{p}', 20) for p in ('rel', 'dir/up', 'dir/./dots', 'q', 'scheme'))
        out += t.cmd('wget -O - http://web.test:8000/chunklen', 20)
        report.add('wget relative redirects', out.count('hello from the peer') == 4 and 'query kept' in out
                   and 'chunks win' in out, out.strip().replace('\r\n', ' | '))
        out = ''.join(t.cmd(f'wget -O - http://web.test:8000/{p}', 20) + t.cmd('echo $?')
                      for p in ('badlen', 'twolen', 'gzip', 'hugelen', 'badchunk', 'nosep'))
        report.add('wget malformed replies', out.count('bad Content-Length') == 2 and 'two different Content-Lengths' in out
                   and 'a Transfer-Encoding other than chunked' in out and out.count('a malformed chunked body') == 2
                   and out.count('\r\n1\r\n') == 6, out.strip().replace('\r\n', ' | '))
        out = (t.cmd('wget -O - http://web.test:8000/missing', 20) + t.cmd('echo $?') + t.cmd('wget -q http://web.test:8000/loop', 30)
               + t.cmd('wget http://nosuch.test/', 20) + t.cmd('wget http://198.18.0.1:8001/', 30)
               + t.cmd('wget https://web.test/') + t.cmd('wget') + t.cmd('wget http://192.18.0.1/', 10))
        report.add('wget errors', 'answered 404\r\n1' in out and 'too many or bad redirects' in out and 'no such host' in out
                   and 'refused or timed out' in out and 'https is not supported' in out and 'usage: wget' in out
                   and 'not on the local network, and no gateway is set' in out,
                   out.strip().replace('\r\n', ' | '))
        t.cmd('rm /home/w.txt /home/w.bin')
        t.cmd(f'ifconfig eth0 198.18.0.2/30 dns {old_dns}')
        time.sleep(0.3)
        after = t.cmd('free')
        fault = serial.wait_for(r'PANIC|HardFault|stack overflow', 1, mark)
        report.add('wget memory freed', before == after and fault is None, after.strip().replace('\r\n', ' | '))
    finally:
        stop.set()
        for s in servers:
            s.join(2)
        t.send('exit\r\n'); t.wait_closed(); t.close()


def dhcp_server(stop, iface, log, lease=120):
    """Offers 198.18.0.2/30 (the board's test address, so sessions survive) with
    gateway and DNS 198.18.0.1. log gets (message type, length, broadcast flag, options)."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())
    u.bind(('0.0.0.0', 67))
    u.settimeout(0.2)
    with u:
        while not stop.is_set():
            try:
                d, _ = u.recvfrom(1500)
            except socket.timeout:
                continue
            opts = nnshare.dhcp_options(d)
            if opts is None or len(opts.get(53, b'')) != 1:
                continue
            kind = opts[53][0]
            log.append((kind, len(d), bool(d[10] & 0x80), opts))
            reply = {1: 2, 3: 5}.get(kind)                    # DISCOVER -> OFFER, REQUEST -> ACK
            if not reply:
                continue
            r = bytearray(240)
            r[0:3] = b'\x02\x01\x06'
            r[4:8], r[10:12] = d[4:8], d[10:12]
            r[16:20], r[20:24], r[28:44] = bytes([198, 18, 0, 2]), bytes([198, 18, 0, 1]), d[28:44]
            r[236:240] = b'\x63\x82\x53\x63'
            r += bytes([53, 1, reply, 54, 4, 198, 18, 0, 1, 51, 4]) + struct.pack('>I', lease)
            r += bytes([1, 4, 255, 255, 255, 252, 3, 4, 198, 18, 0, 1, 6, 4, 198, 18, 0, 1, 255])
            u.sendto(bytes(r), ('255.255.255.255', 68))


def tcp_reply_server(port, replies, got):
    """Accepts len(replies) connections: reads one line, answers, closes."""
    lsock = socket.socket()
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((PEER.split('/')[0], port))
    lsock.listen(2)
    lsock.settimeout(15)
    with lsock:
        for reply in replies:
            try:
                c, _ = lsock.accept()
            except socket.timeout:
                return
            with c:
                c.settimeout(5)
                data = b''
                while not data.endswith(b'\n'):
                    chunk = c.recv(256)
                    if not chunk:
                        break
                    data += chunk
                got.append(data)
                c.sendall(reply(data))
                time.sleep(0.2)


def m5c_tests(ns, serial, report, iface):
    """date/RTC, ed, ifconfig, DHCP, NTP, nc, netstat, services and the M5 boot acceptance."""
    t = Telnet(ns)
    t.read_until(Telnet.PROMPT)
    free_before = t.cmd('free')
    folder = f'/home/m5c-{os.getpid()}'
    t.cmd(f'mkdir {folder}')
    tz_saved = 'tz' in t.cmd('ls /etc').split()
    if tz_saved:
        t.cmd('mv /etc/tz /etc/tz.keep')
    try:
        # Clock.
        now = int(time.time())
        t.cmd("date -s '" + time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(now)) + "'")
        out = t.cmd('date +%s')
        diff = int(out.strip() or 0) - int(time.time())
        utc = t.cmd('date')
        report.add('date set', abs(diff) <= 2 and utc.strip().endswith('UTC'), f'{utc.strip()} (host {diff:+d} s)')
        t.cmd('echo +05:30 > /etc/tz')
        local, utc = t.cmd('date'), t.cmd('date -u')
        bad = t.cmd('date -s 2026-02-29 10:00') + t.cmd('date -s yesterday')
        report.add('date tz, checks', local.strip().endswith('+0530') and bad.count('invalid') == 2
                   and local[11:16] != utc[11:16], f'{local.strip()} / {utc.strip()}')
        t.cmd('rm /etc/tz')
        rtc = serial.wait_for(r'rtc: 32.768 kHz crystal running', 1, 0)
        line = serial.lines[rtc] if rtc is not None else t.cmd('dmesg | grep rtc:').strip()
        report.add('rtc crystal', 'crystal running' in line, line or 'no crystal reported')

        # ed, interactively and from a script.
        f = f'{folder}/notes'
        t.send(f'ed {f}\r\n')
        t.read_until('*')
        for line in ('a', 'first line', 'second line', 'third', '.', '2s/second/2nd/', 'w', 'q'):
            t.send(line + '\r\n')
            time.sleep(0.05)
        t.read_until(Telnet.PROMPT)
        content = t.cmd(f'cat {f}')
        status, _ = ns.call(http_request, 'PUT', f'{folder}/script',
                            b'1d\n$a\nappended\n.\n,s/line/LINE/g\n,n\nw\nq\n')
        out = t.cmd(f'cat {folder}/script | ed -s {f}')
        after = t.cmd(f'cat {f}')
        report.add('ed', content.split('\r\n')[:3] == ['first line', '2nd line', 'third']
                   and status in (201, 204) and '1\t2nd LINE' in out
                   and after.split('\r\n')[:3] == ['2nd LINE', 'third', 'appended'],
                   after.strip().replace('\r\n', ' | ') + f' (HTTP {status}, ed printed {out!r})')
        t.send(f'ed {f}\r\n')
        t.read_until('*')
        t.send('1d\r\n')
        t.read_until('*')
        t.send('q\r\n')
        warned = t.read_until('*')
        t.send('q\r\n')
        t.read_until(Telnet.PROMPT)
        unchanged = t.cmd(f'cat {f}') == after
        report.add('ed unsaved warning', 'unsaved changes' in warned and unchanged, warned.strip())

        # Interface configuration.
        shown = t.cmd('ifconfig')
        bad = ''.join(t.cmd(f'ifconfig {a}') for a in ('1.2.3/24', '1.2.3.4/33', '1.2.3.4 gw', '1.2.3.4 via 1.1.1.1'))
        t.cmd('ifconfig eth0 198.18.0.2/30 gw 198.18.0.1 dns 9.9.9.9')
        shown2 = t.cmd('ifconfig')
        report.add('ifconfig', 'inet 198.18.0.2/30' in shown and 'up 100M' in shown and bad.count('usage') == 4
                   and 'gw 198.18.0.1' in shown2 and 'dns 9.9.9.9' in shown2, shown2.strip().replace('\r\n', ' |'))
        net = t.cmd('netstat')
        report.add('netstat', re.search(r'0 diag +tcp +5000 +LISTEN', net) is not None
                   and re.search(r'[123] telnet +tcp +23 +ESTAB +198\.18\.0\.1:', net) is not None,
                   net.strip().replace('\r\n', ' | ') if 'ESTAB' not in net else 'diag listening, this session')

        # DHCP (offers the same address, so this session survives).
        stop, log = threading.Event(), []
        server = ns.background(dhcp_server, stop, iface, log)
        time.sleep(0.3)
        started = time.monotonic()
        out = t.cmd('dhcp', 25)
        took = time.monotonic() - started
        shown = t.cmd('ifconfig')
        stop.set(); server.join()
        kinds = [k for k, *_ in log]
        report.add('dhcp lease', 'from 198.18.0.1, lease 120 s' in out and 'dns 198.18.0.1' in shown
                   and 'dhcp bound' in shown and kinds[:2] == [1, 3], f'{out.strip()} in {took:.1f} s')
        report.add('dhcp packets', all(n >= 300 and bcast for _, n, bcast, _ in log)
                   and log[1][3].get(50) == bytes([198, 18, 0, 2]) and log[1][3].get(54) == bytes([198, 18, 0, 1]),
                   'broadcast flag, 300 B minimum, requested address and server id')
        started = time.monotonic()
        out = t.cmd('dhcp -t 30', 35)
        took = time.monotonic() - started
        shown = t.cmd('ifconfig')
        report.add('dhcp no server', 'no answer' in out and 'inet 198.18.0.2/30' in shown and 'dhcp failed' in shown,
                   f'{out.strip()} after {took:.1f} s')
        t.cmd('ifconfig eth0 198.18.0.2/30')

        # NTP.
        stop, served = threading.Event(), []
        server = ns.background(ntp_server, stop, served)
        time.sleep(0.3)
        out = t.cmd('ntpdate 198.18.0.1', 10)
        utc = t.cmd('date -u')
        stop.set(); server.join()
        report.add('ntpdate', utc.startswith('2030-01-02 03:04:0') and len(served) == 1, out.strip().replace('\r\n', ' | '))
        out = t.cmd('ntpdate 198.18.0.1', 10)             # the server has stopped
        t.cmd("date -s '" + time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime()) + "'")
        report.add('ntpdate no server', 'no answer' in out, out.strip())

        # nc: client through a pipe (TCP and UDP), server into a file, interactive, refused, ^C.
        got = []
        server = ns.background(tcp_reply_server, 7300, [lambda d: b'reply:' + d, lambda d: b'echo:' + d], got)
        time.sleep(0.3)
        out = t.cmd('echo hello | nc 198.18.0.1 7300', 10)
        report.add('nc tcp client', out.strip() == 'reply:hello' and got[:1] == [b'hello\n'], out.strip())
        t.send('nc 198.18.0.1 7300\r\n')
        time.sleep(0.6)
        t.send('typed\r\n')
        out = t.read_until(Telnet.PROMPT, 5)
        server.join()
        report.add('nc interactive', 'echo:typed' in out and got[1:] == [b'typed\n'], 'line out, reply in, peer closes')

        def udp_echo(port, got):
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.bind((PEER.split('/')[0], port))
            u.settimeout(10)
            with u:
                d, a = u.recvfrom(256)
                got.append(d)
                u.sendto(b'pong:' + d, a)
        got = []
        server = ns.background(udp_echo, 7301, got)
        time.sleep(0.3)
        out = t.cmd('echo ping | nc -u 198.18.0.1 7301', 10)
        server.join()
        report.add('nc udp client', out.strip() == 'pong:ping' and got == [b'ping\n'], out.strip())

        t.send(f'nc -l 7302 > {folder}/rx\r\n')
        time.sleep(0.6)

        def send_to_board():
            c = socket.create_connection((TARGET[0], 7302), timeout=5)
            c.sendall(b'line one\nline two\n')
            time.sleep(0.2)
            c.close()
        ns.call(send_to_board)
        t.read_until(Telnet.PROMPT, 5)
        out = t.cmd(f'cat {folder}/rx')
        report.add('nc listen', out == 'line one\r\nline two\r\n', out.strip().replace('\r\n', ' | '))

        out = t.cmd('nc 198.18.0.1 7399', 10)
        t.send('nc -l 7303\r\n')
        time.sleep(0.6)
        started = time.monotonic()
        t.send('\x03')
        t.read_until(Telnet.PROMPT, 5)
        stopped = time.monotonic() - started
        net = t.cmd('netstat')
        report.add('nc refused, ^C', 'refused' in out and stopped < 1.0 and re.search(r'5 user +- ', net) is not None
                   and re.search(r'6 user +- ', net) is not None, f'^C after {stopped * 1000:.0f} ms, sockets closed')
        bad = ''.join(t.cmd(f'nc {a}') for a in ('1.2.3.4', '1.2.3.4 0', '1.2.3.400 80', '-l', '-x 1.2.3.4 1'))
        report.add('nc arguments', bad.count('usage') == 5, '')

        # Services.
        t.cmd('service telnetd stop')

        def try_connect():
            try:
                socket.create_connection((TARGET[0], 23), timeout=2).close()
                return True
            except OSError:
                return False
        time.sleep(0.3)
        refused = not ns.call(try_connect)
        listing = t.cmd('service')
        t.cmd('service telnetd start')
        time.sleep(0.3)
        t2 = Telnet(ns, connect_within=3)
        t2.read_until(Telnet.PROMPT)
        t2.send('exit\r\n'); t2.wait_closed(); t2.close()
        report.add('service', refused and 'telnetd  off' in listing and 'httpd    on' in listing,
                   'telnetd stopped (session kept), started again')

        time.sleep(0.5)
        free_after = t.cmd('free')
        report.add('m5c heap reclaimed', free_before == free_after, free_after.strip())
    finally:
        t.cmd(f'rm {folder}/notes'); t.cmd(f'rm {folder}/script'); t.cmd(f'rm {folder}/rx')
        t.cmd(f'rmdir {folder}')
        t.cmd('rm /etc/tz')
        if tz_saved:
            t.cmd('mv /etc/tz.keep /etc/tz')
        t.cmd('ifconfig eth0 198.18.0.2/30')
        t.cmd('service telnetd start')
        t.cmd('service httpd start')

    # M5 acceptance: /etc/rc configures the network (DHCP) and starts telnetd.
    keep = 'rc' in t.cmd('ls /etc').split()
    if keep:
        t.cmd('mv /etc/rc /etc/rc.keep')
    stop, log = threading.Event(), []
    server = ns.background(dhcp_server, stop, iface, log)
    try:
        for line in ('service telnetd stop', 'ifconfig eth0 198.18.0.2/30', 'dhcp -t 15', 'service telnetd start'):
            t.cmd(f"echo '{line}' >> /etc/rc")
        before = int(t.cmd('date +%s').strip() or 0) - int(time.time())
        t.send('exit\r\n'); t.wait_closed(); t.close()
        mark = len(serial.lines)
        ns.call(request, b'X')
        # Until the W5500 is reset (~0.2 s into the boot) it still listens with
        # the old firmware's sockets: wait for the new telnetd.
        serial.wait_for(r'telnetd: listening', 30, mark)
        t = Telnet(ns, connect_within=10)
        t.read_until(Telnet.PROMPT, 10)
        shown = t.cmd('ifconfig')
        after = int(t.cmd('date +%s').strip() or 0) - int(time.time())
        leased = serial.wait_for(r'dhcp: lease 120 s', 1, mark)
        report.add('rc network boot', 'dhcp bound' in shown and leased is not None and [k for k, *_ in log][:2] == [1, 3],
                   'rc: dhcp lease, then telnetd started')
        report.add('rtc across reboot', abs(after - before) <= 2, f'clock {before:+d} s before, {after:+d} s after')
    finally:
        stop.set(); server.join()
        t.cmd('rm /etc/rc')
        if keep:
            t.cmd('mv /etc/rc.keep /etc/rc')
        t.cmd('ifconfig eth0 198.18.0.2/30')
        t.send('exit\r\n'); t.wait_closed(); t.close()


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
    if Path('/run/nnshare.json').exists():
        raise SystemExit('internet sharing is on (tools/nnshare.py), and it serves DHCP and DNS on the test '
                         'network too: sudo python3 tools/nnshare.py off first')

    report = Report()
    log = REPO / 'build/nntest-serial.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    serial = Serial(args.serial, log=log)              # streamed: memory stays bounded however long the run
    try:
        if args.flash:
            mark = len(serial.lines)
            ok = flasher.flash(args.flash, REPO / 'build/flash.log')
            report.add('flash', ok, str(args.flash))
            if not ok:
                return 1
            banner = serial.wait_for(BANNER, 10, mark)
            report.add('boot banner', banner is not None, serial.lines[banner] if banner is not None else 'no banner')
        listening = serial.wait_for(r'diag: listening', 15, 0) if args.flash else 0
        if args.flash:                                # only a fresh boot prints it
            report.add('diag listening', listening is not None)
        serial.wait_for(r'w5500: link up', 10, listening or 0)

        with Namespace(args.iface) as ns:
            ping = run(*ns.exec_prefix(), 'ping', '-n', '-c', '3', '-w', '10',
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
            report.add('tasks (K)', {'idle', 'fsd', 'kd', 'diag'} <= set(names), ' '.join(names))

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
                install_programs(ns, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('programs installed', False, repr(exc))
            try:
                console_tests(ns, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('console', False, repr(exc))
            serial.wait_for(r'telnetd: listening', 5, 0)
            try:
                telnet_tests(ns, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('telnet', False, repr(exc))

            try:
                fs_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('file systems', False, repr(exc))

            try:
                m5_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError, RuntimeError) as exc:
                traceback.print_exc()
                report.add('M5 environment', False, repr(exc))

            try:
                job_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('jobs and pipes', False, repr(exc))

            try:
                settings_tests(ns, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('settings', False, repr(exc))

            try:
                review_tests(ns, serial, report, args.iface)
            except (OSError, TimeoutError, ConnectionError, StopIteration) as exc:
                traceback.print_exc()
                report.add('review fixes', False, repr(exc))

            try:
                m6_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('M6 programs', False, repr(exc))

            try:
                nnv_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('NNV player', False, repr(exc))

            try:
                efficiency_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('efficiency', False, repr(exc))

            try:
                audit_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('audit fixes', False, repr(exc))

            try:
                xip_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('xip', False, repr(exc))

            if args.flash:
                try:
                    m7_tests(ns, serial, report, args.flash)
                except (OSError, TimeoutError, ConnectionError) as exc:
                    traceback.print_exc()
                    report.add('M7 firmware update', False, repr(exc))

            try:
                m5c_tests(ns, serial, report, args.iface)
            except (OSError, TimeoutError, ConnectionError, IndexError) as exc:
                traceback.print_exc()
                report.add('M5c tools', False, repr(exc))
            try:
                input_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('input', False, repr(exc))
            try:
                wget_tests(ns, serial, report)
            except (OSError, TimeoutError, ConnectionError) as exc:
                traceback.print_exc()
                report.add('wget', False, repr(exc))

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
                crash = serial.wait_for(r'last crash #\d+: panic: stack overflow (in )?ovf', 5, rebooted or mark)
                report.add('stack overflow', None not in (rebooted, crash),
                           serial.lines[crash] if crash is not None else 'no overflow panic reported')
                serial.wait_for(r'telnetd: listening', 30, rebooted or mark)

                # Watchdog: a task that does not yield above the kernel daemon starves it.
                # Turned off (after toggling it while the daemon kicks), 10 s of that is harmless.
                t = Telnet(ns, connect_within=20)
                t.read_until(Telnet.PROMPT)
                toggles = ''.join(t.cmd(f'sysctl watchdog={v}') for v in (7, 0) * 20)
                mark = len(serial.lines)
                try:
                    ns.call(request, b'W', 4, 10000, b'', None, 3)
                except OSError:
                    pass                                  # no reply while everything below it starves
                time.sleep(9)
                off_ok = serial.wait_for(r'reset cause', 3, mark) is None and 'nnnix' in t.cmd('uname', 10)
                setting = t.cmd('sysctl watchdog=7')
                t.close()
                report.add('watchdog off', off_ok and toggles.count('watchdog=0') == 20,
                           'turned off after 40 changes: 10 s without the kernel daemon, no reset')
                mark = len(serial.lines)
                started = time.monotonic()
                try:
                    ns.call(request, b'W', 4, 0, b'', None, 3)
                except OSError:
                    pass                                  # no reply: everything below it starves
                cause = serial.wait_for(r'reset cause .*watchdog', 20, mark)
                took = time.monotonic() - started
                serial.wait_for(r'telnetd: listening', 30, cause or mark)
                t = Telnet(ns, connect_within=20)
                t.read_until(Telnet.PROMPT)
                default = t.cmd('sysctl watchdog')
                t.close()
                report.add('watchdog', 'watchdog=7' in setting and cause is not None and 'watchdog=26' in default,
                           f'reset {took:.1f} s after the kernel daemon stopped; back to 26 s after boot')
    finally:
        serial.close()
        if os.geteuid() == 0 and 'SUDO_UID' in os.environ:   # hand it back to the invoking user
            os.chown(log, int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID']))
    passed = sum(bool(ok) for _, ok, _ in report.rows)
    print(f'\n{passed}/{len(report.rows)} passed; serial log: {log}')
    return 0 if report.ok else 1


if __name__ == '__main__':
    sys.exit(main())
