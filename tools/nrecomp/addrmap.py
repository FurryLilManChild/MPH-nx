"""Map Rev 0 game addresses to Rev 1 by matching the code that references them.

For a Rev 0 data address D, every literal-pool word L in Rev 0 code with
0 <= D - L < WINDOW (the address itself, or a struct base the code adds an
offset to) is a reference. The halfwords around it, with branch offsets and
other address-like words masked, form a context; the same context in Rev 1
gives the Rev 1 literal, and so D1 = L1 + (D - L). Each match votes for a
delta; the answer is the delta most references agree on.

Code addresses (an instruction in an overlay) are mapped by matching the
bytes around the address directly.

Output is derived from the game: print it, never commit it.
"""
import struct, sys, collections
from rom import Rom

WINDOW = 0x1000
CTX = 12            # halfwords on each side


def images(rom):
    out = [('static:' + n, a, bytes(b)) for n, a, b in rom.arm9_segments()]
    for o in rom.overlays9():
        out.append(('ov%02d' % o['id'], o['ram'], bytes(o['image'])))
    return out


def is_addr(v):
    return 0x02000000 <= v < 0x02400000 or 0x027C0000 <= v < 0x02800000 or 0x01FF8000 <= v < 0x02000000


def masked(img):
    """Halfword tokens with branch offsets and address-like words masked."""
    n = len(img) // 2
    h = list(struct.unpack_from('<%dH' % n, img))
    tok = list(h)
    for i in range(0, n - 1, 2):     # word-aligned 32-bit values
        w = h[i] | (h[i + 1] << 16)
        if is_addr(w):
            tok[i] = tok[i + 1] = -1
        elif (w >> 25) & 7 == 5 and (w >> 28) != 0xF:   # ARM B/BL: keep cond+op
            tok[i] = -2; tok[i + 1] = (h[i + 1] & 0xFF00)
        elif (w >> 25) == 0x7D:                          # BLX imm
            tok[i] = -2; tok[i + 1] = 0xFA00
    for i in range(n - 1):            # Thumb BL/BLX pairs
        if (h[i] >> 11) == 0x1E and (h[i + 1] >> 11) in (0x1F, 0x1D):
            tok[i] = -3; tok[i + 1] = -4
    return h, tok


class Index:
    def __init__(self, rom):
        self.segs = []
        for name, base, img in images(rom):
            h, tok = masked(img)
            self.segs.append((name, base, img, h, tok))

    def refs(self, lo, hi):
        """Word-aligned literal positions with lo <= value < hi."""
        for si, (name, base, img, h, tok) in enumerate(self.segs):
            for i in range(0, len(h) - 1, 2):
                w = h[i] | (h[i + 1] << 16)
                if lo <= w < hi:
                    yield si, i, w

    def ctx(self, si, i):
        tok = self.segs[si][4]
        a = tuple(tok[max(0, i - CTX):i]); b = tuple(tok[i + 2:i + 2 + CTX])
        return a, b


def build_lookup(ix):
    look = collections.defaultdict(list)
    for si, (name, base, img, h, tok) in enumerate(ix.segs):
        for i in range(CTX, len(h) - CTX - 2, 2):
            w = h[i] | (h[i + 1] << 16)
            if is_addr(w):
                key = (tuple(tok[i - CTX:i]), tuple(tok[i + 2:i + 2 + CTX]))
                look[key].append((si, i, w))
    return look


def map_data(ix0, ix1, look1, d):
    votes = collections.Counter(); hits = []
    for si, i, l0 in ix0.refs(d - WINDOW + 1, d + 1):
        m = look1.get(ix0.ctx(si, i))
        if not m or len(m) != 1: continue
        _, _, l1 = m[0]
        votes[l1 - l0] += 1
        hits.append((ix0.segs[si][0], l0, l1))
    return votes, hits


def map_code(ix0, ix1, a, span=24):
    """Find the bytes around code address a (Rev 0) in Rev 1."""
    for name, base, img, h, tok in ix0.segs:
        if base <= a < base + len(img):
            i = (a - base) // 2
            pat = tuple(tok[i - span:i + span])
            res = []
            for n1, b1, img1, h1, t1 in ix1.segs:
                if n1 != name: continue
                for j in range(span, len(t1) - span):
                    if tuple(t1[j - span:j + span]) == pat:
                        res.append(b1 + 2 * j)
            return name, res
    return None, []


def main(rom0, rom1, addrs):
    r0, r1 = Rom(rom0), Rom(rom1)
    ix0, ix1 = Index(r0), Index(r1)
    look1 = build_lookup(ix1)
    for spec in addrs:
        kind, a = spec.split(':') if ':' in spec else ('d', spec)
        a = int(a, 16)
        if kind == 'c':
            name, res = map_code(ix0, ix1, a)
            print('code %08X (%s) -> %s' % (a, name, ' '.join('%08X' % x for x in res) or 'NO MATCH'))
            continue
        votes, hits = map_data(ix0, ix1, look1, a)
        if not votes:
            print('data %08X -> NO MATCH' % a); continue
        (best, n), = votes.most_common(1)
        tot = sum(votes.values())
        print('data %08X -> %08X  (delta %+X, %d of %d refs agree; others %s)' % (
            a, a + best, best, n, tot, dict(list(votes.most_common(4))[1:])))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
