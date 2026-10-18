# nnnix SDK

Build programs that nnnix loads from its file system and runs, without
reflashing the firmware. Their code runs straight from the chip's flash
(APROM), so a program costs RAM only for its data, its stack and what it
allocates.

```sh
make -C sdk                                   # examples/*.c -> sdk/build/*.nxe
sudo python3 tools/nncp.py --iface eth1 put sdk/build/hello.nxe /bin/hello
```

Then on the board:

```
nnnix$ hello one two
hello from a loaded program, pid 9
argv[1] = one
argv[2] = two
nnnix$ echo shout | upper
SHOUT
```

A command that is not built in is looked up as `/bin/NAME`; a name with a `/`
(`/usb/tool.nxe`, `./x`) is used as a path. Programs run as child tasks, so
they work in pipelines, with redirection, in the background (`&`), and stop on
Ctrl-C or `kill`.

## Writing a program

```c
#include "nnnix.h"

int main(int argc, char **argv)
{
    printf("%d arguments\n", argc - 1);
    return 0;                         /* the exit status, $? in the shell */
}
```

Put it in `sdk/examples/` (or build it from the current directory with
`make -f sdk/Makefile build/NAME.nxe`). `nnnix.h` provides:

- the system calls of `lib/sys.h`: `sys_read`, `sys_write`, `sys_open`,
  `sys_sleep`, `sys_socket`, `sys_time`, ... (fd 0 and 1 are standard input and
  output, which the shell connects to the terminal, files or pipes);
- `printf`, `snprintf`, `puts`, `mem*`, `str*`, `errstr`, `malloc`/`free`
  (blocks still allocated are freed when the program ends) and `exit`;
- `sys_spawn` starts a function of the program as another task
  (`examples/spawn.c`); it shares the program's data and its malloc'd
  memory, which stay until the last of them has ended. When the program ends,
  its children are stopped at their next system call.
- `examples/memtest.c` checks that heap blocks keep their contents over time.
- `examples/relocs.c` holds every kind of address a program can have in its
  tables and checks them as it runs (a test).
- `examples/audioshare.c` has a worker and the program write to one open
  `/dev/audio` at once (a test; its samples are silent).
- `examples/readzero.c` checks that reads of 0 bytes return at once (a test).
- `examples/keys.c` prints the events of `/dev/input`: keyboard keys pressed
  and released, the mouse, the board's keypad ([docs/USER_ENV.md](../docs/USER_ENV.md),
  "Keyboard, mouse and keys for programs").
- The games in `bin/` (`snake.c`, `tetris.c`, `flappy.c`) and what they
  share (`bin/game.c`) show how a game works: `/dev/input` opened with
  `O_NONBLOCK` and read between frames, the picture drawn in a 1024-byte
  buffer and written whole to `/dev/lcd`, text in the firmware's fonts. The
  board's SW_INT button ends a game that hangs.

The C library is not linked into the program: it is the firmware's own code in
flash, reached through a function table that `_start` receives
(`lib/nnlib.h`); `nn_write_all`, `fputs_fd` and `nn_interrupted` (Ctrl-C) are
small helpers in `crt0.c`. Version 2 of the table also gives the console's
fonts (`__nn_lib->font3x5`, `font5x7`), so text costs a program no code. `hello` has 232 bytes of code and needs 8 bytes of
RAM for its data.

The system's own programs (`play`, `ed`, `wc`, `head`, `tail`, `grep`,
`hexdump`, `wget`, `host`, and the games `snake`, `tetris`, `flappy`) are built the same way, from `bin/` at the top of the repository
(`make programs`, into `build/bin/`), and installed in `/bin` by
`tools/nnupdate.py --bin build/bin`.

## Limits

- Code (text: code and read-only data): at most 64 KiB, and the APROM above the
  firmware holds about 28.5 KiB of programs at a time (what the firmware leaves). Data and bss: at most
  8 KiB, in RAM. Each program also gets a task stack, 1 KiB by default
  (`make STACK=2048`), and a few hundred bytes of kernel bookkeeping. When
  every page of the APROM area belongs to a program that is running, a new
  program's code goes to RAM instead (and must then fit there too).
- Build with `-fropi -frwpi` (the SDK's Makefile does): r9 holds the address of
  the program's data and must not be used otherwise. Debug information (`-g`)
  does not link with `-frwpi` and is left out.
- No memory protection (the Cortex-M0 has no MPU): a program that writes
  through a bad pointer can damage the kernel, and a fault resets the board.
- Single precision floating point, 64-bit division and similar compiler
  helpers are not provided.

## How it works

Built with `-fropi -frwpi`, code and read-only data (text) reach each other
PC-relative, and data and bss are reached through r9, the static base. So
text runs at any address, and one copy serves every run of the program, also
two at once, each with its own data. `sdk/nxe.ld` links text at 0 and data at
0x10000000 (in a segment of its own, so r9-relative offsets count from the data
start), and `ld.lld --emit-relocs` keeps the relocations. `tools/mknxe.py`
writes an NXE file: a header, the text and the offsets of its words that hold
a text address (tables of pointers to strings or functions), then the data
and the offsets of its words that hold an address (marked when it points into
data or bss). Anything else that would need changing when the program moves
is refused at build time.

The loader (`kernel/exec.c`) looks for the text in APROM above the firmware
(`kernel/xip.c`): a slot holding text with the same CRC-32 (`text_crc`) is used
once the text in it still matches the CRC taken when it was written;
otherwise one is written, its relocations applied on the way and the text
checked against its CRC. The header and the data are checked against
`data_crc` on every run (it covers the header up to itself, so a changed entry
point, size or relocation count is refused too). This is NXE version 3; files
of version 2 must be rebuilt. A slot stays across reboots and is written only when
the program is new or changed; the oldest slots make room for new ones, but
never one a running program uses. Writing costs 20 ms with interrupts off per
512-byte page erased (the flash allows nothing else meanwhile), about 0.3 s
for a 7 KiB program. The data then comes from the file into a RAM block, the
loader adds the block's address (or the text's) to the listed words, clears
bss and starts `_start(argc, argv, &nn_lib)` with r9 at the block, as a child
task; the block goes with the last task that uses it. `cat /proc/xip` lists
the area and its slots (address, text size, CRC, running programs).
