# nnnix

**NuvotoN NUC UNIX**: a tiny UNIX-like operating system for the Nuvoton NUC140 (Cortex-M0, 16 KiB SRAM,
128 KiB flash) on the NU-LB_002 board.

Goals: three consoles (telnet, LCD + USB keyboard, serial), preemptive multitasking, file systems on SPI
flash and a USB drive, and later stand-alone loadable programs. The full plan is in [docs/PLAN.md](docs/PLAN.md).

## Status

**M6 loadable programs and firmware updates over the network complete**: programs built with the SDK run from
`/bin`, and an LDROM bootloader installs new firmware sent over the network (power-loss safe). M0-M6 are done;
the remaining M7 items are optional. Programs run in place from the chip's flash: their code is written once
into the APROM above the firmware, so a program takes RAM only for its data and stack, and `play`, `ed` and the
text tools are programs in `/bin` rather than part of the firmware.

```
nnnix$ ls / | grep usb
usb/
nnnix$ hexdump /dev/zero | head -n 2
00000000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
00000010  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
nnnix$ sleep 30 &
[1] 16
nnnix$ jobs
[1] running  sleep
nnnix$ kill %1
[1] done  sleep
nnnix$ dhcp
dhcp: 192.168.1.50/24 from 192.168.1.1, lease 86400 s
nnnix$ ntpdate 192.168.1.1
2026-10-16 14:30:05 +0800
ntpdate: offset 3 s
nnnix$ echo hello | nc 192.168.1.10 7000
```

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
- Console tty0: onboard 128x64 LCD as a text terminal plus the USB keyboard (Shift, Caps Lock, Ctrl, arrows, auto-repeat); by default the shell starts when Enter is pressed and its RAM is freed at logout. Two text sizes, 21x8 (5x7 font) and 32x10 (3x5 font): Ctrl+- and Ctrl+=; a scrollback: PageUp/PageDown, Shift+Up/Down ([docs/USER_ENV.md](docs/USER_ENV.md#the-lcd-console))
- Run-time settings (`sysctl`): console off/ask/on, idle logout, telnet session limit, telnetd/httpd on or off,
  watchdog timeout (a hardware watchdog resets the board if the kernel daemon stops running)
- Kernel daemon `kd`: one event loop for the network, keyboard, telnet and diag listeners, the console shell and module supervision
- File systems: VFS with `/` (littlefs on the SPI flash), `/usb` (CH376S FAT drive), `/dev`, `/proc`; per-task file descriptors and working directory; littlefs and CH376S work runs in the `fsd` task
- Network: interrupt-driven W5500, socket layer with self re-arming listeners, telnet server with three sessions
- Shell `nsh`: built in `help echo ps free uptime dmesg uname sleep clear reboot exit ls cat cp mv rm mkdir rmdir
  touch pwd cd df mkfile sum date ifconfig dhcp service netstat nc ntpdate`, and from `/bin` the programs
  `wc head tail grep hexdump ed play wget host snake tetris flappy` (`bin/`); redirection `< > >>`, using only system calls
- M5 shell: quotes/escapes, per-session variables (`set`, `unset`, `env`, `$NAME`, `${NAME}`, `$?`),
  `sh`/`source` scripts and optional `/etc/rc` at boot; streaming `wc head tail grep hexdump`
- HTTP transfers: TCP port 8080, GET/PUT under `/home`, temporary-file uploads committed by atomic rename;
  `tools/nncp.py` transfers binary files with no third-party host dependencies
- Pipes (`a | b | c`), background jobs (`&`, `jobs`, `kill`, `wait`) and Ctrl-C
- `ed` line editor; `date` with the on-chip RTC on the 32.768 kHz crystal, `/etc/tz`, `ntpdate`
- Loadable programs (`sdk/`, [sdk/README.md](sdk/README.md)): NXE files built position-independent
  (`-fropi -frwpi`: code PC-relative, data through r9). The loader keeps their code in the APROM above the
  firmware (about 28.5 KiB, `kernel/xip.c`), written once and found again by its CRC after a reboot, checked
  against the CRC of what was written each time it is used, oldest
  first giving way to new ones but never while running; with no room left the code goes to RAM. Data and bss
  get RAM per run; the C library is the firmware's own (a function table), so `hello` needs 24 bytes of RAM
  besides its stack
- Pictures and videos on the LCD ([docs/NNV.md](docs/NNV.md)): `/dev/lcd` gives a program the display as a
  1024-byte frame (the console text returns when it closes), `/dev/audio` plays 8 kHz sound on the buzzer
  (PWM on PB11, timer interrupt), `/dev/adpcm` the same from NNV's ADPCM records, decoded in the interrupt; `tools/mknnv.py` converts pictures and videos through ffmpeg into NNV, a
  delta-coded format of our own with optional ADPCM sound; `play` shows them (all of Bad Apple at 30 fps;
  `play -s` adds the sound, though the buzzer carries rhythm far better than pitch); `nc -l 1234 | play`
  plays a video streamed over the network (all of Bad Apple: 0 frames late)
- Input for programs (games): `/dev/input` gives one program the USB keyboard and mouse and the board's 1-9
  keypad as press/release events (`sdk/examples/keys.c`); the board's SW_INT button ends the program
  holding the keys or the display, even one stuck in a loop
- Games on the LCD: `snake`, `tetris`, `flappy` (`bin/`, sharing `bin/game.c`), with the keyboard or the
  keypad; they also run on the host with a simulated clock and screenshots (`tests/host/game_sim.c`)
- Firmware updates: `fwupdate FILE` (or `tools/nnupdate.py` from the NanoPi) stages an image in the SPI flash;
  the LDROM bootloader (`boot/`) installs it and repeats an installation that a reset or power loss cut short
- Network: `ifconfig`, a DHCP client with lease renewal in the kernel daemon, `netstat`, `service` (telnetd,
  httpd), `nc` (TCP/UDP, client/server); sockets are files for programs (`socket`, `connect`, `listen`);
  `host` (DNS lookups) and `wget` (HTTP downloads with redirects and chunked replies) as programs
- Memory: first-fit kmalloc with coalescing, long-lived blocks (login shells) from the top of the heap; each
  task's struct shares its stack's block; the boot runs on a temporary stack in the heap, so the main stack only
  needs room for interrupts; task stacks get red-zone and canary checks and an overflow panics
- Crash records: HardFaults and panics are saved in `.noinit` and reported after the reboot; the boot log shows the reset cause
- Drivers: CH376S (USB drive, FAT), W5500 (hardware TCP/IP), CH9350 (USB keyboard/mouse, interrupt-driven receive)

| On-target tests (`make test`, 154 checks) | Result |
| --- | --- |
| Files: flash and USB read/write, copies both ways (CRC-checked), USB subdirectories, redirection, `mv` across file systems | pass |
| Two sessions reading two different files on the single-file CH376S at once | pass |
| Files survive a reboot | pass |
| M5: quoting, variables, scripts/nesting, long-line rejection, `/etc/rc` after reboot and text tools | pass |
| HTTP: 32 KiB binary roundtrip, atomic replacement, aborted upload, framing/path checks, TCP half-close | pass |
| Pipes, background jobs, kill, Ctrl-C (back at the prompt in 2-4 ms), hang-up at logout, `/etc/rc` time limit | pass |
| `ed` interactively and from a script, `date`/time zone, RTC across a reboot (within 1 s) | pass |
| DHCP lease (0.2 s), packet format, no-server fallback; NTP; `nc` TCP/UDP client, server, interactive, Ctrl-C | pass |
| `/etc/rc` configures the network by DHCP and starts telnetd at boot (lease 2.4 s after reset) | pass |
| Settings: console on/ask/off (2144 B freed when the console logs out), idle logout, telnet session limit | pass |
| Firmware update through the bootloader, an installation interrupted by a reset, the power-on path | pass |
| Programs: `/bin/hello` with arguments and exit status, in pipelines and the background, Ctrl-C, from USB, damaged files refused, RAM reclaimed | pass |
| `host` and `wget` against a DNS and an HTTP server on the test peer: CNAMEs, unknown names, a forged DNS answer from another port, 100 KB CRC-checked, absolute and relative redirects, chunked and unsized replies, malformed lengths and chunks, 404, a redirect loop, a refused port | pass |
| LCD console: pixels as the fonts give them in both sizes, the scrollback a screen and a line at a time, a key returning to the terminal, `sysctl console_font`/`console_history`; `tail` from a pipe | pass |
| Programs in APROM: written once and reused, two runs of one program at once, every relocation kind, a program filling the whole area, a new program going to RAM while running programs fill it, kept across a reboot | pass |
| LCD player: a 60-frame video at its frame rate, one `/dev/lcd` writer at a time, picture until Enter, Ctrl-C, damaged files refused, RAM reclaimed | pass |
| Sound: 2 s of samples in 2.0 s, one `/dev/audio` writer, a video with sound on time and without gaps, Ctrl-C silences it | pass |
| `/dev/input`: keyboard and mouse frames through the CH9350 parser, the keypad, one reader at a time, SW_INT ending a program that makes no system calls and a game holding the LCD; `snake`, `tetris`, `flappy` start, play and end | pass |
| Efficiency: a video during a USB copy on time (fsd sleeps while the CH376 works), a program killed during a copy exits cleanly, audio interrupt latency under `ps`/`/proc` | pass (13 us) |
| Review fixes (R24-R35): two tasks sharing `/dev/audio`, cut-short NNV files, the last frame's time, `mv` into a directory, out-of-range `kill`, 0-byte reads, `ed`'s last line, flash dumps during writes, a `/usb` file open across the CH376 bench | pass |
| Review fixes: background redirections, memory of killed programs, concurrent terminal output, DHCP lease end, same-file `cp`/`mv`, `nc` both ways with back-pressure (32 KiB) | pass |
| littlefs write / read (32 KiB) | 37-50 KiB/s / 256 KiB/s |
| Console: banner, typing, line editing, clear, scrolling, shell restart after `exit` | pass |
| Two telnet sessions: one runs `sleep 2`, the other answers | in 4 ms |
| Line editing, Ctrl-C, Ctrl-D, three sessions, log-out and reconnect, heap leak checks | pass |
| W5500 send / receive (SPI 12.5 MHz, 4 KiB socket buffer) | 559 KiB/s / 582 KiB/s, verified byte by byte |
| CH376S raw write / read 256 KiB (SPI 12.5 MHz) | 114 kB/s / 230 kB/s, 0 errors |
| Preemption: worst lateness of a 10 ms ticker with the CPU saturated | 0 ms |
| Round-robin between two equal-priority CPU hogs | 500 / 500 ms |
| Stack overflow detection, HardFault capture, crash report kept after reboot, watchdog reset | pass |
| Free RAM floor: heap free while `free` runs with the console shell (and its 512 B scrollback) and one telnet session | 5248 B (test limit 5200 B) |
| Code size / static RAM including crash records / main stack / heap | 99.2 KiB / 2.8 KiB / 512 B / 12.7 KiB |
| APROM for program code / writing a new program / RAM `hello` needs besides its stack | 28.5 KiB / 20 ms per 512 B page / 24 B |

Host tests: 7708 checks pass (including littlefs on a RAM flash, usbfs against a CH376S model, the NNV decoder against the encoder, the sound ring's ADPCM blocks, the APROM slots on a simulated flash, the console's scrollback, URL resolution against RFC 3986's examples, SPI1 with a stuck controller, `mknxe` on damaged ELF files and `/dev/input`'s events), plus 80 243
date conversions compared with the host C library.

## Hardware

NU-LB_002 Rev 2.1 (NUC140VE3CN). The external modules run from an MB-102 5 V supply that shares ground with the board:

| Module | NUC140 pins |
| --- | --- |
| CP2102 (log) | PB1 UART0 TX -> RXD |
| CH9350 (USB keyboard/mouse, state 2, 115200) | TX -> PB4 UART1 RX; the firmware holds PB5/PB6/PB7 high to keep the onboard LIN transceiver idle |
| CH376S (SPI; P_S jumper removed, RD/WR tied to GND) | SCK PC9, MISO PC10, MOSI PC11, CS PC8, INT PB14 |
| W5500 | SCK PC9, MISO PC10, MOSI PC11, CS PB10, INT PB2, RST PB8 |
| Onboard LCD | SPI3: CS PD8, CLK PD9, MOSI PD11 |
| Onboard buzzer | PB11 (low: on), driven as PWM4 for `/dev/audio` and `/dev/adpcm` |
| Onboard keys | SW_INT PB15 (ends the program holding the keys or the display); 1-9 keypad PA0-PA5 (`/dev/input`) |
| Onboard SPI flash (W25Q16, 2 MiB) | SPI2: CS PD0, CLK PD1, MISO PD2, MOSI PD3 |

Board details and constraints (LIN sharing, byte-only CH376S transfers and more) are in [docs/PLAN.md](docs/PLAN.md), section 2.

## Building and running

Requires `clang`, `ld.lld`, `llvm-objcopy`, Python 3 and an OpenOCD (0.12) with Nu-Link support.

```sh
make              # build/nnnix.elf, build/nnnix.bin, and the programs for /bin: build/bin/*.nxe
make host-test    # unit tests on the build machine
make flash        # program APROM through Nu-Link (APROM only; CONFIG and LDROM are never touched)
make test         # build the SDK examples, flash, on-target tests (needs sudo for a network namespace)
make -C sdk       # loadable programs: sdk/build/*.nxe (see sdk/README.md)
make boot         # LDROM bootloader: build/boot.bin
sudo python3 tools/nnupdate.py --iface eth1 build/nnnix.bin --bin build/bin   # firmware and /bin over the network
sudo python3 tools/flash.py --ldrom build/boot.bin             # bootloader into LDROM (Nu-Link)
sudo python3 tools/flash.py --boot ldrom                       # CONFIG0: start in the bootloader
telnet 198.18.0.2 # log in (from the test network; see below)
python3 tools/console.py   # kernel log (/dev/ttyUSB0, 115200 8N1)
sudo python3 tools/nnshare.py on   # internet for the board through this computer (off: undo)
sudo python3 tools/nncp.py --iface eth1 put local.bin /home/local.bin
sudo python3 tools/nncp.py --iface eth1 get /home/local.bin copy.bin
```

`make test` needs a USB Ethernet adapter cabled straight to the W5500. The test moves it into an isolated
network namespace (198.18.0.1/30; nnnix is 198.18.0.2) and moves it back afterwards. The adapter must have
no IPv4 address and must not carry the default route. The default is `eth1`; pass `--iface` to
`tools/nntest.py` to change it.

Shell syntax, script limits, `/etc/rc`, the editor, the clock, network configuration and file transfer are
documented in [docs/USER_ENV.md](docs/USER_ENV.md). The board starts with the static address 198.18.0.2/30;
`/etc/rc` can switch to DHCP or another address at boot.

OpenOCD lookup order: `$NNNIX_OPENOCD_ROOT` (an extracted Debian openocd package root), then
`.tools/openocd/root` one level above the repository, then `openocd` on `PATH`.

## Layout

```
arch/nuc140/   startup, vector table, clock, pins, context switch, HardFault, linker script
kernel/        scheduler, synchronisation, system calls, kmalloc, klog, panic, kd (kernel daemon), tty, line editor, vt, console, input, module supervision, diag service, program loader and APROM slots (xip)
net/           socket layer, sockets for programs, interface config, DHCP client, telnet filter, telnetd, HTTP server
drivers/       uart spi ch376 w5500 ch9350 hidkey kbd lcd font5x7 spiflash rtc
fs/            vfs and fsd, rootfs (littlefs), usbfs (CH376S), devfs, procfs, littlefs port
lib/           printf subset, string functions, calendar time, EABI division (no divide instruction on M0), system-call stubs (sys.h), user stdio
user/          nsh shell and its built-in commands (file, network, settings), parser, scheduler demo programs
bin/           the programs for /bin: play, ed, wc head tail grep hexdump, wget host (built like SDK programs)
third_party/   littlefs v2.11.3 (BSD-3-Clause), unmodified
tests/host/    host unit tests (including a recorded CH9350 capture)
boot/          LDROM bootloader: installs staged firmware images, then starts APROM
tools/         flash.py (APROM, LDROM, CONFIG0), console.py, nntest.py, nncp.py (HTTP file transfer),
               nnupdate.py (firmware update), mknxe.py (ELF to NXE), nnshare.py (internet for the board)
sdk/           loadable program SDK: crt0, nnnix.h, linker script, Makefile, examples
docs/          development plan
```

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). `third_party/littlefs` is BSD-3-Clause; see its `LICENSE.md`.
