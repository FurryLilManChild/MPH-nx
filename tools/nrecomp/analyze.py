"""MPH-nx code discovery.

Finds the game's functions by recursive descent from known entry points, follows
calls and branches in both instruction sets, resolves switch tables, and adds
entries from function pointers found in data. Each overlay is analysed together
with the static image it runs beside.
"""
import struct
from decode import decode_arm, decode_thumb, Insn


class Image:
    def __init__(self, segs):
        self.segs = sorted(segs, key=lambda s: s[1])   # (name, base, bytes)

    def seg(self, addr):
        for s in self.segs:
            if s[1] <= addr < s[1] + len(s[2]):
                return s
        return None

    def r16(self, a):
        s = self.seg(a)
        if not s or a + 2 > s[1] + len(s[2]): return None
        return struct.unpack_from('<H', s[2], a - s[1])[0]

    def r32(self, a):
        s = self.seg(a)
        if not s or a + 4 > s[1] + len(s[2]): return None
        return struct.unpack_from('<I', s[2], a - s[1])[0]

    def code_like(self, a):
        s = self.seg(a)
        return s is not None and s[0] != 'dtcm'


class Function:
    def __init__(self, entry, thumb, owner):
        self.entry, self.thumb, self.owner = entry, thumb, owner
        self.insns = {}          # addr -> Insn
        self.blocks = set()      # block start addresses (branch targets and entry)
        self.calls = set()       # (target, thumb)
        self.tail = set()        # branches leaving the function to other entries
        self.indirect = []       # addrs of unresolved indirect transfers
        self.tables = {}         # addr of switch insn -> list of targets
        self.bad = None          # reason the function could not be decoded


def decode_at(img, a, thumb):
    if thumb:
        h = img.r16(a)
        if h is None: return None
        h2 = img.r16(a + 2)
        return decode_thumb(h, a, h2)
    w = img.r32(a)
    if w is None: return None
    return decode_arm(w, a)


ARM_PROLOGUE_MASKS = [
    (0xFFFF4000, 0xE92D4000),   # STMDB sp!, {..., lr}
    (0xFFFFF000, 0xE24DD000),   # SUB sp, sp, #imm
    (0xFFFF0000, 0xE52DE000),   # STR lr, [sp, #-imm]!
]


def plausible_entry(img, a, thumb):
    if not img.code_like(a): return False
    if thumb:
        h = img.r16(a)
        if h is None: return False
        if (h & 0xFF00) == 0xB500: return True      # PUSH {..., lr}
        if (h & 0xFF80) == 0xB080: return True      # SUB sp, #imm
        return False
    if a & 3: return False
    w = img.r32(a)
    if w is None: return False
    return any((w & m) == v for m, v in ARM_PROLOGUE_MASKS)


