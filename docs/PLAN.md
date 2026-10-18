# nnnix development plan

**nnnix**: NuvotoN NUC UNIX (NuvotoN Nuc NIX), a small UNIX-like operating system for the
NU-LB_002 board (NUC140VE3CN).

Created 2026-10-15. Status: **M5 done** (2026-10-16): scripts, text tools, HTTP transfer (M5a), pipes, jobs,
Ctrl-C (M5b), and the editor, clock and network tools (M5c); then a second RAM diet with run-time settings.
**M6 done**: loadable programs (NXE files built with `sdk/`). M7: the LDROM bootloader and network firmware
updates are done; the other M7 items remain optional.

## 1. Goals

- A UNIX-like experience: a shell, a file system tree, `/dev` devices, processes, commands such as `ls`, `cat` and `ps`.
- Three consoles usable at the same time: **telnet (W5500)**, **onboard LCD + USB keyboard (CH9350)** and **CP2102 serial**.
- **Preemptive multitasking**: several shell sessions and background services run concurrently.
- Program model: **built in first** (commands compiled into the firmware), **loadable later** (stand-alone programs loaded from USB storage or the network).
- Automated testing: the NanoPi flashes the board, logs in over telnet, runs commands and checks the output.

Out of scope: memory protection (the Cortex-M0 has no MMU or MPU), secure multi-user permissions, full POSIX compatibility, graphics.

## 2. Hardware resources and constraints

| Resource | Specification | Use in nnnix |
| --- | --- | --- |
| CPU | Cortex-M0, 50.19 MHz (PLL from HIRC, verified) | No divide instruction, no MPU |
| SRAM | **16 KiB** | The main constraint; budget in section 4 |
| APROM | 128 KiB | Kernel, drivers, shell, built-in commands |
| LDROM | 4 KiB | Later: bootloader / updater (optional) |
| W5500 | Hardware TCP/IP, 8 sockets, 32 KiB on-chip buffers, SPI1 up to 25 MHz | Network and telnet; buffers cost no MCU RAM |
| CH376S | USB mass storage with on-chip FAT32, SPI1 up to 12.5 MHz | `/usb`; **only one open file at a time** |
| CH9350 | USB keyboard/mouse to UART1 PB4 (receive only) | Keyboard input, mouse events |
| CP2102 | UART0 PB1 TX | Kernel log; two-way use needs the PB0 conflict solved (R3) |
| Onboard LCD | 128x64 monochrome, SPI3 (PD8-PD11, PD14) | Text console, about 21x8 characters |
| Onboard SPI flash | W25Q16 2 MiB, SPI2 (PD0-PD3) | Root file system `/` |
| Other onboard parts | LED (PC12), buzzer, keys, I2C EEPROM 24C64, RTC | `/dev/led`, `/dev/beep`, settings, time |

Known hardware constraints (from the bring-up tests):
- The CH376S needs byte-by-byte SPI transfers; back-to-back 32-bit transfers corrupt data.
- PB4/PB5 are shared with the onboard LIN transceiver: LIN must stay in normal idle with PB5 held high, so the CH9350 link is receive only.
- PB0 (UART0 RX) may be driven by the onboard RS-232 transceiver, so the CP2102 TXD cannot be connected yet.
- Code running from APROM **cannot reprogram APROM** (FMC ISPFF: "APROM writes to itself"); it can only reprogram LDROM.

## 3. Architecture

```
 user       nsh (shell)  built-in commands  telnetd  getty    <- later: loaded programs
 ------------- svc system calls (one ABI for built-in and loaded programs) -------------
 kernel     scheduler  processes/tasks  wait queues/mutexes  timers  klog  panic
            VFS (fd tables, mount table)  tty/line editing  pipes  socket layer
 fs         rootfs (W25X16)  usbfs (CH376S)  devfs  procfs
 drivers    uart  spi (bus lock)  gpio/exti  ch9350  ch376  w5500  lcd  w25x16  rtc
 arch       startup, clock, interrupts, PendSV context switch, SysTick, HardFault dump
```

