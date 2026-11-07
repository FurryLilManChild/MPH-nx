"""Turn the Switch log's "no native code at" lines into analysis roots.

A "(call)" miss is a function start. A mid-function miss (the interpreter
reached a branch target inside a routine the analysis never found) is walked
back to the routine's prologue: the nearest earlier PUSH that saves LR, plus a
PUSH {r0-r3} just before it (varargs). Overlay addresses are resolved against
the overlays the log reported present at that point, and the root names its
overlay ("ov=NN") so it is not applied to other overlays at the same address.
Output is derived from the game: never commit it.

usage: missroots.py ROM LOG [LOG...] > roots
"""
import re, sys
from rom import Rom


if __name__ == '__main__':
    rom, logs = sys.argv[1], sys.argv[2:]
    r = Rom(rom)
    statics = [(n, a, bytes(b)) for n, a, b in r.arm9_segments() if n != 'dtcm']
    ovs = {o['id']: (o['ram'], bytes(o['image'])) for o in r.overlays9()}

    def w32(img, base, a):
        o = a - base
        return int.from_bytes(img[o:o + 4], 'little') if 0 <= o <= len(img) - 4 else None

    def prologue(img, base, a, limit=0x1000):
        p = a & ~3
        while p >= base and a - p <= limit:
            w = w32(img, base, p)
            if w is not None and (w & 0xFFFF4000) == 0xE92D4000:
                q = w32(img, base, p - 4)
                if q is not None and (q & 0xFFFFFFF0) == 0xE92D0000:
                    return p - 4
                return p
            p -= 4
        return None

    roots = {}   # (addr, thumb, ov or None) -> why
    present = set()
    miss = re.compile(r'no native code at ([0-9A-F]{8})( \(thumb\))?( \(call\))?')
    ovl = re.compile(r'overlay (\d+): resident \d present (\d)')

    def resolve(a, thumb, call, pres):
        if a >= 0xFFFF0000: return
        places = [(None, base, img) for _, base, img in statics if base <= a < base + len(img)]
        if not places:
            places = [(k, ovs[k][0], ovs[k][1]) for k in sorted(pres) if k in ovs
                      and ovs[k][0] <= a < ovs[k][0] + len(ovs[k][1])]
        for ov, base, img in places:
            if call or thumb:
                start = a
            else:
                start = prologue(img, base, a)
                if start is None: continue
            roots[(start, thumb, ov)] = 'call' if call else 'mid %08X' % a

    for path in logs:
        pending = []
        for line in open(path, errors='replace').read().splitlines() + ['']:
            m = ovl.search(line)
            if m:
                k = int(m.group(1))
                (present.add if m.group(2) == '1' else present.discard)(k)
                continue
            for p in pending: resolve(*p, set(present))
            pending = []
            m = miss.search(line)
            if m: pending.append((int(m.group(1), 16), bool(m.group(2)), bool(m.group(3))))
    print('# runtime roots from Switch miss logs (derived from the game; not committed)')
    for (a, th, ov), why in sorted(roots.items(), key=lambda kv: kv[0][0]):
        print('%08X%s%s  # %s' % (a, ' thumb' if th else '', ' ov=%d' % ov if ov is not None else '', why))
