# nnnix SDK

Build programs that nnnix loads from its file system and runs, without
reflashing the firmware.

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
  (blocks still allocated are freed when the program ends) and `exit`.

The C library is not linked into the program: it is the firmware's own code in
flash, reached through a function table that `_start` receives
(`lib/nnlib.h`). So `hello` takes 276 bytes of RAM for code and data.

## Limits

- Code, data and bss together: at most 8 KiB, and they must fit in free RAM
  (about 5-7 KiB with one shell logged in). Each program also gets a task
  stack, 1 KiB by default (`make STACK=2048`), and a few hundred bytes of
  kernel bookkeeping.
- No memory protection (the Cortex-M0 has no MPU): a program that writes
  through a bad pointer can damage the kernel, and a fault resets the board.
- Single precision floating point, 64-bit division and similar compiler
  helpers are not provided.

## How it works

`sdk/nxe.ld` links the program at address 0 and `ld.lld --emit-relocs` keeps
the relocations. `tools/mknxe.py` writes an NXE file: a header, the image
(code, read-only data, data) and the offsets of every 32-bit absolute address
in it (`R_ARM_ABS32`; calls and branches are PC-relative and need nothing).
The loader (`kernel/exec.c`) reads the file into one heap block, checks the
CRC-32, adds the block's address to those words, clears bss and starts
`_start(argc, argv, &nn_lib)` as a child task that owns the block.
