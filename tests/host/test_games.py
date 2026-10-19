#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The games of bin/ on the host (tests/host/game_sim.c, built as
build/host/sim_NAME): each starts, plays from a script, ends with quit, and
draws what it should (run by `make host-test`)."""
import subprocess
import sys
import tempfile
from pathlib import Path

BUILD = Path(__file__).resolve().parents[2] / 'build/host'
checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f'test_games: FAILED: {what}', file=sys.stderr)


def run(game, *steps):
    """The game's output and its screenshots ({ms: rows of '0'/'1'})."""
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([BUILD / f'sim_{game}', '-o', d, *steps], capture_output=True, text=True, timeout=60)
        shots = {int(p.stem): [l for l in p.read_text().split('\n')[2:] if l] for p in Path(d).glob('*.pbm')}
    return r.returncode, r.stdout, shots


def dark(rows, x0, y0, x1, y1):
    return sum(rows[y][x] == '1' for y in range(y0, y1) for x in range(x0, x1))


# Tetris: held down, the pieces stack in the middle until there is no room.
rc, out, shots = run('tetris', '1000:shot', '1500:+down', '40000:shot', '41000:q')
check(rc == 0 and 'sim: ended' in out, f'tetris ends: {out!r}')
best = int(out.split('tetris: best ')[1].split()[0]) if 'tetris: best ' in out else 0
check(best > 50, f'tetris scores the soft drops: {best}')
check(dark(shots[1000], 48, 1, 80, 63) and dark(shots[1000], 90, 10, 106, 26), 'tetris draws a piece and the next one')
check(dark(shots[40000], 24, 14, 104, 50) > 100, 'tetris ends with GAME OVER')

# Snake: straight on into the right wall (16 steps), then quit at the game over.
rc, out, shots = run('snake', '500:shot', '6000:shot', '6500:esc')
check(rc == 0 and 'snake: best ' in out, f'snake ends: {out!r}')
check(dark(shots[500], 0, 8, 128, 64) >= 3 * 16, 'snake draws the snake and the food')
check(dark(shots[6000], 24, 14, 104, 50) > 100, 'snake: the wall ends it')

# After a game over any key plays again, the letter a too (it is left in play).
rc, out, shots = run('snake', '3500:shot', '4000:a', '4500:shot', '5000:q')
check(rc == 0 and 'snake: best 0' in out, f'snake again: {out!r}')
check(dark(shots[3500], 22, 10, 106, 11) == 84 and dark(shots[4500], 22, 10, 106, 11) < 40,
      'snake: a key after the game over plays again')               # the box's top edge, then not

# Flappy: the first flap starts it; without more it falls to the ground.
rc, out, shots = run('flappy', '300:shot', '1000:space', '1100:shot', '4000:shot', '4500:pad1')
check(rc == 0 and 'flappy: best 0' in out, f'flappy ends: {out!r}')
check(dark(shots[300], 0, 0, 128, 30) > 40, 'flappy shows its title')
check(dark(shots[1100], 24, 0, 29, 61) >= 10, 'flappy draws the bird')
check(dark(shots[4000], 24, 14, 104, 50) > 100, 'flappy: the ground ends it')

# R58: down held for 400 ms drops a step at once and then one every 40 ms
# (a point each), not one per 20 ms pass of the main loop.
rc, out, _ = run('tetris', '1000:+down', '1400:-down', '1500:q')
best = int(out.split('tetris: best ')[1].split()[0]) if 'tetris: best ' in out else -1
check(10 <= best <= 11, f'tetris soft drop every 40 ms: {best} points in 400 ms')

# R56: Left held while a (also left) goes down and up: still held, the piece
# goes on to the wall exactly as with Left alone.
_, _, alone = run('tetris', '1000:+left', '1700:shot', '1800:q')
_, _, both = run('tetris', '1000:+left', '1050:+a', '1100:-a', '1700:shot', '1800:q')
_, _, let_go = run('tetris', '1000:+left', '1100:-left', '1700:shot', '1800:q')
check(alone[1700] == both[1700] and alone[1700] != let_go[1700], 'tetris: letting a go leaves Left held')

# Quit at once, from the keypad too.
for game, key in (('tetris', 'pad1'), ('snake', 'q'), ('flappy', 'esc')):
    rc, out, _ = run(game, f'200:{key}')
    ended = int(out.split('sim: ended at ')[1].split()[0]) if 'sim: ended at ' in out else 99999
    check(rc == 0 and f'{game}: best 0' in out and ended < 500, f'{game} quits with {key}: {out!r}')

print(f'test_games: {checks} checks, {failures} failed')
sys.exit(1 if failures else 0)
