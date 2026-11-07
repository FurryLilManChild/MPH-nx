"""Keep only analysis roots that look like real function starts.

A root is kept when the interpreter reference saw it called (refcalls file),
or when the instruction before it ends a function (return, unconditional
branch, pop of pc): code that can only be reached by a jump to its start.
Mid-function addresses (after an svc, inside a loop) are dropped: as roots
they split a function in two. Output is derived from the game: never commit.

usage: rootfilter.py ROM ROOTS REFCALLS > filtered
"""
import sys, capstone
from rom import Rom


def main(rom_path, roots_path, ref_path):
    r = Rom(rom_path)
    segs = [(a, bytes(b)) for _, a, b in r.arm9_segments()]
    segs += [(o['ram'], bytes(o['image'])) for o in r.overlays9()]
    ref = set()
    for l in open(ref_path):
        p = l.split()
        if len(p) == 2: ref.add(int(p[1], 16))
    arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    thm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)

    def ends_function(a, thumb):
        size = 2 if thumb else 4
        for base, img in segs:
            if base <= a - size and a < base + len(img):
                code = img[a - size - base:a - base]
                ins = list((thm if thumb else arm).disasm(code, a - size))
                if not ins: return False
                i = ins[0]; m = i.mnemonic; ops = i.op_str
                if m.startswith('bx') and m in ('bx',): return True
                if m in ('b', 'b.w'): return True
                if m.startswith('pop') and 'pc' in ops: return True
                if m.startswith('ldm') and 'pc' in ops and m in ('ldm', 'ldmia', 'ldmfd', 'ldmib', 'ldmda', 'ldmdb'): return True
                if m == 'mov' and ops.startswith('pc,'): return True
                return False
        return False

    keep = drop = 0
    out = []
    for line in open(roots_path):
        if line.startswith('#') or not line.strip():
            out.append(line.rstrip('\n')); continue
        p = line.split(); a = int(p[0], 16); th = len(p) > 1
        if a >= 0xFFFF0000: drop += 1; continue
        if (a | (1 if th else 0)) in ref or ends_function(a, th):
            out.append(line.rstrip('\n')); keep += 1
        else:
            drop += 1
    print('\n'.join(out))
    print('kept %d dropped %d' % (keep, drop), file=sys.stderr)


if __name__ == '__main__':
    main(*sys.argv[1:4])
