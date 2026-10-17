#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the nnnix kernel log from the CP2102 serial port (read only, 115200 8N1).

  python3 tools/console.py                       # print until Ctrl-C
  python3 tools/console.py --until 'diag: listening' --timeout 10
"""
import argparse
import fcntl
import os
import re
import select
import sys
import termios
import threading
import time


class History:
    """Lines by absolute index, keeping only the newest `keep` in memory (a long
    soak would otherwise grow without bound). len() counts every line ever
    added; lines that were dropped read as ''."""

    def __init__(self, keep):
        self.keep, self.base, self.items = keep, 0, []

    def append(self, line):
        self.items.append(line)
        if len(self.items) > self.keep + self.keep // 4:     # trim in batches
            drop = len(self.items) - self.keep
            del self.items[:drop]
            self.base += drop

    def __len__(self):
        return self.base + len(self.items)

    def __getitem__(self, i):
        if isinstance(i, slice):
            return [self[k] for k in range(*i.indices(len(self)))]
        if i < 0:
            i += len(self)
        if not 0 <= i < len(self):
            raise IndexError(i)
        return self.items[i - self.base] if i >= self.base else ''


class Serial:
    """Raw read-only tty with a background reader that keeps complete lines
    (the newest `keep`; with `log`, every line is also appended to that file)."""

    def __init__(self, device='/dev/ttyUSB0', baud=115200, keep=20000, log=None):
        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.fd)
            raise SystemExit(f'{device} is in use (picocom?); close it first') from None
        self.saved = termios.tcgetattr(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, f'B{baud}')
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = attrs[5] = speed
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.lines = History(keep)
        self.log = open(log, 'w') if log else None
        self.partial = b''
        self.lock = threading.Condition()
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while self.running:
            ready, _, _ = select.select([self.fd], [], [], .1)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            self.partial += data
            *complete, self.partial = self.partial.split(b'\n')
            if complete:
                with self.lock:
                    for raw in complete:
                        line = raw.rstrip(b'\r').decode('utf-8', 'replace')
                        self.lines.append(line)
                        if self.log:
                            self.log.write(line + '\n')
                            self.log.flush()
                    self.lock.notify_all()

    def wait_for(self, pattern, timeout, start=0):
        """Index of the first line at or after `start` matching `pattern`, else None."""
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        index = start
        with self.lock:
            while True:
                while index < len(self.lines):
                    if regex.search(self.lines[index]):
                        return index
                    index += 1
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self.lock.wait(remaining)

    def close(self):
        self.running = False
        self.thread.join()
        termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        os.close(self.fd)
        if self.log:
            self.log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', default='/dev/ttyUSB0')
    parser.add_argument('--until', help='exit 0 once a line matches this regex')
    parser.add_argument('--timeout', type=float, default=0, help='seconds (0 = forever)')
    args = parser.parse_args()
    serial = Serial(args.device)
    shown = 0
    deadline = time.monotonic() + args.timeout if args.timeout else None
    try:
        while True:
            serial.wait_for(r'.', .2, shown)
            with serial.lock:
                fresh = serial.lines[shown:]
                shown = len(serial.lines)
            for line in fresh:
                print(line, flush=True)
                if args.until and re.search(args.until, line):
                    return 0
            if deadline and time.monotonic() > deadline:
                return 1 if args.until else 0
    except KeyboardInterrupt:
        return 0
    finally:
        serial.close()


if __name__ == '__main__':
    sys.exit(main())
