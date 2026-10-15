# nnnix

**NuvotoN NUC UNIX**: a tiny UNIX-like operating system for the Nuvoton NUC140 (Cortex-M0, 16 KiB SRAM,
128 KiB flash) on the NU-LB_002 board.

Goals: three consoles (telnet, LCD + USB keyboard, serial), preemptive multitasking, file systems on SPI
flash and a USB drive, and later stand-alone loadable programs. The full plan is in [docs/PLAN.md](docs/PLAN.md).

## Status

**M2 telnet console: done** (M0 foundation and M1 kernel core done before it).

```
$ telnet 198.18.0.2
nnnix m2 NUC140VE3CN
Type 'help' for a list of commands.
nnnix$ ps
  PID NAME     PRI STATE  STACK USED/SIZE   CPU MS
    1 idle       7 run            80/384       7293
    2 netd       1 wait          208/512          5
    3 telnetd    3 sleep         540/768          3
    4 input      2 wait          160/768          0
    5 diag       3 wait          804/1280         0
    6 mon        4 sleep         524/896        266
    7 nsh        5 run           704/1280        12
```

- Preemptive multitasking: fixed priorities with round-robin among equals (10 ms slice; a preempted task resumes its slice), PendSV switching, 1 ms tick, sleep and timeouts
- Synchronisation: wait queues, mutexes, semaphores (can be given from interrupts); SPI bus lock and CH376S device lock
- System calls: entered with `svc`; the kernel function runs in the caller's own task context, so it may block; user-side interface in `lib/sys.h`
- Terminals: canonical line editing (backspace, Ctrl-U, Ctrl-C, Ctrl-D, arrow keys ignored) over a byte backend
- Network: interrupt-driven W5500 (`netd` task), socket layer with self re-arming listeners, telnet server with three sessions
- Shell `nsh`: `help echo ps free uptime dmesg uname sleep clear reboot exit`, using only system calls
- Memory: first-fit kmalloc with coalescing; task stacks get red-zone and canary checks and an overflow panics
- Crash records: HardFaults and panics are saved in `.noinit` and reported after the reboot; the boot log shows the reset cause
- Drivers: CH376S (USB drive, FAT), W5500 (hardware TCP/IP), CH9350 (USB keyboard/mouse, interrupt-driven receive)

| On-target tests (`make test`, 28 checks) | Result |
| --- | --- |
| Two telnet sessions: one runs `sleep 2`, the other answers | in 8 ms |
| Line editing, Ctrl-C, Ctrl-D, three sessions, log-out and reconnect, heap leak check | pass |
| W5500 send / receive (SPI 25 MHz, 8 KiB socket buffer) | 834 KiB/s / 835 KiB/s, verified byte by byte |
| CH376S write / read 256 KiB (SPI 12.5 MHz) | 100 kB/s / 230 kB/s, 0 errors |
| Preemption: worst lateness of a 10 ms ticker with the CPU saturated | 0 ms |
| Round-robin between two equal-priority CPU hogs | 500 / 500 ms |
| Stack overflow detection, HardFault capture, reported after reboot | pass |
| Code size / static RAM / heap | 21.4 KiB / 3.7 KiB / 11.4 KiB (5.5 KiB free with one telnet session) |

## Hardware

NU-LB_002 Rev 2.1 (NUC140VE3CN). The external modules run from an MB-102 5 V supply that shares ground with the board:

| Module | NUC140 pins |
| --- | --- |
| CP2102 (log) | PB1 UART0 TX -> RXD |
| CH9350 (USB keyboard/mouse, state 2, 115200) | TX -> PB4 UART1 RX; the firmware holds PB5/PB6/PB7 high to keep the onboard LIN transceiver idle |
| CH376S (SPI; P_S jumper removed, RD/WR tied to GND) | SCK PC9, MISO PC10, MOSI PC11, CS PC8, INT PB14 |
| W5500 | SCK PC9, MISO PC10, MOSI PC11, CS PB10, INT PB2, RST PB8 |

Board details and constraints (LIN sharing, byte-only CH376S transfers and more) are in [docs/PLAN.md](docs/PLAN.md), section 2.

## Building and running

Requires `clang`, `ld.lld`, `llvm-objcopy`, Python 3 and an OpenOCD (0.12) with Nu-Link support.

```sh
make              # build/nnnix.elf, build/nnnix.bin
make host-test    # unit tests on the build machine
make flash        # program APROM through Nu-Link (APROM only; CONFIG and LDROM are never touched)
make test         # flash + on-target tests (needs sudo for a temporary network namespace)
telnet 198.18.0.2 # log in (from the test network; see below)
python3 tools/console.py   # kernel log (/dev/ttyUSB0, 115200 8N1)
```

`make test` needs a USB Ethernet adapter cabled straight to the W5500. The test moves it into an isolated
network namespace (198.18.0.1/30; nnnix is 198.18.0.2) and moves it back afterwards. The adapter must have
no IPv4 address and must not carry the default route. The default is `eth1`; pass `--iface` to
`tools/nntest.py` to change it.

OpenOCD lookup order: `$NNNIX_OPENOCD_ROOT` (an extracted Debian openocd package root), then
`.tools/openocd/root` one level above the repository, then `openocd` on `PATH`.

## Layout

```
arch/nuc140/   startup, vector table, clock, pins, context switch, HardFault, linker script
kernel/        scheduler, synchronisation, system calls, kmalloc, klog, panic, tty and line editor, module supervision, diag service
net/           netd (W5500 interrupts), socket layer, telnet filter, telnetd
drivers/       uart spi ch376 w5500 ch9350 hidkey
lib/           printf subset, string functions, EABI division (no divide instruction on M0), system-call stubs (sys.h), user stdio
user/          nsh shell, scheduler demo programs
tests/host/    host unit tests (including a recorded CH9350 capture)
tools/         flash.py, console.py, nntest.py
docs/          development plan
```

## License

GPL-3.0-or-later; see [LICENSE](LICENSE).
