#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/nnscreen.py and tools/nninput.py without a desktop (run by `make
host-test`): the part of the screen shown at each zoom, the screen scaled into
the LCD's picture with the pointer drawn where it is, the levels stretched, a
live stream that decodes; the board's events replayed as the right key codes,
pointer moves, buttons and wheel steps (on a stand-in for Mutter's session),
its keypad as zoom steps, and everything held let go at the end."""
import os
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np

os.environ['XDG_RUNTIME_DIR'] = tempfile.mkdtemp()           # the state file goes here
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


# ---- the picture ----
colors = np.array([[[0, 0, 255, 0], [0, 255, 0, 255]],
                   [[255, 0, 0, 99], [255, 255, 255, 0]]], dtype=np.uint8)
check(np.array_equal(mutter.bgrx_gray(colors, 1), [[77, 149], [29, 255]]),
      'linear BGRx capture converts colour to grey and ignores alpha/padding')
checker = np.zeros((4, 4, 4), dtype=np.uint8)
checker[::2, ::2, :3] = checker[1::2, 1::2, :3] = 255
check(np.array_equal(mutter.bgrx_gray(checker, .5), np.full((2, 2), 128)),
      'DMA preview averages small details rather than taking every nth pixel')
padded = np.full((3, 7, 4), 200, dtype=np.uint8)
padded[:, :5, :3] = 19
check(np.array_equal(mutter.bgrx_gray(padded[:, :5], .5), np.full((2, 2), 19)),
      'DMA conversion handles padded rows and uneven scaling')
check(mutter.bgrx_gray(colors, .01).shape == (1, 1), 'very small previews still contain a pixel')

g = np.zeros((90, 160), dtype=np.uint8)
g[:, 80:] = 200
small = nnscreen.shrink(g, 4, 2)
check(small.shape == (2, 4) and (small[:, :2] == 0).all() and (small[:, 2:] == 200).all(), f'shrink {small}')
check((nnscreen.shrink(np.full((7, 13), 99, np.uint8), 5, 3) == 99).all(), 'shrink by uneven steps')

screen = (1920, 1080)
v = nnscreen.View(screen)
check(v.region(0, (500, 500)) == (0, 0, 1920, 1080), 'the whole screen')
check(nnscreen.View(screen, [1800, 1000, 400, 400]).region(0, None) == (1800, 1000, 120, 80), '--area cut at the edge')
check(v.region(8, (960, 540)) == (448, 284, 1024, 512), 'zoomed in from the whole screen: the pointer stays put')
r = v.region(2, (960, 540))
check(r == (832, 476, 256, 128), f'closer: the point under the pointer stays put: {r}')
check(v.region(2, (1000, 540)) == r, 'the pointer moves inside: the part stays')
r = v.region(2, (1080, 540))
check(r == (856, 476, 256, 128), f'near the right edge: the part moves along: {r}')
check(v.region(1, (1919, 1079)) == (1792, 1016, 128, 64), 'at the screen\'s corner: kept on the screen')
check(v.region(0, (5, 5)) == (0, 0, 1920, 1080), 'back to the whole screen')
check(nnscreen.View(screen).region(2, (5, 1079)) == (0, 952, 256, 128), 'zoomed from the start, at a corner')
check(nnscreen.scale_for((0, 0, 1920, 1080)) == 2 * 128 / 1920 and nnscreen.scale_for((0, 0, 256, 128)) == 1.0,
      'the stream scaled to about twice what the LCD shows')
check((nnscreen.shrink(np.arange(6, dtype=np.uint8).reshape(2, 3), 6, 4) ==
       np.repeat(np.repeat(np.arange(6, dtype=np.uint8).reshape(2, 3), 2, 0), 2, 1)).all(),
      'shrink repeats pixels where the picture is smaller (just after a zoom)')

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
for e in (ev(1, 0x04, 1), ev(1, 0x04, 0), ev(1, 0xe1, 1), ev(1, 0x52, 1), ev(1, 0x99, 1)):
    r.event(*e)
check(s.calls == [('NotifyKeyboardKeycode', 30, True), ('NotifyKeyboardKeycode', 30, False),
                  ('NotifyKeyboardKeycode', 42, True), ('NotifyKeyboardKeycode', 103, True)],
      f'keys: a, Shift, Up, an unknown usage left out: {s.calls}')
s.calls.clear()
zooms = []
for code in (2, 2, 2, 2, 2, 8, 5, 8, 2, 3):
    r.event(*ev(3, code, 1))
    r.event(*ev(3, code, 0))
    zooms.append(mutter.read_state()[2])
check(zooms == [8, 4, 2, 1, 1, 2, 0, 0, 8, 8] and not s.calls,
      f'the keypad zooms: 2 closer, 8 farther, 5 the whole screen, others nothing: {zooms}')
r.event(*ev(3, 5, 1))
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
check(mutter.read_state() == (980.0, 530.0, 0, False), f'the state file: {mutter.read_state()}')
for _ in range(20):
    r.event(*ev(2, 0, 0, -127, 127))
check(mutter.read_state()[:2] == (0.0, 1079.0), f'the pointer stays on the screen: {mutter.read_state()}')
s.calls.clear()
r.event(*ev(1, 0xe0, 1))
r.event(*ev(2, 4, 0))
s.calls.clear()
r.release()
check(sorted(s.calls) == [('NotifyKeyboardKeycode', 29, False), ('NotifyKeyboardKeycode', 42, False),
                          ('NotifyKeyboardKeycode', 103, False), ('NotifyPointerButton', 0x112, False)],
      f'the end lets go of what is held: {s.calls}')

# ---- a program that locks the pointer: relative deltas, no edge or warp ----
s = Session()
r = nninput.Replay(s, 1.5, False)
r.event(*ev(2, 0, 0, 10, -5))
saved = (r.x, r.y)
s.calls.clear()
r.event(*ev(3, 1, 1))
check(r.relative and mutter.read_state() == (960.0, 540.0, 0, True), 'keypad 1 enters relative mode, view at centre')
check(not s.calls, 'changing mode does not warp the real pointer')
r.event(*ev(3, 1, 1))
r.event(*ev(3, 1, 0))
check(r.relative, 'duplicate down and release do not toggle a second time')
for _ in range(20):
    r.event(*ev(2, 0, 0, 127, -127))
check(s.calls == [('NotifyPointerMotionRelative', 190.5, -190.5)] * 20,
      'relative deltas keep going beyond the screen edges, with fractional speed')
check((r.x, r.y) == saved, 'relative moves keep the desktop pointer position for later')
s.calls.clear()
r.event(*ev(2, 1, 0))
r.event(*ev(2, 0, 255))
check(s.calls == [('NotifyPointerButton', 0x110, True), ('NotifyPointerButton', 0x110, False),
                  ('NotifyPointerAxisDiscrete', 0, 1)], 'relative mode preserves buttons and wheel, without zero-motion warps')
r.event(*ev(3, 2, 1))
check(mutter.read_state() == (960.0, 540.0, 8, True), 'zoom still works in relative mode')
s.calls.clear()
r.event(*ev(3, 1, 1))
r.event(*ev(3, 1, 0))
check(not r.relative and mutter.read_state() == (*saved, 8, False), 'keypad 1 returns to the saved desktop pointer')
r.event(*ev(2, 0, 0, -2, 4))
check(s.calls == [('NotifyPointerMotionAbsolute', Session.stream, saved[0] - 3, saved[1] + 6)],
      'absolute motion resumes normally')

s = Session()
r = nninput.Replay(s, 2.0, False, relative=True)
r.save()
r.event(*ev(2, 1, 0, 1, 2))
r.event(*ev(1, 0xe0, 1))
s.calls.clear()
r.release()
check(sorted(s.calls) == [('NotifyKeyboardKeycode', 29, False), ('NotifyPointerButton', 0x110, False)],
      'starting relative and disconnecting still releases held keys and buttons')
check(mutter.read_state()[3], 'relative startup reaches the screen state')

# Connecting while Enter/Shift is held must not replay an unmatched release.
# The desktop also owns auto-repeat, so only real key transitions are sent.
s = Session()
r = nninput.Replay(s, 2.0, False)
for code in (0x28, 0xe1):
    r.event(*ev(1, code, 0))
check(not s.calls, 'initial Enter and Shift releases are ignored')
for value in (1, 1, 1, 0, 0):
    r.event(*ev(1, 0x04, value))
check(s.calls == [('NotifyKeyboardKeycode', 30, True), ('NotifyKeyboardKeycode', 30, False)],
      'repeated key downs and releases are not sent twice')

class InputError(Exception):
    @property
    def message(self):
        return str(self)


class RejectingSession(Session):
    def __init__(self, reject):
        super().__init__()
        self.reject, self.stopped = reject, False

    def input(self, method, *args):
        super().input(method, *args)
        if (method,) + args == self.reject:
            raise InputError('Invalid key event')

    def stop(self):
        self.stopped = True


s = RejectingSession(('NotifyKeyboardKeycode', 30, True))
r = nninput.Replay(s, 2.0, False)
try:
    r.event(*ev(1, 0x04, 1))
    check(False, 'rejected key press must report failure')
except InputError:
    check(not r.keys, 'rejected press is not recorded as held')
r.release()
check(s.calls == [('NotifyKeyboardKeycode', 30, True)], 'cleanup does not release a rejected press')

s = RejectingSession(('NotifyKeyboardKeycode', 30, False))
r = nninput.Replay(s, 2.0, False)
r.event(*ev(1, 0x04, 1))
r.event(*ev(1, 0xe1, 1))
r.event(*ev(2, 1, 0))
s.calls.clear()
try:
    r.release()
    check(False, 'rejected release must report failure')
except InputError:
    check(set(s.calls) == {('NotifyKeyboardKeycode', 30, False), ('NotifyKeyboardKeycode', 42, False),
                           ('NotifyPointerButton', 0x110, False)}, 'one failed release does not skip other keys or buttons')
check(r.keys == {30} and not r.buttons, 'only the failed release remains held')

# An actual replay error must reach nnremote's exit-status handling.
import io                                                     # noqa: E402
from types import SimpleNamespace                             # noqa: E402
from unittest.mock import MagicMock, patch                    # noqa: E402
s = RejectingSession(('NotifyKeyboardKeycode', 30, True))
with patch.object(mutter, 'Session', return_value=s), \
     patch.object(mutter, 'GLib', SimpleNamespace(Error=InputError), create=True), \
     patch.object(nninput.signal, 'signal'), \
     patch.object(sys, 'argv', ['nninput.py']), \
     patch.object(sys, 'stdin', io.TextIOWrapper(io.BytesIO(nninput.EVENT.pack(1, 4, 1, 0, 0, 0, 0)))):
    check(nninput.main() == 1, 'D-Bus input failure exits with status 1')
check(s.stopped and mutter.read_state() is None, 'error closes the session and removes its preview state')

# EOF is valid only between complete eight-byte events. All prefixes must
# report failure and still release a complete key press preceding the cut.
press = nninput.EVENT.pack(1, 4, 1, 0, 0, 0, 0)
release = nninput.EVENT.pack(1, 4, 0, 0, 0, 0, 0)
for cut in range(8):
    s = RejectingSession(None)
    with patch.object(mutter, 'Session', return_value=s), \
         patch.object(mutter, 'GLib', SimpleNamespace(Error=InputError), create=True), \
         patch.object(nninput.signal, 'signal'), \
         patch.object(sys, 'argv', ['nninput.py']), \
         patch.object(sys, 'stderr', io.StringIO()) as errors, \
         patch.object(sys, 'stdin', io.TextIOWrapper(io.BytesIO(press + release[:cut]))):
        check(nninput.main() == (1 if cut else 0), f'input event truncated at {cut} bytes')
    check(s.calls == [('NotifyKeyboardKeycode', 30, True), ('NotifyKeyboardKeycode', 30, False)]
          and s.stopped and mutter.read_state() is None, f'truncated event {cut}: input released')
    check(('inside an event' in errors.getvalue()) == bool(cut), f'truncated event {cut}: clear error')

# A desktop without mappable DMA-BUF must still capture via shared memory.
gst = SimpleNamespace(SECOND=1000000000, State=SimpleNamespace(NULL=0),
                      MessageType=SimpleNamespace(ERROR=1, EOS=2), MapFlags=SimpleNamespace(READ=1))
for failure in ('negotiation', 'mapping'):
    frames = mutter.Frames.__new__(mutter.Frames)
    frames.np, frames.gray, frames.raw, frames.dma, frames.scale = np, None, None, True, .5
    old_pipe = frames.pipe = MagicMock()
    frames.sink = MagicMock()
    if failure == 'negotiation':
        frames.sink.emit.return_value = None
    else:
        broken = MagicMock()
        broken.get_buffer().map.return_value = (False, None)
        broken.get_caps().get_structure().get_value.side_effect = lambda k: 2
        frames.sink.emit.return_value = broken
    sample = MagicMock()
    sample.get_caps().get_structure().get_value.side_effect = lambda k: 2
    sample.get_buffer().map.return_value = (True, SimpleNamespace(data=bytes((11, 22, 99, 99, 33, 44, 99, 99))))
    starts = []

    def fallback(dma):
        starts.append(dma)
        frames.dma = dma
        frames.pipe, frames.sink = MagicMock(), MagicMock()
        frames.sink.emit.return_value = sample

    frames._start = fallback
    with patch.object(mutter, 'Gst', gst, create=True):
        grey = frames.latest(5)
    check(np.array_equal(grey, [[11, 22], [33, 44]]) and starts == [False],
          f'{failure} failure falls back once and reads a padded shared-memory frame')
    check(old_pipe.set_state.call_args.args == (gst.State.NULL,) and frames.scale == .5,
          f'{failure} fallback stops its old producer and retains zoom')

v = nnscreen.View(screen)
v.region(2, (1800, 900))
check(v.region(2, (960, 540), centered=True) == (832, 476, 256, 128), 'relative view zooms at the screen centre')
dark = np.zeros((64, 128), dtype=bool)
nnscreen.draw_relative(dark)
check(dark[:7, -5:].sum() == 25 and dark.sum() == 25 and not dark[1, 124] and dark[1, 126],
      'relative R indicator stays in its five-by-seven corner')

with open(mutter.STATE_FILE, 'w') as f:
    f.write('123 456 2\n')
check(mutter.read_state() == (123.0, 456.0, 2, False), 'old state files default to absolute mode')
with open(mutter.STATE_FILE, 'w') as f:
    f.write('123 456 2 relative\n')
check(mutter.read_state() is None, 'a damaged mode value is ignored')
mutter.remove_state()
check(mutter.read_state() is None, 'no state file once nninput ends')

print(f'test_nnscreen: {checks} checks, {failures} failed')
sys.exit(1 if failures else 0)
