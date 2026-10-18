# NNV: pictures and videos on the LCD

NNV is nnnix's own file format for the onboard 128x64 monochrome LCD, with
sound for the onboard buzzer. A picture is a video of one frame. The pieces:

| Part | What it does |
| --- | --- |
| `tools/mknnv.py` | On the build machine: turns pictures and videos into NNV through ffmpeg, makes test files, checks files |
| `/dev/lcd` | In the kernel: the display as a 1024-byte frame that a program can write |
| `/dev/audio`, `/dev/adpcm` | In the kernel: the buzzer, playing 8-bit samples at 8 kHz, or NNV sound records that its timer interrupt decodes as it plays them (`drivers/audio.c`) |
| `play` (`bin/play.c`) | On the board: the program `/bin/play`, which shows an NNV file, with its sound if asked; its code runs from flash (APROM), so it takes about 2.6 KiB of RAM while it plays, its stack and its buffers |
| `lib/nnv.h` | The format and its decoders, shared by `play` and the host test |
| `tools/nnscreen.py`, `tools/nninput.py`, `remote` | On a GNOME computer: its screen as a live NNV stream, and the board's keyboard and mouse working it; on the board, `/bin/remote` (`bin/remote.c`) shows the one and sends the other (see "This computer's screen on the LCD") |

## Quick start

```bash
python3 tools/mknnv.py convert photo.jpg photo.nnv
python3 tools/mknnv.py convert clip.mp4 clip.nnv --fps 12 --duration 20
python3 tools/nncp.py put clip.nnv /home/clip.nnv
```

Then, on the board:

```
nnnix$ play /home/photo.nnv        # stays until Enter
nnnix$ play /home/clip.nnv         # plays once
nnnix$ play -l /home/clip.nnv      # loops until Ctrl-C
nnnix$ play -l /home/clip.nnv &    # in the background; `kill %1` ends it
nnnix$ play -v /home/clip.nnv      # then: frames, time taken, frames shown late
nnnix$ play -s /home/clip.nnv      # with its sound on the buzzer (see below)
```

The console text comes back when `play` ends, however it ends. The files can
also live on the USB drive (`play /usb/clip.nnv`).

Without a file (or with `-`), `play` reads standard input, so a video can be
streamed over the network without storing it:

```
nnnix$ nc -l 1234 | play -v        # then, on the computer:
$ nc -q 1 198.18.0.2 1234 < clip.nnv     (or: python3 -c ... sendall)
```

It plays as the data comes; TCP holds the sender back while the board is
ahead, so the sender can send as fast as it likes. `-l` and a file's loop
flag do not apply (a stream cannot go back), and a picture waits for Enter
on the terminal. From a pipe `play` reads 128 bytes at a time (a pipe holds
no more), which leaves 384 bytes for the sound.

Measured with all of Bad Apple (6572 frames, 219 s) sent from the computer:

| | frames late | sound | lowest free RAM |
| --- | --- | --- | --- |
| `nc -l 1234 \| play -v`, two telnet sessions | 0 | - | 1264 B |
| `nc -l 1234 \| play -v -s`, one telnet session | 0 | 0 samples short | 1856 B |
| `nc -l 1234 \| play -v -s`, telnet and the LCD console shell | 0 | 0 samples short | 704 B |
| the same before `/dev/adpcm` (decoded samples in `/dev/audio`) | 6569 (each about 64 ms) | 0 samples short | 736 B |

## This computer's screen on the LCD

The board can be a small screen and keyboard for a computer with a GNOME
desktop. `tools/nnscreen.py` turns the computer's screen into a live NNV stream
on its standard output; `remote` (`bin/remote.c`) on the board shows that
stream and sends the board's keyboard, mouse and keypad back on the same
connection; `tools/nninput.py` takes them from its standard input and works
the computer with them:

```
nnnix$ remote                                     # on the board, then on the computer:
$ python3 tools/nnscreen.py | nc 198.18.0.2 1234 | python3 tools/nninput.py
```

