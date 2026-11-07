"""Disassemble Rev 1 ARM9 code around an address (debug helper, needs capstone).

usage: disarm.py ROM ADDR [count] [thumb]
Looks in the static image, ITCM/DTCM autoloads, then the overlays (all of them
when overlays share the address). Output is derived from the game: never commit it.
"""
import sys
import capstone
from rom import Rom


def main(path, addr, count=24, thumb=False):
    r = Rom(path)
    segs = [(n, a, bytes(b)) for n, a, b in r.arm9_segments()]
    segs += [('ov%02d' % o['id'], o['ram'], bytes(o['image'])) for o in r.overlays9()]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    for name, base, img in segs:
        if not (base <= addr < base + len(img)):
            continue
        start = max(base, addr - (count // 2) * (2 if thumb else 4))
        off = start - base
        print('--', name)
        for ins in md.disasm(img[off:off + count * 4], start):
            mark = '>' if ins.address == addr else ' '
            print('%s %08X  %-8s %s' % (mark, ins.address, ins.mnemonic, ins.op_str))
            count -= 1
            if count <= 0:
                break


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 24,
         len(sys.argv) > 4 and sys.argv[4] == 'thumb')
