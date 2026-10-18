#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/nnscreen.py and tools/nninput.py without a desktop (run by `make
host-test`): the screen scaled into the LCD's picture with the pointer drawn
where it is, the levels stretched, a live stream that decodes; the board's
events replayed as the right key codes, pointer moves, buttons and wheel steps
(on a stand-in for Mutter's session), and everything held let go at the end."""
import os
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np

os.environ['XDG_RUNTIME_DIR'] = tempfile.mkdtemp()           # the pointer file goes here
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import mknnv                                                  # noqa: E402
import mutter                                                 # noqa: E402
import nninput                                                # noqa: E402
import nnscreen                                               # noqa: E402

checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f'test_nnscreen: FAILED: {what}', file=sys.stderr)


class Args:
    follow = False
    area = None
    zoom = 2


# ---- the picture ----
g = np.zeros((90, 160), dtype=np.uint8)
g[:, 80:] = 200
small = nnscreen.shrink(g, 4, 2)
check(small.shape == (2, 4) and (small[:, :2] == 0).all() and (small[:, 2:] == 200).all(), f'shrink {small}')
check((nnscreen.shrink(np.full((7, 13), 99, np.uint8), 5, 3) == 99).all(), 'shrink by uneven steps')

screen = (1920, 1080)
check(nnscreen.region(Args, screen, None) == (0, 0, 1920, 1080), 'region: the whole screen')
a = Args()
a.area = [1800, 1000, 400, 400]
check(nnscreen.region(a, screen, None) == (1800, 1000, 120, 80), 'region: --area cut at the edge')
a = Args()
a.follow = True
check(nnscreen.region(a, screen, (960, 540)) == (832, 476, 256, 128), 'region: --follow around the pointer')
check(nnscreen.region(a, screen, (5, 1079)) == (0, 952, 256, 128), 'region: --follow at a corner')

f = 256 / 1920                                               # the stream comes at 256x144
gray = np.full((144, 256), 30, dtype=np.uint8)
grey, place = nnscreen.picture(gray, (0, 0, 1920, 1080), 'pad', f, lambda x: x)
sx, sy, ox, oy, x0, y0 = place
check(grey.shape == (64, 128) and (grey[:, :ox] == 255).all() and (grey[:, ox + 114:] == 255).all()
      and (grey[:, ox:ox + 114] == 30).all() and ox == 7, f'pad: light bars around the screen, ox {ox}')
check(abs(sx * 1920 - 114) < 1 and abs(sy * 1080 - 64) < 1 and (x0, y0) == (0, 0), f'pad: placed {place}')
grey, place = nnscreen.picture(gray, (0, 0, 1920, 1080), 'pad', f, lambda x: 255 - x)
check((grey[:, :7] == 255).all() and (grey[:, 7:121] == 225).all(), '--invert leaves the bars light')
grey, place = nnscreen.picture(gray, (0, 0, 1920, 1080), 'crop', f, lambda x: x)
check((grey == 30).all() and abs(place[0] * 1920 / 1.0 - 128 * 1920 / 1920) < 1, f'crop fills the screen {place}')

dark = np.zeros((64, 128), dtype=bool)
nnscreen.draw_arrow(dark, (960, 540), place)
tip = (int((960 - place[4]) * place[0]) + place[2], int((540 - place[5]) * place[1]) + place[3])
check(dark[tip[1], tip[0]] and not dark[tip[1] + 2, tip[0] + 1], f'the pointer drawn at {tip}')
dark = np.zeros((64, 128), dtype=bool)
_, pad = nnscreen.picture(gray, (0, 0, 1920, 1080), 'pad', f, lambda x: x)
nnscreen.draw_arrow(dark, (1919, 1079), pad)
check(dark[63, 120] and dark.sum() == 1, 'a pointer at the corner is cut, not lost')

levels = nnscreen.Levels()
pic = np.full((64, 128), 40, dtype=np.uint8)
pic[30:34, 10:60] = 180                                       # bright text on a dark theme
out = levels(pic)
check(out[0, 0] == 0 and out[31, 20] == 255, f'levels stretched: {out[0, 0]} {out[31, 20]}')
check((levels(np.full((64, 128), 128, np.uint8)) < 255).all(), 'a plain screen does not turn white at once')

frames = [mknnv.pack(np.zeros((64, 128), bool)), mknnv.pack(np.eye(64, 128, dtype=bool))]
stream = nnscreen.header(0, 0)
prev = bytes(mknnv.FRAME)
for fr in frames:
    codes = mknnv.delta(prev, fr)
    stream += struct.pack('<BH', mknnv.DELTA, len(codes)) + codes
    prev = fr
d = mknnv.decode(stream)
check(d['frame_ms'] == 0 and d['frames'] == frames, 'a live stream: no frame count, no time per frame')
check(nnscreen.header(5, 100)[8:14] == struct.pack('<HHBB', 5, 100, 0, 0), 'a recording\'s header')


# ---- the board's events ----
class Session:
    size = (1920, 1080)
    stream = '/org/gnome/Mutter/ScreenCast/Stream/u1'

    def __init__(self):
        self.calls = []

    def input(self, method, *args):
        self.calls.append((method,) + args)


def ev(kind, code, value=0, dx=0, dy=0):
    return kind, code, value, dx, dy


s = Session()
r = nninput.Replay(s, 2.0, False)
for e in (ev(1, 0x04, 1), ev(1, 0x04, 0), ev(1, 0xe1, 1), ev(1, 0x52, 1), ev(3, 5, 1), ev(3, 5, 0), ev(1, 0x99, 1)):
    r.event(*e)
check(s.calls == [('NotifyKeyboardKeycode', 30, True), ('NotifyKeyboardKeycode', 30, False),
                  ('NotifyKeyboardKeycode', 42, True), ('NotifyKeyboardKeycode', 103, True),
                  ('NotifyKeyboardKeycode', 28, True), ('NotifyKeyboardKeycode', 28, False)],
      f'keys: a, Shift, Up, the keypad\'s 5 as Enter, an unknown usage left out: {s.calls}')
check(all(0 < k < 256 for k in nninput.HID_TO_EVDEV.values()) and nninput.HID_TO_EVDEV[0x27] == 11
      and nninput.HID_TO_EVDEV[0x45] == 88, 'the key table')
s.calls.clear()
r.event(*ev(2, 0, 0, 10, -5))
r.event(*ev(2, 1, 0))
r.event(*ev(2, 3, 0))
r.event(*ev(2, 0, 1))
r.event(*ev(2, 0, 255))
check(s.calls == [('NotifyPointerMotionAbsolute', Session.stream, 980.0, 530.0),
                  ('NotifyPointerButton', 0x110, True), ('NotifyPointerButton', 0x111, True),
                  ('NotifyPointerButton', 0x110, False), ('NotifyPointerButton', 0x111, False),
                  ('NotifyPointerAxisDiscrete', 0, -1), ('NotifyPointerAxisDiscrete', 0, 1)],
      f'the mouse: a move from the middle, buttons, the wheel up then down: {s.calls}')
check(mutter.read_pointer() == (980.0, 530.0), f'the pointer file: {mutter.read_pointer()}')
for _ in range(20):
    r.event(*ev(2, 0, 0, -127, 127))
check(mutter.read_pointer() == (0.0, 1079.0), f'the pointer stays on the screen: {mutter.read_pointer()}')
s.calls.clear()
r.event(*ev(1, 0xe0, 1))
r.event(*ev(2, 4, 0))
s.calls.clear()
r.release()
check(sorted(s.calls) == [('NotifyKeyboardKeycode', 29, False), ('NotifyKeyboardKeycode', 42, False),
                          ('NotifyKeyboardKeycode', 103, False), ('NotifyPointerButton', 0x112, False)],
      f'the end lets go of what is held: {s.calls}')
mutter.remove_pointer()
check(mutter.read_pointer() is None, 'no pointer file once nninput ends')

print(f'test_nnscreen: {checks} checks, {failures} failed')
sys.exit(1 if failures else 0)
