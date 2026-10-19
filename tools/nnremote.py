#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Serve this GNOME computer's screen and input to the board.

  computer: python3 tools/nnremote.py 1234
  board:    remote 198.18.0.1 1234

Run in the desktop session, without sudo. One board at a time; after it
disconnects the server waits for another connection (Ctrl-C stops it).
The board's keypad 1 toggles absolute/relative mouse motion (R on the LCD).
Use --relative to start with relative motion for games that lock the pointer.
Use --bind IP to listen on a particular local IPv4 address, or --once to
exit after one session. Screen options follow --, for example:

  python3 tools/nnremote.py 1234 -- --fps 15 --invert

Only allow trusted peers to reach this port: a connected peer can see the
screen and control the keyboard and mouse. The protocol has no authentication
or encryption.
"""
import argparse
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def serve_connection(conn, screen_cmd, input_cmd):
    """Run the two stream tools on opposite directions of one TCP socket."""
    children = []
    with tempfile.TemporaryDirectory(prefix='nnremote-') as state:
        env = dict(os.environ, NNINPUT_STATE_FILE=str(Path(state) / 'input'))
        try:
            children.append(subprocess.Popen(input_cmd, stdin=conn, env=env, start_new_session=True))
            children.append(subprocess.Popen(screen_cmd, stdout=conn, env=env, start_new_session=True))
            while all(p.poll() is None for p in children):
                time.sleep(0.05)
        finally:
            # Wake even an idle receiver, and make the board restore its console.
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            # EOF lets nninput release keys normally. Sending SIGTERM at the
            # same moment could interrupt that release while it is in finally.
            if children:
                try:
                    children[0].wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            for p in children:
                if p.poll() is None:
                    p.terminate()                # nninput releases held keys in its finally
            for p in children:
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
    return next((p.returncode for p in children if p.returncode not in (0, -signal.SIGTERM)), 0)


def port_number(text):
    try:
        n = int(text)
        if 1 <= n <= 65535:
            return n
    except ValueError:
        pass
    raise argparse.ArgumentTypeError('port must be 1 to 65535')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port', type=port_number)
    ap.add_argument('--bind', default='0.0.0.0', metavar='IP')
    ap.add_argument('--once', action='store_true')
    ap.add_argument('--monitor', default='')
    ap.add_argument('--speed', type=float, default=2.0)
    ap.add_argument('--relative', action='store_true', help='start in relative mouse mode; board keypad 1 toggles')
    ap.add_argument('-v', '--verbose', action='store_true')
    raw = sys.argv[1:]
    split = raw.index('--') if '--' in raw else len(raw)
    args = ap.parse_args(raw[:split])
    screen_args = raw[split + 1:]
    # The socket must carry a live stream, never a file or help text.
    if any(a in ('-o', '--output', '-h', '--help') or a.startswith(('-o', '--output=')) for a in screen_args):
        ap.error('screen output must go to the board; -o/--output and --help are not allowed after --')
    folder = Path(__file__).resolve().parent
    screen_cmd = [sys.executable, str(folder / 'nnscreen.py'), '--monitor', args.monitor, *screen_args]
    input_cmd = [sys.executable, str(folder / 'nninput.py'), '--monitor', args.monitor, '--speed', str(args.speed)]
    if args.relative:
        input_cmd.append('--relative')
    if args.verbose:
        input_cmd.append('-v')
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((args.bind, args.port))
            listener.listen(1)
            while True:
                print(f'nnremote: waiting on {args.bind}:{args.port}', file=sys.stderr, flush=True)
                conn, peer = listener.accept()
                with conn:
                    print(f'nnremote: {peer[0]}:{peer[1]} connected', file=sys.stderr, flush=True)
                    rc = serve_connection(conn, screen_cmd, input_cmd)
                print(f'nnremote: disconnected (status {rc})', file=sys.stderr, flush=True)
                if args.once:
                    return 1 if rc else 0
    except KeyboardInterrupt:
        return 0
    except OSError as e:
        print(f'nnremote: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