class Analyzer:
    def __init__(self, img, max_insns=20000):
        self.img = img
        self.funcs = {}          # (addr, thumb) -> Function
        self.queue = []
        self.max_insns = max_insns
        self.code_bytes = set()  # addresses decoded as code (instruction starts)

    def add_entry(self, a, thumb, why='call'):
        key = (a & ~1, bool(thumb))
        if key in self.funcs or key in [q[:2] for q in self.queue[-1:]]:
            return
        self.queue.append((key[0], key[1], why))

    def run(self):
        while self.queue:
            a, thumb, why = self.queue.pop()
            if (a, thumb) in self.funcs:
                continue
            f = Function(a, thumb, why)
            self.funcs[(a, thumb)] = f
            self._explore(f)

    def _switch_table(self, f, i, prev):
        """ARM jump tables: CMP rX,#n ; ...; LDRLS pc,[pc,rX,lsl #2] ; B default ; .word ...
        or CMP rX,#n ; ...; ADDLS pc,pc,rX,lsl #2 ; B default ; B case0 ; ...
        An ADD pc,pc,rX,lsl #2 with no bound is a computed jump into the
        instructions that follow it (an unrolled loop)."""
        if i.thumb:
            return None
        def bound(rm):
            for k in range(1, 6):
                p = f.insns.get(i.addr - 4 * k)
                if p is None: return None
                if p.op == 'cmp' and p.f['rn'] == rm and p.f['op2']['kind'] == 'imm':
                    return p.f['op2']['value'] + 1
                if p.writes_pc or p.call: return None
            return None
        if i.op == 'ldr' and i.f['rn'] == 15 and i.f['off']['kind'] == 'regimm' \
                and i.f['off']['sh'] == 0 and i.f['off']['amt'] == 2 and i.f['p'] and i.f['u']:
            n = bound(i.f['off']['rm'])
            if not n or n > 1024: return None
            base = i.addr + 8
            t = []
            for k in range(n):
                v = self.img.r32(base + 4 * k)
                if v is None: return None
                t.append(v)
            return ('ldr', t)
        if i.op == 'add' and i.f['rd'] == 15 and i.f['rn'] == 15 \
                and i.f['op2']['kind'] == 'regimm' and i.f['op2']['sh'] == 0 \
                and i.f['op2']['amt'] == 2:
            base = i.addr + 8
            n = bound(i.f['op2']['rm'])
            if n and n <= 1024:
                return ('add', [base + 4 * k for k in range(n)])
            # unbounded: every instruction up to the first unconditional exit
            t = []
            a = base
            for _ in range(256):
                d = decode_at(self.img, a, False)
                if d is None or d.op == 'undef': break
                t.append(a)
                if d.ends_block and d.cond == 14: break
                a += 4
            return ('add', t) if t else None
        return None

    def _static_jump(self, f, i, prev):
        """Statically known PC writes: BX PC (Thumb to ARM veneer) and
        LDR PC,[PC,#imm] (long jump through a literal). Returns (target, thumb)."""
        if i.op == 'bx' and i.f['rm'] == 15:
            return ((i.addr + 4) & ~3, False) if f.thumb else (i.addr + 8, False)
        if i.op == 'ldr' and i.f['rn'] == 15 and i.f['off']['kind'] == 'imm' and i.cond == 14 \
                and i.f['p'] and not i.f['wb']:
            base = ((i.addr + 4) & ~3) if f.thumb else i.addr + 8
            lit = base + (i.f['off']['value'] if i.f['u'] else -i.f['off']['value'])
            v = self.img.r32(lit)
            if v is not None and self.img.seg(v & ~1):
                return (v & ~1, bool(v & 1))
        return None

    @staticmethod
    def _is_movlr(prev):
        return prev is not None and prev.op == 'mov' and prev.f['rd'] == 14 and \
            prev.f['op2']['kind'] == 'regimm' and prev.f['op2']['rm'] == 15 and \
            prev.f['op2']['amt'] == 0 and prev.f['op2']['sh'] == 0 and not prev.thumb

    def _explore(self, f):
        img = self.img
        work = [f.entry]
        f.blocks.add(f.entry)
        while work:
            a = work.pop()
            prev = None
            while True:
                if a in f.insns:
                    break
                if len(f.insns) >= self.max_insns:
                    f.bad = 'too large'
                    return
                i = decode_at(img, a, f.thumb)
                if i is None or i.op == 'undef':
                    f.bad = 'undecodable at %08x' % a
                    f.insns[a] = i if i else Insn(a, 2 if f.thumb else 4, f.thumb, 0, 'undef')
                    break
                f.insns[a] = i
                self.code_bytes.add(a)
                nxt = a + i.size

                if i.op in ('bl', 'blx_imm'):
                    tthumb = (not f.thumb) if i.exchange else f.thumb
                    f.calls.add((i.target & ~1, tthumb))
                    self.add_entry(i.target, tthumb, 'call')
                    if i.cond != 14 and not f.thumb:
                        pass
                    a = nxt; prev = i
                    continue

                if i.op == 'b':
                    tgt = i.target
                    if self.img.seg(tgt) is None:
                        f.indirect.append(a)
                    elif (tgt, f.thumb) in self.funcs and tgt != f.entry:
                        f.tail.add((tgt, f.thumb))
                    else:
                        f.blocks.add(tgt)
                        work.append(tgt)
                    if i.cond == 14:
                        break
                    f.blocks.add(nxt)
                    a = nxt; prev = i
                    continue

                if i.op == 'blx_reg':
                    a = nxt; prev = i
                    continue

                if i.writes_pc and not i.is_return and i.op in ('bx', 'ldr') and self._is_movlr(prev):
                    # MOV LR, PC ; BX Rn / LDR PC, [...]: a call returning here
                    i.call = True
                    sj = self._static_jump(f, i, prev)
                    if sj:
                        f.calls.add(sj); self.add_entry(sj[0], sj[1], 'call')
                    f.blocks.add(nxt)
                    a = nxt; prev = i
                    continue

                if i.writes_pc and not i.is_return:
                    sj = self._static_jump(f, i, prev)
                    if sj:
                        i.f['static_target'] = sj
                        f.tail.add(sj)
                        self.add_entry(sj[0], sj[1], 'jump')
                        if i.cond != 14:
                            f.blocks.add(nxt)
                            a = nxt; prev = i
                            continue
                        break

                if i.writes_pc:
                    tab = self._switch_table(f, i, prev)
                    if tab:
                        kind, targets = tab
                        f.tables[a] = targets
                        for t in targets:
                            f.blocks.add(t & ~1)
                            work.append(t & ~1)
                        if i.cond != 14:
                            f.blocks.add(nxt)
                            a = nxt; prev = i
                            continue
                        break
                    if not i.is_return:
                        f.indirect.append(a)
                    if i.cond != 14:
                        f.blocks.add(nxt)
                        a = nxt; prev = i
                        continue
                    break

                if i.op == 'swi' or i.op in ('mcr', 'mrc'):
                    pass
                a = nxt
                prev = i

    def pointer_scan(self):
        """Words in non-code memory that point at a plausible function start."""
        added = 0
        for name, base, data in self.img.segs:
            for o in range(0, len(data) - 3, 4):
                a = base + o
                if a in self.code_bytes:
                    continue
                v = struct.unpack_from('<I', data, o)[0]
                thumb = bool(v & 1)
                t = v & ~1
                if (t, thumb) in self.funcs:
                    continue
                if not self.img.seg(t) or t in self.code_bytes:
                    continue
                if plausible_entry(self.img, t, thumb):
                    self.add_entry(t, thumb, 'pointer')
                    added += 1
        return added


