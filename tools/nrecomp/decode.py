"""MPH-nx ARMv5TE decoder (ARM and Thumb).

Thumb instructions are decoded into the same Insn form as their ARM equivalents,
so the code generator has one instruction set to translate. Each Insn carries what
the analysis needs (branch targets, calls, returns, PC writes) and what the code
generator needs (operation and operands).
"""

# 'v5te' (ARM9) or 'v4t' (ARM7). ARMv4T has no BLX, CLZ, saturating or DSP
# multiplies, LDRD/STRD or PLD, and its LDR PC / LDM {pc} / POP {pc} do not
# switch instruction set (only BX does).
ARCH = 'v5te'
V5_ONLY = {'blx_imm', 'blx_reg', 'clz', 'qadd', 'qsub', 'qdadd', 'qdsub', 'pld', 'bkpt',
           'smlaxy', 'smlawy', 'smulwy', 'smlalxy', 'smulxy', 'ldrd', 'strd'}

DP_NAMES = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc',
            'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']
SH_NAMES = ['lsl', 'lsr', 'asr', 'ror']


class Insn:
    __slots__ = ('addr', 'size', 'thumb', 'raw', 'cond', 'op', 'f', 'target',
                 'call', 'exchange', 'writes_pc', 'ends_block', 'is_return', 'pcval')

    def __init__(self, addr, size, thumb, raw, op, cond=14, **f):
        self.addr, self.size, self.thumb, self.raw = addr, size, thumb, raw
        self.cond, self.op, self.f = cond, op, f
        self.target = None      # static branch/call target (with mode in bit 0 for exchange)
        self.call = False       # BL / BLX: returns to addr+size
        self.exchange = False   # target is in the other instruction set
        self.writes_pc = False  # any PC write (static or dynamic)
        self.ends_block = False
        self.is_return = False  # BX LR, POP {..pc}, LDM ...{pc}, MOV PC, LR
        self.pcval = addr + (4 if thumb else 8)

    def __repr__(self):
        return '<%08x %s%s %s %s>' % (self.addr, 'T ' if self.thumb else '', self.op,
                                      self.cond, self.f)


def _v4t_filter(i):
    if ARCH != 'v4t' or i is None:
        return i
    if i.op in V5_ONLY:
        return Insn(i.addr, i.size, i.thumb, i.raw, 'undef', i.cond)
    if i.op in ('mcr', 'mrc') and i.f.get('cp') == 15:
        return Insn(i.addr, i.size, i.thumb, i.raw, 'undef', i.cond)
    if i.op in ('ldr', 'ldm') and i.writes_pc:
        i.exchange = False
    return i