Design points:
- **One address space, no protection.** A process is a task (own stack) plus an fd table, a cwd and environment variables.
- **All programs enter the kernel through svc.** Built-in commands also go through libc and svc, so loaded programs later need no ABI change.
- **Scheduling**: fixed priorities with round-robin among equals; 1 ms SysTick, 10 ms time slice; PendSV switches context.
- **Interrupt-driven drivers with wait queues**: UART1 RX, W5500 INT (PB2) and CH376 INT (PB14) wake waiting tasks through GPIO interrupts instead of polling.
- **SPI1 bus lock**: the CH376S and W5500 share the bus. The CH376S does not hold the bus while it waits for USB (INT).
- **CH376S has one open file**: usbfs remembers each fd's name and position and reopens and seeks when switching. A cache can come later.
- **W5500 buffers live on the chip**: socket reads and writes move data directly between the W5500 and the caller's buffer, without extra MCU buffers.
- **No LCD framebuffer** (the BSP's 1 KiB is too expensive): keep a 21x8 character grid (168 B) and write characters straight into the LCD's pages and columns.
- **Stack overflow detection**: without an MPU, stacks are painted and the bottom is checked (red zone and canary) on every switch.
- **Crash information**: a HardFault dumps registers to UART0, saves them in `.noinit` RAM and reboots; the next boot reports the last crash.

## 4. Memory budget

SRAM 16 KiB. Measured after the second RAM diet (after M5): static 2.2 KiB plus `.noinit` 360 B, MSP 512 B,
heap 13.0 KiB. With nobody logged in the tasks are idle (256 B stack), fsd (1536 B) and kd (1024 B), and about
8.6 KiB of heap is free; a shell (console or telnet) costs about 1.5-1.7 KiB while it runs. The original estimate:

| Item | Size |
| --- | --- |
| Kernel .data/.bss (task table, fds, mount table, driver state) | ~2.5 KiB (3.5 KiB measured in M1) |
| Interrupt/kernel stack | 1 KiB (556 B peak measured in M1) |
| idle + system tasks (telnetd, kbd/tty, services) | ~2 KiB |
| User process stacks: up to 4 x 1-1.5 KiB | ~5 KiB |
| tty buffers (3 consoles + line editing), pipes (2 x 128 B) | ~1 KiB |
| File systems (littlefs caches 3 x 256 B + one shared 512 B block buffer) | ~1.5 KiB |
| Dynamic allocation pool | remaining ~3 KiB |

Flash 128 KiB targets: kernel + drivers <= 45 KiB, file systems <= 20 KiB, shell + built-in commands <= 40 KiB, font <= 1 KiB, keep >= 20 KiB spare.

## 5. Pins and peripherals

| Peripheral | Pins | Status |
| --- | --- | --- |
| UART0 TX -> CP2102 | PB1 | Verified |
| UART0 RX <- CP2102 | PB0 | Open (R3) |
| UART1 RX <- CH9350 | PB4; PB5/PB6/PB7 held high (LIN idle) | Verified |
| SPI1 CLK/MISO/MOSI | PC9/PC10/PC11 | Verified |
| CH376S CS/INT | PC8/PB14 | Verified |
| W5500 CS/INT/RST | PB10/PB2/PB8 | Verified |
| SPI2 -> W25Q16 (CS PD0 as GPIO, CLK PD1, MISO PD2, MOSI PD3) | onboard | Verified |
| SPI3 -> LCD (CS PD8 as GPIO, CLK PD9, MOSI PD11) | PD8-PD11 (onboard); PD14 left alone | Verified |
| LED | PC12 | Verified |
| RTC crystal (32.768 kHz, X32I/X32O) | onboard | Verified in M5 |
| Buzzer, keys, EEPROM | Per board documentation | To verify (R4) |

## 6. File system layout

```
/            rootfs: W25X16 + littlefs (power-loss safe)
/bin         (from M6) loadable programs
/etc         rc, net.conf, motd, passwd (simple)
/home        user files
/dev         devfs: tty0 (LCD + keyboard) tty1/2 (telnet) ttyS0 (serial) null zero
             kbd mouse lcd led beep spiflash usb net
/proc        ps, meminfo, uptime, net (read-only text)
/usb         CH376S USB drive, FAT32 (mounted when inserted)
/tmp         small files in RAM (optional; may be dropped for lack of RAM)
```

## 7. Milestones

A milestone is done only when automated tests cover it.

### M0 Foundation
- Create the nnnix repository (layout in section 8): clang/lld build, linker script, startup, clock, klog (UART0).
- Port the drivers verified during bring-up into `drivers/`.
- HardFault dump and a `.noinit` crash record.
- `tools/`: flashing, serial log capture, an automated on-target test harness (runs on the NanoPi).
- `tests/host/`: unit tests on Linux for hardware-independent modules (CH9350 parser, line editing, shell parser).

**Acceptance**: `make flash test` flashes, sees the boot banner, triggers a fault on purpose and sees it reported after the reboot.

**Result (2026-10-15)**: `make test` 11/11. Code 11.9 KiB, static RAM 2.7 KiB, peak stack 0.8 of 2 KiB.
Hardware behaviour found on the way:
- The W5500's `Sn_TX_WR` and `Sn_TX_FSR` read back their values **as of the last SEND**. Data written before a SEND must be counted in software, otherwise each write overwrites the previous one (the driver tracks `queued[]`).
- Only one SEND may be in flight; a SEND issued before the previous SEND_OK is ignored.
- A SYN that arrives while the peer's link is still coming up can leave the only listening socket in SYN_RECV for about 32 s. The diag service re-listens after 2 s; the M2 socket layer must handle this itself and keep several listening sockets.

### M1 Kernel core
- Task creation and exit, PendSV context switch, SysTick, sleep, priority scheduling.
- Wait queues, mutexes, semaphores, events; svc system-call entry and number table.
- Allocator; stack canaries; a kernel `ps` listing.

**Acceptance**: three tasks with different periods preempt each other; a deliberate stack overflow is detected; measured RAM figures go back into section 4.

**Result (2026-10-15)**: `make test` 15/15, `make host-test` 82 checks pass.
- Two equal-priority CPU hogs got 509/491 ms (ratio 0.96); a higher-priority 10 ms ticker running at the same time was never late (worst 0 ms).
- Stack overflow: when a saved sp enters the 128 B red zone at the stack bottom (or the bottom canary changes) the kernel panics; the message is kept in `.noinit` and reported after the reboot.
- System calls: the svc handler only rewrites the stacked return address, so the kernel function runs in the caller's own task context and may block.
- Measured RAM (16 KiB): static 3.5 KiB (including 10 task control blocks), MSP 1 KiB (556 B peak), `.noinit` 512 B, heap 11.6 KiB.
  System task stack use/size: idle 80/384, input 160/768, diag 756/1280, mon 524/896; 8.5 KiB heap left.
- The host tests on a 64-bit machine caught a kmem header size hard-coded to 8 bytes; it is now derived from the struct. The M0 build was not affected.

### M2 Telnet console (first interactive interface)
- Interrupt-driven W5500 and a socket layer (`socket/bind/listen/accept/send/recv/close`).
- tty layer: line editing (backspace, Ctrl-C, Ctrl-D) and telnet option negotiation (at least echo and SGA).
- `telnetd` and a minimal `nsh`: `help echo ps free uptime reboot dmesg`.
- Network settings stay static at 198.18.0.2/30; DHCP comes in M5.

**Acceptance**: the NanoPi runs commands in two telnet sessions at the same time; the automated tests pass.

**Result (2026-10-16)**: `make test` 28/28, `make host-test` 109 checks pass.
- W5500 INTn on PB2 raises a falling-edge GPIO interrupt; the `netd` task (priority 1) collects and acknowledges
  socket events and wakes the tasks blocked on those sockets. It also polls every 100 ms in case an edge is lost.
- Socket plan: 0 diag (8 KiB), 1-3 telnet (2 KiB each), 4 spare (2 KiB). A shared listener re-arms server sockets
  and drops half-open connections stuck in SYN_RECV.
- telnetd: three sessions on port 23, one `nsh` task each (1280 B stack, about 700 B used). The line editor and the
  telnet filter are pure code with host tests.
- New system calls: read (terminal line), ps, meminfo, klog, reboot, uname. `nsh` uses only system calls.
- Measured: a second session answers in 8 ms while the first runs `sleep 2`; three sessions at once; no heap leak after
  log-out/reconnect cycles; 5.5 KiB heap free with one session.
- Round-robin fix: more high-priority wakeups (netd, diag) made equal-priority sharing unfair (440/560 ms). A task
  preempted by a higher priority now resumes its remaining slice before its peers; the demo shows 500/500 ms.
- W5500 send rate dropped from 1030 to 834 KiB/s because the diag socket buffer went from 16 to 8 KiB.

### M3 Local console: LCD + keyboard
- LCD driver: initialisation, 5x7 font, 21x8 text, scrolling, cursor.
- CH9350 keyboard: HID usages to ASCII, Shift/CapsLock (software state), arrow keys to ANSI sequences; `/dev/kbd`, `/dev/mouse`.
- `tty0` = LCD + keyboard; `nsh` starts on `tty0` at boot.

**Acceptance**: typing on the keyboard shows on the LCD and runs commands while telnet sessions keep working.

**Result (2026-10-16)**: `make test` 34/34, `make host-test` 1224 checks pass; the owner confirmed the LCD and
keyboard on the board.
- LCD (R1): initialisation sequence from the vendor sample (bias, contrast, mapping, display on); 9-bit SPI3 frames
  at 3.1 MHz with bit 8 selecting data; logical x maps to controller column 129 - x, so each character is written
  as one 6-byte run. The terminal keeps only a 21x8 character grid (168 B), no framebuffer.
- Font: a classic 5x7 ASCII table written for nnnix (the vendor font is not used).
- `vt` (character grid, wrap, scroll, CR/LF/BS/TAB, ESC [ H / 2J / K) and `kbd` (Shift, Caps Lock, Ctrl+letter,
  arrows, Home/End/Delete, auto-repeat 500 ms then every 50 ms) are pure code with host tests.
- tty0 = LCD + keyboard; a `getty` task starts a new `nsh` after each logout.
- Tests type on tty0 through diag `I` and read the screen back with diag `V`.
- Findings: W5500 reads at 25 MHz gave about 6 bad bytes per 10 MiB on the breadboard wiring and none at 12.5 MHz,
  so the W5500 now runs at 12.5 MHz (636/623 KiB/s send/receive). The console shell raised the task count to 8, so
  `TASK_MAX` went from 10 to 14; the larger `ps` buffer then overflowed a 1024 B shell stack and the overflow
  detector caught it. `ps` now pages through the task list four entries at a time and shells get 1152 B stacks.
- RAM: 5.3 KiB heap free after boot, 4.0 KiB with one telnet session; three telnet sessions still fit.

### RAM diet (between M3 and M4)

Before the file systems, the system already used about 11 KiB of the 16 KiB before any user program ran. The goal
was to double the free RAM without hardware changes.

**Result (2026-10-16)**: `make test` 35/35 (adds a free-RAM floor check).
- One kernel daemon task (`kd`) replaces netd, telnetd, mon, input and getty: an event loop woken by the W5500 and
  UART1 interrupts and by timers (key repeat, 50 ms listener poll, 1 s module supervision). Shells and diag sessions
  still run in their own tasks.
- The diag service only listens in `kd`; each connection gets a worker task, and its 1.4 KiB of buffers are
  allocated for that session only.
- Busy waits outside start-up became `msleep()` (module power-up, W5500 reset, CH376S mount).
- `printf` and `kprintf` stream through 32-byte chunks instead of formatting into 128/160-byte stack buffers.
- MSP 1024 -> 768 B, HardFault stack 384 -> 256 B, UART1 ring 256 -> 128 B, `TASK_MAX` 14 -> 10.

| | Before | After |
| --- | --- | --- |
| Static RAM (.data + .bss) | 4.3 KiB | 2.4 KiB |
| Heap | 11.0 KiB | 13.1 KiB |
| Free after boot (console shell running) | 5.3 KiB | 10.4 KiB |
| Free with one telnet session | 4.0 KiB | 9.2 KiB |
| Tasks at idle | 8 | 3 (idle, kd, console nsh) |

Swap and zram were considered and rejected for now: without an MMU a swapped-out task must come back to the same
address, compression costs RAM and time on every switch, and the only wear-free backing store on hand (W5500 buffer
memory) is shared with the network. External SPI flash (for example a W25Q64) adds storage, not RAM. The NUC140 EBI
could map a parallel SRAM, but it needs about 20 pins including PB6/PB7, which hold the LIN transceiver idle for the
CH9350. An SPI SRAM such as a 23LC1024 remains an option for buffers or rolled-out idle shells if M6 needs more.
Free blocks can fragment after shells come and go (seen: 9216 B free as 6504 + 2712); worth watching when M4 adds
larger allocations.

### M4 File systems
- VFS: path lookup, mount table, per-process fd tables, `open/read/write/lseek/close/stat/readdir/mkdir/unlink/rename`.
- devfs; W25X16 driver + littlefs as `/`; CH376S usbfs mounted at `/usb`; procfs.
- Commands: `ls cat cp mv rm mkdir rmdir touch df pwd cd`; shell redirection `> >> <`.

**Acceptance**: copy files between the USB drive and `/`, and they survive a power cycle; two sessions read different USB files at the same time.

**Result (2026-10-16)**: `make test` 45/45 (10 file-system checks), `make host-test` passes.
- R2: the onboard flash is a Winbond W25Q16 (JEDEC ef4015, 2 MiB) on SPI2 at 6.3 MHz. It was blank; a full
  image was saved on the development host before first use.
- R5: littlefs v2.11.3 (BSD-3-Clause, vendored unmodified in `third_party/littlefs`) is 19.5 KiB of code. nnnix
  supplies its utility header (`fs/lfs_port.h`: no libc, no CLZ/CTZ on the M0), static 64-byte read/program caches,
  a 16-byte lookahead and `LFS_NAME_MAX` 31. It formats an empty flash on first boot.
- VFS: mount table (`/` littlefs, `/usb` CH376S, `/dev`, `/proc`), per-task file descriptors (fd 0-2 fall back to
  the terminal) and working directory, path normalisation. littlefs and CH376S operations run in the `fsd` task
  on its own 1.5 KiB stack (about 1 KiB used), one at a time, so shells keep small stacks and neither file
  system needs locking of its own.
- usbfs: the CH376S keeps one file open, so every file remembers its 8.3 path and position and the chip is
  switched over (reopen and seek) when another file is used. Directory listings enumerate from the start.
  FILE_ERASE removes whatever is currently open: opening a file's parent and naming the file removes the parent.
  This was caught by testing `rm` in a subdirectory and is fixed (the target itself is opened first).
- System calls: open, close, lseek, stat, readdir, mkdir, unlink, rename, chdir, getcwd, statfs, dup2; read/write
  go to files when fd 0-2 are redirected.
- Commands: `ls cat cp mv rm mkdir rmdir touch pwd cd df`, test helpers `mkfile` and `sum` (CRC-32), and shell
  redirection `<`, `>`, `>>`.
- Measured: littlefs writes about 50 KiB/s (including erases) and reads 256 KiB/s; the USB drive writes about
  45 KiB/s and reads 145 KiB/s through the file layer.
- The last crash report is also kept in a 96-byte buffer (`/proc/crash`, diag `S`), since the longer boot log now
  pushes it out of the 512-byte log ring.
- RAM: fsd and littlefs cost about 2.2 KiB; shells grew to 1280 B stacks (file commands reached 1004 of 1152 B).
  Heap free with the console shell is about 8 KiB, with one telnet session 6.5 KiB.

### M5 User environment and network tools
- Shell: pipes (kernel ring buffers), `&` background jobs, `jobs/kill`, environment variables, scripts, `/etc/rc` at boot.
- Commands: `ed` (a line editor suits a 21x8 LCD), `hexdump wc head tail grep sleep date kill`.
- Network: `ifconfig`, DHCP client, `netstat`, `nc`, simple HTTP or TFTP (file transfer between the NanoPi and nnnix).
- RTC: `date`, optional NTP.

**Acceptance**: `/etc/rc` configures the network and starts telnetd at boot; a file uploaded from the NanoPi lands in `/home`.

**First part (2026-10-16)**: `make test` 70/70; `make host-test` 1326 checks pass.
This is not the full M5 acceptance.
- `nsh` now has a bounded, host-tested parser for single/double quotes, escapes,
  comments, `< > >>` with or without adjacent spaces, and `$NAME`, `${NAME}`, `$?`.
  Syntax and argument overflow errors are rejected before executing a command.
- Eight variables per shell (`set`, `unset`, `env`), `sh`/`source`/`.` scripts,
  up to three nested scripts, LF/CRLF and a final unterminated line. Script state
  lives in a small heap context rather than recursive task stacks. Oversized or
  binary lines stop the script before a truncated command can run.
- An `init` task runs an optional `/etc/rc` once, then exits. LCD and telnet
  shells start afterwards. `/etc` is created if missing; existing boot scripts
  are not overwritten, and no default boot script is installed.
- Streaming `wc`, `head`, `tail`, literal `grep` and `hexdump`; `tail` requires
  seekable input and `grep` explicitly rejects lines over its 192-byte limit.
- The existing spare W5500 socket 4 serves HTTP GET/PUT on port 8080 under
  `/home`. A worker, 128-byte receive buffer and bounded parser storage are
  allocated only for an active transfer. PUT streams to a littlefs temporary
  file and atomically renames it after a successful close; an incomplete upload
  preserves the previous file. Requests need Content-Length for PUT (1 MiB max);
  Expect: 100-continue is supported, chunked upload is not.
- TCP half-close fix: buffered input in CLOSE_WAIT reaches the listener, and
  SEND/flush waits for completion even after a peer FIN. Previously a client
  closing only its sending direction could lose the server's response.
- `tools/nncp.py`: Python-standard-library upload/download client, optional
  temporary test network namespace, and atomic host-side downloads.
- Added alloc/free system calls for shell-owned state. kmalloc rejects sizes
  that could overflow its aligned header calculation. Shell parse buffers exist
  only during a command; waiting shells retain about 200 bytes of extra state.
- Board checks cover scripts and variables in independent sessions, the new
  text tools, `/etc/rc` after a reboot, 32 KiB binary upload/download with CRC,
  the host client, atomic replacement and aborted uploads, invalid paths and
  framing, Expect: 100-continue, TCP half-close and heap reclamation. A shell
  answered in 4 ms while HTTP waited for an unfinished request. An additional
  20 consecutive telnet connect/command/logout cycles passed.
- RAM/code: 5784 bytes heap free while `free` executes with the LCD shell and
  one telnet session (5500-byte floor); all three telnet sessions still fit.
  Code is about 62.0 KiB, static RAM including crash records 3372 bytes, heap
  12216 bytes. Shell stack use measured at 760/1280 bytes; no persistent heap
  loss was seen after scripts, file transfers and reconnects.

Usage and deliberate limits: [USER_ENV.md](USER_ENV.md).
Remaining M5 work: pipes and background jobs with bounded task/stack use,
jobs/kill, `ed`, network diagnostics/configuration, DHCP, RTC/date and optional
NTP. `/etc/rc` currently runs commands but does not configure the static network
or control telnetd; those parts of the final M5 acceptance remain open.

**M5b result (2026-10-16)**: `make test` 80/80, `make host-test` passes.
- Pipelines (`a | b | c`, up to three stages) and background jobs (`cmd &`) run each stage in its own task
  (1152 B stack) connected by 128-byte kernel pipes; single commands still run inside the shell, so they cost no
  extra RAM. Shell built-ins that change the shell (`cd`, `set`, `sh`, ...) are refused in pipelines.
- System calls: spawn (runs a function of this image as a child with its argv copied and fd 0/1 wired), wait (with a
  timeout, so the shell can watch for Ctrl-C), kill, pipe, ttyintr.
- Kill without an MMU: a killed program leaves at its next system call; sleep, pipe, terminal and wait waits are
  interruptible, while waits on fsd requests are not (fsd writes the result into the caller's stack).
  Writing to a pipe without readers ends the writer, like SIGPIPE, so `hexdump /dev/zero | head` stops.
- Ctrl-C: while no task reads the terminal, a non-blocking poll moves typed input into a 16-byte type-ahead buffer
  and reports ^C. The shell kills a foreground pipeline; in-shell commands check between reads/writes, `sleep` every
  100 ms. Measured: back at the prompt 2-4 ms after ^C.
- Hang-up: when a task exits, its children are killed and detached; telnetd frees a session only after no task uses
  its terminal any more (background jobs inherit the session's terminal, which would otherwise be freed under them).
- `jobs`, `kill %N|PID`, `wait`; finished jobs are reported before the next prompt.
- `/etc/rc` gets 10 s: if it is still running, logins start anyway (measured 10.7 s after link-up).
- Costs: about 600 B less free heap than M5a (job tables, task fields, type-ahead, parse buffer); the free-RAM floor
  is now 4800 B with the console shell and one telnet session (measured 5168 B).

**M5c result (2026-10-16)**: `make test` 102/102, `make host-test` 1440 checks plus 80 243 date conversions
compared with the host C library. This completes M5 and its acceptance: a boot script that stops telnetd, sets the
address, gets a DHCP lease and starts telnetd again works after a reboot (lease 2.4 s after power-up), and files
uploaded from the NanoPi land in `/home` (M5a).
- RTC (R4): the onboard 32.768 kHz crystal starts and the RTC runs from it. It keeps counting through resets and
  firmware updates (verified across reboots within 1 s) but not through power loss; the NUC140 has no backup
  supply. Without the crystal the clock falls back to the system timer. `date` (UTC or the `/etc/tz` offset),
  `date -s`, `date +%s`; `ntpdate` (SNTP, IP address only).
- `ed`: a line editor core (`user/edcore.c`, 77 host checks) with literal search and substitution, sized for the
  21x8 LCD (`z` pages 7 lines); the buffer is one heap block that grows in 256-byte steps.
- Network configuration: the interface starts with the static test address so the board is reachable before
  `/etc/rc`; `ifconfig` sets address, prefix, gateway and DNS at run time without disturbing open sockets.
- DHCP client in the kernel daemon (no task, no packet buffer: packets are built in and parsed from the W5500
  buffers in 8-byte pieces): DISCOVER/OFFER/REQUEST/ACK with broadcast replies, renewal at half the lease, retry
  and fallback that keep the old address. kd stack peak 724/1024 B with DHCP.
- Sockets for programs: W5500 sockets 5 and 6 as files (socket, connect, listen, sockinfo system calls; reads and
  writes go through the VFS, so redirection and pipes work). `nc` (TCP/UDP, client/server, interactive with a
  typing child task) and `ntpdate` use them; `netstat` lists all eight sockets; `service` starts and stops telnetd
  and httpd.
- W5500 buffers are now 4/2/2/2/2/2/1/1 KiB (was 8/2/2/2/2/0/0/0), so diag sends at 555 KiB/s instead of 627.
- Fixes found by the new tests: a program killed while reading a telnet or LCD terminal only left after the next
  typed line (terminal input waits now see the kill); a UDP send that timed out (no ARP answer) blocked every later
  send on that socket.
- Heap fragmentation: a boot script or a restarted console shell left long-lived stacks in the middle of the heap,
  and a pipeline then failed with 5 KiB free in two pieces. Login shells, their state and working directories now
  come from the top of the heap (`kmalloc_high`, `sys_alloc_long`); after the full test run the heap is one block.
- Costs: 116 B static RAM (heap 11944 B), about 16 KiB code (83 KiB total). Free heap with the console shell and
  one telnet session is 5056 B (M5b: 5168 B; floor 4800 B).
- Open: in one of five full test runs the second telnet connection of the telnet tests was accepted only after
  about 5 s instead of 0.7 s, so that section timed out. It did not recur in eight repeated console + telnet runs;
  the half-open re-arm in the listener (section M2) is the first suspect.

### Second RAM diet and run-time settings (after M5)

**Result (2026-10-16)**: `make test` 111/111 (free-RAM floor raised from 4800 to 5600 B), `make host-test` passes.

| | M5c | Now |
| --- | --- | --- |
| Static RAM (.data + .bss + .noinit) | 3648 B | 2600 B |
| Main stack (MSP) | 768 B | 512 B |
| Heap | 11944 B | 13272 B |
| Free with the console shell and one telnet session | 5056 B | 5952 B |
| Free with one telnet session, console not logged in (`console=ask`) | 5056 B | 7688 B |

- Task structs (112 B) now live in the same heap block as their stack, just above the stack top, and the task
  table holds pointers: the static 1120-byte table is gone, an unused slot costs 4 bytes, and `TASK_MAX` went from
  10 to 12.
- The boot code runs on a temporary stack at the top of the still unused heap (the vector table's initial SP is
  the heap end; kmain only allocates from the bottom). After the scheduler starts, the main stack serves only
  interrupts and exceptions: measured peak 124 B under network, keyboard and file system load, so it shrank from
  768 to 512 B (headroom for a panic raised inside PendSV).
- The idle task's stack went from 384 to 256 B (it uses 80 B); `.noinit` from 384 to its actual 360 B.
- `sysctl` sets features that hold RAM while they run: `console=off|ask|on` (default `ask`: the LCD shows
  "Press Enter to log in." and the 1.7 KiB shell exists only while someone uses it; `off` logs a running console
  shell out), `idle_logout=SECONDS` (a terminal without input reads as end of file, so its shell logs out; waiting
  readers re-check every second), `telnet_max=0-3`, and `telnetd`/`httpd` on or off. `free` also shows how much
  of the heap is task stacks. `/etc/rc` can apply settings at boot.
- Fix found by the new tests: telnet sessions sometimes stalled at start (first seen once in M5c). The per-socket
  "SEND in flight" flags shared one byte, and tasks on different sockets updated it without locking. Besides
  that, the W5500 sometimes finished a SEND without ever raising SEND_OK (everything sent and acknowledged,
  read pointer at the write pointer). This happened in about 1 of 4 new sessions, most likely because an earlier
  SEND picked up data that was queued just after it. The flags are now per socket, and a wait longer than 20 ms
  accepts an empty buffer with RD == WR as completion. `stat:` in the kernel log counts these: 29 in 120 sessions,
  none stalled.

### Code review fixes (2026-10-16)

An external review of `d84fa3d..359b48a` (M5b, M5c and the second RAM diet) reported six problems; all were
confirmed in the code and fixed. `make test` 115/115 adds a regression test for four of them; the same tests
fail 4/4 on the firmware before the fixes. `make host-test` 1458 checks plus the date conversions.

| ID | Problem | Fix |
| --- | --- | --- |
| R1 | A background command with four redirections wrote a fifth fd into a 4-entry stack array (`/dev/null` takes one) | one more entry |
| R2 | File reference counts, now shared between parent and child tasks, changed without a lock; a lost decrement keeps a pipe end open forever | counts change with interrupts off; only the task that drops the last reference closes |
| R3 | A killed program's `sys_alloc` memory was never freed | allocations carry the owner's pid (the block tag); a reaped task's leftovers are found by walking the heap and freed (logged) |
| R4 | `ed`: a substitution that needed more memory deleted the old text before failing; `r` kept part of a file | the whole `s` command makes room first (all or nothing); a failed read is taken back |
| R5 | A shell, its jobs and pipeline stages wrote to one terminal at once; telnet sends share socket pointers, so output was lost | a write lock per terminal (also for the LCD) and per program socket |
| R6 | DHCP kept using an address after its lease ended | the address is removed at the end of the lease (also when a running lease is refused), and requests repeat every 30 s; static addresses stay |

Measured on the old firmware: R1 broke the redirected output, R3 lost 1 KiB per killed `ed`, R5 dropped a line
in two concurrent 3.4 KB outputs, R6 kept the address. R2 has no direct test (it needs an exact preemption
point); the existing pipe and job tests cover the path. Cost: 40 B less free heap (locks).

### Full code review fixes (2026-10-16)

A second, whole-tree review (R7-R15) found nine more problems; all were confirmed and fixed. `make test` 117/117;
`make host-test` 1496 checks plus the date conversions, with two new host tests: littlefs on a RAM flash
(mount policy, each boot a forked process) and usbfs against a CH376S model (one open file, size written on
close). The usbfs test fails 10 checks on the old `usbfs.c`; the board tests for R10 and R15 fail on the old
firmware.

| ID | Problem | Fix |
| --- | --- | --- |
| R7 | usbfs accepted seven path components, and walk() copied the rest of the path into a 14-byte name | `to83` rejects more than six components; walk() stops instead of copying |
| R8 | Any littlefs mount error formatted the flash | only a blank flash (blocks 0 and 1 all 0xff) is formatted; otherwise `/` stays unmounted and `mkfs /` (new system call and command, refused while mounted) formats on request |
| R9 | A failed USB write-back (size update on close) was ignored, also when it happened while switching files | the error is kept per file and returned by `close`; `cp`/`mv` see it and `mv` keeps the source |
| R10 | `cp`/`mv` of a file onto itself (also `./x`, other case on `/usb`) truncated it; on USB `mv` then deleted it | paths are normalised (case-insensitive under `/usb`); `cp` refuses, `mv` does nothing |
| R11 | Several USB handles of one file used their own stale size (appends overwrote each other) | the size is re-read from the chip whenever a handle becomes the open file (after the other handle's write-back) |
| R12 | PIDs wrapped to 0 and to PIDs still in use | PID 0 and live/zombie PIDs are skipped; a reaped task's leftover memory is freed before its PID can be reused |
| R13 | `ps` and `/proc/tasks` received pointers to task names that may be freed before printing | names are copied (11 characters) into the list |
| R14 | `nntest`'s network namespace was not undone when its setup failed half-way | setup failures roll back; every cleanup step runs even if one fails |
| R15 | `nc` with a pipe or file as input sent everything before reading, so an echoing peer stalled it | a child task sends while the parent prints, as in interactive mode (32 KiB through a back-pressured echo server: the old firmware stopped after 3.3 KiB) |

### Third RAM pass, watchdog and soak test (2026-10-17)

- `/proc/stacks` keeps each configured stack size's high-water mark, ended tasks included (24 bytes). It showed two
  real problems: the diag worker reached 1176 of 1280 bytes, inside the 128-byte red zone (`ps` listed all 12 tasks
  at once on the stack and `report` had a 160-byte line buffer: now paged, formatted straight into the reply: 764 B),
  and a shell waiting for a killed program reached 980 bytes (the "freed N blocks" log ran in the reaping task:
  now the kernel daemon logs it: 592 B).
- Stacks trimmed to the measured peaks plus the red zone and room: shells, the diag and HTTP workers and init
  1280 -> 1152 B (peak 848 B over the whole suite and a 90-round soak), pipeline stages 1152 -> 1024 B (peak below
  740 B). kd stays at 1024 B (peak 740 B). fsd went the other way, 1536 -> 1664 B: its peak kept rising with
  file system use (1104, 1160, 1228, then 1268 B in the second soak run), and littlefs's deepest metadata paths
  (compaction, relocation) are hard to reach on purpose, so it keeps 268 B beyond the red zone.
- The HardFault handler no longer has its own 256-byte stack: it uses the bottom of the main stack, which interrupts
  never reach (peak 124 of 512 B from the top); `.noinit` is now only the 104-byte crash record.
- Free heap with the console shell and one telnet session: 5856 -> 6208 B (6344 B before fsd grew); `make test` 133/133.
- Watchdog: the hardware WDT on the 10 kHz oscillator, restarted by the kernel daemon every second; `sysctl
  watchdog=0|2|7|26` (default 26 s, long enough for the daemon's slowest legitimate waits). It pauses while a
  debugger halts the CPU. Tested with diag `W` (a never-yielding task at priority 0): reset 7-9 s after the daemon
  stopped with `watchdog=7`; the boot log reports "watchdog" (part of `make test`).
- Soak test (60 + 30 rounds of pipelines, programs, background jobs, `ed`, littlefs and USB file work, HTTP
  transfers, `nc`, `/proc`, log-in/log-out): the heap was identical at every check (one free block), and no panic,
  fault or reset occurred.

**Review fixes (2026-10-17)**: a review of `bbfc415..366d4b3` reported three problems; all confirmed and fixed,
`make test` 134/134.

| ID | Problem | Fix |
| --- | --- | --- |
| R22 | When a loaded program ended while a child it spawned still ran, the program's malloc'd memory was freed at once, although the child can use it through the program's globals (reproduced on the board: `spawn share` then `memtest` gave a HardFault on the old firmware) | the memory passes to a living relative sharing the image (`kmem_reown`), and is freed with the last of them |
| R21 | `sysctl watchdog` changed write-protected registers with interrupts on; the kernel daemon's kick in between locked the protection again, so turning the watchdog off could report success while it kept counting | the register writes and the state change happen with interrupts off and are read back; a test turns it off after 40 changes and starves the daemon for 10 s without a reset |
| R18 (follow-up) | `fwupdate FILE` staged and restarted in two system calls; another task could stage a different image in between | one system call stages and restarts while holding the update lock; the kernel writes "restarting" to the terminal |

### M6 Loadable programs
- Executable format `NXE`: header + image linked at 0 + a table of words to relocate (see the result below).
- Loader: read from the file system into RAM and run, about 4 KiB maximum; `exec`/`spawn` system calls.
- SDK: `sdk/` (crt0, libc subset headers, linker script, Makefile), built on the NanoPi.

**Acceptance**: build `hello.nxe` on the NanoPi, upload it, run `/bin/hello`; it exits normally and its RAM is reclaimed.

**Result (2026-10-16)**: `make test` builds the SDK and passes 8 new program checks; acceptance met.
- R6 decided against ROPI/RWPI: the whole program sits in one RAM block, so it is linked at 0
  (`sdk/nxe.ld`, `ld.lld --emit-relocs`) and `tools/mknxe.py` lists the offsets of its `R_ARM_ABS32` words;
  the loader adds the load address to them. Calls and branches are PC-relative. R9 stays free, any clang
  code works, and unknown relocation types are refused at build time.
- The C library is not linked into programs: `_start(argc, argv, lib)` receives a table of firmware functions
  (`lib/nnlib.h`: printf, string functions, the division helpers, errstr, time conversion), which run from
  flash. `hello` is 276 bytes (220 B image, 6 relocations); `primes` 620 bytes.
- `exec` system call (`kernel/exec.c`): checks the header (sizes, entry, relocation range, at most 8 KiB),
  reads the image straight into its heap block, verifies the CRC-32 over image and relocations, patches,
  clears bss and starts the program through the same child-task path as `spawn`. The block is owned by the
  task (the R3 owner tags), so it is freed when the task is reaped, also after a kill.
- The shell runs a non-built-in NAME as `/bin/NAME`, or a name with `/` as a path, always as a child task:
  pipelines, redirection, `&`, Ctrl-C and `kill` work as for built-ins. HTTP uploads may go to `/bin`;
  `/bin` is created at boot.
- Checks: arguments and exit status, programs in pipelines, firmware library calls and malloc failure,
  background job, Ctrl-C, damaged/truncated/non-NXE files refused, a program run from the USB drive, heap
  identical before and after.
- Not protected: without an MPU a faulty program can corrupt the kernel; a fault resets the board.

### M7 Optional
- Two-way serial console (depends on R3).
- LDROM bootloader: program a new APROM image from the W25X16, enabling `nnupdate` kernel updates over the network and "installing" programs into APROM to execute in place. **This changes the CONFIG0 boot selection and needs the owner's explicit consent first.** Done (consent given 2026-10-16), see below.
- Watchdog (done 2026-10-17, see the third RAM pass below), brown-out handling, simple login (`/etc/passwd`).

**Firmware update result (2026-10-16)**: `make test` 130/130 (5 new update checks), `make host-test` 1536 checks plus
the date conversions (28 new bootloader checks against modelled NOR flashes).
- Board state: LDROM was empty (read through ISP, all 0xff; logged in `logs/2026-10-16_ldrom-backup/`) and now
  holds the bootloader (`boot/`, 2.2 KiB of 4 KiB). **CONFIG0 changed from 0xffffffff to 0xffffff7f**: only CBS,
  so the chip starts in LDROM at power-on; CONFIG1 and the lock bit are unchanged. `tools/flash.py --boot aprom`
  restores the factory setting, `--show` reads both words.
- SPI flash layout: littlefs now ends at 0x1df000 (1916 KiB); a record sector and a 128 KiB image area follow
  (`lib/fwupdate.h`). A file system made by older firmware is detected and still mounted, but updates wait for
  `mkfs /` (allowed for such a file system when no file is open on it).
- `fwupdate FILE` checks the image's vectors, copies it to the image area (in fsd, which owns the SPI flash),
  reads it back, writes the record (state "staged") last, and restarts through the bootloader (ISPCON.BS +
  system reset). The bootloader checks the image CRC, erases and writes APROM through ISP, verifies the APROM
  CRC, marks the record "done" (bits only cleared, no erase) and starts APROM by clearing BS and resetting.
  A record still "staged" is installed again at the next boot, so an interrupted installation finishes; a "done"
  image restores APROM if APROM no longer holds a firmware; damaged images or records are ignored.
  `tools/nnupdate.py` does upload, `fwupdate` and the wait in one step (back in about 9 s for 93 KB).
- Measured on the board: an installation interrupted 1 s in (OpenOCD reset) started again and finished; a chip
  reset (`reboot -p`, like power-on) goes through the bootloader.
- Findings on this C-revision chip: a reset through the reset pin (OpenOCD) reloads ISPCON.BS from CONFIG0, so
  without CBS = LDROM an interrupted installation left a half-written APROM (recovered by setting CONFIG0, after
  which the bootloader finished it); LDROM and CONFIG can only be changed while the chip runs from APROM;
  OpenOCD's on-chip algorithms (programming, verify CRC) only work with the chip halted at a reset entry, not
  inside running firmware; while the bootloader works, the W5500 still accepts connections on the old
  firmware's sockets. `tools/flash.py` handles all of these (vector-catch reset into APROM's entry).
- A staged image is installed by the next boot through the bootloader, also right after programming APROM with
  Nu-Link: `fwupdate -c` cancels it. `fwupdate` cancels it itself if the restart fails.

**M6/M7 review fixes (2026-10-16)**: a review of `9860e59..bbfc415` reported five problems (R16-R20), all
confirmed and fixed; `make test` 132/132, `make host-test` 1544 checks plus the date conversions.

| ID | Problem | Fix |
| --- | --- | --- |
| R16 | `fwupdate -c` erased the record sector even while an old file system still covered it | every write to the update area checks the layout first (ENOSPC) |
| R17 | A child spawned by a loaded program ran code from the program's image, which was freed when the program was reaped | the image block has a reference count; the program and every child it spawns hold one, the last reaped frees it (`spawn` example and board test: the worker computes while `primes` allocates) |
| R18 | Staging, cancel and restart were serialized per flash job only; two stagings could both report success and leave an invalid record | one update operation at a time; others get EBUSY ("another update operation is running") |
| R19 | `nnupdate --host` uploaded to the given host but ran the update on 198.18.0.2 | the same host for every connection; `--iface` only with the test network |
| R20 | The bootloader ignored a failure to mark the update done, so every later boot erased and wrote APROM again | the result is read back and reported; a staged image that APROM already holds (CRC) is only marked installed, without writing APROM |

### LCD pictures and videos (2026-10-17)
A file format of our own for the 128x64 LCD and a player ([NNV.md](NNV.md)):
- `/dev/lcd`: the display as a 1024-byte frame in the controller's page layout. An open for writing takes the
  LCD from the console (one writer, others EBUSY); text output still updates the terminal grid, and the close
  (also when a program ends or is killed) clears the LCD and redraws the text. An LCD mutex keeps console text
  and frames apart on SPI3.
- NNV: a 16-byte header, then per frame a raw record or a delta record of literal/skip/fill codes applied in
  place to the previous frame, so the player streams with one frame buffer and a 128-byte read buffer.
- `play` (SDK program, 1.5 KiB file, 2.5 KiB RAM with bss): plays at the file's frame rate, loops with `-l`,
  holds a picture until Enter.
- `tools/mknnv.py`: ffmpeg decodes and scales; Floyd-Steinberg / Atkinson / Bayer / threshold dithering with
  numpy; demo files for tests; `info` checks a file and draws a frame as text.
- Result: `make test` 139/139 (5 new: a 60-frame video at 40 ms per frame plays in 2.6 s including loading
  `play`, one writer, picture until Enter, Ctrl-C back in 20-50 ms, damaged files, RAM back to the same free
  heap); `make host-test` 3716 checks (2170 new for the decoder). Two players at once do not fit next to a
  logged-in shell (each takes about 3.7 KiB with its stack), so the test holds `/dev/lcd` with `sleep > /dev/lcd &`.
- Bad Apple in full (219 s, 30 fps, 6572 frames, 676 KiB thresholded): 6572 frames in 219.058 s, 0 late
  (`play -v`). The header keeps the frame time in 1/256 ms, so 30 fps does not drift.
- Sound (same day): `/dev/audio` plays unsigned 8-bit samples at 8 kHz on the buzzer: PB11 as PWM4 (98 kHz
  carrier, 8-bit) and TIMER0 at the highest interrupt priority loading one sample per tick from a 1 KiB ring
  that exists only while the device is open. NNV sound records carry IMA ADPCM (4 KiB/s), each placed 64 ms
  ahead of its frame; `play` decodes each frame before waiting for its time, so queueing sound never makes a
  frame late. The HTTP upload limit went from 1 to 2 MiB for such files. Bad Apple with its sound: 1572 KiB,
  6572 frames in 219.027 s, 0 late. Heard on the board: music comes out harsh, rhythm without much pitch, so
  sound is opt-in (`mknnv.py --sound`, `play -s`).
  `make test` 141/141 (2 new: raw sound timing and one writer; a video with sound on time and gapless, silent
  without `-s`, Ctrl-C), `make host-test` 5736 checks (ADPCM against the Python decoder and by hand). A first
  run failed the interrupted-update check because Bad Apple with sound had filled the flash and the test's
  upload failed unnoticed; the test now checks that upload.

**NNV/audio review fixes (2026-10-17)**: a review of `e00e230..99497e2` reported three problems; all confirmed
and fixed.

| ID | Problem | Fix |
| --- | --- | --- |
| R24 | `audio_write` assumed one producer, but a program can share its open `/dev/audio` with a spawned worker (`fd_out`) and `/dev` writes are not serialized: a writer preempted between reading and committing `head` could move it behind `tail`, after which the computed room exceeded the ring and a copy ran past it into the next heap block (reproduced by the reviewer with the real driver and allocator on a host model) | a driver mutex makes each write one producer from start to end (a writer waiting for it gives up when killed); a ring holding more than its size panics instead of copying; `audioshare` (SDK) has a worker and the program write to one open file at once, and the test checks that the kernel played exactly the 16000 samples written |
| R25 | `play` used the header's frame count only to spot pictures: a file cut at a record boundary ended "successfully", and with a loop flag the remnant looped | at the end of each pass the frames read must match a non-zero header count (`cut short: N of M frames`, exit 1), also before a rewind; tests with one whole frame of 60, the header alone, and `-l` |
| R26 | At the end of a video the LCD went back to text as soon as the last frame was drawn, so the last frame lost its time (a two-frame 1 s video took 1 s) | the last frame stays until its time is up, then `-w` waits for Enter; a two-frame 1 s video takes 2 s |

**Full audit fixes (2026-10-17)**: a whole-tree audit at `99497e2` (plus the R24-R26 work) reported nine
problems (R27-R35), all confirmed and fixed, and twelve RAM/efficiency ideas (O1-O12), of which O1, O2 and O4
are done.

| ID | Problem | Fix |
| --- | --- | --- |
| R34 | The diag flash dump (`M`) called the SPI flash driver directly; at priority 3 it could cut into a littlefs or fwupdate transaction of fsd (priority 4), so a page program could go on with CS released and still report success | the size query and every 1 KiB read run in fsd (`vfs_in_fsd`), the network send outside it; test: four dumps of 64 KiB while a 32 KiB file is copied three times, CRCs unchanged |
| R35 | The CH376 benchmark reset the chip (mount) while usbfs could have a file open with unwritten data, then cleared usbfs's state without writing it back; that file's close still returned 0 | the bench runs in fsd; `usbfs_release` first writes back the open file (a failure stops the bench and stays for the file's close) and forgets the mount, so the file is reopened on next use; host tests (dirty file, a second handle, a failed write-back) and a board test (`cat > /usb/...` open across a bench) |
| R32 | `ed` returned a last line without `\n` without terminating it, so the end of a longer earlier line came along (`w /home/x` became `w /home/xongfilename`) | the line is terminated; a line cut by a read error is dropped rather than run; board test with such a script |
| R30 | Reads of a socket shared by a program and its children were not serialized: the W5500 read pointer or a UDP header could be read twice and the pointer moved back | a read lock per socket (separate from the write lock, so `nc` stays full duplex), taken with `mutex_lock_killable` (new) so a kill still ends a waiting reader |
| R28 | `kill` and `wait` cut PIDs to 16 bits: `kill 65578` killed PID 42; the shell's number parse could overflow | the system calls refuse PIDs outside 1-65535 (ESRCH); the shell takes at most 7 digits; board test with a running job |
| R29 | `spawn` reported success when copying the working directory failed, and the child ran in `/` | the copy is made first; without it the spawn fails (ENOMEM) and frees what it took |
| R27 | `mv FILE DIR` worked out `DIR/FILE` but renamed onto `DIR` itself (EISDIR on littlefs) | the rename uses the computed target; an overlong one is ENAMETOOLONG; board test |
| R31 | A read of 0 bytes blocked (a pipe with a writer, TCP, the terminal) or, on UDP, consumed a datagram | `vfs_read` and the terminal fallback return 0 for 0 bytes after checking the descriptor; `readzero` (SDK) tests a terminal and an empty pipe |
| R33 | A background job whose `/dev/null` could not be opened (EMFILE in a script with pipes) started anyway, reading the terminal | the job is not started; the error is reported |

RAM: O1, the shell frees its 472 B parser before waiting for a foreground command (the children have their own
argv); O2, a `/proc` file's text gets a block of its own size (a first pass counts) instead of 512 B, except
`klog`; O4, the diag worker allocates its 1 KiB transfer buffer only for the commands that move bulk data
(`L T R I M`), not for short text replies.

Result: `make test` 151/151 (10 new), `make host-test` 5751 checks plus the date conversions. The first board
run showed two consequences of the changes, both fixed: the CH376 bench, now in fsd, reported into the kernel
log instead of the diag reply (fsd now reports into the reply while the worker waits), and `play` closed the
LCD before the sound after holding the last frame, so the console redraw (about 8 ms) left `/dev/audio`
running dry while still open (64-71 samples counted short; the sound is now closed first, 0 short). Free RAM
with the console shell and one telnet session: 6136 B.

**O7, O8 and a crash found on the way (2026-10-17)**. Measured first, with a 676 KiB copy to the USB drive
while `play` runs (before / after):

| | b956785 | now |
| --- | --- | --- |
| copy time | 28.6 s | 19.7 s |
| fsd CPU during the copy | 19.1 s | 7.1 s |
| a 4 s video during the copy | 19.0 s, 80 frames late | 4.0 s, 9-11 late |
| CH376 bench write / read (256 KiB) | 122 / 227 kB/s | 114 / 224 kB/s |
| audio interrupt latency, worst (`ps`, `/proc` while playing) | 100 us | 12 us |

- O7: the CH376 wait spun on its INT pin inside fsd (priority 4), starving every task below it during USB
  I/O. INT (PB14) now has an interrupt: the wait polls 50 us, then sleeps until the edge (10 ms re-checks of the
  level stay as a safety net). PB14 is the chip's /INT0 pin, whose interrupt has its own vector (EINT0, IRQ 2),
  not the GPIO A/B one: wired to the shared vector first, it never fired, and every wait ran into the 10 ms
  timeout (USB at 31 kB/s). The W5500 waits for transmit room, a flush and a disconnect spin for 1 ms and then
  sleep on the socket's event semaphore 1 ms at a time.
- Found through O7 (the waits no longer hid it): every TCP close took 1 s. Captured on eth1: after the FIN
  exchange the W5500 stayed in CLOSING although the peer's FIN+ACK acknowledged its FIN (the two arrived in one
  segment), and answered each RST of the peer with another ACK, about 28 000 packets a second, until the
  disconnect's 1 s timeout closed the socket. kd spent that second in every telnet logout. `w5500_disconnect`
  now closes as soon as both FINs are through (CLOSING, TIME_WAIT, LAST_ACK): 48 packets, and a session's RAM is
  back 10 ms after logout instead of 1.2 s. The first attempt, closing listener sockets without waiting and
  finishing in `listener_poll`, only moved the second elsewhere and was dropped.
- O8: measured with the audio timer itself (its counter at the interrupt is the latency): 8-9 us under load,
  except `ps` and `/proc/tasks` and `/proc/stacks`, which scanned every task's stack for its fill pattern with
  interrupts off (100 us, close to the 125 us sample period). The scans now take 32 words at a time with
  interrupts on in between, checking the task is still there. Reaping and kmem_free_owned measured 8-9 us and
  were left as they are.
- A crash found while measuring: killing a program while it is deep in a system call (a player with sound,
  during a USB copy) overflowed its 1 KiB stack, because the exit (closing its files, `/dev/audio` logging)
  ran on top of the program's own frames. The stack check caught it as a panic, or the heap was corrupted
  first and the board hung. The overflow panic now says how deep the stack was, whether only its painted
  bottom was hit, and where it ran; with the heap block below printed to the serial log this showed pushed
  frame pointers below the stack. A program's exit (a kill or `exit`) now starts on a fresh frame at the top
  of its stack: its frames are not needed any more. 4 minutes of copies, playback and kills: no panic.
- Result: `make test` 154/154 (3 new), host tests unchanged.
- Since closed (below).

### The rest of the audit's ideas and the open points (2026-10-17)

| Item | Done | Measured |
| --- | --- | --- |
| O3 | an open littlefs directory takes only the flag and the directory state | 56 B instead of 164 per open directory |
| O5 | `play` is a built-in command (runs from flash, in a child task); `user/play.c` also builds as the SDK example | free RAM while `play -s` runs: 2984 B instead of 1392 (with a 2 KiB sound ring instead of 1 KiB); a telnet login fits again |
| O6 | `kmem_grow` (new system call `grow`) enlarges a block in place when the free block after it suffices; `ed` grows its buffer with it before falling back to allocate-copy-free | host tests: grow into a neighbour, exact fit, blocked by a used block |
| O9 | a USB directory listing goes on where it stopped while no other command used the CH376 (a command counter in the driver), else restarts and skips | 10 entries: 11 listing steps instead of 55; correct when another file is read in between |
| O10 | `ed`'s search walks the line offsets forward instead of locating each line from the buffer's start | host tests including the wrap to the first line |
| O11 | scripts are read 64 bytes per system call; one buffer, allocated only while a script runs; a nested script's start hands the read-ahead back to the outer file (lseek) | |
| O12 | `console.py` keeps the newest 20 000 lines (indices stay absolute) and `nntest` streams the serial log to its file | |

Gaps in the sound under load (`play -l -s` in the background, then each load; samples short before / now):
programs x10 9880 / 0, memtest 1715 / 0, flash copy 12848 / 0, USB copy 4476 / 612, `ls`/`ps`/`/proc` 3445 / 0.
The fixes, in the order they mattered:
- the encoder's lead: sound records came 64 ms ahead of their frames, so no ring size helped against a
  longer wait; now 192 ms (the 2 KiB ring holds it);
- littlefs opens did two lookups (`lfs_stat`, then the open); a file is now opened directly and a directory
  only when littlefs says so: opening `/home/mixed.nnv` went from 124-166 ms of fsd time to under 30;
- SPI flash reads: 12.5 MHz instead of 6.3 and 32 bits per transfer; reading the 676 KiB Bad Apple file
  went from 256 to 315 KiB/s, its CRC matches the host copy;
- `play` reads 512 bytes at a time (each fsd read waits its turn behind other jobs).

Reconnecting: a client that connected again within kd's 50 ms poll after a close got the W5500's reset to a
SYN on a closed port, whose ack is off by one; Linux ignores it and retries the SYN 1 and then 2 s later
(captured). diag, httpd and telnetd now listen again as soon as they close: back-to-back diag requests
0.25 s instead of 2.2, HTTP GETs 0.15 s instead of 1.8.

Result: `make test` 154/154, `make host-test` 5797 checks plus the date conversions. Free RAM with the console shell
and one telnet session: 6088 B (32 B less: the firmware grew by the built-in player). One run failed the
console checks because an aborted earlier run had left the network-boot test's `/etc/rc` (with `dhcp -t 15`)
on the board, which the next runs kept as "the board's own"; removed.

### The audit's remaining notes (2026-10-17)

- Short writes: `nn_write_all` (lib/stdio) writes again after a short write and turns a write that made no
  progress into EIO; printf's flush, `fputs_fd`, `cp`/`mv`'s copy, `mkfile`, `ed`, `nc` and `dmesg` use it.
- SPI timeouts: a flash transfer the controller never finished made the operation return whatever the
  receive register held. Reads now fail (littlefs gets LFS_ERR_IO, firmware staging EIO, the diag dump
  stops), programs and erases report it too, and an unreadable flash is never taken for a blank one (which
  would be formatted). The LCD cannot report an error to whoever draws: its timeouts are counted. Both counts
  are in the kernel daemon's minute line (`spi timeouts N (flash N, lcd N)`).
- Interrupt latency of reaping and spawning (the 8 kHz audio timer's count at its interrupt): reaping freed a
  program's leftover blocks, one heap walk each, with interrupts off, and spawning painted the new stack with
  them off. Reaping now takes the lock only for the parts that must be atomic and frees the blocks between
  (the zombie keeps its pid meanwhile); a child is created paused (`TASK_PAUSED`), set up, then started. Worst
  latency, starting ten programs: 60 -> 11 us; memtest: 47 -> 8 us; nothing above 14 us now.
- The old `scripts/w5500_network_test.py` (outside the repository) runs every cleanup step even when one fails
  and reports the failures, like `nntest`'s Namespace; not run (it refuses to start while eth1 has an address).
- Result: `make test` 154/154; free RAM with the console shell and one telnet session 6072 B.

### Programs run from APROM (2026-10-18)

Loaded programs used to sit whole in RAM (code, data, bss), and commands that would not fit there
(`play`, `ed`) were compiled into the firmware instead. Now their code runs in place from the APROM above
the firmware, and only data, bss and the stack take RAM.

- Position independence (R6, revisited): programs are built with `-fropi -frwpi`. Code and read-only data
  (text) reach each other PC-relative; data and bss are reached through r9, the static base. Text then
  needs patching only for words that hold a text address (pointer tables in read-only data), which depend
  on where the text lies, not on where the data lies: one copy of the text serves every run, also two runs
  at once. `sdk/nxe.ld` links text at 0 and data at 0x10000000 in its own segment, so lld's SBREL offsets
  count from the data start. NXE version 2 (`lib/nnlib.h`, `tools/mknxe.py`): header, text, text
  relocations, data, data relocations (bit 0: the word points into data or bss, rewritten as an offset);
  `text_crc` names the text, `data_crc` checks the rest; the file must have exactly the size the header
  gives. (Version 3, below: `data_crc` covers the header too.) Debug information does not link with `-frwpi` (SBREL in `.debug_info`), so programs are built
  without `-g`.
- Writing APROM from APROM: the NUC140 refuses it (ISPFF) unless ISPCON.APUEN (bit 3, in the BSP's header,
  not in the TRM excerpt) is set, and nothing may be fetched from APROM while an erase or a write runs. The
  ISP trigger and wait (`isp_run`, `arch/nuc140/fmc.c`) are copied onto the stack and run from there with
  interrupts off: 20 ms for a 512-byte page erase, about 40 us for a word. The wait counts SysTick periods
  and adds the missed ones to `jiffies`, so the clock does not fall behind. Measured on the board: erase
  20 ms, words read back as written.
- Slots (`kernel/xip.c`): the area runs from the firmware's end, rounded up to a page, to 128 KiB (35 KiB
  with this firmware; 31 KiB after the review fixes below). A slot is a 32-byte header at a page start, then the text, in whole pages. The
  header (magic, size, text CRC, a sequence number, a check word written last) is written after the text,
  so a slot whose writing a reset cut short is no slot; a slot is dropped by clearing its `live` word,
  before any of its pages is erased. The area is scanned when it is needed (no table in RAM): the kernel
  keeps only a list of the images in use, through the images themselves. New slots go after the newest one
  and wrap around, dropping whatever is in the way except a slot a running program uses; if nothing can be
  made free, the text is read into RAM after the data instead (it runs anywhere). A firmware update that
  grows overwrites only the slots at the start of the area, which are then below it and ignored.
- The loader (`kernel/exec.c`), under the slots' lock from the lookup until the image is registered: a
  slot with the same text CRC and size is used as it is (the text is not read from the file); otherwise
  pages are erased (other tasks run between pages), the text is read in 64-byte pieces, its relocations
  added on the way (they are read from their place in the file, a few at a time) and each word read back;
  only when the CRC over text and relocations matches is the header written. Then the data is read into
  the RAM block and relocated, bss cleared, and the task starts through `arch_call_sb`, which sets r9 (also
  for functions a program spawns, which share its data). `/proc/xip` lists the area and its slots. All of it
  runs on the caller's stack (the shell's): writing a slot first took the shell to 968 of its 1152 B; with
  relocations read one at a time, the data loader's buffer out of the frame below and `isp_run`'s copy cut
  to its 48 B, 848 B.
- Programs instead of built-ins: `play`, `ed` and `wc head tail grep hexdump` moved to `bin/` and are built
  by `make` as programs (`build/bin/*.nxe`, from `bin/main.c` calling `cmd_NAME` and the SDK's `crt0.c`,
  which gained `nn_write_all`, `fputs_fd` and `nn_interrupted`). `tools/nnupdate.py --bin build/bin`
  installs them, `nntest` too, and `help` lists the programs in `/bin` after the built-ins. The firmware
  went from 102.4 KB to 95.2 KB; `play` is 2780 B of text and 4 B of bss, `ed` 5372 B and 4 B.
- A stack overflow found on the way: `play -s` as a program died at the close of `/dev/audio`, 996 B deep
  in its 1 KiB stack. The kernel log's time stamp was formatted with `nn_snprintf` under `kvprintf`'s own
  `nn_vformat`, 200 B more on whatever stack logs. It is now built by hand: `play -s` peaks at 820 B, fsd
  at 932 B instead of 1260 B, kd at 600 B instead of 748 B.
- RAM: `hello` takes 24 B besides its stack (it took its whole 276-byte image); `primes 100000` found room
  for its 6 KiB sieve (until the console's scrollback, below, took 200 B of static RAM again). The
  firmware's static RAM grew by 40 B (the slot state), so free RAM with the console shell and one telnet
  session was 6032 B; `make test` 165/165.
- Tests: `test_xip` (54 checks on a simulated APROM: slots found again after a reboot, an unfinished slot
  ignored, the oldest giving way, slots in use never, no room, failing writes, damaged headers). On the
  board (`xip_tests`): `relocs` (every relocation kind) twice with one slot written, two runs at once, a
  program filling the whole area (35 KiB written in 2.1 s), three running programs filling it and a fourth
  going to RAM without touching theirs, a slot after they ended, the heap as before, slots kept across a
  reboot. The serial adapter would not enumerate on this day, so the sections that need the kernel log
  were run without it; the full `make test` is still to be repeated.
- Cost: the first run of a new program holds interrupts off 20 ms per page erased (the sound stutters, a
  burst of console input could overflow the UART FIFO); later runs write nothing.

### A bigger LCD terminal, scrollback, `tail` from pipes (2026-10-18)

- Two text sizes: the 5x7 font in 6x8 cells (21x8) and a new 3x5 font (`drivers/font3x5.c`, drawn for
  nnnix, 192 B) in 4x6 cells (32x10). A 6-pixel row straddles the LCD's 8-pixel pages and the LCD cannot
  be read back, so the console no longer draws characters but pixel columns: a page is computed from the
  (at most two) rows it shows and streamed to the controller column by column (`lcd_run_*`), no frame
  buffer. A changed cell repaints its columns of the pages it touches; a scroll repaints the 8 pages.
- `kernel/vt.c` keeps a 32x10 grid and uses `cols x rows` of it (`vt_resize`): the text stays at the
  bottom, rows that no longer fit go to the scrollback, and growing takes the latest back. Columns past
  the width keep their text, so a 32-column row is whole again after a trip to 21 columns.
- Scrollback (`kernel/scrollback.c`): rows that leave the top, trimmed, in a byte ring of
  `console_history` bytes (default 512, about 30 lines) allocated from the top of the heap while the
  console shell runs. While scrolled back the screen stays put even when programs print, and drawing then
  happens only for the keys (in the kernel daemon), so the 64 B of row buffers it needs are never on a
  writer's stack.
- Keys (`drivers/kbd.c`): PageUp/PageDown, Shift+Up/Down, Ctrl+-, Ctrl+= (and Ctrl+0, the keypad's
  -/+) become bytes from 0x80 up, which no key produced before; `console_input` acts on them and passes
  nothing on. Any other key while scrolled back returns to the terminal first. `sysctl console_font` and
  `console_history` set the same from a shell or `/etc/rc`.
- Tests: `test_console` (resizing both ways with the scrollback, the keys) and `test_scrollback` (order,
  trimming, the full ring never showing a cut line, taking lines back). On the board a new diag command
  (`G`) returns the pixels the console computes, and `nntest` compares them with the fonts' sources for
  both sizes (only the cursor's underline may differ), then pages and scrolls through the scrollback.
- `tail` reads input that cannot seek (a pipe, the terminal) into a 2 KiB ring (512 B if that is not
  free) and prints the last lines of it.
- RAM: the grid and console state take 200 B more static RAM, the scrollback 520 B while the console shell
  runs: free RAM with the console shell and one telnet session is 5304 B (the test's floor is now
  5200 B); `sysctl console_history=0` gives the 520 B back. Logging the console out frees 2144 B.
- `make test` 171/172 (the CP2102 is back): the failure was the program test's expectation that
  `primes 100000` fits, which the 200 B ended; it now checks `primes 50000` and passes. Interrupt latency
  at most 14 us.

### `wget` and `host` (2026-10-18)

- `bin/dns.c`: a stub resolver (RFC 1035) for programs: one A question with recursion desired, over UDP to
  the configured DNS server, three tries of 2 s, compression pointers and CNAME chains in the answer.
  Also the command `host NAME`.
- `bin/wget.c`: `wget [-q] [-S] [-O FILE] URL` over HTTP/1.0 with `Connection: close`; the body ends at
  `Content-Length` or the close, chunked bodies are decoded; up to 5 redirects (absolute or same
  server); exit status 0 only for a 2xx reply saved whole. Text 4952 B, RAM about 2 KiB while it runs.
  The first version overflowed its 1 KiB stack while saving to a file: a 192-byte path buffer of the
  redirect code sat in the inlined frame under the whole download; the URL is now built in place, and
  the peak is 728 B.
- Tests (`wget_tests`): an HTTP server and a DNS server on the test peer (inside the namespace): names,
  a CNAME, NXDOMAIN, 100 KB CRC-checked (55 KiB/s to the flash), both redirect kinds, chunked and
  unsized replies, 404, a redirect loop, a closed port, `https://` refused, heap unchanged.
- HTTPS is not supported yet (`wget` says so).
- `make test` 178/178.
- Typing `wget` on the LCD console reset the board: the usage message overflowed wget's 1 KiB stack
  (912 B). Printing to the LCD went 176 B deeper than to telnet, because the console drew the screen
  under the terminal's own calls (`vt_write`, `put`, `newline`, `vt_redraw`, then the page painting).
  The terminal's callbacks now only mark the pixel columns to draw per page, and the console draws them
  once `vt_write` has returned (also once per page, however many cells changed). On the LCD: wget's
  usage 872 -> 576 B, `hello` 704 -> 588 B, the console shell 928 -> 864 B, `ed` 632 B. And wget's steps
  are no longer inlined into one 192-byte frame. A test now runs programs on the LCD console.
  `make test` 179/179; free RAM with the console shell and one telnet session 5288 B (16 B of marks).

### Internet through the computer (2026-10-18)

`tools/nnshare.py on|off|status|run [--iface eth1] [--dry-run]`: the adapter's address (if missing), IPv4
forwarding (if off), nftables tables `nnshare` (masquerade for 198.18.0.0/30 out of every other
interface, forwarding to and from the adapter), a DHCP server offering the board 198.18.0.2/30 with the
computer as gateway and DNS, and a DNS relay on 198.18.0.1:53 to the computer's resolver (often
systemd-resolved on 127.0.0.53, out of the board's reach). `on` starts a background process that records
what it changed in `/run/nnshare.json` before changing it; `off` (or a stale record found by the next
`on`) undoes exactly that, every step even if one fails. `make test` refuses to run while it is on.
Tried: the DHCP server and the DNS relay against the board (a lease, `host example.org`, an unknown
name); on, status, a second on refused, and off, in a throwaway network namespace with dummy
interfaces (also with forwarding off before: turned on, and off again). NAT towards the real uplink was
not applied here: that is the user's to turn on.
- First use: `wget http://192.18.0.1:8080/` (a typo for 198.18.0.1) and external sites failed with
  "connection refused or timed out": the board had no gateway (`gw 0.0.0.0`), so nothing off its /30 could
  be reached, and the W5500 just times out. wget now says so before trying. `nnshare status` without sudo
  took the root-owned server for dead (the signal check got EPERM). With the gateway set, the user's
  HTTP server on this computer and http://example.org/ came through.

### Review fixes R36-R47 (2026-10-18)

A review at `e0f01f6` (`docs/reports/NNNIX_UPDATE_REVIEW_2026-10-18.md`, outside the repository) reported
twelve problems; all confirmed and fixed.

| ID | Problem | Fix |
| --- | --- | --- |
| R36 | `nnshare` trusted a recorded pid: after a crash it could report an unrelated process as the server, and `off` (as root) signal it; two `on` could run at once; the record was not written atomically | the server holds `/run/nnshare.lock` (flock) from before its first change to after its last undo; the record also holds the server's start time and a random token from its command line, and `off` signals through a pidfd only the process all of these match; liveness is the lock (also without root); records are written to a temporary file, synced and renamed. Tried in a network namespace (with its own `/run`): a second `on` and a second server refused, a record pointing at `sleep` reported off and the `sleep` left alone, `off` after a `kill -9` undid everything |
| R37 | an APROM slot's text was found by the program's CRC but never checked again, and the NXE header (entry, sizes, relocation counts) was in no CRC | slots (`XIP2`) also hold the CRC of the relocated text as written, checked on every use: a damaged slot is dropped (unless in use) and written again, counted as `damaged` in `/proc/xip`; NXE version 3: `data_crc` covers the header up to itself, and data is read on every run. Host test: a cleared bit in a slot; board test: `hello` with only its entry changed is refused although its text is in APROM |
| R38 | every UDP datagram became the socket's destination, also on a connected socket: a stranger's packet redirected DNS retries; `ntpdate` took any server-mode packet | a connected UDP socket keeps its destination and drops other senders' datagrams (`SYS_EAGAIN` when nothing else waits); a listening one still answers the last sender. `ntpdate` sends a nonce as its transmit time and takes only a reply that returns it (also mode, version 3-4, stratum 1-15). Board tests: a forged DNS answer from another port, two forged NTP answers (another port, another originate time) before the real one |
| R39 | `wget` took a relative `Location` (`next.html`, `//host/x`, `?q`) for a host name or a path on the old host | `bin/url.c`: RFC 3986 reference resolution for http, `.`/`..` removal, fragments cut; host test with RFC 3986's examples; the 192-byte `location` buffer is gone (the reference is resolved from the header line into the URL) |
| R40 | lengths parsed leniently (empty, junk suffix, overflow), the last of `Content-Length`/`Transfer-Encoding` won, `compress` counted as chunked, chunk separators unchecked | strict decimal/hex parsing with overflow checks; chunked wins; any other transfer coding, a bad or second different `Content-Length`, a bad chunk size line or separator, or a missing trailer end is refused (exit 1) |
| R41 | a DHCP option running past the packet ended the DHCP thread (option 53 of length 0: IndexError), dead threads went unnoticed, the DNS relay started a thread per query without limit | options are bounds-checked and a request without a one-byte message type ignored; each request is answered under a try; the threads are checked every second and if one ends sharing stops and everything is undone; at most 16 DNS queries in flight, more dropped and counted |
| R42 | an SPI1 transfer that timed out returned the receive register as data and the transaction went on | a timeout fails the transaction: chip select up at once, SPI1 reset (IPRSTC2 bit 13) and set up again, the rest of the transaction skipped (reads zeros), `spi_end()` reports it. W5500: data reads and writes report it (nothing received is taken, nothing queued is counted), register reads fail as zeros (a closed socket, no data, no room); CH376: the command's status is `CH376_SPI_ERROR` (usbfs: EIO), data read in a failed transaction is not used. Host test with a stuck GO_BUSY |
| R43 | the loader computed `block + 1` with `block` null after a failed allocation | computed only once the allocation succeeded |
| R44 | `mkfile FILE 1M` made 1 byte, `junk` 0, overflow wrapped, and the file was truncated first | the size is parsed strictly (digits, optional `k`, overflow refused) before the file is opened; board test: five bad sizes leave a 32 KiB file as it was |
| R45 | changing the scrollback size freed the old ring before allocating the new one; a failure left none while `sysctl` showed the size asked for | the new ring is allocated first; a failure keeps the old ring and size and `sysctl` says `out of memory`; a shell started without memory for a ring reports 0. Board test: 8192 bytes beside the shells |
| R46 | `console.py`'s History kept 20 000 lines but slices and `wait_for` walked every dropped index (200 000 lines: 1.6 MB, 0.65 s for 11 kept lines) | slices start at the oldest kept line, `wait_for` too, and `console.py` prints `[N lines dropped]` when it fell behind: 11 lines, 292 bytes, 0.1 ms |
| R47 | `mknxe` indexed the ELF without bounds (four bytes `\x7fELF`: IndexError) and allocated whatever span the sections named | header, section table, names, links and symbol indices checked; text over 64 KiB or data and bss over 8 KiB refused before allocating; anything left becomes a clear error. Host test: truncations and corrupted fields of a real program |

Also from the review: the documents still said 35 KiB of APROM for programs, 93 KiB of code, that all
commands are built in and that there is no DNS resolver. The fixes cost 844 bytes of firmware (the
checks in xip, spi, the W5500 and CH376 drivers, sockets and `ntpdate`), so the program area is now 31 KiB
(firmware 99 060 B; static RAM 2812 B). Free RAM with the console shell and one telnet session: 5272 B.

Checked on the board after installing it (with `nnshare` running, so not with `make test`, which refuses
then): `hello` runs and a copy with only its entry changed is refused; `/proc/xip` with `damaged 0`; relative
redirects (`small.txt`, `../small.txt`, `./../dir/../small.txt`, `?x=1`) and chunked-over-length; the six
malformed replies refused with exit status 1; the 100 KB download CRC-checked; `mkfile` with four bad sizes
leaving its file as it was; 8 KiB of scrollback refused beside the shells with the heap unchanged;
`ntpdate` setting the clock from the third answer after two forged ones; http://example.org/ through
`nnshare`. Not run on the board: the forged DNS answer (the relay of `nnshare` holds port 53).

### Input for programs: `/dev/input` and the SW_INT button (2026-10-18)

For games. `/dev/input` (`kernel/input.c`, `fs/devfs.c`): opened read-only by one task at a time (others
EBUSY), it takes the keyboard from the console and gives the holder 8-byte events (`struct nn_input_event`,
`lib/sys.h`): keyboard keys pressed and released by HID usage, with the character typed; modifiers as keys
`0xe0`-`0xe7`; mouse buttons, motion and wheel; the board keypad's 1-9. A read waits for the first event,
or with the new `O_NONBLOCK` returns `SYS_EAGAIN`; the 32-event queue (264 B) is allocated while the file
is open, and mouse motion merges in it. Key events come from the difference between two boot-keyboard
reports (`kernel/inputev.c`, host tested: modifiers, several keys, releases first, ErrorRollOver, the queue).
- The board (from the BSP's `Scankey.c`): the keypad on PA0-PA5, quasi-bidirectional, PA2/PA1/PA0 low in
  turn and the rows read on PA3-PA5 (keys 1 4 7, 2 5 8, 3 6 9); SW_INT on PB15, low when pressed. Both are
  scanned by kd every 20 ms, and only while a program holds `/dev/input` or `/dev/lcd`; a state counts when
  two scans agree. All nine keys are read, so several may be held.
- SW_INT ends the holder of `/dev/input`, or else of `/dev/lcd`: `task_kill`, which takes effect at the next
  system call, and, for a program that makes none (a game stuck in a loop), `task_end_now`: once the task
  has been preempted with its pc in program text (APROM above the firmware or RAM, where the kernel never
  runs), its saved frame is replaced by one at the top of its stack that calls `task_exit(130)`. A task
  blocked in a system call is never rewritten (its pc is in the kernel); kd retries every scan for 3 s.
- Tests: diag `J` feeds CH9350 frames through the parser as if from UART1 (`uart1_inject`), or holds board
  keys down (bit 16 of the length); `input_tests` in nntest: `keys` (`sdk/examples/keys.c`) sees `a`, Shift+`a`,
  a mouse move and two keypad keys; a second reader gets EBUSY; SW_INT ends `keys spin` (no system calls);
  `snake` (then `sdk/examples/snake.c`, now in `bin/`) ends with Esc and with SW_INT; heap as before. Run on
  the board by hand (not `make test`: `nnshare` was on), also typing into the LCD console through the
  CH9350 path while nobody holds `/dev/input`.
- Cost: 2.5 KiB of firmware (the program area is now 28.5 KiB), 24 B of static RAM; free RAM with the
  console shell and one telnet session 5248 B.

### Games: snake, tetris, flappy (2026-10-18)

`bin/snake.c`, `bin/tetris.c`, `bin/flappy.c` (about 3 KiB of text each, 1-1.5 KiB of bss with the 1 KiB
picture), sharing `bin/game.c`: `/dev/input` with `O_NONBLOCK` turned into actions (the same keys in every
game, keyboard or keypad), a picture in `/dev/lcd`'s layout drawn with pixels, boxes and text, shown whole
per frame, a xorshift random number, and the game-over box with the best score. The text uses the
firmware's fonts, handed to programs in `nn_lib` version 2 (`font3x5`, `font5x7`: 8 bytes of firmware);
`mknxe` now marks programs as needing version 2.
- Tetris: a 10x20 board of 3-pixel cells between the score and the next piece; pieces turn clockwise in
  their n x n box with kicks of 0, -1, +1, -2, +2 columns; DAS 180 ms then 60 ms; the speed rises every 10
  rows. It redraws only when something moved: drawing the whole picture every 20 ms cost 30% of the CPU,
  now 2%. Flappy: 30 ms frames, gravity and flap in 1/16 pixel, the bird's pixels tested against what was
  drawn; 14% CPU while flying. Snake: 4-pixel cells under a score bar, a step every 160 ms down to 40.
- On the host: `lib/sys.h` turns system calls into `host_syscall()` under `NNNIX_HOST`, and
  `tests/host/game_sim.c` answers them with a clock of its own (a sleep takes no time), keys from a script
  and PBM screenshots; `tests/host/test_games.py` plays each game (tetris until it is full, snake into a
  wall, flappy to the ground, quit from both keyboards) and checks the pictures. On the board: each game
  started, played and ended with Esc (`input_tests`), heap unchanged; stack peak 640 of 1024 B.

### Streaming video: `nc -l 1234 | play` (2026-10-18)

- `play` without a file (or `-`) reads standard input; `-l` and the file's loop flag do not apply, a picture
  waits for Enter on `/dev/tty`. From a pipe (lseek fails) its read buffer is 128 B instead of 512: a pipe
  holds 128 B, and the 384 B saved let `/dev/audio` open beside the console shell.
- `nc`, from the terminal into a pipe or a file, only receives: no typist task (1 KiB of stack), Ctrl-C
  still works; its messages go to fd 2, never into the data.
- `free -l` shows the lowest free heap since boot or `free -r` (kmem keeps the free byte count and its low
  mark; `meminfo` takes a reset argument; the stats field that was always 0 now carries it).
- Measured, all of Bad Apple sent over TCP from the computer: without sound 6572 frames in 219.058 s, 0
  late, lowest free 1264 B with two telnet sessions; with sound and one telnet session 0 late, 0 samples
  short, lowest free 1856 B; with sound, telnet and the console shell, `/dev/audio` gets only its 1 KiB
  buffer (before the smaller read buffer: no sound at all, "out of memory"), so every frame waits behind
  the 192 ms of sound the file puts ahead and is shown about 64 ms late (6569 late, 0 samples short, 219 s
  in all; lowest free 736 B). A ^C while connected takes 1 s, as before: the close waits for the peer.
- Tests: `nnv_tests` streams the 60-frame and the sound demo through `nc -l 1234 | play` (0 late) and a cut
  one (`cut short`); `test_kmem` the low mark.

### `/dev/adpcm`: the sound decoded in the timer interrupt (2026-10-18)

- The problem: with the console shell and a telnet session logged in, `/dev/audio` gets only its 1 KiB ring
  (128 ms of 8-bit samples), but NNV files put their sound 192 ms ahead of the frames (224 ms with the
  record's own 32 ms, about 257 ms while `play` reads the next frame), so `play -s` blocked on the sound and
  showed every frame about 64 ms late. Weighed: a shorter lead in `mknnv.py` would have to drop to the old
  64 ms (the margin for fsd stalls that 192 ms was chosen for) and re-encode every file; `play` cannot shift
  its clock, because a frame's data only comes after the sound placed before it; holding the codes in `play`
  costs RAM it does not have. Taken: keep the sound compressed until it plays.
- `/dev/adpcm` (`fs/devfs.c`) opens the same buzzer for NNV sound records as they are in a file (type 2,
  length, i16 predictor, u8 step index, codes), written back to back in pieces of any size. The ring logic
  (`drivers/audioring.h`, header-only so the host test runs it as is) stores blocks of [n, predictor, index]
  and n code bytes (1-255; longer records go on in blocks that keep the state) and the 8 kHz TIMER0 handler
  decodes one 4-bit code per tick with `lib/nnv.h`'s IMA decoder. A block is queued together with its first
  codes, so the handler never counts a header alone as a gap. 1 KiB holds 248 ms (4 bytes of block per
  256-sample record), 2 KiB 496 ms. A record that is not a sound record (or has a step index above 88)
  fails the write with `EINVAL`. One writer between the two devices (`EBUSY`).
- `play -s` hands the records over unchanged (no decoding of its own, no 64-byte sample buffer on its stack;
  `play` 488 B smaller). A waiting ADPCM writer is woken once 32 B (8 ms) are free instead of half the ring,
  because the ring is nearly full in normal play; `/dev/audio` keeps waking at half.
- The close's log line also gives the handler's longest run and the ring's size: `audio: N samples played,
  M short, latency up to L us, interrupt up to C cycles, R B ring`. Longest run: 81 cycles (1.6 us) with
  8-bit samples, 243 cycles (4.8 us) decoding while `play` reads a file, 369 cycles (7.4 us) when streamed
  (the semaphore given to the waiting writer), of the 6274 cycles between samples. Static RAM: 32 B more.
- Measured, all of Bad Apple through `nc -l 1234 | play -v -s` from a telnet session with the console
  shell logged in (`sysctl console=on`): 6572 frames in 219.058 s, 0 late (6569 before), 1 752 534 samples
  played, 0 short, 1024 B ring, lowest free 704 B. Interrupt latency in this streaming case is 82-83 us, the
  same with the previous firmware (8 us playing a file): something on the network/pipe path holds
  interrupts off, not this handler.
- Tests: `test_audio` (host) feeds the tone demo's sound records in pieces of 1-64 bytes and larger into
  rings of 64 B, 1 and 2 KiB drained at different rates and compares every sample with the Python decoder;
  also records split into blocks, records without codes, a state that jumps back (a rewind), a header alone
  queueing nothing, bad records, a full ring and PCM passed through. On the board: `play -s` and the
  existing sound tests now go through `/dev/adpcm` (32000 samples, 0 short); `cat x.u8 > /dev/audio` and
  `audioshare` (16000 samples) unchanged.

## 8. Source layout

```
nnnix/
  Makefile            make / make flash / make test / make host-test
  arch/nuc140/        startup  vectors  link.ld  clock  board  switch (context switch)  fault
  kernel/             task (scheduler)  sync  syscall  kmem  klog  panic  tty  diag
  drivers/            uart spi ch9350 ch376 w5500 hidkey  (later: lcd w25x16 rtc)
  fs/                 (M4) vfs devfs procfs usbfs rootfs pipe  third_party/littlefs/
  net/                socket layer, telnetd  (later: dhcp)
  lib/                string, printf, EABI division, system-call stubs (shared by kernel and users)
  user/               nsh and built-in commands, demo programs
  bin/                the programs for /bin (play, ed, text tools), built like SDK programs
  sdk/                (M6) programs: crt0, nxe.ld, examples (hello ... relocs)
  tests/host/         unit tests on the build machine
  tools/              flash.py  console.py  nntest.py (on-target tests)  mknxe.py  mknnv.py  nncp.py  nnupdate.py
```

Repository: <https://github.com/willie6666/nnnix>; at least one commit and tag per milestone.

## 9. Open research items (before the milestone that needs them)

| ID | Item | Needed by |
| --- | --- | --- |
| R1 | LCD controller initialisation sequence and SPI3 framing. Done in M3; PD14 (backlight on this board revision) is left at its reset state | M3 |
| R2 | Onboard SPI flash: W25Q16 (ef4015, 2 MiB) on SPI2. Done in M4 | M4 |
| R3 | The RS-232 transceiver on PB0 and how to isolate it; fallback: software UART RX or another pin | M7 |
| R4 | RTC 32.768 kHz crystal: present, stable within 0.3 s, RTC counts through resets. Done in M5. Buzzer: PB11, low active, a magnetic 2.4 kHz buzzer; driven as PWM4 for `/dev/audio` (2026-10-17) and `/dev/adpcm` (2026-10-18). Keys, EEPROM pins remain | M5/M7 |
| R5 | littlefs on the Cortex-M0: 19.5 KiB code, about 2.2 KiB RAM with fsd. Done in M4 | M4 |
| R6 | Position independence for loaded programs: first not needed (linked at 0, relocated in RAM, M6); since 2026-10-18 `-fropi -frwpi`, so the text runs in place from APROM | M6 |
| R7 | W5500 interrupts (Sn_IMR/SIMR) with the PB2 GPIO interrupt, measured | M2 |

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| 16 KiB of RAM is not enough | Measure RAM at every milestone since M1; process count and stack sizes are configurable; `/tmp` can be dropped |
| Without memory protection one program can corrupt the kernel | Stack checks, HardFault records, watchdog reboot |
| The CH376S single-file limit makes multi-session access slow | usbfs reopen cache; use USB mainly for transfers and keep everyday files on `/` |
| The LIN transceiver pulls PB4 low again | Keep the verified LIN normal-idle setup in arch initialisation |
| A bad flash image stops the board from booting | The original APROM is backed up; only M7 touches CONFIG/LDROM, and only with consent |
| Keyboard input cannot be automated | Automated tests use telnet; keyboard parsing is unit tested with recorded CH9350 frames |
