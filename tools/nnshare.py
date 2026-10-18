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

Then on the board:  dhcp      (or: ifconfig eth0 198.18.0.2/30 gw 198.18.0.1 dns 198.18.0.1)
and for example:    host example.org;  wget http://example.org/
`make test` refuses to run while sharing is on (they would both serve DHCP and DNS).
"""
import argparse
import json
import os
from pathlib import Path
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
LOG = Path('/run/nnshare.log')
LEASE = 3600


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


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


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
        if r.returncode and 'No such file' not in r.stderr:
            failed.append(f'{" ".join(cmd)}: {r.stderr.strip()}')
    return failed


# ---- the servers ----

def dhcp_server(iface, stop):
    """Offers BOARD/PREFIX with HOST as router and DNS to whoever asks on iface."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())
    u.bind(('0.0.0.0', 67))
    u.settimeout(0.5)
    host, board = socket.inet_aton(HOST), socket.inet_aton(BOARD)
    mask = struct.pack('>I', (0xffffffff << (32 - PREFIX)) & 0xffffffff)
    with u:
        while not stop.is_set():
            try:
                d, _ = u.recvfrom(1500)
            except socket.timeout:
                continue
            if len(d) < 240 or d[0] != 1 or d[236:240] != b'\x63\x82\x53\x63':
                continue
            opts, i = {}, 240
            while i + 1 < len(d) and d[i] != 255:
                if d[i] == 0:
                    i += 1
                    continue
                opts[d[i]] = d[i + 2:i + 2 + d[i + 1]]
                i += 2 + d[i + 1]
            kind = opts.get(53, b'\0')[0]
            mac = d[28:34].hex(':')
            if kind == 3 and opts.get(50, board) != board:     # asks for an address we do not give
                reply = 6                                       # NAK
            else:
                reply = {1: 2, 3: 5}.get(kind)                  # DISCOVER -> OFFER, REQUEST -> ACK
            if not reply:
                continue
            r = bytearray(240)
            r[0:3] = b'\x02\x01\x06'
            r[4:8], r[10:12], r[28:44] = d[4:8], d[10:12], d[28:44]
            if reply != 6:
                r[16:20], r[20:24] = board, host
            r[236:240] = b'\x63\x82\x53\x63'
            r += bytes([53, 1, reply, 54, 4]) + host
            if reply != 6:
                r += bytes([51, 4]) + struct.pack('>I', LEASE) + bytes([1, 4]) + mask
                r += bytes([3, 4]) + host + bytes([6, 4]) + host
            r += b'\xff'
            u.sendto(bytes(r), ('255.255.255.255', 68))
            log(f'dhcp: {({2: "offered", 5: "leased", 6: "refused"})[reply]} {BOARD}/{PREFIX} to {mac}')


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
    """UDP DNS from the board, passed to this computer's resolver and back."""
    upstream = (resolver(), 53)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind((HOST, 53))
    u.settimeout(0.5)

    def relay(query, client):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as up:
            up.settimeout(4)
            try:
                up.sendto(query, upstream)
                answer, _ = up.recvfrom(4096)
            except OSError:
                counts['failed'] += 1
                return
        u.sendto(answer[:512] if len(answer) > 512 else answer, client)
        counts['answered'] += 1

    log(f'dns: relaying {HOST}:53 to {upstream[0]}')
    with u:
        while not stop.is_set():
            try:
                query, client = u.recvfrom(512)
            except socket.timeout:
                continue
            threading.Thread(target=relay, args=(query, client), daemon=True).start()


def serve(p, ready=None):
    stop, counts = threading.Event(), {'answered': 0, 'failed': 0}
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    servers = [threading.Thread(target=dhcp_server, args=(p['iface'], stop), daemon=True),
               threading.Thread(target=dns_relay, args=(stop, counts), daemon=True)]
    for t in servers:
        t.start()
    time.sleep(0.3)
    if not all(t.is_alive() for t in servers):
        raise RuntimeError('a server did not start (port 53 or 67 in use on the adapter?)')
    log(f'sharing {", ".join(p["uplinks"])} with the board on {p["iface"]}: dhcp gives {BOARD}/{PREFIX}, '
        f'gateway and dns {HOST}')
    if ready:
        ready()
    try:
        last = dict(counts)
        while not stop.is_set():
            stop.wait(60)
            if counts != last:
                log(f'dns: {counts["answered"]} answered, {counts["failed"]} failed')
                last = dict(counts)
    except KeyboardInterrupt:
        stop.set()
    for t in servers:
        t.join(2)


# ---- the commands ----

def start(args, foreground):
    state = load_state()
    if state and alive(state['pid']):
        print(f'nnshare: already on (pid {state["pid"]}); "off" first')
        return 1
    if state:                                           # a run that died: tidy up after it
        undo(state)
        STATE.unlink(missing_ok=True)
    p = plan(args.iface)
    print(f'uplink: {", ".join(p["uplinks"])}; board cable: {p["iface"]}')
    if args.dry_run:
        print('would run:')
        apply(p, True)
        print(f'and serve DHCP on {p["iface"]}, DNS on {HOST}:53 (to {resolver()})')
        return 0
    if not foreground:
        with open(LOG, 'w') as out:
            child = subprocess.Popen([sys.executable, os.path.abspath(__file__), 'run', '--iface', args.iface],
                                     stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     start_new_session=True)
        for _ in range(100):                            # until it is serving, or has failed
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
    p['pid'] = os.getpid()
    STATE.write_text(json.dumps(p))                     # before the first change: off can undo a partial one
    try:
        apply(p, False)

        def ready():
            p['serving'] = True
            STATE.write_text(json.dumps(p))
        serve(p, ready)
    except Exception as exc:
        log(f'nnshare: {exc}')
        return_code = 1
    else:
        return_code = 0
    finally:
        failed = undo(p)
        STATE.unlink(missing_ok=True)
        log('stopped; changes undone' + (': but ' + '; '.join(failed) if failed else ''))
    return return_code


def stop_sharing():
    state = load_state()
    if not state:
        print('nnshare: not on')
        return 0
    if alive(state['pid']):
        os.kill(state['pid'], signal.SIGTERM)
        for _ in range(50):
            if not alive(state['pid']):
                break
            time.sleep(0.1)
    if load_state():                                    # it could not tidy up itself
        failed = undo(state)
        STATE.unlink(missing_ok=True)
        if failed:
            print('nnshare: could not undo: ' + '; '.join(failed))
            return 1
    print(LOG.read_text().strip().splitlines()[-1] if LOG.exists() else 'nnshare: off')
    return 0


def status():
    state = load_state()
    if not state or not alive(state['pid']):
        print('nnshare: off' + (' (a stale state file: "off" tidies up)' if state else ''))
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
