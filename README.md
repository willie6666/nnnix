# nnnix

**NuvotoN NUC UNIX**: a tiny UNIX-like operating system for the Nuvoton NUC140 (Cortex-M0, 16 KiB SRAM,
128 KiB flash) on the NU-LB_002 board.

Goals: three consoles (telnet, LCD + USB keyboard, serial), preemptive multitasking, file systems on SPI
flash and a USB drive, and later stand-alone loadable programs. The full plan is in [docs/PLAN.md](docs/PLAN.md).

## Status

**M5 in progress: shell scripts, text tools and HTTP file transfer implemented.**
M0 foundation, M1 kernel core, M2 telnet, M3 LCD/keyboard, the RAM diet and M4 file systems are complete.
Pipes/background jobs, the editor, DHCP and RTC tools remain in M5.

nnnix runs a shell on the board's own LCD with a USB keyboard, and up to three more over telnet. Files live on
the onboard 2 MiB SPI flash (littlefs) and on a USB drive:

```
nnnix$ ls /
dev/
hello.txt          12
home/
proc/
usb/
nnnix$ mkfile /home/big.bin 32k
32768 bytes in 593 ms (55200 B/s)
nnnix$ cp /home/big.bin /usb
nnnix$ sum /home/big.bin /usb/big.bin
31782b52 32768 /home/big.bin 128 ms
31782b52 32768 /usb/big.bin 225 ms
nnnix$ df
FS      KiB   used   free
/       2048     48   2000
/usb 15343968      8 15343960
nnnix$ cat /proc/tasks
pid name prio state stack_used stack_size cpu_ms
1 idle 7 1 80 384 43374
2 fsd 4 3 1028 1536 780
3 kd 2 3 716 1024 1060
4 nsh 5 3 696 1152 10
```

- Preemptive multitasking: fixed priorities with round-robin among equals (10 ms slice; a preempted task resumes its slice), PendSV switching, 1 ms tick, sleep and timeouts
- Synchronisation: wait queues, mutexes, semaphores (can be given from interrupts); SPI bus lock and CH376S device lock
- System calls: entered with `svc`; the kernel function runs in the caller's own task context, so it may block; user-side interface in `lib/sys.h`
- Terminals: canonical line editing (backspace, Ctrl-U, Ctrl-C, Ctrl-D, arrow keys ignored) over a byte backend
- Console tty0: onboard 128x64 LCD as a 21x8 text terminal plus the USB keyboard (Shift, Caps Lock, Ctrl, arrows, auto-repeat); the shell restarts after logout
- Kernel daemon `kd`: one event loop for the network, keyboard, telnet and diag listeners, console shell restart and module supervision
- File systems: VFS with `/` (littlefs on the SPI flash), `/usb` (CH376S FAT drive), `/dev`, `/proc`; per-task file descriptors and working directory; littlefs and CH376S work runs in the `fsd` task
- Network: interrupt-driven W5500, socket layer with self re-arming listeners, telnet server with three sessions
- Shell `nsh`: `help echo ps free uptime dmesg uname sleep clear reboot exit ls cat cp mv rm mkdir rmdir touch pwd cd df mkfile sum`, redirection `< > >>`, using only system calls
- M5 shell: quotes/escapes, per-session variables (`set`, `unset`, `env`, `$NAME`, `${NAME}`, `$?`),
  `sh`/`source` scripts and optional `/etc/rc` at boot; streaming `wc head tail grep hexdump`
- HTTP transfers: TCP port 8080, GET/PUT under `/home`, temporary-file uploads committed by atomic rename;
  `tools/nncp.py` transfers binary files with no third-party host dependencies
- Memory: first-fit kmalloc with coalescing; task stacks get red-zone and canary checks and an overflow panics
- Crash records: HardFaults and panics are saved in `.noinit` and reported after the reboot; the boot log shows the reset cause
- Drivers: CH376S (USB drive, FAT), W5500 (hardware TCP/IP), CH9350 (USB keyboard/mouse, interrupt-driven receive)

