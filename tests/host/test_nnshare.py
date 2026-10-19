#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/nnshare.py's start and undo paths with every system command replaced
(run by `make host-test`; nothing on this computer is changed): --dry-run
changes nothing for `on` or `run`, and an undo that fails keeps the state file
so that `off` can try again and `on` does not start over it."""
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import nnshare                                                # noqa: E402

checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f'test_nnshare: FAILED: {what}', file=sys.stderr)


tmp = Path(tempfile.mkdtemp())
nnshare.STATE, nnshare.LOCK, nnshare.LOG = tmp / 'state.json', tmp / 'lock', tmp / 'log'
calls = []
fail_on = set()


def sh(*cmd, input=None, check=True):
    calls.append(cmd)
    bad = any(' '.join(cmd).startswith(f) for f in fail_on)
    return SimpleNamespace(returncode=1 if bad else 0, stdout='', stderr='injected failure' if bad else '')


PLAN = {'iface': 'eth9', 'uplinks': ['eth0'], 'add_address': True, 'was_up': True, 'enable_forwarding': False}
nnshare.sh = sh
nnshare.plan = lambda iface: dict(PLAN)
nnshare.resolver = lambda: '127.0.0.53'
nnshare.serve = lambda p, ready=None: calls.append(('serve',))
nnshare.print = lambda *a, **k: None                          # quiet
nnshare.log = lambda msg: None


def args(dry_run):
    return SimpleNamespace(iface='eth9', dry_run=dry_run, token='')


for foreground in (False, True):
    calls.clear()
    rc = nnshare.start(args(True), foreground)
    check(rc == 0 and not calls and not nnshare.STATE.exists(),
          f'{"run" if foreground else "on"} --dry-run changes nothing: {calls}')
try:
    nnshare.run_server(args(True))
    check(False, 'run_server refuses --dry-run')
except RuntimeError:
    check(True, 'run_server refuses --dry-run')

# An undo that fails keeps the record, with what failed.
state = dict(PLAN, pid=1, start=1, token='x', serving=True)
nnshare.save_state(state)
fail_on = {'ip addr del'}
failed = nnshare.tidy(state)
kept = nnshare.load_state()
check(failed and kept and kept.get('undo_failed') == failed and not kept['serving'],
      f'a failed undo keeps the state file: {failed} {kept}')
calls.clear()
rc = nnshare.start(args(False), False)
check(rc == 1 and not any(c[0] == sys.executable for c in calls) and nnshare.STATE.exists(),
      'on does not start over a record it could not undo')
fail_on = set()
failed = nnshare.tidy(nnshare.load_state())
check(not failed and not nnshare.STATE.exists(), 'once the undo works, the record goes')

# Undone before (an address already gone, a table already deleted): not a failure.
real_sh = sh


def gone(*cmd, input=None, check=True):
    calls.append(cmd)
    msg = {'ip': 'RTNETLINK answers: Cannot assign requested address', 'nft': 'Error: No such file or directory'}
    return SimpleNamespace(returncode=2, stdout='', stderr=msg.get(cmd[0], 'other'))


nnshare.sh = gone
check(nnshare.undo(PLAN) == [], 'an undo of what is already undone succeeds')
nnshare.sh = real_sh

print(f'test_nnshare: {checks} checks, {failures} failed')
sys.exit(1 if failures else 0)
