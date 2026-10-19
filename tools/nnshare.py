#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Share this computer's internet connection with nnnix through the USB Ethernet
adapter cabled to the board's W5500.

  sudo python3 tools/nnshare.py on        start, in the background
  sudo python3 tools/nnshare.py status    what is running, the latest log lines
  sudo python3 tools/nnshare.py off       stop, and undo every change
  sudo python3 tools/nnshare.py run       the same as on, in the foreground (Ctrl-C undoes)
  add --dry-run to on/run to see the changes without making them

What `on` sets up, all undone by `off`:
  - the adapter's address 198.18.0.1/30 (the test network's), unless it has it;
  - IPv4 forwarding, unless it is on already;
  - nftables tables `nnshare`: NAT (masquerade) for 198.18.0.0/30 towards every
    other interface, forwarding to and from the adapter allowed;
  - a DHCP server on the adapter that offers the board 198.18.0.2/30 with this
    computer as gateway and DNS server (the board keeps its usual address, so
    telnet sessions survive);
  - a DNS relay on 198.18.0.1:53 to this computer's own resolver (here often
    systemd-resolved on 127.0.0.53, which the board cannot reach itself).

The server holds /run/nnshare.lock from before its first change to after its
last undo (one nnshare at a time), and records what it changed, its pid, start
time and a token in /run/nnshare.json; off signals only the process all of these
match, and undoes the record itself when the server is gone. If a server
thread ends with an error, everything is undone. DNS queries in flight are
limited (DNS_IN_FLIGHT); malformed DHCP requests are ignored.

Then on the board:  dhcp      (or: ifconfig eth0 198.18.0.2/30 gw 198.18.0.1 dns 198.18.0.1)
and for example:    host example.org;  wget http://example.org/
`make test` refuses to run while sharing is on (they would both serve DHCP and DNS).
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import secrets
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

HOST, BOARD, PREFIX = '198.18.0.1', '198.18.0.2', 30
NET = '198.18.0.0/30'
STATE = Path('/run/nnshare.json')
LOCK = Path('/run/nnshare.lock')     # held by the running server, from its first change to its last undo
LOG = Path('/run/nnshare.log')
LEASE = 3600
DNS_IN_FLIGHT = 16                   # queries relayed at once; more are dropped


def log(msg):
    print(f'{time.strftime("%H:%M:%S")} {msg}', flush=True)


def sh(*cmd, input=None, check=True):
    r = subprocess.run(cmd, input=input, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f'{" ".join(cmd)}: {r.stderr.strip() or r.returncode}')
    return r


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return None


def save_state(state):
    """Whole or not at all: a reader never sees half a file."""
    tmp = STATE.with_name(STATE.name + '.tmp')
    with open(tmp, 'w') as f:
        json.dump(state, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, STATE)


