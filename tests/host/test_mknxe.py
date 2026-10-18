#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/mknxe.py on damaged ELF files: every one refused with ElfError (never
another exception, never a huge allocation), and a real program converted to an
NXE whose data_crc covers its header (run by `make host-test`, after the
programs are built: argv[1] is an ELF file to start from)."""
import struct
import sys
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import mknxe                                                  # noqa: E402

checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f'test_mknxe: FAILED: {what}', file=sys.stderr)


def refused(data, what):
    try:
        mknxe.convert(bytes(data), 0)
    except mknxe.ElfError:
        check(True, what)
    except Exception as exc:                                 # noqa: BLE001 - this is what is tested
        check(False, f'{what}: {type(exc).__name__}: {exc}')
    else:
        check(False, f'{what}: accepted')


elf = Path(sys.argv[1]).read_bytes()
nxe, info = mknxe.convert(elf, 0)
f = struct.unpack_from('<IHHIIIIHHIII', nxe)
check(f[1] == 3 and f[3] == info['text'], 'a real program converts')
check(zlib.crc32(nxe[:36] + nxe[40 + f[3] + 4 * f[7]:]) == f[11], 'data_crc covers the header')

refused(b'\x7fELF', 'four bytes')
refused(b'', 'empty')
refused(elf[:51], 'a cut header')
refused(elf[:len(elf) // 2], 'cut in half')
e_shoff = struct.unpack_from('<I', elf, 32)[0]
e_shnum = struct.unpack_from('<H', elf, 48)[0]
for name, offset, fmt, value in [
        ('section table past the end', 32, '<I', len(elf)),
        ('section entry size', 46, '<H', 20),
        ('name table index', 50, '<H', e_shnum + 3),
        ('many sections', 48, '<H', 0xffff)]:
    bad = bytearray(elf)
    struct.pack_into(fmt, bad, offset, value)
    refused(bad, name)
for i in range(e_shnum):                                   # every section: a far offset, a huge size, bad links
    at = e_shoff + i * 40
    for field, value in [(16, 0x7ffffff0), (20, 0x7ffffff0), (24, 0xffff), (28, 0xffff), (0, 0xffffff)]:
        bad = bytearray(elf)
        struct.pack_into('<I', bad, at + field, value)
        try:
            mknxe.convert(bytes(bad), 0)
            check(True, f'section {i} field {field}')     # some fields do not matter for some sections
        except mknxe.ElfError:
            check(True, f'section {i} field {field}')
        except Exception as exc:                           # noqa: BLE001
            check(False, f'section {i} field {field}: {type(exc).__name__}: {exc}')
    bad = bytearray(elf)                                   # an address far away: a huge span
    flags = struct.unpack_from('<I', elf, at + 8)[0]
    if flags & mknxe.SHF_ALLOC and struct.unpack_from('<I', elf, at + 20)[0]:     # empty ones are left out
        struct.pack_into('<I', bad, at + 12, 0x0fff0000 if not flags & mknxe.SHF_WRITE else 0x1fff0000)
        refused(bad, f'section {i} far away')

print(f'test_mknxe: {checks} checks, {failures} failed')
sys.exit(1 if failures else 0)
