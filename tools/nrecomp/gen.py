"""MPH-nx native code generator.

Turns each discovered guest function into one C function. Guest registers and
flags are C locals; branches inside a function are gotos; known calls are direct
C calls; loads and stores go through the page-table helpers in nr_runtime.h;
cycles are added per block and the hardware is serviced at back-edges, calls and
returns.

Function ABI: void F(uint32_t entry). entry == 0: a normal call (LR holds the
return address). entry != 0: resume at guest address `entry` (from the top-level
loop). A return to anything other than the entry LR, or any resumed function's
return, hands the target back through nr_redirect.
"""
from decode import DP_NAMES

COND = {
    0: 'fz', 1: '!fz', 2: 'fc', 3: '!fc', 4: 'fn', 5: '!fn', 6: 'fv', 7: '!fv',
    8: '(fc && !fz)', 9: '(!fc || fz)', 10: '(fn == fv)', 11: '(fn != fv)',
    12: '(!fz && fn == fv)', 13: '(fz || fn != fv)',
}

LOGICAL = {'and', 'eor', 'tst', 'teq', 'orr', 'mov', 'bic', 'mvn'}
NOWRITE = {'tst', 'teq', 'cmp', 'cmn'}


def h32(v):
    return '0x%08Xu' % (v & 0xFFFFFFFF)


