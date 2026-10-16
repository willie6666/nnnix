#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make NNV pictures and videos for the board's 128x64 LCD (lib/nnv.h, docs/NNV.md).

  mknnv.py convert INPUT OUT.nnv      a picture or a video, through ffmpeg
  mknnv.py demo NAME OUT.nnv          a generated test file (see DEMOS)
  mknnv.py info FILE.nnv [--ascii N]  check a file, optionally draw frame N

Then copy it to the board (tools/nncp.py put OUT.nnv /OUT.nnv) and run
`play /OUT.nnv` there.

Pictures are scaled to fit 128x64 (or cropped or stretched) and turned into
black and white by dithering. Floyd-Steinberg looks best on pictures; on video
its dots crawl from frame to frame, so videos get the steadier ordered (Bayer)
dither unless told otherwise.
"""
import argparse
import os
import random
import struct
import subprocess
import sys

import numpy as np

W, H = 128, 64
FRAME = W * H // 8
RAW, DELTA = 0, 1
LOOP = 0x01
STILL = {'.png', '.jpg', '.jpeg', '.bmp', '.webp', '.pgm', '.ppm', '.pbm', '.tif', '.tiff'}


# ---- frames ----
def pack(dark):
    """A 64x128 bool array (True = dark) to the LCD's 1024-byte layout."""
    bands = dark.reshape(8, 8, W).astype(np.uint8)          # band, row in band, x
    weights = (1 << np.arange(8, dtype=np.uint16)).reshape(1, 8, 1)
    return bytes((bands * weights).sum(axis=1).astype(np.uint8).reshape(-1))


def unpack(frame):
    b = np.frombuffer(frame, dtype=np.uint8).reshape(8, 1, W)
    bits = (b >> np.arange(8, dtype=np.uint8).reshape(1, 8, 1)) & 1
    return bits.reshape(H, W).astype(bool)


BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]])


