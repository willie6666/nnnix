#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The board's keyboard, mouse and keypad, working this computer.

  nnscreen.py | nc BOARD 1234 | nninput.py        on the board: remote

Reads the board's input events (sent by the board's `remote`: /dev/input
events, 8 bytes each, lib/sys.h struct nn_input_event) from standard input and
replays them on the GNOME desktop through Mutter's remote desktop service:
keys as the same keys (this computer's keyboard layout applies), the mouse as
the pointer. The board's keypad zooms the picture nnscreen.py sends, at the
pointer: 2 closer, 8 farther, 5 the whole screen again (mutter.ZOOMS). Whoever
can send to standard input controls this computer: feed it only from your
board.

The pointer is moved to positions this program keeps (starting in the middle
of the screen), so a mouse used here meanwhile makes it jump back on the next
move from the board. It writes the position and the zoom for nnscreen.py, which
draws the pointer on the LCD (the screen is too small there to show GNOME's
own) and shows the part of the screen around it.
When the input ends (the board's SW_INT button ends nc, or the stream
stops), keys and buttons still held are let go.

  --speed X       pointer pixels per mouse count (default 2)
  --monitor NAME  a monitor other than the primary one
  -v              print the events
"""
import argparse
import signal
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mutter  # noqa: E402

KEY, MOUSE, PAD = 1, 2, 3
EVENT = struct.Struct('<BBBbbbH')                            # type, code, value, ch, dx, dy, ms

# USB HID keyboard usages to Linux input key codes (as the kernel's hid-input).
HID_TO_EVDEV = {
    0x04: 30, 0x05: 48, 0x06: 46, 0x07: 32, 0x08: 18, 0x09: 33, 0x0a: 34, 0x0b: 35, 0x0c: 23, 0x0d: 36,
    0x0e: 37, 0x0f: 38, 0x10: 50, 0x11: 49, 0x12: 24, 0x13: 25, 0x14: 16, 0x15: 19, 0x16: 31, 0x17: 20,
    0x18: 22, 0x19: 47, 0x1a: 17, 0x1b: 45, 0x1c: 21, 0x1d: 44,                       # a-z
    0x1e: 2, 0x1f: 3, 0x20: 4, 0x21: 5, 0x22: 6, 0x23: 7, 0x24: 8, 0x25: 9, 0x26: 10, 0x27: 11,   # 1-9 0
    0x28: 28, 0x29: 1, 0x2a: 14, 0x2b: 15, 0x2c: 57, 0x2d: 12, 0x2e: 13, 0x2f: 26, 0x30: 27, 0x31: 43,
    0x32: 43, 0x33: 39, 0x34: 40, 0x35: 41, 0x36: 51, 0x37: 52, 0x38: 53, 0x39: 58,
    0x3a: 59, 0x3b: 60, 0x3c: 61, 0x3d: 62, 0x3e: 63, 0x3f: 64, 0x40: 65, 0x41: 66, 0x42: 67, 0x43: 68,
    0x44: 87, 0x45: 88,                                                               # F1-F12
    0x46: 99, 0x47: 70, 0x48: 119, 0x49: 110, 0x4a: 102, 0x4b: 104, 0x4c: 111, 0x4d: 107, 0x4e: 109,
    0x4f: 106, 0x50: 105, 0x51: 108, 0x52: 103,                                       # arrows
    0x53: 69, 0x54: 98, 0x55: 55, 0x56: 74, 0x57: 78, 0x58: 96, 0x59: 79, 0x5a: 80, 0x5b: 81,
    0x5c: 75, 0x5d: 76, 0x5e: 77, 0x5f: 71, 0x60: 72, 0x61: 73, 0x62: 82, 0x63: 83,  # keypad
    0x64: 86, 0x65: 127, 0x66: 116, 0x67: 117,
    0x68: 183, 0x69: 184, 0x6a: 185, 0x6b: 186, 0x6c: 187, 0x6d: 188, 0x6e: 189, 0x6f: 190,
    0x70: 191, 0x71: 192, 0x72: 193, 0x73: 194,                                       # F13-F24
    0x7f: 113, 0x80: 115, 0x81: 114,                                                  # mute, volume
    0x87: 89, 0x88: 93, 0x89: 124, 0x8a: 92, 0x8b: 94, 0x90: 122, 0x91: 123,
    0xe0: 29, 0xe1: 42, 0xe2: 56, 0xe3: 125, 0xe4: 97, 0xe5: 54, 0xe6: 100, 0xe7: 126,   # modifiers
}
# The board's keypad: zoom steps (mutter.ZOOMS) closer, farther, or back to the whole screen.
KEYPAD = {2: +1, 8: -1, 5: 0}
BUTTONS = ((0x01, 0x110), (0x02, 0x111), (0x04, 0x112))      # left, right, middle: BTN_*


class Replay:
    def __init__(self, session, speed, verbose):
        self.s, self.speed, self.verbose = session, speed, verbose
        self.w, self.h = session.size
        self.x, self.y = self.w / 2, self.h / 2
        self.keys, self.buttons = set(), 0
        self.moved = False
        self.zoom = 0                                        # an index into mutter.ZOOMS

    def key(self, code, down):
        if down:
            self.keys.add(code)
        else:
            self.keys.discard(code)
        self.s.input('NotifyKeyboardKeycode', code, bool(down))

    def event(self, kind, code, value, dx, dy):
        if self.verbose:
            print(f'type {kind} code {code:#04x} value {value} dx {dx} dy {dy}', file=sys.stderr)
        if kind == KEY and code in HID_TO_EVDEV:
            self.key(HID_TO_EVDEV[code], value)
        elif kind == PAD and code in KEYPAD and value:
            step = KEYPAD[code]
            self.zoom = min(max(self.zoom + step, 0), len(mutter.ZOOMS) - 1) if step else 0
            self.save()
        elif kind == MOUSE:
            if dx or dy or not self.moved:
                self.x = min(max(self.x + dx * self.speed, 0), self.w - 1)
                self.y = min(max(self.y + dy * self.speed, 0), self.h - 1)
                self.s.input('NotifyPointerMotionAbsolute', self.s.stream, self.x, self.y)
                self.save()
                self.moved = True
            for bit, button in BUTTONS:
                if (code ^ self.buttons) & bit:
                    self.s.input('NotifyPointerButton', button, bool(code & bit))
            self.buttons = code
            wheel = value - 256 if value > 127 else value
            if wheel:
                self.s.input('NotifyPointerAxisDiscrete', 0, -wheel)  # the wheel turned away: up

    def save(self):
        mutter.write_state(self.x, self.y, mutter.ZOOMS[self.zoom])

    def release(self):
        for code in list(self.keys):
            self.key(code, 0)
        for bit, button in BUTTONS:
            if self.buttons & bit:
                self.s.input('NotifyPointerButton', button, False)
        self.buttons = 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--speed', type=float, default=2.0)
    ap.add_argument('--monitor', default='')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()
    if sys.stdin.isatty():
        ap.error('standard input is a terminal: it takes the board\'s events (see --help)')
    try:
        session = mutter.Session(args.monitor, mutter.CURSOR_HIDDEN, remote=True)
    except mutter.Error as e:
        sys.exit(f'nninput: {e}')
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    replay = Replay(session, args.speed, args.verbose)
    replay.save()
    data = b''
    try:
        while True:
            chunk = sys.stdin.buffer.read1(4096)
            if not chunk:
                break
            data += chunk
            while len(data) >= EVENT.size:
                kind, code, value, _, dx, dy, _ = EVENT.unpack_from(data)
                data = data[EVENT.size:]
                replay.event(kind, code, value, dx, dy)
    except KeyboardInterrupt:
        pass
    except mutter.GLib.Error as e:                         # the session ended (GNOME's indicator)
        print(f'nninput: {e.message}', file=sys.stderr)
    finally:
        try:
            replay.release()
        except mutter.GLib.Error:
            pass
        mutter.remove_state()
        session.stop()


if __name__ == '__main__':
    main()