| On-target tests (`make test`, 70 checks) | Result |
| --- | --- |
| Files: flash and USB read/write, copies both ways (CRC-checked), USB subdirectories, redirection, `mv` across file systems | pass |
| Two sessions reading two different files on the single-file CH376S at once | pass |
| Files survive a reboot | pass |
| M5: quoting, variables, scripts/nesting, long-line rejection, `/etc/rc` after reboot and text tools | pass |
| HTTP: 32 KiB binary roundtrip, atomic replacement, aborted upload, framing/path checks, TCP half-close | pass |
| `nncp.py` upload/download, concurrent shell use and heap reclamation | pass |
| littlefs write / read (32 KiB) | 50 KiB/s / 256 KiB/s |
| Console: banner, typing, line editing, clear, scrolling, shell restart after `exit` | pass |
| Two telnet sessions: one runs `sleep 2`, the other answers | in 5 ms |
| Line editing, Ctrl-C, Ctrl-D, three sessions, log-out and reconnect, heap leak check | pass |
| W5500 send / receive (SPI 12.5 MHz, 8 KiB socket buffer) | 627 KiB/s / 580 KiB/s, verified byte by byte |
| CH376S raw write / read 256 KiB (SPI 12.5 MHz) | 114 kB/s / 229 kB/s, 0 errors |
| Preemption: worst lateness of a 10 ms ticker with the CPU saturated | 0 ms |
| Round-robin between two equal-priority CPU hogs | 500 / 500 ms |
| Stack overflow detection, HardFault capture, crash report kept after reboot | pass |
| Free RAM floor: heap free while `free` runs with the console shell and one telnet session | 5784 B (test limit 5500 B) |
| Code size / static RAM including crash records / heap | 62.0 KiB / 3.3 KiB / 11.9 KiB |

Host tests: 1326 checks pass. An additional 20 consecutive telnet
connect/command/logout cycles pass.

## Hardware

NU-LB_002 Rev 2.1 (NUC140VE3CN). The external modules run from an MB-102 5 V supply that shares ground with the board:

| Module | NUC140 pins |
| --- | --- |
| CP2102 (log) | PB1 UART0 TX -> RXD |
| CH9350 (USB keyboard/mouse, state 2, 115200) | TX -> PB4 UART1 RX; the firmware holds PB5/PB6/PB7 high to keep the onboard LIN transceiver idle |
| CH376S (SPI; P_S jumper removed, RD/WR tied to GND) | SCK PC9, MISO PC10, MOSI PC11, CS PC8, INT PB14 |
| W5500 | SCK PC9, MISO PC10, MOSI PC11, CS PB10, INT PB2, RST PB8 |
| Onboard LCD | SPI3: CS PD8, CLK PD9, MOSI PD11 |
| Onboard SPI flash (W25Q16, 2 MiB) | SPI2: CS PD0, CLK PD1, MISO PD2, MOSI PD3 |

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
sudo python3 tools/nncp.py --iface eth1 put local.bin /home/local.bin
sudo python3 tools/nncp.py --iface eth1 get /home/local.bin copy.bin
```

`make test` needs a USB Ethernet adapter cabled straight to the W5500. The test moves it into an isolated
network namespace (198.18.0.1/30; nnnix is 198.18.0.2) and moves it back afterwards. The adapter must have
no IPv4 address and must not carry the default route. The default is `eth1`; pass `--iface` to
`tools/nntest.py` to change it.

Shell syntax, script limits, `/etc/rc` and file-transfer usage are documented in
[docs/USER_ENV.md](docs/USER_ENV.md). The network address is still static; boot-time network
configuration and service control remain pending in M5.

OpenOCD lookup order: `$NNNIX_OPENOCD_ROOT` (an extracted Debian openocd package root), then
`.tools/openocd/root` one level above the repository, then `openocd` on `PATH`.

## Layout

```
arch/nuc140/   startup, vector table, clock, pins, context switch, HardFault, linker script
kernel/        scheduler, synchronisation, system calls, kmalloc, klog, panic, kd (kernel daemon), tty, line editor, vt, console, input, module supervision, diag service
net/           socket layer, telnet filter, telnetd, bounded HTTP parser and file-transfer server
drivers/       uart spi ch376 w5500 ch9350 hidkey kbd lcd font5x7 spiflash
fs/            vfs and fsd, rootfs (littlefs), usbfs (CH376S), devfs, procfs, littlefs port
lib/           printf subset, string functions, EABI division (no divide instruction on M0), system-call stubs (sys.h), user stdio
user/          nsh shell, parser, file/text commands, scheduler demo programs
third_party/   littlefs v2.11.3 (BSD-3-Clause), unmodified
tests/host/    host unit tests (including a recorded CH9350 capture)
tools/         flash.py, console.py, nntest.py, nncp.py (HTTP file transfer)
docs/          development plan
```

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). `third_party/littlefs` is BSD-3-Clause; see its `LICENSE.md`.