def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def sx(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def op2_imm(rot, imm):
    return dict(kind='imm', value=ror32(imm, rot * 2), rot=rot * 2)


def _mark_pc_write(i, rd_is_pc):
    if rd_is_pc:
        i.writes_pc = True
        i.ends_block = True


# ----------------------------------------------------------------------------
# ARM
# ----------------------------------------------------------------------------

def decode_arm(w, addr):
    return _v4t_filter(_decode_arm(w, addr))


def _decode_arm(w, addr):
    cond = w >> 28
    if cond == 15:
        return _decode_arm_uncond(w, addr)
    b27_25 = (w >> 25) & 7

    # Branch / branch with link
    if b27_25 == 5:
        off = sx(w & 0xFFFFFF, 24) << 2
        i = Insn(addr, 4, False, w, 'bl' if (w >> 24) & 1 else 'b', cond)
        i.target = (addr + 8 + off) & 0xFFFFFFFF
        i.call = bool((w >> 24) & 1)
        i.writes_pc = True
        i.ends_block = True
        return i

    if b27_25 in (0, 1):
        return _decode_arm_dp_misc(w, addr, cond)
    if b27_25 in (2, 3):
        if b27_25 == 3 and (w >> 4) & 1:
            return Insn(addr, 4, False, w, 'undef', cond)
        return _decode_arm_ldst(w, addr, cond)
    if b27_25 == 4:
        rn = (w >> 16) & 15
        regs = w & 0xFFFF
        load = bool((w >> 20) & 1)
        i = Insn(addr, 4, False, w, 'ldm' if load else 'stm', cond,
                 rn=rn, regs=regs, p=(w >> 24) & 1, u=(w >> 23) & 1,
                 s=(w >> 22) & 1, wb=(w >> 21) & 1)
        if load and regs & 0x8000:
            i.writes_pc = True
            i.ends_block = True
            if rn == 13 and not ((w >> 22) & 1):
                i.is_return = True
        return i
    if b27_25 == 6:
        return Insn(addr, 4, False, w, 'coproc_ldst', cond)
    # b27_25 == 7
    if (w >> 24) & 1:
        i = Insn(addr, 4, False, w, 'swi', cond, imm=w & 0xFFFFFF)
        return i
    if (w >> 4) & 1:
        i = Insn(addr, 4, False, w, 'mrc' if (w >> 20) & 1 else 'mcr', cond,
                 opc1=(w >> 21) & 7, crn=(w >> 16) & 15, rd=(w >> 12) & 15,
                 cp=(w >> 8) & 15, opc2=(w >> 5) & 7, crm=w & 15)
        if i.op == 'mrc' and i.f['rd'] == 15:
            pass  # MRC to PC sets flags only
        return i
    return Insn(addr, 4, False, w, 'cdp', cond)


def _decode_arm_uncond(w, addr):
    # BLX immediate
    if (w >> 25) & 7 == 5:
        h = (w >> 24) & 1
        off = (sx(w & 0xFFFFFF, 24) << 2) | (h << 1)
        i = Insn(addr, 4, False, w, 'blx_imm', 14)
        i.target = ((addr + 8 + off) & 0xFFFFFFFF) | 1
        i.call = True
        i.exchange = True
        i.writes_pc = True
        i.ends_block = True
        return i
    if (w & 0x0D70F000) == 0x0550F000:
        return Insn(addr, 4, False, w, 'pld', 14)
    return Insn(addr, 4, False, w, 'undef', 15)


def _shift_operand(w):
    rm = w & 15
    if (w >> 4) & 1:
        return dict(kind='regreg', rm=rm, sh=(w >> 5) & 3, rs=(w >> 8) & 15)
    return dict(kind='regimm', rm=rm, sh=(w >> 5) & 3, amt=(w >> 7) & 31)


def _decode_arm_dp_misc(w, addr, cond):
    imm = (w >> 25) & 1
    opc = (w >> 21) & 15
    s = (w >> 20) & 1
    rn = (w >> 16) & 15
    rd = (w >> 12) & 15

    if not imm:
        # Multiplies and extra load/store space: bits 7 and 4 set
        if (w >> 4) & 1 and (w >> 7) & 1:
            sh = (w >> 5) & 3
            if sh == 0:
                if (w >> 24) & 1:
                    # SWP / SWPB
                    if (w & 0x0FB00FF0) == 0x01000090:
                        return Insn(addr, 4, False, w, 'swp', cond, b=(w >> 22) & 1,
                                    rn=rn, rd=rd, rm=w & 15)
                    return Insn(addr, 4, False, w, 'undef', cond)
                # MUL/MLA/UMULL/UMLAL/SMULL/SMLAL
                kind = (w >> 21) & 7
                names = {0: 'mul', 1: 'mla', 4: 'umull', 5: 'umlal', 6: 'smull', 7: 'smlal'}
                if kind not in names:
                    return Insn(addr, 4, False, w, 'undef', cond)
                return Insn(addr, 4, False, w, names[kind], cond, s=s,
                            rd=(w >> 16) & 15, rn=(w >> 12) & 15, rs=(w >> 8) & 15, rm=w & 15)
            # LDRH/STRH/LDRSB/LDRSH/LDRD/STRD
            p = (w >> 24) & 1
            u = (w >> 23) & 1
            i_ = (w >> 22) & 1
            wb = (w >> 21) & 1
            load = (w >> 20) & 1
            if i_:
                off = dict(kind='imm', value=((w >> 4) & 0xF0) | (w & 15))
            else:
                off = dict(kind='reg', rm=w & 15)
            if load:
                op = {1: 'ldrh', 2: 'ldrsb', 3: 'ldrsh'}[sh]
            else:
                op = {1: 'strh', 2: 'ldrd', 3: 'strd'}[sh]
            i = Insn(addr, 4, False, w, op, cond, rn=rn, rd=rd, p=p, u=u, wb=wb, off=off)
            if load and rd == 15:
                _mark_pc_write(i, True)
            return i

        # Miscellaneous instructions: opcode 10xx with S=0
        if (opc >> 2) == 2 and not s:
            bits7_4 = (w >> 4) & 15
            if bits7_4 == 0:
                if (w >> 21) & 1:
                    return Insn(addr, 4, False, w, 'msr', cond, spsr=(w >> 22) & 1,
                                mask=(w >> 16) & 15, src=dict(kind='reg', rm=w & 15))
                return Insn(addr, 4, False, w, 'mrs', cond, spsr=(w >> 22) & 1, rd=rd)
            if bits7_4 == 1 and opc == 9:
                i = Insn(addr, 4, False, w, 'bx', cond, rm=w & 15)
                i.writes_pc = i.ends_block = i.exchange = True
                if (w & 15) == 14:
                    i.is_return = True
                return i
            if bits7_4 == 1 and opc == 11:
                return Insn(addr, 4, False, w, 'clz', cond, rd=rd, rm=w & 15)
            if bits7_4 == 3 and opc == 9:
                i = Insn(addr, 4, False, w, 'blx_reg', cond, rm=w & 15)
                i.writes_pc = i.ends_block = i.exchange = True
                i.call = True
                return i
            if bits7_4 == 5:
                names = {8: 'qadd', 9: 'qsub', 10: 'qdadd', 11: 'qdsub'}
                return Insn(addr, 4, False, w, names[opc], cond, rd=rd, rn=rn, rm=w & 15)
            if bits7_4 == 7 and opc == 9:
                return Insn(addr, 4, False, w, 'bkpt', 14)
            if (bits7_4 & 9) == 8:
                x = (w >> 5) & 1
                y = (w >> 6) & 1
                names = {8: 'smlaxy', 9: 'smlawy' if not x else 'smulwy',
                         10: 'smlalxy', 11: 'smulxy'}
                op = names[opc]
                if opc == 9:
                    op = 'smulwy' if x else 'smlawy'
                return Insn(addr, 4, False, w, op, cond, rd=(w >> 16) & 15,
                            rn=(w >> 12) & 15, rs=(w >> 8) & 15, rm=w & 15, x=x, y=y)
            return Insn(addr, 4, False, w, 'undef', cond)
        op2 = _shift_operand(w)
    else:
        if (opc >> 2) == 2 and not s:
            if (w >> 21) & 1:
                return Insn(addr, 4, False, w, 'msr', cond, spsr=(w >> 22) & 1,
                            mask=(w >> 16) & 15, src=op2_imm((w >> 8) & 15, w & 0xFF))
            return Insn(addr, 4, False, w, 'undef', cond)
        op2 = op2_imm((w >> 8) & 15, w & 0xFF)

    i = Insn(addr, 4, False, w, DP_NAMES[opc], cond, s=s, rn=rn, rd=rd, op2=op2)
    if opc < 8 or opc >= 12:
        if rd == 15:
            _mark_pc_write(i, True)
            if opc == 13 and op2['kind'] == 'regimm' and op2['rm'] == 14 and op2['amt'] == 0 \
                    and op2['sh'] == 0 and not s:
                i.is_return = True
    return i


def _decode_arm_ldst(w, addr, cond):
    reg = (w >> 25) & 1
    p = (w >> 24) & 1
    u = (w >> 23) & 1
    b = (w >> 22) & 1
    wb = (w >> 21) & 1
    load = (w >> 20) & 1
    rn = (w >> 16) & 15
    rd = (w >> 12) & 15
    if reg:
        off = dict(kind='regimm', rm=w & 15, sh=(w >> 5) & 3, amt=(w >> 7) & 31)
    else:
        off = dict(kind='imm', value=w & 0xFFF)
    op = ('ldrb' if b else 'ldr') if load else ('strb' if b else 'str')
    t = (not p) and wb   # LDRT/STRT (user mode access); treated as normal on the DS
    i = Insn(addr, 4, False, w, op, cond, rn=rn, rd=rd, p=p, u=u, wb=wb and p, off=off, t=t)
    if load and rd == 15:
        _mark_pc_write(i, True)
        i.exchange = True   # ARMv5: LDR PC interworks
        if rn == 13 and not p and u and off['kind'] == 'imm' and off['value'] == 4:
            i.is_return = True
    return i


# ----------------------------------------------------------------------------
# Thumb (decoded into ARM-equivalent Insn objects)
# ----------------------------------------------------------------------------

def _dp(addr, raw, name, s, rd, rn, op2, cond=14):
    return Insn(addr, 2, True, raw, name, cond, s=s, rn=rn, rd=rd, op2=op2)


def decode_thumb(h, addr, h2=None):
    return _v4t_filter(_decode_thumb(h, addr, h2))


def _decode_thumb(h, addr, h2=None):
    """h: halfword at addr; h2: the following halfword (for BL pairs)."""
    top = h >> 13
    if top == 0:
        op = (h >> 11) & 3
        if op < 3:
            amt = (h >> 6) & 31
            return _dp(addr, h, 'mov', 1, h & 7, 0,
                       dict(kind='regimm', rm=(h >> 3) & 7, sh=op, amt=amt))
        # add/sub
        imm = (h >> 10) & 1
        sub = (h >> 9) & 1
        rn = (h >> 3) & 7
        rd = h & 7
        v = (h >> 6) & 7
        op2 = dict(kind='imm', value=v, rot=0) if imm else dict(kind='regimm', rm=v, sh=0, amt=0)
        return _dp(addr, h, 'sub' if sub else 'add', 1, rd, rn, op2)
    if top == 1:
        op = (h >> 11) & 3
        rd = (h >> 8) & 7
        op2 = dict(kind='imm', value=h & 0xFF, rot=0)
        name = ['mov', 'cmp', 'add', 'sub'][op]
        return _dp(addr, h, name, 1, rd, rd, op2)
    if (h >> 10) == 0x10:
        op = (h >> 6) & 15
        rs = (h >> 3) & 7
        rd = h & 7
        regop = dict(kind='regimm', rm=rs, sh=0, amt=0)
        if op in (2, 3, 4, 7):   # LSL, LSR, ASR, ROR by register
            sh = {2: 0, 3: 1, 4: 2, 7: 3}[op]
            return _dp(addr, h, 'mov', 1, rd, 0, dict(kind='regreg', rm=rd, sh=sh, rs=rs))
        if op == 9:   # NEG
            return _dp(addr, h, 'rsb', 1, rd, rs, dict(kind='imm', value=0, rot=0))
        if op == 13:  # MUL
            return Insn(addr, 2, True, h, 'mul', 14, s=1, rd=rd, rn=0, rs=rd, rm=rs)
        names = {0: 'and', 1: 'eor', 5: 'adc', 6: 'sbc', 8: 'tst', 10: 'cmp', 11: 'cmn',
                 12: 'orr', 14: 'bic', 15: 'mvn'}
        return _dp(addr, h, names[op], 1, rd, rd, regop)
    if (h >> 10) == 0x11:
        op = (h >> 8) & 3
        rs = ((h >> 3) & 15)
        rd = (h & 7) | ((h >> 4) & 8)
        if op == 3:
            if (h >> 7) & 1:
                i = Insn(addr, 2, True, h, 'blx_reg', 14, rm=rs)
                i.call = True
            else:
                i = Insn(addr, 2, True, h, 'bx', 14, rm=rs)
                if rs == 14:
                    i.is_return = True
            i.writes_pc = i.ends_block = i.exchange = True
            return i
        op2 = dict(kind='regimm', rm=rs, sh=0, amt=0)
        name = ['add', 'cmp', 'mov'][op]
        i = _dp(addr, h, name, 1 if op == 1 else 0, rd, rd, op2)
        if op != 1 and rd == 15:
            _mark_pc_write(i, True)
            if op == 2 and rs == 14:
                i.is_return = True
        return i
    if (h >> 11) == 9:
        rd = (h >> 8) & 7
        return Insn(addr, 2, True, h, 'ldr', 14, rn=15, rd=rd, p=1, u=1, wb=0,
                    off=dict(kind='imm', value=(h & 0xFF) << 2), t=0)
    if (h >> 12) == 5:
        op = (h >> 9) & 7
        rm = (h >> 6) & 7
        rn = (h >> 3) & 7
        rd = h & 7
        name = ['str', 'strh', 'strb', 'ldrsb', 'ldr', 'ldrh', 'ldrb', 'ldrsh'][op]
        off = dict(kind='regimm', rm=rm, sh=0, amt=0) if name in ('str', 'strb', 'ldr', 'ldrb') \
            else dict(kind='reg', rm=rm)
        return Insn(addr, 2, True, h, name, 14, rn=rn, rd=rd, p=1, u=1, wb=0, off=off, t=0)
    if top == 3:
        b = (h >> 12) & 1
        load = (h >> 11) & 1
        imm = (h >> 6) & 31
        name = ('ldrb' if b else 'ldr') if load else ('strb' if b else 'str')
        return Insn(addr, 2, True, h, name, 14, rn=(h >> 3) & 7, rd=h & 7, p=1, u=1, wb=0,
                    off=dict(kind='imm', value=imm if b else imm << 2), t=0)
    if (h >> 12) == 8:
        load = (h >> 11) & 1
        return Insn(addr, 2, True, h, 'ldrh' if load else 'strh', 14, rn=(h >> 3) & 7,
                    rd=h & 7, p=1, u=1, wb=0, off=dict(kind='imm', value=((h >> 6) & 31) << 1))
    if (h >> 12) == 9:
        load = (h >> 11) & 1
        return Insn(addr, 2, True, h, 'ldr' if load else 'str', 14, rn=13, rd=(h >> 8) & 7,
                    p=1, u=1, wb=0, off=dict(kind='imm', value=(h & 0xFF) << 2), t=0)
    if (h >> 12) == 10:
        sp = (h >> 11) & 1
        rd = (h >> 8) & 7
        v = (h & 0xFF) << 2
        if sp:
            return _dp(addr, h, 'add', 0, rd, 13, dict(kind='imm', value=v, rot=0))
        # ADD Rd, PC, #imm uses (PC+4) & ~3
        return Insn(addr, 2, True, h, 'adr', 14, rd=rd, value=((addr + 4) & ~3) + v)
    if (h >> 8) == 0xB0:
        v = (h & 0x7F) << 2
        return _dp(addr, h, 'sub' if (h >> 7) & 1 else 'add', 0, 13, 13,
                   dict(kind='imm', value=v, rot=0))
    if (h >> 12) == 11 and ((h >> 9) & 3) == 2:
        load = (h >> 11) & 1
        regs = h & 0xFF
        r = (h >> 8) & 1
        if load:
            regs |= (1 << 15) if r else 0
            i = Insn(addr, 2, True, h, 'ldm', 14, rn=13, regs=regs, p=0, u=1, s=0, wb=1)
            if r:
                i.writes_pc = i.ends_block = True
                i.exchange = True   # ARMv5 POP {pc} interworks
                i.is_return = True
            return i
        regs |= (1 << 14) if r else 0
        return Insn(addr, 2, True, h, 'stm', 14, rn=13, regs=regs, p=1, u=0, s=0, wb=1)
    if (h >> 8) == 0xBE:
        return Insn(addr, 2, True, h, 'bkpt', 14)
    if (h >> 12) == 12:
        load = (h >> 11) & 1
        rn = (h >> 8) & 7
        regs = h & 0xFF
        # LDMIA writes back unless Rn is in the list
        wb = 0 if (load and regs & (1 << rn)) else 1
        return Insn(addr, 2, True, h, 'ldm' if load else 'stm', 14, rn=rn, regs=regs,
                    p=0, u=1, s=0, wb=wb)
    if (h >> 12) == 13:
        cond = (h >> 8) & 15
        if cond == 15:
            return Insn(addr, 2, True, h, 'swi', 14, imm=h & 0xFF)
        if cond == 14:
            return Insn(addr, 2, True, h, 'undef', 14)
        i = Insn(addr, 2, True, h, 'b', cond)
        i.target = (addr + 4 + (sx(h & 0xFF, 8) << 1)) & 0xFFFFFFFF
        i.writes_pc = i.ends_block = True
        return i
    if (h >> 11) == 0x1C:
        i = Insn(addr, 2, True, h, 'b', 14)
        i.target = (addr + 4 + (sx(h & 0x7FF, 11) << 1)) & 0xFFFFFFFF
        i.writes_pc = i.ends_block = True
        return i
    if (h >> 11) == 0x1E and h2 is not None and (h2 >> 11) in (0x1F, 0x1D):
        hi = sx(h & 0x7FF, 11) << 12
        lo = (h2 & 0x7FF) << 1
        tgt = (addr + 4 + hi + lo) & 0xFFFFFFFF
        if (h2 >> 11) == 0x1F:
            i = Insn(addr, 4, True, h | (h2 << 16), 'bl', 14)
            i.target = tgt
        else:
            i = Insn(addr, 4, True, h | (h2 << 16), 'blx_imm', 14)
            i.target = (tgt & ~3) | 0   # ARM target (bit 0 clear)
            i.exchange = True
        i.call = True
        i.writes_pc = i.ends_block = True
        return i
    return Insn(addr, 2, True, h, 'undef', 14)