def report(an, label):
    fs = list(an.funcs.values())
    good = [f for f in fs if not f.bad]
    insns = sum(len(f.insns) for f in good)
    ind = sum(len(f.indirect) for f in good)
    by = {}
    for f in fs:
        by[f.owner] = by.get(f.owner, 0) + 1
    print('%s: %d functions (%d failed), %d instructions, %d unresolved indirect, entries by source %s'
          % (label, len(fs), len(fs) - len(good), insns, ind, by))


if __name__ == '__main__':
    import sys
    from rom import Rom
    r = Rom(sys.argv[1])
    segs = r.arm9_segments()
    img = Image(segs)
    an = Analyzer(img)
    an.add_entry(r.arm9_entry, False, 'entry')
    an.run()
    report(an, 'static, entry only')
    for k in range(3):
        n = an.pointer_scan()
        an.run()
        report(an, 'static, pointer pass %d (+%d)' % (k + 1, n))
    # code coverage of the main and ITCM segments
    for name, base, data in segs:
        cov = sum(1 for a in an.code_bytes if base <= a < base + len(data))
        print('  %s: %d instruction starts in %d bytes' % (name, cov, len(data)))
    bad = [f for f in an.funcs.values() if f.bad]
    for f in bad[:15]:
        print('  failed %08x %s %s (%s)' % (f.entry, 'T' if f.thumb else 'A', f.bad, f.owner))


def analyze_all(rom_path, verbose=True):
    from rom import Rom
    r = Rom(rom_path)
    segs = r.arm9_segments()
    an = Analyzer(Image(segs))
    an.add_entry(r.arm9_entry, False, 'entry')
    an.run()
    for _ in range(3):
        if not an.pointer_scan(): break
        an.run()
    static_end = segs[0][1] + len(segs[0][2])
    res = {'static': an, 'overlays': {}}
    for o in r.overlays9():
        oseg = ('ov%02d' % o['id'], o['ram'], o['image'])
        oan = Analyzer(Image(segs + [oseg]))
        # entries: static-init table, calls from static code into this overlay's range,
        # and pointers in the overlay
        s0, s1 = o['sinit']
        for a in range(s0, s1, 4):
            v = oan.img.r32(a)
            if v: oan.add_entry(v, bool(v & 1), 'sinit')
        lo, hi = o['ram'], o['ram'] + len(o['image'])
        for f in an.funcs.values():
            for t, th in f.calls | f.tail:
                if lo <= t < hi: oan.add_entry(t, th, 'static-call')
        # known static functions are not re-explored: mark them done
        for key, f in an.funcs.items():
            if not f.bad and not (lo <= key[0] < hi):
                oan.funcs[key] = f
                oan.code_bytes.update(f.insns.keys())
        oan.run()
        for _ in range(3):
            if not oan.pointer_scan(): break
            oan.run()
        own = {k: f for k, f in oan.funcs.items() if lo <= k[0] < hi}
        res['overlays'][o['id']] = (o, own)
        if verbose:
            insns = sum(len(f.insns) for f in own.values() if not f.bad)
            bad = sum(1 for f in own.values() if f.bad)
            ind = sum(len(f.indirect) for f in own.values() if not f.bad)
            print('ov%02d %08x size %6x: %4d functions (%d failed) %6d insns (%3.0f%% of bytes) %d indirect'
                  % (o['id'], lo, len(o['image']), len(own), bad, insns,
                     100.0 * sum(f.insns[a].size for f in own.values() if not f.bad for a in f.insns if f.insns[a]) / max(1, len(o['image'])), ind))
    return res