def take_lock(blocking=False):
    """The server lock (LOCK), or None when another process holds it. The
    kernel releases it when its holder dies, however that happens."""
    fd = os.open(LOCK, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        return fd
    except BlockingIOError:
        os.close(fd)
        return None


def server_running():
    """Whether a server holds the lock (also without root: a shared lock is refused then)."""
    try:
        fd = os.open(LOCK, os.O_RDONLY)
    except FileNotFoundError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        return False
    except BlockingIOError:
        return True
    finally:
        os.close(fd)


def start_time(pid):
    """When the process started (clock ticks after boot, /proc/PID/stat field 22), or None."""
    try:
        stat = Path(f'/proc/{pid}/stat').read_text()
        return int(stat[stat.rindex(')') + 2:].split()[19])
    except (OSError, ValueError, IndexError):
        return None


def is_server(state):
    """Whether state's pid is still the server that wrote it: the same start
    time, and its command line carries the state's token. A pid alone is not
    enough: after a crash it can belong to anything."""
    pid = state.get('pid')
    if not isinstance(pid, int) or pid <= 0:
        return False
    try:
        args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    except OSError:
        return False
    ours = any(a.endswith(b'nnshare.py') for a in args) and b'run' in args
    if 'start' not in state:                            # written by a version that did not record it
        return ours
    return ours and start_time(pid) == state['start'] and (not state['token'] or state['token'].encode() in args)


# ---- the system changes ----

def nft_rules(iface):
    return f'''table ip nnshare {{
    chain postrouting {{
        type nat hook postrouting priority srcnat; policy accept;
        ip saddr {NET} oifname != "{iface}" masquerade
    }}
}}
table inet nnshare {{
    chain forward {{
        type filter hook forward priority filter; policy accept;
        iifname "{iface}" ip saddr {NET} accept
        oifname "{iface}" ct state established,related accept
    }}
    chain input {{
        type filter hook input priority filter; policy accept;
        iifname "{iface}" udp dport {{ 53, 67 }} accept
    }}
}}
'''


def plan(iface):
    """The changes `on` makes: (description, undo info) pairs, checked first."""
    info = json.loads(sh('ip', '-j', 'addr', 'show', 'dev', iface).stdout)[0]
    inet = [f"{a['local']}/{a['prefixlen']}" for a in info.get('addr_info', []) if a['family'] == 'inet']
    if inet and inet != [f'{HOST}/{PREFIX}']:
        raise RuntimeError(f'{iface} has another IPv4 address ({", ".join(inet)}): is it the right adapter?')
    routes = json.loads(sh('ip', '-j', 'route', 'show', 'default').stdout or '[]')
    if any(r.get('dev') == iface for r in routes):
        raise RuntimeError(f'{iface} carries the default route: it is this computer\'s uplink, not the board\'s cable')
    uplinks = sorted({r['dev'] for r in routes if 'dev' in r})
    if not uplinks:
        raise RuntimeError('this computer has no default route: nothing to share')
    if sh('nft', 'list', 'tables', check=False).stdout.count(' nnshare') and not load_state():
        raise RuntimeError('nftables tables nnshare exist without nnshare running: remove them with '
                           '"nft delete table ip nnshare; nft delete table inet nnshare"')
    return {
        'iface': iface,
        'uplinks': uplinks,
        'add_address': not inet,
        'was_up': 'UP' in info['flags'],
        'enable_forwarding': Path('/proc/sys/net/ipv4/ip_forward').read_text().strip() != '1',
    }


def apply(p, dry):
    def do(*cmd, input=None):
        if dry:
            print('  ' + ' '.join(cmd) + (' <<EOF\n' + input + 'EOF' if input else ''))
        else:
            sh(*cmd, input=input)
    if p['add_address']:
        do('ip', 'addr', 'add', f'{HOST}/{PREFIX}', 'dev', p['iface'])
    do('ip', 'link', 'set', p['iface'], 'up')
    if p['enable_forwarding']:
        do('sysctl', '-q', '-w', 'net.ipv4.ip_forward=1')
    do('nft', '-f', '-', input=nft_rules(p['iface']))


def undo(p):
    """Every step runs even if an earlier one fails."""
    steps = [('nft', 'delete', 'table', 'ip', 'nnshare'), ('nft', 'delete', 'table', 'inet', 'nnshare')]
    if p.get('enable_forwarding'):
        steps.append(('sysctl', '-q', '-w', 'net.ipv4.ip_forward=0'))
    if p.get('add_address'):
        steps.append(('ip', 'addr', 'del', f'{HOST}/{PREFIX}', 'dev', p['iface']))
    if p.get('add_address') and not p.get('was_up'):
        steps.append(('ip', 'link', 'set', p['iface'], 'down'))
    failed = []
    for cmd in steps:
        r = sh(*cmd, check=False)
        gone = 'No such file' in r.stderr or 'Cannot assign requested address' in r.stderr   # undone before
        if r.returncode and not gone:
            failed.append(f'{" ".join(cmd)}: {r.stderr.strip()}')
    return failed


# ---- the servers ----

def dhcp_options(d):
    """A BOOTREQUEST's options {code: value}, or None when it is not one or an
    option runs past the end."""
    if len(d) < 240 or d[0] != 1 or d[236:240] != b'\x63\x82\x53\x63':
        return None
    opts, i = {}, 240
    while i < len(d) and d[i] != 255:
        if d[i] == 0:
            i += 1
            continue
        if i + 2 > len(d) or i + 2 + d[i + 1] > len(d):
            return None
        opts[d[i]] = d[i + 2:i + 2 + d[i + 1]]
        i += 2 + d[i + 1]
    return opts


def dhcp_reply(d):
    """The answer to request d: (reply, message type), or None to stay silent."""
    opts = dhcp_options(d)
    if opts is None or len(opts.get(53, b'')) != 1:
        return None                                     # not a well-formed DHCP request
    host, board = socket.inet_aton(HOST), socket.inet_aton(BOARD)
    kind = opts[53][0]
    if kind == 3 and opts.get(50, board) != board:      # asks for an address we do not give
        reply = 6                                       # NAK
    else:
        reply = {1: 2, 3: 5}.get(kind)                  # DISCOVER -> OFFER, REQUEST -> ACK
    if not reply:
        return None
    r = bytearray(240)
    r[0:3] = b'\x02\x01\x06'
    r[4:8], r[10:12], r[28:44] = d[4:8], d[10:12], d[28:44]
    if reply != 6:
        r[16:20], r[20:24] = board, host
    r[236:240] = b'\x63\x82\x53\x63'
    r += bytes([53, 1, reply, 54, 4]) + host
    if reply != 6:
        mask = struct.pack('>I', (0xffffffff << (32 - PREFIX)) & 0xffffffff)
        r += bytes([51, 4]) + struct.pack('>I', LEASE) + bytes([1, 4]) + mask
        r += bytes([3, 4]) + host + bytes([6, 4]) + host
    r += b'\xff'
    return bytes(r), reply


def dhcp_server(iface, stop):
    """Offers BOARD/PREFIX with HOST as router and DNS to whoever asks on iface."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())
    u.bind(('0.0.0.0', 67))
    u.settimeout(0.5)
    with u:
        while not stop.is_set():
            try:
                d, _ = u.recvfrom(1500)
            except socket.timeout:
                continue
            try:
                answer = dhcp_reply(d)
            except Exception as exc:                     # noqa: BLE001 - one bad packet must not stop the server
                log(f'dhcp: ignored a request ({exc!r})')
                continue
            if answer:
                u.sendto(answer[0], ('255.255.255.255', 68))
                log(f'dhcp: {({2: "offered", 5: "leased", 6: "refused"})[answer[1]]} {BOARD}/{PREFIX} '
                    f'to {d[28:34].hex(":")}')


def resolver():
    """This computer's resolver, as /etc/resolv.conf names it."""
    try:
        for line in Path('/etc/resolv.conf').read_text().splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0] == 'nameserver' and ':' not in parts[1]:
                return parts[1]
    except OSError:
        pass
    return '127.0.0.53'