The board's keypad zooms, at the mouse pointer: **2** closer, **8** farther,
**5** the whole screen again. The steps are 8, 4, 2 and 1 screen pixels per
LCD pixel (at 2, a 256x128 part of the screen, text can be read; at 1 it is
sharp); a new step keeps the point under the pointer where it was on the LCD,
and the zoomed part moves along once the pointer comes near its edge.

`remote [-v] [PORT]` waits for one connection on PORT (default 1234). Once
connected it holds `/dev/lcd` and `/dev/input` (the keyboard types nothing into
the console meanwhile), shows each frame as soon as it has come (whatever time
per frame the header gives; sound records are skipped) and sends each input
event (8 bytes, `struct nn_input_event` in `lib/sys.h`) as it comes. The
board's SW_INT button ends it (Ctrl-C too, from a telnet session); so does the
computer closing the connection. Either way the pipeline on the computer ends
too, letting go of any key still held. `-v` prints the frames, the bytes and
the events at the end. It is one task: it reads the socket and polls
`/dev/input` in the same loop and decodes the stream a piece at a time into its
one frame, so it takes about 1.2 KiB of RAM besides its stack, less than
`nc -l 1234 | play` (two tasks and a pipe). That also works, to only watch:
`nnscreen.py | nc 198.18.0.2 1234` on the computer.

How the computer's side works:

- Both scripts need a GNOME desktop session: they use Mutter's own D-Bus
  services (`tools/mutter.py`), its screen cast (the pictures, through PipeWire
  and GStreamer: `python3-gi`, `gir1.2-gstreamer-1.0`, `gstreamer1.0-pipewire`)
  and its remote desktop service (the input). Nothing is installed or set:
  GNOME shows its screen sharing indicator while they run, and stopping the
  sharing there ends them.
- `nnscreen.py` takes the newest picture `--fps` times a second (default 10),
  has GStreamer scale it to about twice the size the LCD shows of it (changed
  as the zoom changes), averages that down, stretches the grey levels (a dark
  theme's greys would otherwise all turn into the same sparse dots;
  `--no-levels` keeps them) and dithers it (Bayer by default, which keeps
  still parts still). The header gives no frame count and no time per frame,
  and a frame goes out only when the picture changed, so nothing piles up
  between the computer and the board.
- Options: `--zoom N` starts zoomed; `--area X,Y,W,H` shows that part of the
  screen instead of the whole; `--invert` suits dark themes; `--fit`,
  `--dither` and `--threshold` are as in `mknnv.py convert`. Into a file (`-o
  FILE` or a redirection) it records an ordinary timed video instead.
