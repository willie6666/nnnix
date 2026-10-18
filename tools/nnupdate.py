#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Update nnnix over the network: upload an APROM image, have the board stage it
and restart through its LDROM bootloader, wait until it is back, show the result.
With --bin, also (or only) install the programs for /bin (build/bin/*.nxe:
`play`, `ed`, `grep`... are programs, not part of the firmware).

  sudo python3 tools/nnupdate.py --iface eth1 build/nnnix.bin --bin build/bin

Needs the bootloader in LDROM (tools/flash.py --ldrom build/boot.bin). The
board keeps running the old firmware if anything before the restart fails.
"""
import argparse
import http.client
import os
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from nntest import Namespace, Telnet  # noqa: E402

REMOTE = '/home/nnnix.bin'


class Direct:
    """The board is reachable from here: no namespace."""
    def call(self, fn, *args):
        return fn(*args)


def upload(host, data):
    c = http.client.HTTPConnection(host, 8080, timeout=30)
    try:
        c.request('PUT', REMOTE, body=data, headers={'Content-Length': str(len(data))})
        r = c.getresponse()
        r.read()
        if r.status not in (201, 204):
            raise RuntimeError(f'upload: HTTP {r.status} {r.reason}')
    finally:
        c.close()


def put(host, path, data):
    c = http.client.HTTPConnection(host, 8080, timeout=30)
    try:
        c.request('PUT', path, body=data, headers={'Content-Length': str(len(data))})
        r = c.getresponse()
        r.read()
        return r.status
    finally:
        c.close()


def install_programs(ns, host, folder):
    files = sorted(folder.glob('*.nxe'))
    if not files:
        raise RuntimeError(f'no programs (*.nxe) in {folder}')
    t = Telnet(ns, host=host)
    t.read_until(Telnet.PROMPT)
    t.cmd('mkdir /bin')                              # an error if it is there already
    t.send('exit\r\n'); t.wait_closed(); t.close()
    for f in files:
        status = ns.call(put, host, f'/bin/{f.stem}', f.read_bytes())
        if status not in (201, 204):
            raise RuntimeError(f'/bin/{f.stem}: HTTP {status}')
    print(f'installed in /bin: {" ".join(f.stem for f in files)}')


def update(ns, host, data):
    print(f'uploading {len(data)} bytes')
    deadline = time.monotonic() + 20                 # the board may still be starting
    while True:
        try:
            ns.call(upload, host, data)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(1)
    t = Telnet(ns, host=host)
    t.read_until(Telnet.PROMPT)
    before = t.cmd('uname').strip()
    print(f'running: {before}')
    t.send(f'fwupdate {REMOTE}\r\n')
    try:
        t.read_until('restarting through the bootloader', 60)
    except (TimeoutError, ConnectionError):
        raise RuntimeError('not staged: ' + t.text.strip().replace('\r\n', ' | '))
    t.close()                         # the board resets; no FIN arrives
    print('staged; the bootloader is installing it')
    started = time.monotonic()
    time.sleep(3)                     # the reset follows the message after about 0.3 s
    # Until nnnix resets the W5500 at boot, the chip still accepts connections on
    # the old firmware's sockets without anyone answering: wait for a prompt.
    while True:
        try:
            t = Telnet(ns, timeout=5, connect_within=5, host=host)
            t.read_until(Telnet.PROMPT, 5)
            break
        except (OSError, TimeoutError, ConnectionError):
            if time.monotonic() - started > 120:
                raise RuntimeError('the board did not come back within 2 minutes')
            time.sleep(1)
    after = t.cmd('uname').strip()
    info = t.cmd('fwupdate').strip().replace('\r\n', ' | ')
    t.cmd(f'rm {REMOTE}')
    t.send('exit\r\n'); t.wait_closed(); t.close()
    print(f'back after {time.monotonic() - started:.0f} s: {after}\n{info}')
    if 'installed' not in info:
        raise RuntimeError('the update is not installed')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path, nargs='?')
    parser.add_argument('--bin', type=Path, metavar='DIR', help='install DIR/*.nxe in /bin (after the update)')
    parser.add_argument('--host', default='198.18.0.2')
    parser.add_argument('--iface', help='temporarily use this unused adapter (requires root)')
    args = parser.parse_args()
    if not args.image and not args.bin:
        parser.error('nothing to do: give an image, --bin DIR, or both')
    data = args.image.read_bytes() if args.image else None
    if data is not None and not 8 <= len(data) <= 128 * 1024:
        parser.error('not an APROM image (8 bytes to 128 KiB)')
    if args.iface and args.host != '198.18.0.2':
        parser.error('--iface uses the isolated 198.18.0.0/30 test network')
    if args.iface and os.geteuid() != 0:
        parser.error('--iface requires root for the temporary network namespace')
    try:
        def run(ns):
            if data is not None:
                update(ns, args.host, data)
            if args.bin:
                install_programs(ns, args.host, args.bin)
        if args.iface:
            with Namespace(args.iface) as ns:
                run(ns)
        else:
            run(Direct())
    except (OSError, RuntimeError, TimeoutError, ConnectionError, http.client.HTTPException) as exc:
        print(f'nnupdate: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
