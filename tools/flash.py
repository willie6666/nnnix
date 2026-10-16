#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Program the NUC140VE3CN through Nu-Link (OpenOCD), verify, reset.

  flash.py [IMAGE]            APROM (default build/nnnix.bin)
  flash.py --ldrom BOOT.BIN   LDROM: the bootloader (build/boot.bin), verified through ISP
  flash.py --boot ldrom       CONFIG0 CBS: start in LDROM (the bootloader) at power-on
  flash.py --boot aprom       CONFIG0 CBS: start in APROM (the factory setting)
  flash.py --show             CONFIG0, CONFIG1 and whether LDROM holds a program

CONFIG0 is changed by read-modify-write of the CBS bit only; every other bit
(the security lock included) keeps its value, and the result is read back.

OpenOCD lookup order:
  1. $NNNIX_OPENOCD_ROOT: an extracted Debian openocd package root (usr/bin/openocd, usr/share/openocd)
  2. ../.tools/openocd/root next to this repository (the author's NanoPi setup)
  3. openocd on PATH with its built-in scripts
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parent.parent
PDID = 0x20014018                    # NUC140VE3CN


def openocd_command():
    candidates = [os.environ.get('NNNIX_OPENOCD_ROOT'), REPO.parent / '.tools/openocd/root']
    for root in filter(None, candidates):
        root = Path(root)
        binary = root / 'usr/bin/openocd'
        if binary.exists():
            libdir = root / 'usr/lib/aarch64-linux-gnu'
            return ['env', f'LD_LIBRARY_PATH={libdir}', str(binary),
                    '-s', str(root / 'usr/share/openocd/scripts')]
    binary = shutil.which('openocd')
    if binary:
        return [binary]
    raise SystemExit('OpenOCD not found: set NNNIX_OPENOCD_ROOT or install openocd')


def write_user_file(path, text):
    """Write a log; under sudo, hand it back to the invoking user so later builds can replace it."""
    path = Path(path)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if os.geteuid() == 0 and 'SUDO_UID' in os.environ:
            os.chown(path, int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID']))
    except OSError as exc:
        print(f'warning: cannot write {path}: {exc}', file=sys.stderr)


def check_image(data):
    if not 8 <= len(data) <= 128 * 1024:
        raise SystemExit('image does not fit the 128 KiB APROM')
    sp, reset = int.from_bytes(data[:4], 'little'), int.from_bytes(data[4:8], 'little')
    if not (0x20000000 < sp <= 0x20004000 and reset & 1 and reset < len(data)):
        raise SystemExit(f'implausible vector table: sp=0x{sp:08x} reset=0x{reset:08x}')


# ISP through the flash controller's registers, driven from the debugger.
ISP_SETUP = [
    'write_memory 0x50000100 32 0x59', 'write_memory 0x50000100 32 0x16', 'write_memory 0x50000100 32 0x88',
    'write_memory 0x50000204 32 [expr {[lindex [read_memory 0x50000204 32 1] 0] | 4}]',   # ISP clock
    'write_memory 0x5000c000 32 [expr {[lindex [read_memory 0x5000c000 32 1] 0] | 0x39}]',  # ISPEN LDUEN CFGUEN APUEN
    'proc isp {cmd addr data} { write_memory 0x5000c00c 32 $cmd; write_memory 0x5000c004 32 $addr; '
    'write_memory 0x5000c008 32 $data; write_memory 0x5000c010 32 1; '
    'while {[lindex [read_memory 0x5000c010 32 1] 0]} {}; '
    'set f [expr {[lindex [read_memory 0x5000c000 32 1] 0] & 0x40}]; '
    'if {$f} {write_memory 0x5000c000 32 [expr {[lindex [read_memory 0x5000c000 32 1] 0] | 0x40}]; error "ISP command failed"}; '
    'return [lindex [read_memory 0x5000c008 32 1] 0] }',
    'proc show {addr} { echo [format "isp 0x%08x 0x%08x" $addr [isp 0 $addr 0]] }',
]


# Halt at APROM's reset entry with the chip running from APROM: clear
# ISPCON.BS and reset with a vector catch. Needed to verify APROM (mapped at 0
# only then) and to change LDROM or CONFIG, which the chip refuses while it
# runs from LDROM (as it does right after connecting once CONFIG0 says LDROM).
APROM_ENTRY = [
    'write_memory 0x50000100 32 0x59', 'write_memory 0x50000100 32 0x16', 'write_memory 0x50000100 32 0x88',
    'write_memory 0x5000c000 32 [expr {[lindex [read_memory 0x5000c000 32 1] 0] & ~2}]',      # BS = APROM
    'write_memory 0xe000edfc 32 [expr {[lindex [read_memory 0xe000edfc 32 1] 0] | 1}]',       # VC_CORERESET
    'catch {write_memory 0xe000ed0c 32 0x05fa0004}', 'sleep 200', 'catch {halt}', 'wait_halt 1000',
    'write_memory 0xe000edfc 32 [expr {[lindex [read_memory 0xe000edfc 32 1] 0] & ~1}]',
    'if {[lindex [read_memory 0x5000c000 32 1] 0] & 2} {error {still running from LDROM}}',
]


def run_openocd(steps, log=None, timeout=180):
    try:
        socket.create_connection(('127.0.0.1', 6666), timeout=.3).close()
        raise SystemExit('another OpenOCD session holds the Nu-Link (tcl port 6666); stop it first')
    except OSError:
        pass
    command = openocd_command()
    if os.geteuid() != 0:
        command = ['sudo', '-n'] + command
    command += ['-f', 'interface/nulink.cfg', '-f', 'target/numicro.cfg']
    for step in ['gdb_port disabled; tcl_port disabled; telnet_port disabled', 'init', 'halt', 'wait_halt 1000',
                 f'set pdid [lindex [read_memory 0x50000000 32 1] 0]; '
                 f'if {{$pdid != 0x{PDID:08x}}} {{error {{unexpected MCU, not programming}}}}'] + steps:
        command += ['-c', step]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if log:
        write_user_file(log, result.stdout)
    return result


def isp_words(output):
    """{address: value} from the 'isp ADDR VALUE' lines of show."""
    words = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == 'isp':
            words[int(parts[1], 16)] = int(parts[2], 16)
    return words


def flash(image, log=None):
    image = Path(image).resolve()
    check_image(image.read_bytes())
    # OpenOCD's on-chip algorithms (programming, the CRC of verify_image) only
    # work with the chip halted at a reset entry, before firmware touched the
    # clock, the flash controller or the stacks: so program right where
    # connecting halted it. With CONFIG0 set to LDROM that is the bootloader's
    # entry, and LDROM is mapped at 0; to verify, clear ISPCON.BS and reset with
    # a vector catch, which halts at APROM's entry with APROM mapped. ISP
    # addresses APROM directly either way. The chip always ends up running.
    steps = ['flash probe 0', f'flash write_image erase {{{image}}} 0x0 bin'] + APROM_ENTRY + [
             f'verify_image {{{image}}} 0x0 bin', 'reset run', 'shutdown']
    result = run_openocd(steps, log)
    ok = result.returncode == 0 and 'verified' in result.stdout
    for line in result.stdout.splitlines():
        if line.startswith(('wrote', 'verified', 'Error')) or 'Device Name' in line:
            print(line.strip())
    if not ok:
        print(result.stdout[-2000:], file=sys.stderr)
    return ok


def flash_ldrom(image, log=None):
    data = Path(image).read_bytes()
    if not 8 <= len(data) <= 4096:
        raise SystemExit('the bootloader must be 8 bytes to 4 KiB')
    data += b'\xff' * (-len(data) % 4)
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    steps = APROM_ENTRY + ISP_SETUP + [f'isp 0x22 0x{0x100000 + page:x} 0' for page in range(0, 4096, 512)]
    steps += [f'isp 0x21 0x{0x100000 + 4 * i:x} 0x{w:08x}' for i, w in enumerate(words)]
    steps += [f'show 0x{0x100000 + 4 * i:x}' for i in range(len(words))] + ['reset run', 'shutdown']
    result = run_openocd(steps, log, timeout=600)
    got = isp_words(result.stdout)
    bad = [i for i, w in enumerate(words) if got.get(0x100000 + 4 * i) != w]
    ok = result.returncode == 0 and not bad
    print(f'LDROM: {len(data)} bytes written, ' + ('verified' if ok else f'{len(bad)} words differ'))
    if not ok:
        print(result.stdout[-2000:], file=sys.stderr)
    return ok


def show_config(log=None):
    result = run_openocd(ISP_SETUP + ['show 0x300000', 'show 0x300004', 'show 0x100000', 'reset run', 'shutdown'], log)
    got = isp_words(result.stdout)
    if result.returncode or len(got) != 3:
        print(result.stdout[-2000:], file=sys.stderr)
        return None
    c0, c1, ld = got[0x300000], got[0x300004], got[0x100000]
    print(f'CONFIG0 0x{c0:08x} (power-on boot from {"APROM" if c0 & 0x80 else "LDROM"}, '
          f'{"unlocked" if c0 & 2 else "LOCKED"}), CONFIG1 0x{c1:08x}, '
          f'LDROM {"empty" if ld == 0xffffffff else "programmed"}')
    return c0, c1, ld


def set_boot(target, log=None):
    current = show_config(log)
    if current is None:
        return False
    c0, c1, ld = current
    want = (c0 & ~0x80) if target == 'ldrom' else (c0 | 0x80)
    if target == 'ldrom' and ld == 0xffffffff:
        raise SystemExit('LDROM is empty: program the bootloader first (--ldrom build/boot.bin)')
    if not c0 & 2:
        raise SystemExit('CONFIG0 says the chip is locked; not touching it')
    if want == c0:
        print('CONFIG0 already set')
        return True
    if want & ~c0:                      # a bit goes 0 -> 1: erase the CONFIG page, write both words back
        steps = ['isp 0x22 0x300000 0', f'isp 0x21 0x300000 0x{want:08x}', f'isp 0x21 0x300004 0x{c1:08x}']
    else:                               # only clearing bits: program in place
        steps = [f'isp 0x21 0x300000 0x{want:08x}']
    result = run_openocd(APROM_ENTRY + ISP_SETUP + steps + ['show 0x300000', 'show 0x300004', 'reset run', 'shutdown'], log)
    got = isp_words(result.stdout)
    ok = result.returncode == 0 and got.get(0x300000) == want and got.get(0x300004) == c1
    print(f'CONFIG0 0x{c0:08x} -> 0x{got.get(0x300000, 0):08x}, CONFIG1 0x{got.get(0x300004, 0):08x}: '
          + ('verified' if ok else 'FAILED'))
    if not ok:
        print(result.stdout[-2000:], file=sys.stderr)
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', nargs='?', default=REPO / 'build/nnnix.bin', type=Path)
    parser.add_argument('--ldrom', type=Path, metavar='BOOT.BIN', help='program LDROM instead of APROM')
    parser.add_argument('--boot', choices=('ldrom', 'aprom'), help='set where the chip starts at power-on')
    parser.add_argument('--show', action='store_true', help='show CONFIG0/1 and LDROM')
    parser.add_argument('--log', type=Path, default=REPO / 'build/flash.log')
    args = parser.parse_args()
    if args.show:
        sys.exit(0 if show_config(args.log) else 1)
    if args.ldrom:
        sys.exit(0 if flash_ldrom(args.ldrom, args.log) else 1)
    if args.boot:
        sys.exit(0 if set_boot(args.boot, args.log) else 1)
    sys.exit(0 if flash(args.image, args.log) else 1)


if __name__ == '__main__':
    main()