def dither(gray, method, threshold=128):
    """gray: uint8 rows, 0 black .. 255 white. Returns True where dark."""
    g = gray.astype(np.float32)
    h, w = g.shape
    if method == 'threshold':
        return g < threshold
    if method == 'bayer':
        limit = (np.tile(BAYER, (h // 8 + 1, w // 8 + 1))[:h, :w] + 0.5) * 4.0
        return g < limit
    # Error diffusion, row by row.
    if method == 'floyd':
        spread = ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16))
    else:                                                    # atkinson: lighter, crisper
        spread = ((1, 0, 1 / 8), (2, 0, 1 / 8), (-1, 1, 1 / 8), (0, 1, 1 / 8), (1, 1, 1 / 8), (0, 2, 1 / 8))
    rows = [list(map(float, r)) for r in g] + [[0.0] * w, [0.0] * w]
    out = np.zeros((h, w), dtype=bool)
    for y in range(h):
        row = rows[y]
        for x in range(w):
            old = row[x]
            dark = old < threshold
            out[y, x] = dark
            err = old - (0.0 if dark else 255.0)
            for dx, dy, f in spread:
                if 0 <= x + dx < w:
                    rows[y + dy][x + dx] += err * f
    return out


# ---- encoding ----
def delta(prev, cur):
    """Codes that turn prev into cur (lib/nnv.h)."""
    out = bytearray()
    lit = bytearray()

    def flush():
        if lit:
            out.append(len(lit) - 1)
            out.extend(lit)
            lit.clear()

    i = 0
    while i < FRAME:
        j = i
        while j < FRAME and cur[j] == prev[j]:
            j += 1
        if j == FRAME:                                       # the rest stays: say nothing
            break
        if j - i >= 2:
            flush()
            for k in range(i, j, 64):
                out.append(0x80 | (min(64, j - k) - 1))
            i = j
            continue
        j = i
        while j < FRAME and cur[j] == cur[i]:
            j += 1
        if j - i >= 3:
            flush()
            for k in range(i, j, 64):
                out += bytes((0xc0 | (min(64, j - k) - 1), cur[i]))
            i = j
            continue
        lit.append(cur[i])
        if len(lit) == 128:
            flush()
        i += 1
    flush()
    return bytes(out)


def apply(prev, codes):
    """Python decoder, the reference for the C one."""
    f = bytearray(prev)
    pos = i = 0
    while i < len(codes):
        c = codes[i]
        i += 1
        n = c + 1 if c < 0x80 else (c & 0x3f) + 1
        if pos + n > FRAME:
            raise ValueError('code runs past the end of the frame')
        if c < 0x80:
            if i + n > len(codes):
                raise ValueError('literal cut short')
            f[pos:pos + n] = codes[i:i + n]
            i += n
        elif c >= 0xc0:
            if i >= len(codes):
                raise ValueError('fill cut short')
            f[pos:pos + n] = bytes([codes[i]]) * n
            i += 1
        pos += n
    return bytes(f)


def encode(frames, frame_ms, flags=0):
    """frame_ms may have a fraction: the header keeps it in 1/256 ms."""
    frames = list(frames)
    units = round(frame_ms * 256)
    out = bytearray(b'NNV1' + struct.pack('<HHHHBB2x', W, H, len(frames) if len(frames) < 65536 else 0,
                                          units >> 8, flags, units & 0xff))
    prev = bytes(FRAME)
    for f in frames:
        codes = delta(prev, f)
        if len(codes) >= FRAME:
            out += struct.pack('<BH', RAW, FRAME) + f
        else:
            out += struct.pack('<BH', DELTA, len(codes)) + codes
        prev = f
    return bytes(out)


def decode(data):
    """Header fields and the frames of an NNV file; raises ValueError if damaged."""
    if len(data) < 16 or data[:4] != b'NNV1':
        raise ValueError('not an NNV file')
    w, h, count, frame_ms, flags, frac = struct.unpack_from('<HHHHBB', data, 4)
    frame_ms += frac / 256
    if (w, h) != (W, H):
        raise ValueError(f'{w}x{h}, not {W}x{H}')
    frames, sizes = [], []
    prev = bytes(FRAME)
    at = 16
    while at < len(data):
        if at + 3 > len(data):
            raise ValueError('record header cut short')
        kind, n = struct.unpack_from('<BH', data, at)
        body = data[at + 3:at + 3 + n]
        if len(body) != n:
            raise ValueError('record cut short')
        at += 3 + n
        if kind == RAW:
            if n != FRAME:
                raise ValueError('raw frame of the wrong size')
            prev = bytes(body)
        elif kind == DELTA:
            prev = apply(prev, body)
        else:
            continue
        frames.append(prev)
        sizes.append(3 + n)
    if count and count != len(frames):
        raise ValueError(f'header says {count} frames, found {len(frames)}')
    return {'frame_ms': frame_ms, 'flags': flags, 'frames': frames, 'sizes': sizes}


# ---- input through ffmpeg ----
def ffmpeg_frames(path, fps, fit, still, start, duration, vf, background):
    if fit == 'pad':
        geom = (f'scale={W}:{H}:force_original_aspect_ratio=decrease,'
                f'pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:color={background}')
    elif fit == 'crop':
        geom = f'scale={W}:{H}:force_original_aspect_ratio=increase,crop={W}:{H}'
    else:
        geom = f'scale={W}:{H}'
    chain = ([] if still else [f'fps={fps}']) + ([vf] if vf else []) + [geom, 'format=gray']
    cmd = ['ffmpeg', '-v', 'error', '-nostdin']
    if start:
        cmd += ['-ss', str(start)]
    cmd += ['-i', path]
    if duration:
        cmd += ['-t', str(duration)]
    if still:
        cmd += ['-frames:v', '1']
    cmd += ['-vf', ','.join(chain), '-f', 'rawvideo', '-pix_fmt', 'gray', '-']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = W * H
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        yield np.frombuffer(buf, dtype=np.uint8).reshape(H, W)
    if proc.wait():
        raise SystemExit(f'mknnv: ffmpeg failed on {path}')


# ---- demos ----
def demo_pattern():
    """Orientation and layout check: a corner block at the top left, a frame,
    a diagonal, circles, and a grey ramp along the bottom."""
    d = np.zeros((H, W), dtype=bool)
    d[0, :] = d[-1, :] = d[:, 0] = d[:, -1] = True
    d[2:10, 2:10] = True                                     # top left
    for x in range(W):
        d[x * (H - 1) // (W - 1), x] = True
    yy, xx = np.mgrid[0:H, 0:W]
    r = np.hypot(xx - 96, yy - 24)
    d |= (np.abs(r - 14) < 0.7) | (np.abs(r - 7) < 0.7)
    ramp = np.tile(np.linspace(255, 0, W - 8), (10, 1))
    d[50:60, 4:W - 4] = dither(ramp, 'bayer')
    return [pack(d)], 0, 0


def demo_bounce():
    """A ball bouncing around a frame, looping (sizes in the 30-60 byte range)."""
    frames = []
    x, y, dx, dy, r = 20.0, 15.0, 2.5, 1.5, 6
    yy, xx = np.mgrid[0:H, 0:W]
    for _ in range(200):
        d = np.zeros((H, W), dtype=bool)
        d[0, :] = d[-1, :] = d[:, 0] = d[:, -1] = True
        d |= np.hypot(xx - x, yy - y) <= r
        frames.append(pack(d))
        x += dx
        y += dy
        if not r + 1 <= x <= W - 2 - r:
            dx = -dx
        if not r + 1 <= y <= H - 2 - r:
            dy = -dy
    return frames, 50, LOOP


def demo_mixed():
    """Every kind of record and code: noise (raw frames), small changes, fills
    and long skips, repeated frames, a blank frame, a full black frame."""
    rnd = random.Random(140)
    frames = []
    cur = bytearray(FRAME)
    for n in range(60):
        kind = n % 6
        if kind == 0:
            cur = bytearray(rnd.getrandbits(8) for _ in range(FRAME))
        elif kind == 1:
            for _ in range(rnd.randrange(1, 40)):
                cur[rnd.randrange(FRAME)] = rnd.getrandbits(8)
        elif kind == 2:
            a = rnd.randrange(FRAME)
            b = rnd.randrange(a, FRAME + 1)
            cur[a:b] = bytes([rnd.getrandbits(8)]) * (b - a)
        elif kind == 3:
            pass                                             # the same frame again
        elif kind == 4:
            cur = bytearray(FRAME) if n % 12 == 4 else bytearray(b'\xff' * FRAME)
        else:
            for i in range(rnd.randrange(FRAME), FRAME, rnd.randrange(2, 200)):
                cur[i] ^= 1 << rnd.randrange(8)
        frames.append(bytes(cur))
    return frames, 40, 0


DEMOS = {'pattern': demo_pattern, 'bounce': demo_bounce, 'mixed': demo_mixed}


def ascii_art(frame):
    return '\n'.join(''.join('#' if v else '.' for v in row) for row in unpack(frame))


def write(path, data, frames, raw):
    with open(path, 'wb') as f:
        f.write(data)
    if raw:
        with open(raw, 'wb') as f:
            f.write(b''.join(frames))
    print(f'{path}: {len(frames)} frame(s), {len(data)} bytes')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('convert', help='a picture or a video, through ffmpeg')
    c.add_argument('input')
    c.add_argument('output')
    c.add_argument('--fps', type=float, default=12, help='video frames per second (default 12)')
    c.add_argument('--fit', choices=('pad', 'crop', 'stretch'), default='pad',
                   help='keep the whole picture (pad), fill the screen (crop), or distort (stretch)')
    c.add_argument('--dither', choices=('auto', 'floyd', 'atkinson', 'bayer', 'threshold'), default='auto')
    c.add_argument('--threshold', type=int, default=128, help='grey level that turns dark (0-255)')
    c.add_argument('--invert', action='store_true', help='swap dark and light')
    c.add_argument('--still', action='store_true', help='take one picture even from a video')
    c.add_argument('--start', type=float, help='skip this many seconds of the input')
    c.add_argument('--duration', type=float, help='seconds to take')
    c.add_argument('--vf', help='extra ffmpeg filters before scaling, e.g. eq=contrast=1.5')
    c.add_argument('--background', default='white', help='padding colour (default white)')
    c.add_argument('--loop', action='store_true', help='mark the video to loop')
    c.add_argument('--raw', help=argparse.SUPPRESS)
    d = sub.add_parser('demo', help='a generated test file: ' + ', '.join(DEMOS))
    d.add_argument('name', choices=sorted(DEMOS))
    d.add_argument('output')
    d.add_argument('--raw', help='also write the frames, back to back (host test reference)')
    i = sub.add_parser('info', help='check a file')
    i.add_argument('file')
    i.add_argument('--ascii', type=int, metavar='N', help='draw frame N')
    a = ap.parse_args()

    if a.cmd == 'convert':
        if not os.path.exists(a.input):
            raise SystemExit(f'mknnv: {a.input}: no such file')
        still = a.still or os.path.splitext(a.input)[1].lower() in STILL
        method = a.dither if a.dither != 'auto' else ('floyd' if still else 'bayer')
        frames = []
        for gray in ffmpeg_frames(a.input, a.fps, a.fit, still, a.start, a.duration, a.vf, a.background):
            dark = dither(gray, method, a.threshold)
            frames.append(pack(~dark if a.invert else dark))
            if len(frames) % 50 == 0:
                print(f'\r{len(frames)} frames', end='', file=sys.stderr, flush=True)
        if len(frames) >= 50:
            print(file=sys.stderr)
        if not frames:
            raise SystemExit(f'mknnv: {a.input}: no frames')
        frame_ms = 0 if len(frames) == 1 else 1000 / a.fps
        write(a.output, encode(frames, frame_ms, LOOP if a.loop else 0), frames, a.raw)
    elif a.cmd == 'demo':
        frames, frame_ms, flags = DEMOS[a.name]()
        write(a.output, encode(frames, frame_ms, flags), frames, a.raw)
    else:
        data = open(a.file, 'rb').read()
        try:
            v = decode(data)
        except ValueError as e:
            raise SystemExit(f'mknnv: {a.file}: {e}')
        n = len(v['frames'])
        sizes = v['sizes']
        kind = 'picture' if n == 1 else f'video, {v["frame_ms"]:.2f} ms per frame, {n * v["frame_ms"] / 1000:.1f} s'
        print(f'{a.file}: {kind}, {n} frame(s), {len(data)} bytes'
              f'{", loops" if v["flags"] & LOOP else ""}')
        if n > 1:
            print(f'record bytes: min {min(sizes)}, mean {sum(sizes) / n:.0f}, max {max(sizes)}')
        if a.ascii is not None:
            print(ascii_art(v['frames'][a.ascii]))


if __name__ == '__main__':
    main()
