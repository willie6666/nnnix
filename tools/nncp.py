#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Transfer files to/from nnnix's /home over HTTP (TCP port 8080).

An existing route can be used directly. --iface temporarily isolates an unused
Ethernet adapter, as nntest does, and requires root.

  sudo python3 tools/nncp.py --iface eth1 put local.bin /home/local.bin
  sudo python3 tools/nncp.py --iface eth1 get /home/local.bin copy.bin
"""
import argparse
import http.client
import os
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True


def transfer(args):
    c = http.client.HTTPConnection(args.host, args.port, timeout=10)
    temporary = None
    try:
        if args.action == 'put':
            with open(args.source, 'rb') as source:
                size = os.fstat(source.fileno()).st_size
                c.request('PUT', args.dest, body=source, headers={'Content-Length': str(size)})
                r = c.getresponse()
                r.read()
                if r.status not in (201, 204):
                    raise RuntimeError(f'upload: HTTP {r.status} {r.reason}')
                print(f'uploaded {size} bytes to {args.dest}')
        else:
            c.request('GET', args.source)
            r = c.getresponse()
            if r.status != 200:
                raise RuntimeError(f'download: HTTP {r.status} {r.reason}')
            declared = r.getheader('Content-Length')
            if declared is None or not declared.isdecimal():
                raise RuntimeError('download: missing or invalid Content-Length')
            size, got = int(declared), 0
            dest = Path(args.dest).resolve()
            with tempfile.NamedTemporaryFile(dir=dest.parent, prefix=f'.{dest.name}.', delete=False) as out:
                temporary = out.name
                while block := r.read(8192):
                    out.write(block)
                    got += len(block)
                if got != size:
                    raise RuntimeError(f'download incomplete: received {got} of {size} bytes')
                if os.geteuid() == 0 and 'SUDO_UID' in os.environ:
                    os.fchown(out.fileno(), int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID']))
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, dest)
            temporary = None
            print(f'downloaded {got} bytes to {args.dest}')
    finally:
        c.close()
        if temporary:
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='198.18.0.2')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--iface', help='temporarily use this unused adapter (requires root)')
    parser.add_argument('action', choices=('get', 'put'))
    parser.add_argument('source')
    parser.add_argument('dest')
    args = parser.parse_args()
    remote = args.source if args.action == 'get' else args.dest
    if not remote.startswith('/home/'):
        parser.error('remote path must start with /home/')
    if args.iface and args.host != '198.18.0.2':
        parser.error('--iface uses the isolated 198.18.0.0/30 test network')
    if args.iface and os.geteuid() != 0:
        parser.error('--iface requires root for the temporary network namespace')
    try:
        if args.iface:
            from nntest import Namespace
            with Namespace(args.iface) as ns:
                ns.call(transfer, args)
        else:
            transfer(args)
    except (OSError, RuntimeError, http.client.HTTPException) as exc:
        print(f'nncp: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
