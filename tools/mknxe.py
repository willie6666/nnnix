#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a program built with -fropi -frwpi and linked by sdk/nxe.ld (ld.lld
--emit-relocs) into an NXE file.

Text (code and read-only data, linked at 0) reaches itself PC-relative and data
(linked at DATA_BASE) through r9, so neither needs changing when it moves;
only words that hold an absolute address do. An R_ARM_ABS32 relocation in
text becomes a text relocation (the word holds a text address; one holding a
data address cannot work, as text is shared by every run); one in data becomes
a data relocation, bit 0 set when the word points into data or bss, which is
then rewritten as an offset from the data start. R_ARM_SBREL32 (r9-relative)
and PC-relative relocations are already final; any other kind is refused.

Format (little-endian, lib/nnlib.h):
  magic "NXE1", version u16 = 2, lib_version u16, text u32, data u32, bss u32,
  entry u32, text_relocs u16, data_relocs u16, stack u32, text_crc u32,
  data_crc u32 | text | text relocations | data | data relocations
"""
import argparse
import struct
import sys
import zlib

NXE_MAGIC = 0x3145584e
NXE_VERSION = 2
LIB_VERSION = 1
DATA_BASE = 0x10000000                                    # sdk/nxe.ld
RELOC_DATA = 1

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 1, 2, 8, 9, 4
SHF_WRITE, SHF_ALLOC = 0x1, 0x2
SHN_UNDEF, SHN_ABS = 0, 0xfff1
R_ARM_ABS32, R_ARM_TARGET1, R_ARM_SBREL32 = 2, 38, 9
# PC-relative, r9-relative or no-op: final as linked.
RELATIVE = {
    0: 'NONE', 3: 'REL32', 9: 'SBREL32', 10: 'THM_CALL', 11: 'THM_PC8', 30: 'THM_JUMP24', 40: 'V4BX', 42: 'PREL31',
    51: 'THM_JUMP19', 102: 'THM_JUMP11', 103: 'THM_JUMP8', 129: 'THM_PC12',
}


class ElfError(Exception):
    pass


def sections(data):
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ElfError('not a 32-bit little-endian ELF file')
    e_type, e_machine, _, e_entry, _, e_shoff = struct.unpack_from('<HHIIII', data, 16)
    if e_machine != 40:
        raise ElfError('not an ARM ELF file')
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 46)
    out = []
    for i in range(e_shnum):
        f = struct.unpack_from('<IIIIIIIIII', data, e_shoff + i * e_shentsize)
        out.append(dict(name=f[0], type=f[1], flags=f[2], addr=f[3], offset=f[4], size=f[5],
                        link=f[6], info=f[7], entsize=f[9]))
    strtab = out[e_shstrndx]
    for s in out:
        end = data.index(b'\0', strtab['offset'] + s['name'])
        s['name'] = data[strtab['offset'] + s['name']:end].decode()
    return e_entry, out


def convert(data, stack):
    entry, secs = sections(data)
    alloc = [s for s in secs if s['flags'] & SHF_ALLOC and s['size']]
    if any(s['type'] == SHT_RELA and secs[s['info']]['flags'] & SHF_ALLOC for s in secs):
        raise ElfError('RELA relocations are not expected')
    for s in alloc:
        if s['type'] not in (SHT_PROGBITS, SHT_NOBITS):
            raise ElfError(f'unexpected section {s["name"]}')
        if (s['addr'] >= DATA_BASE) != bool(s['flags'] & SHF_WRITE):
            raise ElfError(f'section {s["name"]} is in the wrong place (link with sdk/nxe.ld)')
    text_secs = [s for s in alloc if s['addr'] < DATA_BASE]
    data_secs = [s for s in alloc if s['addr'] >= DATA_BASE and s['type'] == SHT_PROGBITS]
    bss_secs = [s for s in alloc if s['addr'] >= DATA_BASE and s['type'] == SHT_NOBITS]
    if any(s['type'] == SHT_NOBITS for s in text_secs):
        raise ElfError('bss below the data (link with sdk/nxe.ld)')

    def gather(group, start):
        end = max((s['addr'] + s['size'] for s in group), default=start)
        out = bytearray((end - start + 3) & ~3)
        for s in group:
            out[s['addr'] - start:s['addr'] - start + s['size']] = data[s['offset']:s['offset'] + s['size']]
        return out
    text = gather(text_secs, 0)
    rw = gather(data_secs, DATA_BASE)
    data_end = DATA_BASE + len(rw)
    if any(s['addr'] < data_end for s in bss_secs):
        raise ElfError('bss must follow the data (link with sdk/nxe.ld)')
    bss_end = max((s['addr'] + s['size'] for s in bss_secs), default=data_end)
    bss = (bss_end - data_end + 3) & ~3
    ram_end = DATA_BASE + len(rw) + bss

    text_relocs, data_relocs = set(), {}
    rel_secs = [s for s in secs if s['type'] == SHT_REL and secs[s['info']]['flags'] & SHF_ALLOC]
    if not rel_secs and text:
        raise ElfError('no relocations found: link with --emit-relocs')
    for s in rel_secs:
        symtab = secs[s['link']]
        for k in range(s['size'] // 8):
            offset, info = struct.unpack_from('<II', data, s['offset'] + k * 8)
            kind, sym = info & 0xff, info >> 8
            if kind in RELATIVE:
                continue
            if kind not in (R_ARM_ABS32, R_ARM_TARGET1):
                raise ElfError(f'relocation type {kind} at 0x{offset:x} in {secs[s["info"]]["name"]} is not supported '
                               '(build with -fropi -frwpi)')
            shndx = struct.unpack_from('<H', data, symtab['offset'] + sym * 16 + 14)[0]
            if shndx == SHN_ABS:
                continue                                  # a constant, not an address
            if shndx == SHN_UNDEF and sym:
                raise ElfError(f'undefined symbol referenced at 0x{offset:x}')
            if offset % 4:
                raise ElfError(f'relocation at 0x{offset:x} is unaligned')
            if offset + 4 <= len(text):
                word = struct.unpack_from('<I', text, offset)[0]
                if word >= DATA_BASE:
                    raise ElfError(f'read-only word at 0x{offset:x} holds a data address: make the object it is in '
                                   'writable, or build with -frwpi')
                text_relocs.add(offset)
            elif DATA_BASE <= offset and offset + 4 <= data_end:
                at = offset - DATA_BASE
                word = struct.unpack_from('<I', rw, at)[0]
                if DATA_BASE <= word <= ram_end:
                    struct.pack_into('<I', rw, at, word - DATA_BASE)
                    data_relocs[at] = at | RELOC_DATA
                elif at not in data_relocs:
                    data_relocs[at] = at
            else:
                raise ElfError(f'relocation at 0x{offset:x} is outside text and data')
    if not entry & 1 or entry >= len(text):
        raise ElfError(f'entry 0x{entry:x} is not a Thumb address inside the text')
    if stack and not 512 <= stack <= 4096:
        raise ElfError('stack must be 512-4096 bytes')
    if len(text_relocs) > 0xffff or len(data_relocs) > 0xffff:
        raise ElfError('too many relocations')
    trel = b''.join(struct.pack('<I', r) for r in sorted(text_relocs))
    drel = b''.join(struct.pack('<I', data_relocs[r]) for r in sorted(data_relocs))
    text_crc = zlib.crc32(bytes(text) + trel) & 0xffffffff
    data_crc = zlib.crc32(bytes(rw) + drel) & 0xffffffff
    header = struct.pack('<IHHIIIIHHIII', NXE_MAGIC, NXE_VERSION, LIB_VERSION, len(text), len(rw), bss, entry,
                         len(text_relocs), len(data_relocs), stack, text_crc, data_crc)
    return (header + bytes(text) + trel + bytes(rw) + drel,
            dict(text=len(text), data=len(rw), bss=bss, text_relocs=len(text_relocs), data_relocs=len(data_relocs)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf')
    parser.add_argument('nxe')
    parser.add_argument('--stack', type=int, default=0, help='task stack in bytes (default: the loader\'s 1024)')
    args = parser.parse_args()
    with open(args.elf, 'rb') as f:
        data = f.read()
    try:
        out, info = convert(data, args.stack)
    except ElfError as exc:
        sys.exit(f'mknxe: {args.elf}: {exc}')
    with open(args.nxe, 'wb') as f:
        f.write(out)
    print(f'{args.nxe}: text {info["text"]} B, data {info["data"]} B, bss {info["bss"]} B, '
          f'relocations {info["text_relocs"]} + {info["data_relocs"]}, file {len(out)} B')


if __name__ == '__main__':
    main()