def dns_relay(stop, counts):
    """UDP DNS from the board, passed to this computer's resolver and back;
    at most DNS_IN_FLIGHT at once, the rest dropped (the board asks again)."""
    upstream = (resolver(), 53)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind((HOST, 53))
    u.settimeout(0.5)
    slots = threading.BoundedSemaphore(DNS_IN_FLIGHT)

    def relay(query, client):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as up:
                up.settimeout(4)
                up.sendto(query, upstream)
                answer, _ = up.recvfrom(4096)
            u.sendto(answer[:512] if len(answer) > 512 else answer, client)
            counts['answered'] += 1
        except OSError:
            counts['failed'] += 1
        finally:
            slots.release()

    log(f'dns: relaying {HOST}:53 to {upstream[0]}')
    with u:
        while not stop.is_set():
            try:
                query, client = u.recvfrom(512)
            except socket.timeout:
                continue
            if not slots.acquire(blocking=False):
                counts['dropped'] += 1
                continue
            threading.Thread(target=relay, args=(query, client), daemon=True).start()


def guarded(name, server, stop, failure, *args):
    """Runs a server; if it ends with an error, the whole sharing stops."""
    try:
        server(*args, stop)
    except Exception as exc:                             # noqa: BLE001 - reported, then everything is undone
        failure.append(f'{name}: {exc!r}')
        stop.set()


