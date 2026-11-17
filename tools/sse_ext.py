"""Instructions pcrecomp's lift32/lift32_sse do not lift, for liftlib's
CheckedLifter: the packed-integer SSE2 forms on xmm registers, a few SSE3,
SSSE3 and SSE4.1 forms, the SSE1 additions to the MMX file, and fisttp.

lift_ext(lifter, insn, m, comment) returns the C lines, or None when the
instruction is not one of these (the vendor lifter then handles it, and an
unknown one still becomes a counted void_lift_trap). The C helpers live in
android/runtime/recomp_sse_ext.h.
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_MM0, X86_REG_ST0, X86_REG_XMM0

XMM = {X86_REG_XMM0 + i: i for i in range(8)}
MMX = {X86_REG_MM0 + i: i for i in range(8)}

# xmm_<name>(dst, src) helpers: two-operand lane-wise forms.
XMM_BINARY = {
    'paddb', 'paddw', 'psubb', 'psubw', 'paddsb', 'paddsw', 'psubsb', 'psubsw', 'paddusb', 'paddusw',
    'psubusb', 'psubusw', 'pmullw', 'pmulhw', 'pmulhuw', 'pmulld', 'pmaddwd', 'pavgb', 'pavgw', 'pminub',
    'pmaxub', 'pminuw', 'pmaxuw', 'pminsb', 'pmaxsb', 'pminsd', 'pmaxsd', 'pminud', 'pmaxud', 'pcmpeqq',
    'psadbw', 'packsswb', 'packuswb', 'packssdw', 'packusdw', 'pshufb', 'haddps', 'hsubps', 'haddpd',
    'addsubps', 'addsubpd',
}
XMM_UNPACK = {'punpcklbw': ('bw', 0), 'punpckhbw': ('bw', 1), 'punpcklwd': ('wd', 0), 'punpckhwd': ('wd', 1),
              'punpckldq': ('dq', 0), 'punpckhdq': ('dq', 1)}
XMM_ABS = {'pabsb', 'pabsw', 'pabsd'}
MMX_EXT = {'pmulhuw', 'pavgb', 'pavgw', 'pminub', 'pmaxub', 'pminsw', 'pmaxsw', 'psadbw', 'pmuludq'}
PMOV = {'b': 1, 'w': 2, 'd': 4, 'q': 8}


def _xi(op):
    return XMM.get(op.reg) if op.type == X86_OP_REG else None


def _mi(op):
    return MMX.get(op.reg) if op.type == X86_OP_REG else None


def lift_ext(L, insn, m, comment):
    ops = list(insn.operands or [])
    if not ops:
        return None
    d = ops[0]
    s = ops[1] if len(ops) > 1 else None
    di = _xi(d)

    def addr(op):
        return L._fmt_mem_addr(op.mem)

    def X(op):
        i = _xi(op)
        return f"_xmm[{i}]" if i is not None else f"xmm_ld({addr(op)})"

    def one(c):
        return [f"{c} {comment}"]

    # x87 environment: only the control word is modelled (fldenv loads it,
    # fnstenv stores it with an empty tag word and a clear status word).
    if m == 'fldenv' and d.type == X86_OP_MEM:
        return one(f"_fpu_cw = MEM16({addr(d)});")
    if m in ('fnstenv', 'fstenv') and d.type == X86_OP_MEM:
        a = addr(d)
        return one(f"{{ uint32_t _e = {a}; memset((void *)ADDR(_e), 0, 28); MEM16(_e) = _fpu_cw; "
                   f"MEM16(_e + 4) = (uint16_t)((_fp_top & 7) << 11); MEM16(_e + 8) = 0xFFFFu; }}")
    # fcmovcc st0, st(i): the condition from the live flag state.
    FCMOV = {'fcmovb': 'CC_B', 'fcmove': 'CC_E', 'fcmovbe': 'CC_BE', 'fcmovu': 'CC_P',
             'fcmovnb': 'CC_AE', 'fcmovne': 'CC_NE', 'fcmovnbe': 'CC_A', 'fcmovnu': 'CC_NP'}
    if m in FCMOV and len(ops) == 2 and s is not None and s.type == X86_OP_REG:
        i = s.reg - X86_REG_ST0
        if 0 <= i < 8:
            return one(f"if (recomp_cond_cf(_flag_k, _flag_a, _flag_b, {FCMOV[m]}, _cf)) _st[0] = _st[{i}];")
    # tzcnt (BMI1; on a CPU without it the same bytes run as rep bsf, but the
    # CPUID we report has neither, so only the definition matters).
    if m == 'tzcnt' and len(ops) == 2:
        L._flag_state = ('eflags', '_flag_a, _flag_b')
        L._flag_seq += 1
        return one(f"{{ uint32_t _v = {L._fmt_read(s)}; uint32_t _n = _v ? (uint32_t)__builtin_ctz(_v) : 32u; "
                   f"{L._fmt_write(d, '_n')}; _flag_a = (_v ? 0u : 1u) | (_n ? 0u : 0x40u); _flag_b = 0; _cf = _flag_a & 1u; }}")
    # fst st(i): ST(i) <- ST(0), no pop. lift32 lifts only the memory forms
    # and dropped this one without a trace (no C, no comment): D3DX's BC1
    # encoder (0xA00847A6, the only one in the game) then kept the red delta
    # where the green one belongs, and fitted every DXT1 block on that axis.
    if m == 'fst' and d.type == X86_OP_REG:
        i = d.reg - X86_REG_ST0
        if 0 <= i < 8:
            return one(f"{{ _st[{i}] = _st[0]; g_st_i64[{i}] = g_st_i64[0]; }}")
    if m == 'fisttp' and d.type == X86_OP_MEM:
        bits = d.size * 8
        if bits not in (16, 32, 64):
            return None
        return one(f"MEM{bits}({addr(d)}) = (uint{bits}_t)sx_fisttp(fp_pop(), {bits});")

    any_xmm = di is not None or (s is not None and _xi(s) is not None)
    if any_xmm:
        if m in XMM_BINARY and di is not None and s is not None:
            return one(f"_xmm[{di}] = xmm_{m}(_xmm[{di}], {X(s)});")
        if m in XMM_UNPACK and di is not None:
            kind, high = XMM_UNPACK[m]
            return one(f"_xmm[{di}] = xmm_punpck{kind}(_xmm[{di}], {X(s)}, {high});")
        if m in XMM_ABS and di is not None:
            return one(f"_xmm[{di}] = xmm_{m}_(_xmm[{di}], {X(s)});")
        if m in ('pshufhw', 'pshuflw') and di is not None and len(ops) == 3:
            return one(f"_xmm[{di}] = xmm_{m}({X(s)}, {ops[2].imm & 0xFF});")
        if m == 'palignr' and di is not None and len(ops) == 3:
            return one(f"_xmm[{di}] = xmm_palignr(_xmm[{di}], {X(s)}, {ops[2].imm & 0xFF});")
        if m in ('movddup', 'movshdup', 'movsldup') and di is not None:
            if m == 'movddup' and s.type == X86_OP_MEM:
                return one(f"{{ uint64_t _v = MEM64({addr(s)}); _xmm[{di}].u64[0] = _v; _xmm[{di}].u64[1] = _v; }}")
            return one(f"_xmm[{di}] = xmm_{m}({X(s)});")
        if m in ('cvtpd2dq', 'cvttpd2dq') and di is not None:
            return one(f"_xmm[{di}] = xmm_cvtpd2dq({X(s)}, {int(m.startswith('cvtt'))});")
        if m in ('cvtpd2pi', 'cvttpd2pi') and _mi(d) is not None:
            chop = int(m.startswith('cvtt'))
            return one(f"{{ xmm_t _s = {X(s)}; _mm[{_mi(d)}] = (uint64_t)(uint32_t)sse_cvt_i32(_s.f64[0], {chop}) | "
                       f"((uint64_t)(uint32_t)sse_cvt_i32(_s.f64[1], {chop}) << 32); }}")
        if m in ('roundss', 'roundsd', 'roundps', 'roundpd') and di is not None and len(ops) == 3:
            imm = ops[2].imm & 0xF
            kind, n = {'roundss': ('f32', 1), 'roundsd': ('f64', 1), 'roundps': ('f32', 4), 'roundpd': ('f64', 2)}[m]
            ct = 'float' if kind == 'f32' else 'double'
            body = ' '.join(f"_xmm[{di}].{kind}[{j}] = ({ct})sx_round(_s.{kind}[{j}], {imm});" for j in range(n))
            return one(f"{{ xmm_t _s = {X(s)}; {body} }}")
        if m.startswith(('pmovzx', 'pmovsx')) and di is not None and len(m) == 8:
            frm, to = PMOV[m[6]], PMOV[m[7]]
            width = 16 // to * frm
            if _xi(s) is not None:
                src = f"_xmm[{_xi(s)}].u8"
                return one(f"{{ xmm_t _t = _xmm[{_xi(s)}]; _xmm[{di}] = xmm_{m[:6]}(_t.u8, {frm}, {to}); }}")
            return one(f"{{ uint8_t _b[16]; memcpy(_b, (const void *)ADDR({addr(s)}), {width}); "
                       f"_xmm[{di}] = xmm_{m[:6]}(_b, {frm}, {to}); }}")
        if m in ('pextrd', 'pextrb') and _xi(s) is not None and len(ops) == 3:
            lane = ('u32', 3) if m == 'pextrd' else ('u8', 15)
            value = f"(uint32_t)_xmm[{_xi(s)}].{lane[0]}[{ops[2].imm & lane[1]}]"
            if d.type == X86_OP_MEM and m == 'pextrb':
                return one(f"MEM8({addr(d)}) = (uint8_t){value};")
            return one(f"{L._fmt_write(d, value)};")
        if m in ('pinsrd', 'pinsrb') and di is not None and len(ops) == 3:
            lane = ('u32', 3, 'uint32_t') if m == 'pinsrd' else ('u8', 15, 'uint8_t')
            src = f"MEM8({addr(s)})" if (s.type == X86_OP_MEM and m == 'pinsrb') else L._fmt_read(s)
            return one(f"_xmm[{di}].{lane[0]}[{ops[2].imm & lane[1]}] = ({lane[2]})({src});")
        if m == 'ptest' and di is not None:
            # ZF = (dst & src) == 0, CF = (~dst & src) == 0; the rest clear.
            L._flag_state = ('eflags', '_flag_a, _flag_b')
            L._flag_seq += 1
            return one(f"{{ xmm_t _s = {X(s)}; uint64_t _z = (_xmm[{di}].u64[0] & _s.u64[0]) | (_xmm[{di}].u64[1] & _s.u64[1]); "
                       f"uint64_t _c = (~_xmm[{di}].u64[0] & _s.u64[0]) | (~_xmm[{di}].u64[1] & _s.u64[1]); "
                       f"_flag_a = (_z ? 0u : 0x40u) | (_c ? 0u : 1u); _flag_b = 0; _cf = _flag_a & 1u; }}")
        return None

    # The SSE1 additions to the MMX file (mm registers).
    dm = _mi(d)
    if m == 'pshufw' and dm is not None and len(ops) == 3:
        return one(f"_mm[{dm}] = mmx_pshufw({L._mm_read(s)}, {ops[2].imm & 0xFF});")
    if m in MMX_EXT and dm is not None and s is not None:
        return one(f"_mm[{dm}] = mmx_{m}(_mm[{dm}], {L._mm_read(s)});")
    if m == 'pmovmskb' and s is not None and _mi(s) is not None:
        return one(f"{L._fmt_write(d, f'mmx_pmovmskb(_mm[{_mi(s)}])')};")
    if m == 'pextrw' and s is not None and _mi(s) is not None and len(ops) == 3:
        return one(f"{L._fmt_write(d, f'(uint32_t)(uint16_t)(_mm[{_mi(s)}] >> {16 * (ops[2].imm & 3)})')};")
    if m == 'pinsrw' and dm is not None and len(ops) == 3:
        sh = 16 * (ops[2].imm & 3)
        src = f"MEM16({addr(s)})" if s.type == X86_OP_MEM else L._fmt_read(s)
        return one(f"_mm[{dm}] = (_mm[{dm}] & ~(0xFFFFull << {sh})) | ((uint64_t)(uint16_t)({src}) << {sh});")
    return None
