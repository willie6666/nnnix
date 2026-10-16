# NNV: pictures and videos on the LCD

NNV is nnnix's own file format for the onboard 128x64 monochrome LCD. A picture
is a video of one frame. The pieces:

| Part | What it does |
| --- | --- |
| `tools/mknnv.py` | On the build machine: turns pictures and videos into NNV through ffmpeg, makes test files, checks files |
| `/dev/lcd` | In the kernel: the display as a 1024-byte frame that a program can write |
| `play` (`sdk/examples/play.c`) | On the board: shows an NNV file (a 1.5 KiB loadable program) |
| `lib/nnv.h` | The format and its decoder, shared by `play` and the host test |

## Quick start

```bash
python3 tools/mknnv.py convert photo.jpg photo.nnv
python3 tools/mknnv.py convert clip.mp4 clip.nnv --fps 12 --duration 20
make sdk
python3 tools/nncp.py put sdk/build/play.nxe /bin/play
python3 tools/nncp.py put clip.nnv /home/clip.nnv
```

Then, on the board:

```
nnnix$ play /home/photo.nnv        # stays until Enter
nnnix$ play /home/clip.nnv         # plays once
nnnix$ play -l /home/clip.nnv      # loops until Ctrl-C
nnnix$ play -l /home/clip.nnv &    # in the background; `kill %1` ends it
```

The console text comes back when `play` ends, however it ends. The files can
also live on the USB drive (`play /usb/clip.nnv`).

## Converting

`mknnv.py convert INPUT OUTPUT` reads anything ffmpeg reads. Files ending in
`.png .jpg .jpeg .bmp .webp .pgm .ppm .pbm .tif .tiff` are pictures; anything
else is a video (`--still` takes one picture from a video).

| Option | Meaning |
| --- | --- |
| `--fps N` | Video frames per second, default 12. Each frame needs 40-100 bytes when little moves, up to 1 KiB for a new scene |
| `--fit pad\|crop\|stretch` | Keep the whole picture with white bars (default), fill the screen and cut the edges, or distort |
| `--dither auto\|floyd\|atkinson\|bayer\|threshold` | `auto`: Floyd-Steinberg for pictures, ordered (Bayer) for videos, whose dots then stay put from frame to frame |
| `--threshold N` | The grey level (0-255) below which a pixel turns dark, default 128 |
| `--invert` | Swap dark and light |
| `--start S`, `--duration S` | Take part of the input |
| `--vf FILTERS` | Extra ffmpeg filters before scaling, for example `eq=contrast=1.4:brightness=0.05` |
| `--loop` | Mark a video to loop (as if `play -l`) |

`mknnv.py info FILE [--ascii N]` checks a file, prints its frame count and
record sizes, and can draw frame N as text. `mknnv.py demo NAME OUTPUT` makes
test files: `pattern` (a picture for checking orientation: the solid block is
at the top left), `bounce` (a looping ball) and `mixed` (every kind of record).

Space: the root file system has about 1.9 MiB, and it reads at about
256 KiB/s, far more than any video needs. A 12 fps video of ordinary footage
takes roughly 1-5 KiB per second.

## /dev/lcd

`/dev/lcd` is the display as one frame of 1024 bytes:

- byte `i` holds the pixels at x = `i % 128` in the 8-row band `i / 128`;
- bit 0 is the top row of the band, bit 7 the bottom; a set bit is a dark pixel.

Opening it for writing takes the display from the console: until the file is
closed, text output (the console shell, kernel notices) keeps updating the
terminal's screen contents but not the LCD; the close clears the LCD and
redraws the text. Programs that end or are killed have their files closed,
so the console always comes back. One writer at a time: another open for
writing fails with `EBUSY`.

A write goes to the file position modulo 1024, so a program writes whole
frames one after another without seeking; `lseek` positions a partial update
(a single 8-row band is 128 bytes at `band * 128`). Reading returns nothing.
From the shell, `echo hi > /dev/lcd` puts three bytes of dots in the top left
corner until the shell closes the file a moment later.

## File format

All numbers are little-endian.

Header, 16 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `NNV1` |
| 4 | 2 | width, 128 |
| 6 | 2 | height, 64 |
| 8 | 2 | number of frames (0: unknown; play reads to the end) |
| 10 | 2 | time per frame in milliseconds (0 for a picture) |
| 12 | 1 | flags: bit 0, loop |
| 13 | 3 | zero |

Then one record per frame, until the end of the file:

| Size | Field |
| --- | --- |
| 1 | type: 0 raw, 1 delta; players skip other types |
| 2 | length of the data |
| length | data |

A **raw** record holds the 1024-byte frame (the layout of `/dev/lcd`). A
**delta** record holds codes that turn the previous frame into this one; the
frame before the first one, and before the first one again after a rewind, is
blank. The codes, each followed by its operands:

| Code | Meaning |
| --- | --- |
| `0x00`-`0x7f` | `c + 1` literal bytes follow (1-128) |
| `0x80`-`0xbf` | skip `(c & 0x3f) + 1` bytes (1-64): they stay as in the previous frame |
| `0xc0`-`0xff` | one byte follows, repeated `(c & 0x3f) + 1` times (1-64) |

The codes never go past the end of the frame, and a record ends between codes.
Bytes after the last code stay as they were, so an empty delta record repeats
the previous frame. The encoder writes a raw record whenever the codes would
not be shorter.

Why this shape: the board has about 6 KiB of free RAM. A delta is applied in
place to the one frame buffer, a few bytes at a time as they are read, so
`play` needs 1 KiB for the frame and 128 bytes for reading, and no second frame.
The page layout matches the LCD controller, so a frame goes to the display with
no conversion.

## Tests

- Host (`make host-test`, `tests/host/test_nnv.c`): the C decoder against the
  frames the Python encoder started from, for the three demo files, fed in
  pieces of 1, 7, 128 bytes and whole; codes at their limits; codes that run
  past the end, records cut inside a code, bad headers.
- Board (`make test`, `nnv_tests` in `tools/nntest.py`): the video plays at its
  frame rate; one writer at a time and the device free again after `kill`; a
  picture waits for Enter; Ctrl-C; damaged and foreign files refused; no RAM
  left behind.