def serve(p, ready=None):
    stop, counts, failure = threading.Event(), {'answered': 0, 'failed': 0, 'dropped': 0}, []
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    servers = [threading.Thread(target=guarded, args=('dhcp', dhcp_server, stop, failure, p['iface']), daemon=True),
               threading.Thread(target=guarded, args=('dns', lambda s: dns_relay(s, counts), stop, failure),
                                daemon=True)]
    for t in servers:
        t.start()
    time.sleep(0.3)
    if not all(t.is_alive() for t in servers):
        raise RuntimeError('a server did not start (port 53 or 67 in use on the adapter?) ' + '; '.join(failure))
    log(f'sharing {", ".join(p["uplinks"])} with the board on {p["iface"]}: dhcp gives {BOARD}/{PREFIX}, '
        f'gateway and dns {HOST}')
    if ready:
        ready()
    try:
        last, logged = dict(counts), time.monotonic()
        while not stop.is_set():
            stop.wait(1)
            if not stop.is_set() and not all(t.is_alive() for t in servers):
                failure.append('a server stopped')
                stop.set()
            if counts != last and time.monotonic() - logged >= 60:
                log(f'dns: {counts["answered"]} answered, {counts["failed"]} failed, {counts["dropped"]} dropped')
                last, logged = dict(counts), time.monotonic()
    except KeyboardInterrupt:
        stop.set()
    for t in servers:
        t.join(2)
    if failure:
        raise RuntimeError('; '.join(failure))


# ---- the commands ----

def tidy(state):
    """Undo what a server left (under the lock). The state file goes only once
    everything is undone: otherwise it stays, with what failed, so `off` can
    try again and `on` does not start over it (review R55)."""
    failed = undo(state)
    if failed:
        state = dict(state, serving=False, undo_failed=failed)
        save_state(state)
    else:
        STATE.unlink(missing_ok=True)
    return failed


def preview(args):
    """--dry-run, for `on` and `run` alike: what would be done, nothing done
    (review R49: `run --dry-run` used to apply)."""
    p = plan(args.iface)
    state = load_state()
    if state:
        print(f'first, undo what the state file {STATE} records (pid {state.get("pid")})')
    print(f'uplink: {", ".join(p["uplinks"])}; board cable: {p["iface"]}')
    print('would run:')
    apply(p, True)
    print(f'and serve DHCP on {p["iface"]}, DNS on {HOST}:53 (to {resolver()})')
    return 0


def start(args, foreground):
    if args.dry_run:
        return preview(args)
    if foreground:
        return run_server(args)
    state = load_state()
    if server_running() or (state and is_server(state)):
        print(f'nnshare: already on (pid {state["pid"] if state else "?"}); "off" first')
        return 1
    lock = take_lock()
    if lock is None:
        print('nnshare: another nnshare is starting or stopping; try again')
        return 1
    try:
        if state:                                       # a server that died: tidy up after it
            failed = tidy(state)
            if failed:
                print('nnshare: could not undo what an earlier server left: ' + '; '.join(failed))
                return 1
        p = plan(args.iface)
    finally:
        os.close(lock)                                  # the server takes it for itself
    print(f'uplink: {", ".join(p["uplinks"])}; board cable: {p["iface"]}')
    with open(LOG, 'w') as out:
        child = subprocess.Popen([sys.executable, os.path.abspath(__file__), 'run', '--iface', args.iface,
                                  '--token', secrets.token_hex(8)],
                                 stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                 start_new_session=True)
    for _ in range(100):                                # until it is serving, or has failed
        state = load_state()
        if state and state.get('pid') == child.pid and state.get('serving'):
            print(LOG.read_text().strip().splitlines()[-1])
            print('on the board: dhcp   (or ifconfig eth0 198.18.0.2/30 gw 198.18.0.1 dns 198.18.0.1)')
            return 0
        if child.poll() is not None:
            break
        time.sleep(0.1)
    print('nnshare: did not start:\n' + LOG.read_text().strip())
    return 1