- `nninput.py` replays keys as the same keys (HID usages to Linux key codes;
  the computer's keyboard layout applies), buttons and the wheel as they are.
  It keeps the pointer's position itself (starting in the middle; `--speed`
  pointer pixels per mouse count, default 2) and moves the pointer there,
  because GNOME tells no program where the pointer is. It writes the position
  and the zoom to a file in `$XDG_RUNTIME_DIR` for `nnscreen.py`, which draws
  the pointer as a small arrow (GNOME's own is a dot or nothing at this size)
  and shows the part around it. A mouse used on the computer meanwhile makes
  the pointer jump back at the board's next move.
- Whoever can write to `nninput.py`'s standard input controls the computer:
  feed it only from the board.

Measured on the board, `remote -v` with the LCD console's shell and a telnet
session logged in: full 1 KiB raw frames as fast as the computer sends them go
through at about 120 frames a second (a 10 fps desktop needs far less: mostly
10-400 byte deltas, and nothing while the screen stays the same), lowest free
RAM 2848 B (1768 B with `nc -l 1234 | play` the same way). An event reaches
the computer's pointer within about 25 ms. On the computer (RK3588)
`nnscreen.py` takes about 45% of one core at 10 fps, most of it fetching and
scaling the pictures in GStreamer.

With the LCD console's shell logged in as well, the 2 KiB sound buffer does
not fit and the device takes its 1 KiB one. As 8-bit samples (`/dev/audio`)
that is 128 ms, less than the 192 ms the file's sound runs ahead of its
pictures, so every frame waited until the sound had room and was shown about
64 ms after its time. `play -s` now hands the sound records to `/dev/adpcm`,
which keeps them compressed until its interrupt plays them: 1 KiB holds
248 ms, and nothing is late. The ring it got, the samples short and the
interrupt's latency are in the kernel log (`dmesg`) when the sound closes.

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
| `--sound` | Keep the video's sound, for `play -s` (left out otherwise) |
| `--af FILTERS` | ffmpeg audio filters, default `highpass=f=200` (the buzzer has no bass) |
| `--volume X` | The sound is scaled so its loudest peaks reach full scale; above 1 it is louder and clips |

`mknnv.py info FILE [--ascii N]` checks a file, prints its frame count and
record sizes, and can draw frame N as text. `mknnv.py demo NAME OUTPUT` makes
test files: `pattern` (a picture for checking orientation: the solid block is
at the top left), `bounce` (a looping ball), `mixed` (every kind of record)
and `tone` (a rising tone and a beep, with a bar and a block in time with them).

Measured: Bad Apple, the whole 219 s at its own 30 fps (6572 frames, 85x64
in the middle of the screen), is 676 KiB with `--dither threshold`, and
1572 KiB with its sound (`--sound`); `play -s -v` reported 6572 frames in 219.027 s, none
late, with the sound playing. Sending a frame to the LCD takes about 4 ms; a
30 fps delta averages about 105 bytes.

Space: the root file system has about 1.9 MiB, and it reads at about
256 KiB/s, far more than any video needs. A 12 fps video of ordinary footage
takes roughly 1-5 KiB per second, and sound 4 KiB per second. Uploads over
HTTP (`nncp.py`) take files up to 2 MiB at about 50 KiB/s.

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

## /dev/audio and the buzzer

The board's buzzer (on PB11, on while the pin is low) is a magnetic one made
for a 2.4 kHz beep. `/dev/audio` drives it as a crude loudspeaker: PB11 is
switched to the PWM4 output, a 98 kHz carrier whose on-time follows the
samples, and the TIMER0 interrupt (the highest priority, for steady timing)
loads the next sample 8000 times a second.

How it sounds: rough. On music the buzzer gets the rhythm across, but little
of the pitch, and the parts run together. So sound is opt-in: `mknnv.py
--sound` keeps it in a file and `play -s` plays it.

- Writes take unsigned 8-bit mono samples at 8000 Hz, 128 being silence
  (`cat sound.u8 > /dev/audio` plays a raw file).
- `/dev/adpcm` is the same buzzer for NNV sound: writes take sound records
  as they are in a file (type 2, length, the decoder's state, the codes; see
  below), back to back, in pieces of any size. The timer interrupt decodes
  them as it plays, so the ring holds twice as long: 496 ms in 2 KiB, 248 ms
  in 1 KiB (each 256-sample record takes 4 bytes of the ring besides its 128
  bytes of codes). A record header that is not a sound record (or has a step
  index above 88) fails the write with `EINVAL`. `play -s` uses it.
- The kernel keeps a 2 KiB ring (256 ms of samples; 1 KiB when memory is
  short), allocated only while the device is open; writes block while it is
  full.
- One open for writing at a time, of either device (others get `EBUSY`); tasks sharing that open
  file (a program and the workers it spawns) take turns, one write at a time. The close plays out what is
  queued, or stops at once when the program was killed, and the pin goes back
  to a plain output, high, so the buzzer is silent.
- Each close logs `audio: N samples played, M short, latency up to L us,
  interrupt up to C cycles, R B ring`: M counts the samples the ring could not supply
  between the first write and the close (it stays 0 when the writer keeps
  up), L how long the timer interrupt was held off at worst, and C its longest
  run in 50 MHz clock cycles, R the ring it got.

The timer and the scheduler tick both run from HCLK, so sound and pictures
stay together however long a video is.

## File format

All numbers are little-endian.

Header, 16 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `NNV1` |
| 4 | 2 | width, 128 |
| 6 | 2 | height, 64 |
| 8 | 2 | number of frames (0: unknown; play reads to the end) |
| 10 | 2 | time per frame in milliseconds (0 for a picture; with any other frame count, a live stream: each frame shows as soon as it has come) |
| 12 | 1 | flags: bit 0, loop |
| 13 | 1 | more time per frame, in 1/256 ms (30 fps: 33 ms + 85/256) |
| 14 | 2 | sound: samples per second (8000), or 0 for none |

Then records until the end of the file: one per frame, and the sound records
between them:

| Size | Field |
| --- | --- |
| 1 | type: 0 raw, 1 delta, 2 sound; players skip other types |
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

A **sound** record holds IMA ADPCM: the predictor (i16) and the step index
(u8, 0-88) to start from, then the codes, two per byte, low nibble first, so
`2 x (length - 3)` samples. The decoder's 16-bit output becomes the device's
8-bit samples as `(value >> 8) + 128`. Each record holds 256 samples (32 ms),
the records follow each other in time, and the encoder places each one 192 ms
before the frame that shows at its start. A player queues sound records as
they come and shows each frame at its time: before showing a frame it has
queued up to 224 ms of sound, and up to about 257 ms while it reads the next
frame, and a frame (its file reads waiting behind other storage work in fsd)
may be up to 192 ms slow before the sound runs dry. `play` hands the records
to `/dev/adpcm`, whose ring holds 248 ms even when only 1 KiB is free; decoded
into 8-bit samples (`/dev/audio`) they would need 2 KiB. Files made with the
earlier 64 ms lead still play, with that much less margin.

Why this shape: the board has about 6 KiB of free RAM. A delta is applied in
place to the one frame buffer, a few bytes at a time as they are read, so
`play` needs 1 KiB for the frame and 512 bytes for reading, and no second frame.
The page layout matches the LCD controller, so a frame goes to the display with
no conversion.

## Tests

- Host (`make host-test`, `tests/host/test_audio.c`): the ring of
  `/dev/audio` and `/dev/adpcm` (`drivers/audioring.h`) fed the tone demo's
  sound records in pieces of every size from 1 to 64 bytes and larger, into
  rings of 64 B, 1 and 2 KiB drained at different rates, gives the samples
  of the Python decoder; long records split into blocks, records without
  codes, a state that jumps back (a rewind), nothing queued for a header
  alone, bad records, a full ring, PCM unchanged.
- Host (`make host-test`, `tests/host/test_nnv.c`): the C decoders against
  the frames the Python encoder started from and the sound the Python decoder
  makes, for the four demo files, fed in pieces of 1, 7, 128 bytes and whole;
  codes at their limits; codes that run past the end, records cut inside a
  code, bad headers; ADPCM steps worked out by hand and clamping.
- Board (`make test`, `nnv_tests` in `tools/nntest.py`): the video plays at its
  frame rate; one writer at a time and the device free again after `kill`; a
  picture waits for Enter; Ctrl-C; 2 s of raw sound take 2 s; a video with
  sound on time with no sound gaps, and Ctrl-C silences it at once; damaged
  and foreign files refused; files with fewer frames than their header says
  refused, also with `-l`; the last frame shown for its whole time; two tasks
  writing to one open `/dev/audio` at once; one writer between `/dev/audio`
  and `/dev/adpcm`, and raw samples refused by `/dev/adpcm`; no RAM left
  behind. The sound tests play samples
  of 0, which keep the buzzer off: they make no noise.
