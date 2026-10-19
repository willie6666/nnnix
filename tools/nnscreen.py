#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""This computer's screen as an NNV stream for the board's LCD (docs/NNV.md).

  nnremote.py PORT -- [options]             board: remote COMPUTER-IP PORT
  nnscreen.py [options] | nc BOARD 1234                   board: nc -l 1234 | play

The first form listens on the computer and hands it the board's keyboard
and mouse (tools/nninput.py), and its keypad zooms the picture at the pointer: 2 closer,
8 farther, 5 the whole screen. Keypad 1 toggles relative mouse mode: the
preview stays centred and shows R instead of the synthetic pointer.
The second only shows the screen. Standard
output gets a live stream: its header gives no frame count and no time per
frame, so the board shows each frame as soon as it comes, and frames go out
only when the picture changes. Into a file (`-o FILE`, or a redirection) it
records a timed video instead, every frame, with the count filled in at the
end (Ctrl-C or --duration ends it).

Needs a GNOME desktop session (Mutter's screen cast, through PipeWire and
GStreamer). The picture is scaled to 128x64 by averaging, then dithered. The
whole screen at that size shows windows, not text; zoomed in (2: two screen
pixels per LCD pixel) text can be read. The zoomed part follows the pointer
once it comes near an edge.

  --fps N              pictures per second (default 10)
  --zoom N             start zoomed: N screen pixels per LCD pixel, one of
                       8 4 2 1 (default 0: the whole screen); the board's
                       keypad changes it while nninput.py runs
  --area X,Y,W,H       show this part of the screen (in screen pixels) instead
                       of the whole screen
  --monitor NAME       a monitor other than the primary one (such as HDMI-1)
  --fit pad|crop|stretch, --dither bayer|threshold|floyd|atkinson,
  --threshold N, --invert          as in mknnv.py convert
  --no-levels          keep the grey levels (by default they are stretched to
                       the whole range: a dark theme's greys stay apart)
  --no-cursor          do not draw the pointer
  --duration S, -o FILE
"""
import argparse
import os
import signal
import stat
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mknnv  # noqa: E402
import mutter  # noqa: E402

W, H = mknnv.W, mknnv.H

# The pointer as drawn on the LCD: 1 dark, 2 light, its tip at the top left.
ARROW = ['1', '11', '121', '1221', '12221', '122221', '1222111', '12121', '11 11', '    11']


class View:
    """Which part of the screen to show (x, y, w, h in screen pixels). Zoomed,
    it is 128 x 64 times the zoom; a new zoom keeps the point under the
    pointer where it was on the LCD, and the part moves along once the pointer
    comes within an eighth of an edge."""

    def __init__(self, screen, area=None):
        self.screen, self.area = screen, area
        self.zoom, self.cur = None, None

    def whole(self):
        sw, sh = self.screen
        if not self.area:
            return 0, 0, sw, sh
        x, y, w, h = self.area
        x, y = min(max(x, 0), sw - 1), min(max(y, 0), sh - 1)
        return x, y, max(1, min(w, sw - x)), max(1, min(h, sh - y))

    def region(self, zoom, pointer, centered=False):
        sw, sh = self.screen
        if not zoom:
            r = self.whole()
        else:
            w, h = min(sw, W * zoom), min(sh, H * zoom)
            px, py = pointer if pointer else (sw / 2, sh / 2)
            if self.cur is None or centered:
                x, y = px - w / 2, py - h / 2
            elif zoom != self.zoom:                      # the point under the pointer stays put
                ox, oy, ow, oh = self.cur
                x, y = px - (px - ox) * w / ow, py - (py - oy) * h / oh
            else:
                x, y = self.cur[:2]
            mx, my = w / 8, h / 8                        # and the pointer stays away from the edges
            x = px - mx if px < x + mx else px - w + mx if px > x + w - mx else x
            y = py - my if py < y + my else py - h + my if py > y + h - my else y
            r = int(min(max(x, 0), sw - w)), int(min(max(y, 0), sh - h)), w, h
        self.zoom, self.cur = zoom, r
        return r


def scale_for(region):
    """How far the stream is scaled down for a region: to about twice what
    the LCD shows of it, so GStreamer does the heavy part of the averaging."""
    _, _, w, h = region
    return min(1.0, 2 * max(W / w, H / h))


def shrink(gray, ow, oh):
    """Average gray (rows) down to oh x ow (or repeat its pixels where it is
    smaller: just after a zoom, until the stream comes at its new scale)."""
    h, w = gray.shape
    if oh > h:
        gray, h = gray[np.arange(oh) * h // oh], oh
    if ow > w:
        gray, w = gray[:, np.arange(ow) * w // ow], ow
    rows = np.linspace(0, h, oh + 1).astype(int)
    cols = np.linspace(0, w, ow + 1).astype(int)
    rows, cols = rows[:-1], cols[:-1]
    s = np.add.reduceat(gray.astype(np.uint32), rows, axis=0)
    s = np.add.reduceat(s, cols, axis=1)
    count = np.outer(np.diff(np.append(rows, h)), np.diff(np.append(cols, w)))
    return (s // count).astype(np.uint8)


def picture(gray, area, fit, f, tone):
    """The area (in screen pixels) of gray (the screen scaled by f) as a 64x128
    grey picture, its levels changed by tone (but not those of the bars of
    --fit pad), and how it was placed: (scale x, scale y, offset x, offset y,
    area x, area y) from screen to LCD pixels."""
    x, y, w, h = (round(v * f) for v in area)
    part = gray[y:y + h, x:x + w]
    h, w = part.shape                                        # the stream may be smaller than the monitor
    if not w or not h:
        return np.full((H, W), 255, dtype=np.uint8), (1, 1, 0, 0, 0, 0)
    out = np.full((H, W), 255, dtype=np.uint8)               # pad: light bars
    ox = oy = 0
    if fit == 'stretch':
        out = tone(shrink(part, W, H))
        sx, sy = W / w, H / h
    elif fit == 'crop':
        s = max(W / w, H / h)
        cw, ch = min(w, round(W / s)), min(h, round(H / s))
        cx, cy = (w - cw) // 2, (h - ch) // 2
        out = tone(shrink(part[cy:cy + ch, cx:cx + cw], W, H))
        sx, sy, x, y = W / cw, H / ch, x + cx, y + cy
    else:
        s = min(W / w, H / h)
        ow, oh = max(1, min(W, round(w * s))), max(1, min(H, round(h * s)))
        ox, oy = (W - ow) // 2, (H - oh) // 2
        out[oy:oy + oh, ox:ox + ow] = tone(shrink(part, ow, oh))
        sx, sy = ow / w, oh / h
    return out, (sx * f, sy * f, ox, oy, x / f, y / f)


class Levels:
    """Stretches the picture's grey levels to the whole range, so a dark desktop
    theme's greys do not all turn into the same dots. The range follows the
    picture slowly, so it does not flicker as windows change."""

    def __init__(self):
        self.lo = self.hi = None

    def __call__(self, grey):
        lo, hi = np.percentile(grey, (1, 99))
        if self.lo is None:
            self.lo, self.hi = lo, hi
        self.lo += (lo - self.lo) * 0.25
        self.hi += (hi - self.hi) * 0.25
        lo, hi = self.lo, max(self.hi, self.lo + 64)       # a plain screen stays as it is
        if hi > 255:
            lo, hi = 255 - (hi - lo), 255
        margin = (hi - lo) * 0.08                            # near the ends: solid, not a few stray dots
        lo, hi = lo + margin, hi - margin
        return np.clip((grey.astype(np.float32) - lo) * (255 / (hi - lo)), 0, 255).astype(np.uint8)


def draw_arrow(dark, pointer, place):
    sx, sy, ox, oy, x0, y0 = place
    px = int((pointer[0] - x0) * sx) + ox
    py = int((pointer[1] - y0) * sy) + oy
    for dy, row in enumerate(ARROW):
        for dx, c in enumerate(row):
            if c != ' ' and 0 <= px + dx < W and 0 <= py + dy < H:
                dark[py + dy, px + dx] = c == '1'


def header(count, frame_ms):
    units = round(frame_ms * 256)
    return b'NNV1' + struct.pack('<HHHHBBH', W, H, count, units >> 8, 0, units & 0xff, 0)


def draw_relative(dark):
    """A small inverse R at the top right; the synthetic arrow is hidden."""
    dark[:7, -5:] = True
    for y, row in enumerate(('110', '101', '110', '101', '101'), 1):
        for x, c in enumerate(row, W - 4):
            dark[y, x] = c != '1'


def parse_area(text):
    try:
        v = [int(p) for p in text.split(',')]
    except ValueError:
        v = []
    if len(v) != 4 or v[2] <= 0 or v[3] <= 0:
        raise argparse.ArgumentTypeError('X,Y,W,H with W and H above 0')
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fps', type=float, default=10)
    ap.add_argument('--area', type=parse_area, metavar='X,Y,W,H')
    ap.add_argument('--zoom', type=int, default=0, choices=mutter.ZOOMS)
    ap.add_argument('--monitor', default='')
    ap.add_argument('--fit', choices=('pad', 'crop', 'stretch'), default='pad')
    ap.add_argument('--dither', choices=('bayer', 'threshold', 'floyd', 'atkinson'), default='bayer')
    ap.add_argument('--threshold', type=int, default=128)
    ap.add_argument('--invert', action='store_true')
    ap.add_argument('--no-levels', action='store_true')
    ap.add_argument('--no-cursor', action='store_true')
    ap.add_argument('--duration', type=float)
    ap.add_argument('-o', '--output', type=Path)
    args = ap.parse_args()
    if not 0.5 <= args.fps <= 60:
        ap.error('--fps: 0.5 to 60')

    out = open(args.output, 'wb') if args.output else sys.stdout.buffer
    if out.isatty():
        ap.error('standard output is a terminal: pipe it into nc (see --help), or use -o FILE')
    record = stat.S_ISREG(os.fstat(out.fileno()).st_mode)

    try:
        session = mutter.Session(args.monitor, mutter.CURSOR_EMBEDDED)
    except mutter.Error as e:
        sys.exit(f'nnscreen: {e}')
    view = View(session.size, args.area)
    frames = mutter.Frames(session, args.fps, scale_for(view.region(args.zoom, None)))
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    period = 1 / args.fps
    levels = None if args.no_levels else Levels()

    def tone(grey):
        if levels:
            grey = levels(grey)
        return 255 - grey if args.invert else grey

    sent = count = status = 0
    prev = None
    try:
        gray = frames.latest(5)
        if gray is None:
            sys.exit('nnscreen: no picture from the screen cast')
        out.write(header(0, period * 1000 if record else 0))
        start = due = time.monotonic()
        while args.duration is None or time.monotonic() - start < args.duration:
            state = mutter.read_state()
            pointer, zoom = (state[:2], state[2]) if state else (None, args.zoom)
            relative = state[3] if state else False
            area = view.region(zoom, pointer, centered=relative)
            frames.set_scale(scale_for(area))
            gray = frames.latest()
            f = gray.shape[1] / session.size[0]          # what came: the scale may have just changed
            grey, place = picture(gray, area, args.fit, f, tone)
            dark = mknnv.dither(grey, args.dither, args.threshold)
            if relative:
                draw_relative(dark)
            elif pointer and not args.no_cursor:
                draw_arrow(dark, pointer, place)
            frame = mknnv.pack(dark)
            if record or frame != prev:
                codes = mknnv.delta(prev or bytes(mknnv.FRAME), frame)
                if len(codes) >= mknnv.FRAME:
                    rec = struct.pack('<BH', mknnv.RAW, mknnv.FRAME) + frame
                else:
                    rec = struct.pack('<BH', mknnv.DELTA, len(codes)) + codes
                out.write(rec)
                out.flush()
                sent += len(rec)
                count += 1
                prev = frame
            due += period
            wait = due - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            else:
                due = time.monotonic()                       # behind: no catching up
    except KeyboardInterrupt:
        pass
    except BrokenPipeError:                                  # nc ended: the board closed the connection
        os.dup2(os.open(os.devnull, os.O_WRONLY), out.fileno())
    except mutter.Error as e:
        print(f'nnscreen: {e}', file=sys.stderr)
        status = 1
    finally:
        frames.stop()
        session.stop()
        try:
            if record and count < 65536:
                out.seek(8)
                out.write(struct.pack('<H', count))
            out.flush()
        except (OSError, ValueError):
            pass
        if record:
            print(f'nnscreen: {count} frames, {sent} bytes', file=sys.stderr)
    return status


if __name__ == '__main__':
    sys.exit(main())
