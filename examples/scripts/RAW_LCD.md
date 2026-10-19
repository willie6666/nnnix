# Generate LCD pixels in nsh

Run `sh /home/nsh-examples/raw-box.sh` on the board. The script calculates
a filled rectangle, x=24..103 and y=16..47, using `if`, `while` and `inc`.
`printf '%b'` turns escape sequences into bytes, including NUL, and writes
a complete 1024-byte frame to `/home/.nsh-box.raw`. An optional first argument
chooses another output path. There is no precomputed bitmap or LCD command.
Generation took about nine seconds on the development board.

The script batches 32 pixels per append to avoid opening the file for every
byte. It uses six shell variables, then displays the generated frame with:

```
cat /home/.nsh-box.raw /dev/tty > /dev/lcd
```

After copying the bitmap, `cat` waits on `/dev/tty` with the display still
open. Ctrl-D on an empty line ends it normally; Ctrl-C or SW_INT cancels the
script and restores the terminal. The small generated file remains for reuse.
Entering other text writes those bytes as pixels. Closing `/dev/lcd` restores
the terminal immediately, so `printf '\xff' > /dev/lcd` alone only flashes.

`/dev/lcd` remains a raw 128x64 one-bit display. Byte `x + 128*(y/8)` contains
pixel (x,y) in bit `y%8`. It does not interpret text or drawing commands.

Binary output examples (single quotes preserve backslashes through nsh):

```
printf '\0\x7f\xff' > /home/three.raw
printf '%B' 0 127 255 >> /home/three.raw
set BYTE 128
printf '%B' "$BYTE" >> /home/three.raw
unset BYTE
```

`%B` is the nsh numeric-byte extension: 0..255, decimal or `0x` hexadecimal.
`%s`, `%b`, `%c`, `%d`, `%i`, `%u`, `%x`, `%X` and `%%` are also supported.
No newline is added. Variables and script arguments remain NUL-terminated
strings; NUL bytes are created only at output time.
