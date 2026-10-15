#!/usr/bin/env python3
"""Program an image into NUC140VE3CN APROM through Nu-Link (OpenOCD), verify, reset.

Only APROM is written; CONFIG and LDROM are never touched.

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


def flash(image, log=None):
    image = Path(image).resolve()
    check_image(image.read_bytes())
    try:
        socket.create_connection(('127.0.0.1', 6666), timeout=.3).close()
        raise SystemExit('another OpenOCD session holds the Nu-Link (tcl port 6666); stop it first')
    except OSError:
        pass
    command = openocd_command()
    if os.geteuid() != 0:
        command = ['sudo', '-n'] + command
    command += ['-f', 'interface/nulink.cfg', '-f', 'target/numicro.cfg']
    for step in ('gdb_port disabled; tcl_port disabled; telnet_port disabled',
                 'init', 'halt', 'wait_halt 1000',
                 f'set pdid [lindex [read_memory 0x50000000 32 1] 0]; '
                 f'if {{$pdid != 0x{PDID:08x}}} {{error {{unexpected MCU, not programming}}}}',
                 'flash probe 0',
                 f'flash write_image erase {{{image}}} 0x0 bin',
                 f'verify_image {{{image}}} 0x0 bin',
                 'reset run', 'shutdown'):
        command += ['-c', step]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    if log:
        write_user_file(log, result.stdout)
    ok = result.returncode == 0 and 'verified' in result.stdout
    for line in result.stdout.splitlines():
        if line.startswith(('wrote', 'verified', 'Error')) or 'Device Name' in line:
            print(line.strip())
    if not ok:
        print(result.stdout[-2000:], file=sys.stderr)
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', nargs='?', default=REPO / 'build/nnnix.bin', type=Path)
    parser.add_argument('--log', type=Path, default=REPO / 'build/flash.log')
    args = parser.parse_args()
    sys.exit(0 if flash(args.image, args.log) else 1)


if __name__ == '__main__':
    main()
