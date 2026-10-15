# nnnix development plan

**nnnix**: NuvotoN NUC UNIX (NuvotoN Nuc NIX), a small UNIX-like operating system for the
NU-LB_002 board (NUC140VE3CN).

Created 2026-10-15. Status: **M4 done; M5 in progress** (2026-10-16).
The first M5 part adds shell scripts, variables, text tools and HTTP file transfer;
pipes/jobs, editing, network configuration/DHCP and RTC remain pending.

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

SRAM 16 KiB. Measured after the RAM diet: static 2.4 KiB, `.noinit` 384 B, MSP 768 B, heap 13.1 KiB
(idle 384 B and kd 1024 B stacks, console shell 1152 B; 10.4 KiB free; each telnet session about 1.3 KiB).
The original estimate:

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
| Buzzer, keys, EEPROM, RTC crystal | Per board documentation | To verify (R4) |

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

### M6 Loadable programs
- Executable format `NXE`: header + position-independent code (clang `-fropi -frwpi`, R9 as the data base) + relocations.
- Loader: read from the file system into RAM and run, about 4 KiB maximum; `exec`/`spawn` system calls.
- SDK: `sdk/` (crt0, libc subset headers, linker script, Makefile), built on the NanoPi.

**Acceptance**: build `hello.nxe` on the NanoPi, upload it, run `/bin/hello`; it exits normally and its RAM is reclaimed.

### M7 Optional
- Two-way serial console (depends on R3).
- LDROM bootloader: program a new APROM image from the W25X16, enabling `nnupdate` kernel updates over the network and "installing" programs into APROM to execute in place. **This changes the CONFIG0 boot selection and needs the owner's explicit consent first.**
- Watchdog, brown-out handling, simple login (`/etc/passwd`).

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
  sdk/                (M6)
  tests/host/         unit tests on the build machine
  tools/              flash.py  console.py  nntest.py (on-target tests)
```

Repository: <https://github.com/willie6666/nnnix>; at least one commit and tag per milestone.

## 9. Open research items (before the milestone that needs them)

| ID | Item | Needed by |
| --- | --- | --- |
| R1 | LCD controller initialisation sequence and SPI3 framing. Done in M3; PD14 (backlight on this board revision) is left at its reset state | M3 |
| R2 | Onboard SPI flash: W25Q16 (ef4015, 2 MiB) on SPI2. Done in M4 | M4 |
| R3 | The RS-232 transceiver on PB0 and how to isolate it; fallback: software UART RX or another pin | M7 |
| R4 | RTC 32.768 kHz crystal, buzzer, keys, EEPROM pins | M5 |
| R5 | littlefs on the Cortex-M0: 19.5 KiB code, about 2.2 KiB RAM with fsd. Done in M4 | M4 |
| R6 | Whether clang's ROPI/RWPI works for thumbv6m | M6 |
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