def run_server(args):
    """The server: holds the lock from before its first change to after its
    last undo, so no other nnshare changes anything meanwhile."""
    if args.dry_run:                                    # start() previews instead; never apply for it
        raise RuntimeError('run_server with --dry-run')
    lock = take_lock()
    if lock is None:
        log('nnshare: another nnshare is running, starting or stopping')
        return 1
    try:
        state = load_state()
        if state:                                       # a server that died left this
            if is_server(state):                        # one of the old kind, without the lock
                log(f'nnshare: already on (pid {state["pid"]})')
                return 1
            failed = tidy(state)
            if failed:
                log('nnshare: could not undo what an earlier server left: ' + '; '.join(failed))
                return 1
        p = plan(args.iface)
        # How off and status know this process (is_server): its start time,
        # and the token `on` put on its command line ('' when run by hand).
        p.update(pid=os.getpid(), start=start_time(os.getpid()), token=args.token)
        save_state(p)                                   # before the first change: off can undo a partial one
        try:
            apply(p, False)

            def ready():
                p['serving'] = True
                save_state(p)
            serve(p, ready)
        except Exception as exc:                         # noqa: BLE001 - logged, then everything is undone
            log(f'nnshare: {exc}')
            return_code = 1
        else:
            return_code = 0
        finally:
            failed = tidy(p)
            log('stopped; changes undone' if not failed else
                'stopped; could not undo: ' + '; '.join(failed) + ' ("off" tries again)')
            if failed:
                return_code = 1
        return return_code
    finally:
        os.close(lock)


def stop_sharing():
    state = load_state()
    if not state:
        print('nnshare: not on')
        return 0
    if is_server(state):
        try:
            pidfd = os.pidfd_open(state['pid'])         # this process, whatever its pid is used for later
        except ProcessLookupError:
            pidfd = None
        if pidfd is not None:
            try:
                if is_server(state):                    # checked again: the pidfd now pins the process
                    signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                    select.select([pidfd], [], [], 10)  # readable once it has ended
            finally:
                os.close(pidfd)
    elif server_running():
        print(f'nnshare: pid {state.get("pid")} holds the lock but is not the server the state names; '
              f'not signalling it (see {LOCK}, {STATE})')
        return 1
    lock = take_lock(blocking=True)                     # once the server has undone everything
    try:
        if load_state():                                # it could not tidy up itself
            failed = tidy(state)
            if failed:
                print('nnshare: could not undo: ' + '; '.join(failed))
                return 1
            print('nnshare: off (the server had died; its changes are undone)')
            return 0
    finally:
        os.close(lock)
    print(LOG.read_text().strip().splitlines()[-1] if LOG.exists() else 'nnshare: off')
    return 0


def status():
    state = load_state()
    if not state or not (server_running() or is_server(state)):
        print('nnshare: off' + (' (a stale state file: "off" tidies up)' if state else ''))
        if state and state.get('undo_failed'):
            print('nnshare: could not undo: ' + '; '.join(state['undo_failed']))
    else:
        print(f'nnshare: on (pid {state["pid"]}), {state["iface"]} shares {", ".join(state["uplinks"])}')
    if LOG.exists():
        print('\n'.join(LOG.read_text().strip().splitlines()[-8:]))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('on', 'off', 'status', 'run'))
    parser.add_argument('--iface', default='eth1', help='the USB Ethernet adapter cabled to the board (default eth1)')
    parser.add_argument('--dry-run', action='store_true', help='show the changes, make none')
    parser.add_argument('--token', default='', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.geteuid() != 0 and not (args.command == 'status' or args.dry_run):
        sys.exit('nnshare: run as root (sudo): it changes network settings')
    try:
        if args.command == 'off':
            return stop_sharing()
        if args.command == 'status':
            return status()
        return start(args, args.command == 'run')
    except RuntimeError as exc:
        print(f'nnshare: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
