#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a program linked at address 0 (sdk/nxe.ld, ld.lld --emit-relocs) into an NXE file.

The image is the allocated PROGBITS sections; NOBITS sections become bss. Every
R_ARM_ABS32 relocation in the image becomes an entry of the relocation table:
the loader adds the load address to that word. PC-relative relocations need no
change when the whole image moves; any other kind is refused.

Format (little-endian, lib/nnlib.h):
  magic "NXE1", version u16, lib_version u16, image u32, bss u32, entry u32,
  nrelocs u32, stack u32, crc u32 | image | nrelocs x u32 offsets
"""
import argparse
import struct
import sys
import zlib

NXE_MAGIC = 0x3145584e
NXE_VERSION = 1
LIB_VERSION = 1

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 1, 2, 8, 9, 4
SHF_ALLOC = 0x2
SHN_UNDEF, SHN_ABS = 0, 0xfff1
R_ARM_ABS32, R_ARM_TARGET1 = 2, 38
# PC-relative or no-op: unchanged when code and data move together.
RELATIVE = {
    0: 'NONE', 3: 'REL32', 10: 'THM_CALL', 11: 'THM_PC8', 30: 'THM_JUMP24', 40: 'V4BX', 42: 'PREL31',
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
    progbits = [s for s in secs if s['flags'] & SHF_ALLOC and s['type'] == SHT_PROGBITS and s['size']]
    nobits = [s for s in secs if s['flags'] & SHF_ALLOC and s['type'] == SHT_NOBITS and s['size']]
    if any(s['type'] == SHT_RELA and secs[s['info']]['flags'] & SHF_ALLOC for s in secs):
        raise ElfError('RELA relocations are not expected')
    end = max((s['addr'] + s['size'] for s in progbits), default=0)
    end = (end + 3) & ~3
    image = bytearray(end)
    for s in progbits:
        image[s['addr']:s['addr'] + s['size']] = data[s['offset']:s['offset'] + s['size']]
    bss_end = max((s['addr'] + s['size'] for s in nobits), default=end)
    if any(s['addr'] < end for s in nobits):
        raise ElfError('bss must follow the image (link with sdk/nxe.ld)')
    bss = (bss_end - end + 3) & ~3

    relocs = set()
    rel_secs = [s for s in secs if s['type'] == SHT_REL and secs[s['info']]['flags'] & SHF_ALLOC]
    if not rel_secs and progbits:
        raise ElfError('no relocations found: link with --emit-relocs')
    for s in rel_secs:
        symtab = secs[s['link']]
        for k in range(s['size'] // 8):
            offset, info = struct.unpack_from('<II', data, s['offset'] + k * 8)
            kind, sym = info & 0xff, info >> 8
            if kind in RELATIVE:
                continue
            if kind not in (R_ARM_ABS32, R_ARM_TARGET1):
                raise ElfError(f'relocation type {kind} at 0x{offset:x} in {secs[s["info"]]["name"]} is not supported')
            shndx = struct.unpack_from('<H', data, symtab['offset'] + sym * 16 + 14)[0]
            if shndx == SHN_ABS:
                continue                                  # a constant, not an address
            if shndx == SHN_UNDEF and sym:
                raise ElfError(f'undefined symbol referenced at 0x{offset:x}')
            if offset % 4 or offset + 4 > end:
                raise ElfError(f'relocation at 0x{offset:x} is outside the image or unaligned')
            relocs.add(offset)
    if not entry & 1 or entry >= end:
        raise ElfError(f'entry 0x{entry:x} is not a Thumb address inside the image')
    if stack and not 512 <= stack <= 4096:
        raise ElfError('stack must be 512-4096 bytes')
    table = b''.join(struct.pack('<I', r) for r in sorted(relocs))
    crc = zlib.crc32(bytes(image) + table) & 0xffffffff
    header = struct.pack('<IHHIIIIII', NXE_MAGIC, NXE_VERSION, LIB_VERSION, end, bss, entry, len(relocs),
                         stack, crc)
    return header + bytes(image) + table, end, bss, len(relocs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf')
    parser.add_argument('nxe')
    parser.add_argument('--stack', type=int, default=0, help='task stack in bytes (default: the loader\'s 1024)')
    args = parser.parse_args()
    with open(args.elf, 'rb') as f:
        data = f.read()
    try:
        out, image, bss, nrelocs = convert(data, args.stack)
    except ElfError as exc:
        sys.exit(f'mknxe: {args.elf}: {exc}')
    with open(args.nxe, 'wb') as f:
        f.write(out)
    print(f'{args.nxe}: image {image} B, bss {bss} B, {nrelocs} relocations, file {len(out)} B')


if __name__ == '__main__':
    main()