class FuncGen:
    def __init__(self, f, name, resolve, entries):
        """resolve(target, thumb) -> ('direct', cname) | ('dyn', None)
        entries: set of addresses that must be resumable (block starts)."""
        self.f = f
        self.name = name
        self.resolve = resolve
        self.out = []
        self.labels = set(f.blocks) | set(entries)
        # every instruction after a call is a resume point
        for a, i in f.insns.items():
            if i and i.call:
                self.labels.add(a + i.size)
        self.labels = {a for a in self.labels if a in f.insns}

    def w(self, s):
        self.out.append(s)

    # -- register access -------------------------------------------------
    def reg(self, i, r):
        if r == 15:
            return h32(i.pcval)
        return 'r%d' % r

    def shifter(self, i, op2, need_carry):
        """Return (value_expr, carry_expr or None). Emits temporaries."""
        k = op2['kind']
        if k == 'imm':
            v = h32(op2['value'])
            if need_carry:
                rot = op2.get('rot', 0)
                return v, ('fc' if rot == 0 else str((op2['value'] >> 31) & 1))
            return v, None
        rm = self.reg(i, op2['rm'])
        if k == 'regimm':
            sh, amt = op2['sh'], op2['amt']
            if sh == 0:
                if amt == 0:
                    return rm, ('fc' if need_carry else None)
                c = '((%s >> %d) & 1)' % (rm, 32 - amt) if need_carry else None
                return '(%s << %d)' % (rm, amt), c
            if sh == 1:
                if amt == 0:
                    return '0u', ('(%s >> 31)' % rm if need_carry else None)
                return '(%s >> %d)' % (rm, amt), ('((%s >> %d) & 1)' % (rm, amt - 1) if need_carry else None)
            if sh == 2:
                if amt == 0:
                    return '((uint32_t)((int32_t)%s >> 31))' % rm, ('(%s >> 31)' % rm if need_carry else None)
                return '((uint32_t)((int32_t)%s >> %d))' % (rm, amt), \
                    ('((%s >> %d) & 1)' % (rm, amt - 1) if need_carry else None)
            if amt == 0:   # RRX
                return '((fc << 31) | (%s >> 1))' % rm, ('(%s & 1)' % rm if need_carry else None)
            return 'nr_ror(%s, %d)' % (rm, amt), ('((%s >> %d) & 1)' % (rm, amt - 1) if need_carry else None)
        # register-specified shift
        if op2['rm'] == 15:
            rm = h32(i.pcval + 4)
        rs = self.reg(i, op2['rs'])
        fn = ['nr_lsl', 'nr_lsr', 'nr_asr', 'nr_rorr'][op2['sh']]
        if need_carry:
            self.w('    { uint32_t _c = fc; _sv = %s_c(%s, %s & 0xFF, &_c); _sc = _c; }' % (fn, rm, rs))
            return '_sv', '_sc'
        return '%s(%s, %s & 0xFF)' % (fn, rm, rs), None

    # -- flow helpers ----------------------------------------------------
    def sync_store(self):
        return 'NR_STORE()'

    def tick(self, n):
        return 'nr_now += %d;' % n

    def check(self, resume):
        return ('if (nr_now >= nr_deadline) { NR_STORE(); g_cpu.R[15] = %s; nr_service(); '
                'if (nr_redirect) return; NR_LOAD(); }' % h32(resume))

    def emit_call(self, i, target, thumb_target, ret):
        kind, cname = self.resolve(target, thumb_target)
        retv = ret | (1 if self.f.thumb else 0)
        self.w('    r14 = %s; NR_STORE();' % h32(retv))
        if kind == 'direct':
            self.w('    %s(0);' % cname)
        else:
            self.w('    nr_call_dyn(%s);' % h32(target | (1 if thumb_target else 0)))
        self.w('    if (nr_redirect) return;')
        self.w('    NR_LOAD();')

    def emit_static_transfer(self, i, target, thumb_target, is_call):
        """A PC write whose target is known at generation time."""
        if is_call:
            self.emit_call(i, target, thumb_target, i.addr + i.size)
            return
        if thumb_target == self.f.thumb and target in self.f.insns:
            self.w('    goto L_%08X;' % target)
            return
        kind, cname = self.resolve(target, thumb_target)
        self.w('    NR_STORE();')
        if kind == 'direct':
            self.w('    %s(0);' % cname)
        else:
            self.w('    nr_call_dyn(%s);' % h32(target | (1 if thumb_target else 0)))
        self.w('    if (!nr_redirect && g_cpu.R[15] != _elr) nr_redirect = 1;')
        self.w('    NR_RET(); return;')

    def emit_dyn_jump(self, i, texpr, interwork, is_call=False):
        """Indirect transfer to texpr: a return, a jump inside this function
        (switch tables, computed jumps), or a tail call elsewhere."""
        st = i.f.get('static_target')
        if st:
            self.emit_static_transfer(i, st[0], st[1], is_call or i.call)
            return
        self.w('    { uint32_t _t = %s;' % texpr)
        if is_call or i.call:
            ret = i.addr + i.size
            retv = ret | (1 if self.f.thumb else 0)
            self.w('      r14 = %s; NR_STORE(); nr_call_dyn(_t | %s);' %
                   (h32(retv), '0' if interwork else ('1' if self.f.thumb else '0')))
            self.w('      if (nr_redirect) return; NR_LOAD(); }')
            return
        if not interwork:
            self.w('      _t = (_t & ~1u) | %d;' % (1 if self.f.thumb else 0))
        self.w('      ' + self.tick(1))
        self.w('      if (_t == _elr) { NR_STORE(); g_cpu.R[15] = _t; NR_RET(); return; }')
        self.w('      if ((_t & 1u) == _thumb) {')
        self.w('        if (nr_now >= nr_deadline) { NR_STORE(); g_cpu.R[15] = _t & ~1u; nr_service(); if (nr_redirect) return; NR_LOAD(); }')
        self.w('        _dj = _t; _djk = 1; goto L_DYN; }')
        self.w('      NR_STORE(); nr_call_dyn(_t);')
        self.w('      if (!nr_redirect && g_cpu.R[15] != _elr) nr_redirect = 1;')
        self.w('      return; }')

    # -- instructions ----------------------------------------------------
    def emit_insn(self, i, nxt_label_needed):
        f = i.f
        op = i.op
        cond = i.cond
        if cond != 14:
            if cond == 15:
                self.w('    /* never */')
                return
            self.w('    if (%s) {' % COND[cond])
        getattr(self, 'op_' + self.opclass(op))(i)
        if cond != 14:
            self.w('    }')

    @staticmethod
    def opclass(op):
        if op in DP_NAMES: return 'dp'
        if op in ('ldr', 'ldrb', 'str', 'strb'): return 'ldst'
        if op in ('ldrh', 'strh', 'ldrsb', 'ldrsh', 'ldrd', 'strd'): return 'ldsth'
        return op

    def op_dp(self, i):
        f = i.f
        op = i.op
        s = f['s']
        rd = f['rd']
        logical = op in LOGICAL
        need_c = bool(s) and logical and rd != 15
        sv, sc = self.shifter(i, f['op2'], need_c)
        a = self.reg(i, f['rn'])
        if i.thumb and f['rn'] == 15:
            a = h32(i.addr + 4)
        res = {
            'and': '%s & %s' % (a, sv), 'eor': '%s ^ %s' % (a, sv), 'tst': '%s & %s' % (a, sv),
            'teq': '%s ^ %s' % (a, sv), 'orr': '%s | %s' % (a, sv), 'mov': sv,
            'bic': '%s & ~%s' % (a, sv), 'mvn': '~%s' % sv,
        }
        self.w('    { uint32_t _b = %s;' % sv if not logical else '    {')
        if logical:
            self.w('      uint32_t _r = %s;' % res[op])
            if s and rd != 15:
                self.w('      fn = _r >> 31; fz = _r == 0; fc = %s;' % sc)
        else:
            if op in ('add', 'cmn'):
                self.w('      uint32_t _a = %s; uint32_t _r = _a + _b;' % a)
                if s: self.w('      fc = _r < _a; fv = ((~(_a ^ _b) & (_a ^ _r)) >> 31);')
            elif op in ('sub', 'cmp'):
                self.w('      uint32_t _a = %s; uint32_t _r = _a - _b;' % a)
                if s: self.w('      fc = _a >= _b; fv = (((_a ^ _b) & (_a ^ _r)) >> 31);')
            elif op == 'rsb':
                self.w('      uint32_t _a = _b; _b = %s; uint32_t _r = _a - _b;' % a)
                if s: self.w('      fc = _a >= _b; fv = (((_a ^ _b) & (_a ^ _r)) >> 31);')
            elif op == 'adc':
                self.w('      uint32_t _a = %s; uint64_t _w = (uint64_t)_a + _b + fc; uint32_t _r = (uint32_t)_w;' % a)
                if s: self.w('      fc = (uint32_t)(_w >> 32); fv = ((~(_a ^ _b) & (_a ^ _r)) >> 31);')
            elif op == 'sbc':
                self.w('      uint32_t _a = %s; uint64_t _w = (uint64_t)_a + (uint32_t)~_b + fc; uint32_t _r = (uint32_t)_w;' % a)
                if s: self.w('      fc = (uint32_t)(_w >> 32); fv = (((_a ^ _b) & (_a ^ _r)) >> 31);')
            elif op == 'rsc':
                self.w('      uint32_t _a = _b; _b = %s; uint64_t _w = (uint64_t)_a + (uint32_t)~_b + fc; uint32_t _r = (uint32_t)_w;' % a)
                if s: self.w('      fc = (uint32_t)(_w >> 32); fv = (((_a ^ _b) & (_a ^ _r)) >> 31);')
            if s and rd != 15:
                self.w('      fn = _r >> 31; fz = _r == 0;')
        if op not in NOWRITE:
            if rd == 15:
                if s:
                    # exception return: CPSR = SPSR
                    self.w('      NR_STORE(); nr_exception_return(_r); return;')
                else:
                    self.emit_dyn_jump(i, '_r', interwork=False)
            else:
                self.w('      r%d = _r;' % rd)
        self.w('    }')

    def addr_expr(self, i):
        f = i.f
        base = self.reg(i, f['rn'])
        if f['rn'] == 15 and i.thumb:
            base = h32((i.addr + 4) & ~3)
        off = f['off']
        if off['kind'] == 'imm':
            o = h32(off['value'])
        elif off['kind'] == 'reg':
            o = self.reg(i, off['rm'])
        else:
            o, _ = self.shifter(i, dict(kind='regimm', rm=off['rm'], sh=off['sh'], amt=off['amt']), False)
        sign = '+' if f['u'] else '-'
        offset_addr = '(%s %s %s)' % (base, sign, o)
        if f['p']:
            return offset_addr, offset_addr if f['wb'] else None
        # post-indexed: access at base, then base = offset_addr
        return base, offset_addr

    def wb(self, i, new):
        if new is not None and i.f['rn'] != 15:
            self.w('      r%d = %s;' % (i.f['rn'], new))

    def op_ldst(self, i):
        f = i.f
        load = i.op.startswith('ldr')
        byte = i.op.endswith('b')
        acc, new = self.addr_expr(i)
        if load and f['rd'] == 15 and f.get('static_target'):
            self.emit_dyn_jump(i, None, interwork=True)
            return
        self.w('    { uint32_t _ea = %s;' % acc)
        if f['wb'] or not f['p']:
            self.w('      uint32_t _nb = %s;' % (new if new else '_ea'))
        if load:
            if byte:
                self.w('      uint32_t _v = NR_RD8(_ea);')
            else:
                self.w('      uint32_t _v = NR_LDR32(_ea);')
            if f['wb'] or not f['p']:
                self.wb(i, '_nb')
            if f['rd'] == 15:
                self.emit_dyn_jump(i, '_v', interwork=True)
                self.w('    }')
                return
            self.w('      r%d = _v;' % f['rd'])
        else:
            v = self.reg(i, f['rd'])
            if f['rd'] == 15:
                v = h32(i.addr + 12) if not i.thumb else h32(i.addr + 4)
            if byte:
                self.w('      NR_WR8(_ea, (uint8_t)%s);' % v)
            else:
                self.w('      NR_WR32(_ea & ~3u, %s);' % v)
            if f['wb'] or not f['p']:
                self.wb(i, '_nb')
        self.w('    }')

    def op_ldsth(self, i):
        f = i.f
        acc, new = self.addr_expr(i)
        self.w('    { uint32_t _ea = %s;' % acc)
        if f['wb'] or not f['p']:
            self.w('      uint32_t _nb = %s;' % (new if new else '_ea'))
        op = i.op
        rd = f['rd']
        if op == 'ldrh':
            self.w('      uint32_t _v = NR_RD16(_ea & ~1u);')
        elif op == 'ldrsh':
            self.w('      uint32_t _v = (uint32_t)(int32_t)(int16_t)NR_RD16(_ea & ~1u);')
        elif op == 'ldrsb':
            self.w('      uint32_t _v = (uint32_t)(int32_t)(int8_t)NR_RD8(_ea);')
        elif op == 'strh':
            self.w('      NR_WR16(_ea & ~1u, (uint16_t)%s);' % self.reg(i, rd))
        elif op == 'ldrd':
            self.w('      uint32_t _v = NR_RD32(_ea & ~3u); uint32_t _v2 = NR_RD32((_ea + 4) & ~3u);')
        elif op == 'strd':
            self.w('      NR_WR32(_ea & ~3u, %s); NR_WR32((_ea + 4) & ~3u, %s);'
                   % (self.reg(i, rd), self.reg(i, (rd + 1) & 15)))
        if f['wb'] or not f['p']:
            self.wb(i, '_nb')
        if op in ('ldrh', 'ldrsh', 'ldrsb'):
            self.w('      r%d = _v;' % rd)
        elif op == 'ldrd':
            self.w('      r%d = _v; r%d = _v2;' % (rd, (rd + 1) & 15))
        self.w('    }')

    def op_ldm(self, i):
        f = i.f
        regs = [r for r in range(16) if f['regs'] >> r & 1]
        n = len(regs)
        if f['s'] or n == 0:
            self.w('    NR_STORE(); g_cpu.R[15] = %s; nr_fallback(); return;' % h32(i.addr))
            return
        rn = f['rn']
        self.w('    { uint32_t _b = r%d;' % rn)
        start = {(0, 1): '_b', (1, 1): '_b + 4', (0, 0): '_b - %d' % (4 * n - 4),
                 (1, 0): '_b - %d' % (4 * n)}[(f['p'], f['u'])]
        self.w('      uint32_t _ea = (%s) & ~3u;' % start)
        for k, r in enumerate(regs):
            if r == 15:
                self.w('      uint32_t _pc = NR_RD32(_ea + %d);' % (4 * k))
            else:
                self.w('      uint32_t _l%d = NR_RD32(_ea + %d);' % (r, 4 * k))
        if f['wb'] and not (f['regs'] >> rn & 1):
            self.w('      r%d = _b %s %d;' % (rn, '+' if f['u'] else '-', 4 * n))
        for r in regs:
            if r != 15:
                self.w('      r%d = _l%d;' % (r, r))
        if 15 in regs:
            self.emit_dyn_jump(i, '_pc', interwork=True)
        self.w('    }')

    def op_stm(self, i):
        f = i.f
        regs = [r for r in range(16) if f['regs'] >> r & 1]
        n = len(regs)
        if f['s'] or n == 0:
            self.w('    NR_STORE(); g_cpu.R[15] = %s; nr_fallback(); return;' % h32(i.addr))
            return
        rn = f['rn']
        self.w('    { uint32_t _b = r%d;' % rn)
        start = {(0, 1): '_b', (1, 1): '_b + 4', (0, 0): '_b - %d' % (4 * n - 4),
                 (1, 0): '_b - %d' % (4 * n)}[(f['p'], f['u'])]
        self.w('      uint32_t _ea = (%s) & ~3u;' % start)
        for k, r in enumerate(regs):
            v = 'r%d' % r if r != 15 else h32(i.addr + (4 if i.thumb else 8))
            self.w('      NR_WR32(_ea + %d, %s);' % (4 * k, v))
        if f['wb']:
            self.w('      r%d = _b %s %d;' % (rn, '+' if f['u'] else '-', 4 * n))
        self.w('    }')

    def op_mul(self, i):
        f = i.f
        self.w('    { uint32_t _r = %s * %s;' % (self.reg(i, f['rm']), self.reg(i, f['rs'])))
        if i.op == 'mla':
            self.w('      _r += %s;' % self.reg(i, f['rn']))
        if f['s']:
            self.w('      fn = _r >> 31; fz = _r == 0;')
        self.w('      r%d = _r; }' % f['rd'])

    op_mla = op_mul

    def _long_mul(self, i, signed, acc):
        f = i.f
        lo, hi = f['rn'], f['rd']
        cast = '(int64_t)(int32_t)' if signed else '(uint64_t)'
        self.w('    { uint64_t _r = (uint64_t)(%s%s * %s%s);' %
               (cast, self.reg(i, f['rm']), cast, self.reg(i, f['rs'])))
        if acc:
            self.w('      _r += ((uint64_t)r%d << 32) | r%d;' % (hi, lo))
        if f['s']:
            self.w('      fn = (uint32_t)(_r >> 63); fz = _r == 0;')
        self.w('      r%d = (uint32_t)_r; r%d = (uint32_t)(_r >> 32); }' % (lo, hi))

    def op_umull(self, i): self._long_mul(i, False, False)
    def op_umlal(self, i): self._long_mul(i, False, True)
    def op_smull(self, i): self._long_mul(i, True, False)
    def op_smlal(self, i): self._long_mul(i, True, True)

    def _half(self, i, r, top):
        v = self.reg(i, r)
        return '((int32_t)(int16_t)(%s >> 16))' % v if top else '((int32_t)(int16_t)%s)' % v

    def op_smulxy(self, i):
        f = i.f
        self.w('    r%d = (uint32_t)(%s * %s);' % (f['rd'], self._half(i, f['rm'], f['x']), self._half(i, f['rs'], f['y'])))

    def op_smlaxy(self, i):
        f = i.f
        self.w('    { int64_t _p = (int64_t)%s * %s; int64_t _s = _p + (int64_t)(int32_t)%s;'
               % (self._half(i, f['rm'], f['x']), self._half(i, f['rs'], f['y']), self.reg(i, f['rn'])))
        self.w('      if (_s != (int32_t)_s) g_nr_q = 1; r%d = (uint32_t)_s; }' % f['rd'])

    def op_smulwy(self, i):
        f = i.f
        self.w('    r%d = (uint32_t)(((int64_t)(int32_t)%s * %s) >> 16);' % (f['rd'], self.reg(i, f['rm']), self._half(i, f['rs'], f['y'])))

    def op_smlawy(self, i):
        f = i.f
        self.w('    { int64_t _s = (((int64_t)(int32_t)%s * %s) >> 16) + (int64_t)(int32_t)%s;'
               % (self.reg(i, f['rm']), self._half(i, f['rs'], f['y']), self.reg(i, f['rn'])))
        self.w('      if (_s != (int32_t)_s) g_nr_q = 1; r%d = (uint32_t)_s; }' % f['rd'])

    def op_smlalxy(self, i):
        f = i.f
        lo, hi = f['rn'], f['rd']
        self.w('    { int64_t _a = (int64_t)(((uint64_t)r%d << 32) | r%d); _a += (int64_t)%s * %s;'
               % (hi, lo, self._half(i, f['rm'], f['x']), self._half(i, f['rs'], f['y'])))
        self.w('      r%d = (uint32_t)_a; r%d = (uint32_t)((uint64_t)_a >> 32); }' % (lo, hi))

    def _q(self, i, expr):
        self.w('    r%d = %s;' % (i.f['rd'], expr))

    def op_qadd(self, i): self._q(i, 'nr_qadd(%s, %s)' % (self.reg(i, i.f['rm']), self.reg(i, i.f['rn'])))
    def op_qsub(self, i): self._q(i, 'nr_qsub(%s, %s)' % (self.reg(i, i.f['rm']), self.reg(i, i.f['rn'])))
    def op_qdadd(self, i): self._q(i, 'nr_qadd(%s, nr_qadd(%s, %s))' % (self.reg(i, i.f['rm']), self.reg(i, i.f['rn']), self.reg(i, i.f['rn'])))
    def op_qdsub(self, i): self._q(i, 'nr_qsub(%s, nr_qadd(%s, %s))' % (self.reg(i, i.f['rm']), self.reg(i, i.f['rn']), self.reg(i, i.f['rn'])))

    def op_clz(self, i):
        v = self.reg(i, i.f['rm'])
        self.w('    r%d = %s ? (uint32_t)__builtin_clz(%s) : 32u;' % (i.f['rd'], v, v))

    def op_swp(self, i):
        f = i.f
        rn = self.reg(i, f['rn'])
        if f['b']:
            self.w('    { uint32_t _a = %s; uint32_t _v = NR_RD8(_a); NR_WR8(_a, (uint8_t)%s); r%d = _v; }'
                   % (rn, self.reg(i, f['rm']), f['rd']))
        else:
            self.w('    { uint32_t _a = %s; uint32_t _v = NR_LDR32(_a); NR_WR32(_a & ~3u, %s); r%d = _v; }'
                   % (rn, self.reg(i, f['rm']), f['rd']))

    def op_adr(self, i):
        self.w('    r%d = %s;' % (i.f['rd'], h32(i.f['value'])))

    def op_mrs(self, i):
        if i.f['spsr']:
            self.w('    NR_STORE(); r%d = nr_get_spsr();' % i.f['rd'])
        else:
            self.w('    r%d = NR_CPSR();' % i.f['rd'])

    def op_msr(self, i):
        f = i.f
        src = f['src']
        v = h32(src['value']) if src['kind'] == 'imm' else self.reg(i, src['rm'])
        mask = f['mask']
        if f['spsr']:
            self.w('    nr_set_spsr(%s, %d);' % (v, mask))
            return
        if mask & 1:
            # control field: mode / interrupt bits can change; registers are banked
            self.w('    { uint32_t _v = %s; NR_STORE(); nr_msr_cpsr(_v, %d); NR_LOAD(); }' % (v, mask))
        elif mask & 8:
            self.w('    { uint32_t _v = %s; fn = _v >> 31; fz = (_v >> 30) & 1; fc = (_v >> 29) & 1; fv = (_v >> 28) & 1;'
                   ' if (_v & (1u << 27)) g_nr_q = 1; }' % v)

    def op_swi(self, i):
        num = i.f['imm'] if i.thumb else (i.f['imm'] >> 16) & 0xFF
        self.w('    NR_STORE(); g_cpu.R[15] = %s; nr_swi(%d); if (nr_redirect) return; NR_LOAD();'
               % (h32(i.addr + i.size), num))

    def op_mcr(self, i):
        f = i.f
        if f['cp'] != 15:
            return
        self.w('    NR_STORE(); g_cpu.R[15] = %s; nr_cp15_write(%d, %d, %d, %d, %s); if (nr_redirect) return; NR_LOAD();'
               % (h32(i.addr + i.size), f['opc1'], f['crn'], f['crm'], f['opc2'], self.reg(i, f['rd'])))

    def op_mrc(self, i):
        f = i.f
        if f['cp'] != 15:
            return
        if f['rd'] == 15:
            return
        self.w('    r%d = nr_cp15_read(%d, %d, %d, %d);' % (f['rd'], f['opc1'], f['crn'], f['crm'], f['opc2']))

    def op_pld(self, i): pass
    def op_cdp(self, i): pass
    def op_coproc_ldst(self, i): pass

    def op_bkpt(self, i):
        self.op_undef(i)

    def op_undef(self, i):
        self.w('    NR_STORE(); g_cpu.R[15] = %s; nr_fallback(); return;' % h32(i.addr))

    def op_b(self, i):
        tgt = i.target
        if tgt in self.f.insns:
            if tgt <= i.addr:
                self.w('    ' + self.check(tgt))
            self.w('    goto L_%08X;' % tgt)
            return
        # tail transfer to another function (LR unchanged)
        kind, cname = self.resolve(tgt, self.f.thumb)
        self.w('    NR_STORE();')
        if kind == 'direct':
            self.w('    %s(0);' % cname)
        else:
            self.w('    nr_call_dyn(%s);' % h32(tgt | (1 if self.f.thumb else 0)))
        self.w('    if (!nr_redirect && g_cpu.R[15] != _elr) nr_redirect = 1;')
        self.w('    NR_RET(); return;')

    def op_bl(self, i):
        self.emit_call(i, i.target, i.thumb, i.addr + i.size)

    def op_blx_imm(self, i):
        tthumb = not i.thumb
        self.emit_call(i, i.target & ~1, tthumb, i.addr + i.size)

    def op_bx(self, i):
        self.emit_dyn_jump(i, self.reg(i, i.f['rm']), interwork=True, is_call=i.call)

    def op_blx_reg(self, i):
        self.emit_dyn_jump(i, self.reg(i, i.f['rm']), interwork=True, is_call=True)

    # -- whole function ----------------------------------------------------
    def generate(self):
        f = self.f
        addrs = sorted(a for a in f.insns if f.insns[a] is not None)
        self.w('void %s(uint32_t entry) {' % self.name)
        self.w('    const uint32_t _thumb = %d; (void)_thumb;' % (1 if f.thumb else 0))
        self.w('    NR_ENTER();')
        self.w('    NR_LOCALS();')
        self.w('    uint32_t _sv, _sc; (void)_sv; (void)_sc;')
        self.w('    const uint32_t _elr = entry ? 0xFFFFFFFFu : r14;')
        self.w('    uint32_t _dj = entry; int _djk = 0;')
        self.w('    if (!entry) goto L_%08X;' % f.entry)
        self.w('L_DYN:')
        self.w('    switch (_dj & ~1u) {')
        targets = set(self.labels)
        for tab in f.tables.values():
            targets |= {t & ~1 for t in tab}
        for a in sorted(targets):
            if a in f.insns and f.insns[a] is not None:
                self.w('    case %s: goto L_%08X;' % (h32(a), a))
        self.w('    default: break; }')
        self.w('    if (!_djk) { NR_STORE(); g_cpu.R[15] = entry; nr_redirect = 2; return; }')
        self.w('    NR_STORE(); nr_call_dyn(_dj);')
        self.w('    if (!nr_redirect && g_cpu.R[15] != _elr) nr_redirect = 1;')
        self.w('    return;')
        cyc = 0
        for idx, a in enumerate(addrs):
            i = f.insns[a]
            if a in self.labels:
                if cyc:
                    self.w('    ' + self.tick(cyc))
                    cyc = 0
            self.w('L_%08X:' % a)
            self.w('    NR_TRACE(%s);' % h32(a))
            cost = 1
            if i.op in ('ldm', 'stm'):
                cost += bin(i.f['regs']).count('1')
            elif i.op in ('ldr', 'str', 'ldrb', 'strb', 'ldrh', 'strh', 'ldrsb', 'ldrsh', 'swp'):
                cost += 1
            cyc += cost
            if i.ends_block or i.call or i.op in ('swi', 'mcr', 'msr'):
                self.w('    ' + self.tick(cyc))
                cyc = 0
            self.emit_insn(i, False)
            nxt = a + i.size
            if i.ends_block and i.cond == 14:
                # unconditional control transfer: nothing falls through
                if nxt in f.insns and nxt in self.labels:
                    pass
                continue
            if nxt not in f.insns:
                # falls off the decoded code (into data or another function)
                if cyc:
                    self.w('    ' + self.tick(cyc))
                    cyc = 0
                self.w('    NR_STORE(); g_cpu.R[15] = %s; nr_redirect = 1; return;' %
                       h32(nxt | (1 if f.thumb else 0)))
            elif idx + 1 < len(addrs) and addrs[idx + 1] != nxt:
                self.w('    goto L_%08X;' % nxt)
        self.w('}')
        return '\n'.join(self.out)
