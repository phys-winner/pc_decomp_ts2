"""Build the ARM64 Android stage: generated x86 C for all lifted modules plus
the Win32 HLE host, compiled with the NDK clang and linked into either a
command-line executable (adb shell bring-up) or the shared library for the APK.

Objects are reused only when the source, every header the generated code can
see, the compiler and the options are byte-identical (SHA-256 fingerprint).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MODULES = ("sims2", "ijl15")
NDK = Path(os.environ.get("ANDROID_NDK", r"C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018"))
TOOLCHAIN = NDK / "toolchains/llvm/prebuilt/windows-x86_64"
CLANG = TOOLCHAIN / "bin/clang.exe"
API = 29
TARGET = f"aarch64-linux-android{API}"
OUT = ROOT / "build/arm64"
# -ffunction-sections: the link packs the functions the device runs together
# (tools/make_symbol_order.py), which needs each in a section of its own.
COMMON = ["--target=" + TARGET, "-fno-strict-aliasing", "-fwrapv", "-fvisibility=hidden", "-fPIC",
          "-ffp-contract=off", "-fno-math-errno", "-g0", "-ffunction-sections"]
# The ring of the last function entries (RECOMP_TRACE: what a trap or fatal
# signal dumps) costs a load and two stores at every call; --trace builds it.
GENERATED_FLAGS = COMMON + ["-O2", "-w", "-DRECOMP_GENERATED_CODE", "-Iandroid/runtime"]
HOST_FLAGS = COMMON + ["-O2", "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                       "-DRECOMP_TRACE", "-Iandroid/runtime", "-Iandroid/host"]
# MojoShader (zlib), pinned in dependencies.json; only the profiles the host uses.
MOJO_DIR = "reference/mojoshader"
MOJO_SOURCES = ["mojoshader.c", "mojoshader_common.c", "mojoshader_assembler.c", "mojoshader_preprocessor.c",
                "mojoshader_lexer.c", "profiles/mojoshader_profile_common.c", "profiles/mojoshader_profile_glsl.c",
                "profiles/mojoshader_profile_d3d.c", "profiles/mojoshader_profile_bytecode.c"]
MOJO_FLAGS = COMMON + ["-O2", "-w", "-DMOJOSHADER_FLIP_RENDERTARGET", "-DMOJOSHADER_DEPTH_CLIPPING",
                       "-DSUPPORT_PROFILE_HLSL=0", "-DSUPPORT_PROFILE_ARB1=0", "-DSUPPORT_PROFILE_ARB1_NV=0",
                       "-DSUPPORT_PROFILE_METAL=0", "-DSUPPORT_PROFILE_SPIRV=0", "-DSUPPORT_PROFILE_GLSPIRV=0",
                       "-DSUPPORT_PROFILE_GLSL120=0", "-I" + MOJO_DIR, "-Iandroid/runtime"]


# Lifted functions the host implements natively (android/host/*.c). Their
# generated definitions are compiled from a patched copy as <name>_lifted, so
# every call and the dispatch table bind to the native version while the host
# can still run the lifted body for differential checks.
NATIVE_OVERRIDES = (
    "sub_A0649404",   # original options radio binder: display-specific FPS choices
    "sub_A0680D8B",   # keep the selected object orientation through purchase

    "sub_A05C089B",   # launcher intro skip covers the EA clip and main movie
    "sub_A03582A0", "sub_A035A1B0", "sub_A035AB10",  # camera identity before the first touch
    "sub_A0B4C1B0",   # common camera update: refresh touch camera identity after Options reuse

    "sub_A0ACB780",   # UI-only Reset: remove duplicate material parse, retain GPU buffers and later scene rebind (sims2_patches.c)
    "sub_A0039752",   # guarded D3DX RGBX row quantizer; exact lifted fallback (sims2_fast.c)
    "sub_A003AA9F",   # guarded D3DX A8 row quantizer; exact lifted fallback (sims2_fast.c)
    "sub_A0084173",   # D3DX BC1 (DXT1) block encoder, bit-exact; lifted fallback (sims2_bc1.c)
    "sub_A06815D7",   # hold an owned object's position until explicit touch preview
    "sub_A067D8A4",   # initial object preview uses its undo record's exact WorldLocation
    "sub_A067D1BD",   # touch object rotation is explicit, never the original mouse-drag facing
    "sub_A09CC369",   # QFS/RefPack decoder: native forward byte copies (sims2_fast.c)
    "sub_A09AB1A0",   # guarded DBPF resource index; unknown layouts use the lifted query
    "sub_A08264A0",   # Sims2EP9.exe: CRT memcpy/memmove (android/host/crt_native.c)
    "sub_A08281E0",   # CRT memset
    "sub_A0831280",   # CRT strlen
    "sub_A0C52180",   # crashpad::CrashpadClient::StartHandler (android/host/sims2_patches.c)
    "sub_A0145325",   # cGZCOM unique ID: borrow the next second instead of sleeping for it (sims2_patches.c)
    "sub_A01453AB",   # cGZCOM unique IDs in bulk: the same (sims2_patches.c)
    "sub_A064B9C7",   # Graphics Options: allow shadow choices in every scene (sims2_patches.c)
    "sub_A064ADC6",   # Graphics Apply: report live shadow properties/backend state (sims2_patches.c)
    "sub_A01BF515",   # cGZPropertySetString::SetProperty: the port's settings (android/host/sims2_settings.c)
    "sub_A0156260",   # Android IME bytes use the original input handler's UTF-8 branch (sims2_shims.c)
    "sub_A064CB41",   # Graphics/Performance Options controller refresh: + its property bindings (sims2_settings.c)
    "sub_A08B8390",   # renderer UI scale: the port already scales the logical desktop (sims2_settings.c)
    "sub_A08B8600",   # original sprite sampler sees physical UI magnification without changing layout
    "sub_A08C2670", "sub_A08C2840", "sub_A08C27E0",   # original font Begin/Draw/End with a render-only high-resolution glyph cache
    "sub_A08BB970",   # map high-resolution glyph pixels into their original logical ink rectangle
    "sub_A090A94E",   # release weak-keyed glyph copies before original font destruction
    "sub_A09713BE",   # window-manager UI scale, including pointer coordinates (sims2_settings.c)
    "sub_A05E01E5",   # original CAS paint keeps the bounded native window pan before child painting
    "sub_A0972F48",   # modal dialogs: temporarily fit fixed layouts (sims2_settings.c)
    "sub_A0648ADF",   # options categories: temporarily fit fixed layouts (sims2_settings.c)
    "sub_A064785E",   # options resize: keep the fit through category reconstruction (sims2_settings.c)
    "sub_A06491B9",   # whole options panel visibility: balance the temporary fit (sims2_settings.c)
    "sub_A0647521",   # options destructor: release the temporary fit (sims2_settings.c)
    "sub_A06195A1",   # CAS constructor: fit the fixed editor layout (sims2_settings.c)
    "sub_A061A2F9",   # CAS destructor: restore the requested scale (sims2_settings.c)
    "sub_A0619442", "sub_A061A29D",   # Build UI mode lifetime: touch context
    "sub_A06194E1", "sub_A061A2CB",   # Buy UI mode lifetime: touch context
    "sub_A0619A89", "sub_A061A67F",   # Live UI mode lifetime: touch context
    "sub_A0619C09", "sub_A061A796",   # Neighborhood UI mode lifetime: touch context
    "sub_A0619E26", "sub_A061A919",   # Options UI mode lifetime: allow its category buttons on a lot
    "sub_A0619F86", "sub_A061A9F2",   # Rewards UI mode: same touch controls as Buy, original reward currency
    "sub_A0973692", "sub_A0973E55",   # Native UI manager lifetime: main-thread touch queries
    "sub_A05BDFD7", "sub_A05BE0F2",   # Actual scene viewport attach/detach: touch ownership
    "sub_A076C5EB", "sub_A076CABE",   # Native active tool handler lifetime: touch profile
    "sub_A07495DA",   # selected catalog details open only through a long hold (sims2_tooltip.c)
    "sub_A0B46E90",                  # Actual nTSSG camera clamp: read-only zoom diagnostics
    "sub_A0769278",   # neighborhood chooser attach: fit its fixed layout (sims2_settings.c)
    "sub_A0769B01",   # neighborhood chooser detach: restore the requested scale (sims2_settings.c)
    "sub_A076662A",   # chooser destructor: release any remaining fit (sims2_settings.c)
    "sub_A05C0B82",   # game loading window attach: fit its fixed layout (sims2_settings.c)
    "sub_A05C0C89",   # game loading window detach: restore the requested scale (sims2_settings.c)
    "sub_A0C47EFC", "sub_A0C471DA", "sub_A0C46C58",   # asynchronous town-load fit through original completion/failure
    "sub_A0C44B8C", "sub_A0C42D84", "sub_A0C42483",   # asynchronous lot-load fit through original completion/failure
    "sub_A07B7B27",   # lot information card show: fit the fixed preview (sims2_settings.c)
    "sub_A07B6484",   # lot information card hide: restore the requested scale (sims2_settings.c)
    "sub_A07B6F59",   # lot information refresh: retain fit during hide/show (sims2_settings.c)
    "sub_A07B2A6F",   # lot information destructor: release any remaining fit (sims2_settings.c)
    "sub_A0931546",   # the window tree's search by ID, walked natively (android/host/sims2_fast.c)
    "sub_A014189B",   # uint32 hash-table lookup, with native bucket traversal (sims2_fast.c)
    "sub_A0ADFAE0",   # material parameter name vector and MSVC small-string selection (sims2_fast.c)
    "sub_A052F900",   # object/person bounded word attributes (sims2_fast.c)
    "sub_A0144C35",   # uint32 red-black tree lookup, without guest iterators (sims2_fast.c)
    "sub_A015DC50",   # tree iterator successor, with native parent/child traversal (sims2_fast.c)
    "sub_A017FC00",   # unchanged pipeline application; exact dirty/changed-program fallback (sims2_fast.c)
    "sub_A01BEFFF",   # a name's offset in a packed name table (property sets), with libc strlen (sims2_fast.c)
    "sub_A01BF191",   # the same, adding an absent name (sims2_fast.c)
    "sub_A01D5B50",   # persistence filename buckets: per-lookup original locale snapshot (sims2_fast.c)
    "sub_A09187BE",   # guarded font-parser substring search with native first-byte skipping (sims2_fast.c)
    "sub_A0845799",   # CRT _stricmp without a locale: ASCII folding (sims2_fast.c)
    "sub_A0229E45",   # a job queue's pump: no spinning on an empty queue (sims2_fast.c)
)


# With RECOMP_LOCAL_REGS a lifted body works on local copies of the
# registers, which go back to the globals around every call, tail, yield and
# exit (recomp_types.h RECOMP_REGS_OUT/IN). Each body copies only the
# registers its code names: one that never touches ebx leaves it in g_ebx, and
# one that uses xmm0-xmm2 keeps three SSE registers in a local copy instead of
# the whole file (generated code indexes it only by constants); a register it
# does not name cannot change under it, so the global stays exact. Comments
# (each line's x86 text) do not count; PUSHAD/POPAD name all eight and the
# call macros esp. A body that uses the x87 stack keeps its window pointer
# (recomp_types.h g_stp) in a local the same way. The generated sources stay
# shared with the Windows build; these definitions go before each body in the
# Android copy.
FUNCTION_START = re.compile(rb"^void sub_\w+\(void\) \{\r?$", re.M)   # generated files use CRLF
FUNCTION_END = re.compile(rb"\n\}\r?\n")
FUNCTION_ENTER = re.compile(rb"\bRECOMP_ENTER\((0x[0-9A-Fa-f]+u?)\)")
COMMENT = re.compile(rb"/\*.*?\*/", re.S)
GPR_NAME = re.compile(rb"\b(eax|ecx|edx|ebx|esp|esi|edi|ebp)\b")
XMM_NAME = re.compile(rb"\b_xmm\b")
XMM_INDEX = re.compile(rb"\b_xmm\[([0-7])\]")
X87_NAME = re.compile(rb"\b(_st|g_st|g_st_i64|_fp_top|fp_push|fp_pop|fp_xch|fp_push_i64|void_fpu_remainder)\b")
GPRS = (b"eax", b"ecx", b"edx", b"ebx", b"esp", b"esi", b"edi", b"ebp")


def register_macros(gprs, xmm, x87, caller):
    """The register transfer definitions for a body naming `gprs` and `xmm`
    (and the x87 stack if `x87`)."""
    xmm_out = b", ".join(b"g_xmm[%d] = l_xmm[%d]" % (i, i) for i in xmm) or b"(void)0"
    xmm_in = b", ".join(b"l_xmm[%d] = g_xmm[%d]" % (i, i) for i in xmm) or b"(void)0"
    locals_ = b" ".join(b"uint32_t l_%s = g_%s;" % (r, r) for r in gprs)
    out = b", ".join([b"g_%s = l_%s" % (r, r) for r in gprs] + [b"RECOMP_XMM_OUT()", b"RECOMP_X87_OUT()"])
    in_ = b", ".join([b"l_%s = g_%s" % (r, r) for r in gprs] + [b"RECOMP_XMM_IN()", b"RECOMP_X87_IN()"])
    return (b"#undef RECOMP_CALLER_VA\n#define RECOMP_CALLER_VA " + caller + b"\n"
            b"#undef _xmm\n#undef RECOMP_XMM_OUT\n#undef RECOMP_XMM_IN\n#undef RECOMP_REGS_LOCALS\n"
            b"#undef RECOMP_REGS_OUT\n#undef RECOMP_REGS_IN\n#undef g_st\n#undef RECOMP_X87_OUT\n#undef RECOMP_X87_IN\n" +
            (b"#define g_st l_stp\n#define RECOMP_X87_OUT() (g_stp = l_stp)\n#define RECOMP_X87_IN() (l_stp = g_stp)\n"
             if x87 else b"#define g_st g_stp\n#define RECOMP_X87_OUT() ((void)0)\n#define RECOMP_X87_IN() ((void)0)\n") +
            (b"#define _xmm l_xmm\n" if xmm else b"#define _xmm g_xmm\n") +
            b"#define RECOMP_XMM_OUT() (" + xmm_out + b")\n#define RECOMP_XMM_IN() (" + xmm_in + b")\n"
            b"#define RECOMP_REGS_LOCALS " + locals_ + b"\n"
            b"#define RECOMP_REGS_OUT() (" + out + b")\n#define RECOMP_REGS_IN() (" + in_ + b")\n")


def body_registers(body):
    """The integer and SSE registers a lifted body's code names, and whether
    it uses the x87 stack."""
    code = COMMENT.sub(b" ", body)
    if b"PUSHAD(" in code or b"POPAD(" in code:
        gprs = GPRS
    else:
        named = set(GPR_NAME.findall(code))
        if b"RECOMP_CALL(" in code or b"RECOMP_ICALL(" in code:
            named.add(b"esp")
        gprs = tuple(r for r in GPRS if r in named)
    indices = XMM_INDEX.findall(code)
    xmm = tuple(range(8)) if len(indices) != len(XMM_NAME.findall(code)) else tuple(sorted({int(i) for i in indices}))
    return gprs, xmm, bool(X87_NAME.search(code))


def localize_registers(text):
    parts, pos = [], 0
    for match in FUNCTION_START.finditer(text):
        end = FUNCTION_END.search(text, match.end())
        if not end or match.start() < pos:
            continue
        body = text[match.end():end.end()]
        entries = FUNCTION_ENTER.findall(COMMENT.sub(b" ", body))
        if len(entries) != 1:
            raise ValueError(f"expected one literal RECOMP_ENTER in {match.group(0)!r}, got {len(entries)}")
        gprs, xmm, x87 = body_registers(body)
        init = (b"\n    xmm_t l_xmm[8]; " + b" ".join(b"l_xmm[%d] = g_xmm[%d];" % (i, i) for i in xmm)) if xmm else b""
        if x87:
            init += b"\n    double *l_stp = g_stp;"
        parts += [text[pos:match.start()], register_macros(gprs, xmm, x87, entries[0]), match.group(0), init, body]
        pos = end.end()
    return b"".join(parts + [text[pos:]]) if parts else text


def prefetch_particles(text):
    """Warm the next particle in the two measured linked-list loops."""
    sites = {
        b"void sub_A0BB6830(void) {": (b"L_A0BB7AA0:", b"MEM32(edi + 0x14)"),
        b"void sub_A0BBDD50(void) {": (b"L_A0BBE771:", b"MEM32(MEM32(MEM32(ebp - 0x30) + 0x74) + 0x14)"),
    }
    # Keep the new guard in a helper: a call leaves each large body's
    # instrumentation hash and trained branch counts intact.
    helper = b"""static inline __attribute__((always_inline)) void prefetch_particle(uint32_t node, uint32_t head)
{
    uint32_t next = MEM32(node);
    /* A non-sentinel successor is a live 12-byte node; only hint its payload. */
    if (next != head && next >= 0x10000u && next <= 0xFFFDFFF4u)
        __builtin_prefetch((const void *)ADDR(MEM32(next + 8)), 0, 3);
}

"""
    parts, pos = [], 0
    for match in FUNCTION_START.finditer(text):
        site = sites.get(match.group(0).rstrip(b"\r"))
        if site is None:
            continue
        end = FUNCTION_END.search(text, match.end())
        if end is None:
            raise ValueError("unterminated particle body")
        body = text[match.end():end.end()]
        label, head = site
        newline = b"\r\n" if b"\r\n" in body else b"\n"
        anchor = label + newline
        if body.count(anchor) != 1:
            raise ValueError(f"expected one particle prefetch site {label!r}")
        hint = b"    prefetch_particle(esi, " + head + b");" + newline
        parts += [text[pos:match.start()], helper.replace(b"\n", newline) if not parts else b"",
                  text[match.start():match.end()], body.replace(anchor, anchor + hint, 1)]
        pos = end.end()
    return b"".join(parts + [text[pos:]]) if parts else text


def skip_startup_intro(text):
    """Use GameLoad's original input-skip path for the Android main intro."""
    for match in FUNCTION_START.finditer(text):
        if match.group(0).rstrip(b"\r") != b"void sub_A05C0D83(void) {":
            continue
        end = FUNCTION_END.search(text, match.end())
        if end is None:
            raise ValueError("unterminated GameLoad startup body")
        body = text[match.end():end.end()]
        newline = b"\r\n" if b"\r\n" in body else b"\n"
        anchor = b"L_A05C11F9:" + newline
        if body.count(anchor) != 1:
            raise ValueError("expected one GameLoad main-intro state 6")
        # State 6 checks this exact key-skip latch before opening the main
        # intro. Keep its original branch/cleanup and the PGO control flow.
        skip = (b"    /* Android: request the original main-intro key-skip path. */" + newline +
                b"    s2_intro_latch(ebx);" + newline)
        return b"extern void s2_intro_latch(unsigned int owner);\n" + text[:match.end()] + body.replace(anchor, anchor + skip, 1) + text[end.end():]
    return text


def default_neighborhood_flythrough_off(text):
    """Use the camera's existing no-flythrough path unless explicitly enabled."""
    for match in FUNCTION_START.finditer(text):
        if match.group(0).rstrip(b"\r") != b"void sub_A0361370(void) {":
            continue
        end = FUNCTION_END.search(text, match.end())
        if end is None:
            raise ValueError("unterminated camera-effects initialization body")
        body = text[match.end():end.end()]
        newline = b"\r\n" if b"\r\n" in body else b"\n"
        anchor = (b"L_A036161D:" + newline +
                  b"    MEM32(ebp + (-0x28)) = 0xA0D9C670u; /* 0xA036161D: mov dword ptr [ebp - 0x28], 0xa0d9c670 */" + newline +
                  b"    MEM8(ebp + (-0x24)) = (uint8_t)(1); /* 0xA0361624: mov byte ptr [ebp - 0x24], 1 */")
        if body.count(anchor) != 1:
            raise ValueError("expected one absent neighborhood-flythrough default")
        # The preceding property-exists branch preserves explicit true/false.
        replacement = anchor.replace(b"(uint8_t)(1); /* 0xA0361624", b"(uint8_t)(0); /* 0xA0361624")
        return text[:match.end()] + body.replace(anchor, replacement, 1) + text[end.end():]
    return text


def touch_camera_hover_guard(text):
    """Reuse StandardAction's held-input path during touch camera navigation."""
    marker = b"void sub_A068C624(void) {"
    if marker not in text:
        return text
    if text.count(marker) != 1:
        raise ValueError("duplicate StandardAction input handler")
    start = text.index(marker)
    end = FUNCTION_END.search(text, start)
    if end is None:
        raise ValueError("unterminated StandardAction input handler")
    body = text[start:end.end()]
    newline = b"\r\n" if b"\r\n" in body else b"\n"
    anchor = b"L_A068D4EA:" + newline
    if body.count(anchor) != 1:
        raise ValueError("StandardAction hover branch layout changed")
    # The original event-3/10 branch already skips this work for held input.
    # Preserve its point update, normal clicks and exact shared return path.
    check = b"    if(s2_touch_camera_hover_active())goto L_A068D4F5;" + newline
    declaration = b"extern int s2_touch_camera_hover_active(void);" + newline
    return declaration + text[:start] + body.replace(anchor, anchor + check, 1) + text[end.end():]


DOT_INLINE_CALLS = (0xA0AF26A9, 0xA0AF2739, 0xA0AF2754, 0xA0AF2841, 0xA0AF2856, 0xA0AF286B)
# Keep the rare x87 recenter branches outside the caller's pre-inlining CFG,
# preserving its trained PGO hash while clang still inlines the helper.
DOT_INLINE_HELPER = b'''static inline __attribute__((always_inline)) double *dot_inline_push(double *sp, double value)
{
    if (__builtin_expect(sp == g_stbuf, 0)) sp += fp_recentre_by(sp);
    --sp;
    sp[0] = value;
    X87_SHADOW(sp)[0] = 0;
    return sp;
}

'''


def inline_dot(text, callee=None):
    """Inline the exact float4 leaf at six measured A0AF25A0 call sites."""
    marker = b"void sub_A0AF25A0(void) {"
    if marker not in text:
        return text
    crlf = b"\r\n" in text
    text = text.replace(b"\r\n", b"\n")
    if callee is None:
        source = (GENERATED / "sims2-x86/sims2_A01A0000.c").read_bytes().replace(b"\r\n", b"\n")
        first = source.index(b"void sub_A01A6840(void) {")
        callee = source[first:source.index(b"\n}\n", first) + 3]
    callee = callee.replace(b"\r\n", b"\n")
    if hashlib.sha256(callee).hexdigest() != "37248e73b518dd252148842585b577f31d052f0ef85b841b442d14817ccfd862":
        raise ValueError("changed original dot leaf")
    if text.count(marker) != 1:
        raise ValueError("duplicate dot caller")
    start = text.index(marker)
    end = text.index(b"\n}\n", start) + 3
    body = text[start:end]
    first = callee.index(b"    ecx = MEM32(esp + 0x4);")
    last = callee.index(b"    RECOMP_FLAGS_OUT(); esp += 4;", first)
    arithmetic = callee[first:last]
    old_push = b"    fp_push(*(float*)&MEM32(esp + 0x4));"
    if arithmetic.count(old_push) != 1:
        raise ValueError("changed dot x87 result")
    arithmetic = arithmetic.replace(old_push,
        b"    g_st = dot_inline_push(g_st, *(float*)&MEM32(esp + 0x4));")
    if re.search(rb"\b(_cf|_flag_\w*|RECOMP_CALL|RECOMP_ICALL|goto)\b", arithmetic):
        raise ValueError("dot is no longer a straight-line leaf")
    for address in DOT_INLINE_CALLS:
        old = (f"    RECOMP_CALL(sub_A01A6840); /* 0x{address:08X}: call 0xa01a6840 */\n"
               "    RECOMP_FLAGS_IN();").encode()
        if body.count(old) != 1:
            raise ValueError(f"changed/missing dot site {address:08X}")
        # Preserve the guest return/argument writes, diagnostics and global
        # flags. The leaf reads no GPR/XMM bank or guest callback, so those
        # stay in the caller's locals instead of being packed around a call.
        new = (b"    /* Exact leaf inline: retain the guest return slot and argument overwrite. */\n"
               b"    PUSH32(esp, RECOMP_RETADDR);\n"
               b"    RECOMP_FLAGS_OUT();\n"
               b"    g_cur_func = 0xA01A6840u;\n"
               b"#ifdef RECOMP_TRACE\n"
               b"    g_enter_trace[g_enter_idx++ & (RECOMP_ENTER_SIZE - 1)] = 0xA01A6840u;\n"
               b"#endif\n" + arithmetic +
               b"    esp += 4;\n"
               b"    g_cur_func = 0xA0AF25A0u;")
        body = body.replace(old, new)
    if body.count(b"RECOMP_CALL(sub_A01A6840)"):
        raise ValueError("unexpected extra dot site")
    result = text[:start] + DOT_INLINE_HELPER + body + text[end:]
    return result.replace(b"\n", b"\r\n") if crlf else result




# `jmp [reg*4 + table]` reaches its target through the body's `_ljump`
# switch over guest addresses; with a few hundred sparse cases clang emits a
# binary search (the script VM's opcode dispatch spent most of its time
# there). Each such jump gets a dense switch over the table index built from
# the jump table in the image the code was lifted from (clang lowers it to a
# table of its own); an index past it, or an entry that is not a label of the
# body, still goes through the address switch. Not `goto *` over a table of
# label addresses: clang 18 (NDK r27) dropped a destination from such an
# indirectbr's successors, so liveness and AArch64 Redundant Copy Elimination
# saw a block with one predecessor and deleted the `eax = 0` the table entry
# relied on: the CRT memcmp returned a code address for equal 32-byte buffers.
# Capstone prints a displacement at or above 0x80000000 as a negative number
# (Sims2EP9.exe lives at 0xA0000000): `MEM32(eax * 4 + (-0x5FBE8AE0))`.
JUMP_TABLE = re.compile(rb"\{ _itail_tgt = \(uint32_t\)\(MEM32\((e[a-d]x|e[sd]i|ebp|esp) \* 4 \+ "
                        rb"(?:0x([0-9A-F]{8})|\(-0x([0-9A-F]{1,8})\))\)\); goto _ljump; \}")
LABEL = re.compile(rb"^L_([0-9A-F]{8}):", re.M)
MODULE_IMAGES = {"sims2": "generated/input/Sims2EP9.exe", "ijl15": "generated/input/ijl15.dll"}


class Image:
    """A PE image's bytes by virtual address (the lifted, possibly rebased, copy)."""
    def __init__(self, path):
        data = Path(path).read_bytes()
        pe = int.from_bytes(data[0x3C:0x40], "little")
        count = int.from_bytes(data[pe + 6:pe + 8], "little")
        optional = pe + 24
        self.base = int.from_bytes(data[optional + 28:optional + 32], "little")
        table = optional + int.from_bytes(data[pe + 20:pe + 22], "little")
        self.sections = []
        for i in range(count):
            at = table + 40 * i
            vsize, va, raw_size, raw = (int.from_bytes(data[at + o:at + o + 4], "little") for o in (8, 12, 16, 20))
            self.sections.append((self.base + va, max(vsize, raw_size), raw, raw_size))
        self.data = data
        self.sha256 = hashlib.sha256(data).hexdigest()

    def u32(self, va):
        for start, size, raw, raw_size in self.sections:
            if start <= va and va + 4 <= start + size:
                offset = va - start
                return int.from_bytes(self.data[raw + offset:raw + offset + 4], "little") if offset + 4 <= raw_size else 0
        return None


_images = {}


# Another release (tools/lift_release.py) builds from generated/<release>/
# into build/arm64-<release>; set by --release.
GENERATED = ROOT / "generated"
RELEASE = None


def module_image(module):
    """The image a generated module was lifted from, checked against its report."""
    if module not in _images:
        image = Image(ROOT / MODULE_IMAGES[module])
        report = json.loads((GENERATED / f"{module}-x86/report.json").read_text())
        expected = report.get("sha256")
        if expected and expected != image.sha256:
            raise SystemExit(f"{module}: image {MODULE_IMAGES[module]} does not match report.json")
        _images[module] = image
    return _images[module]


def direct_jump_tables(text, image):
    parts, pos = [], 0
    for match in FUNCTION_START.finditer(text):
        end = FUNCTION_END.search(text, match.end())
        if not end or match.start() < pos:
            continue
        body = text[match.end():end.end()]
        if b"  _ljump:" not in body or not JUMP_TABLE.search(body):
            continue
        labels = {int(label, 16) for label in LABEL.findall(body)}
        if len(labels) > 2000:
            # Bodies labelled at nearly every instruction (the CRT's memcpy
            # family: 112 tables, ~5,000 labels) took clang over ten minutes
            # with label tables; they keep the switch.
            continue
        low, high = min(labels), max(labels)

        def table_jump(jump):
            register = jump.group(1)
            table = int(jump.group(2), 16) if jump.group(2) else (-int(jump.group(3), 16)) & 0xFFFFFFFF
            targets = []
            while len(targets) < 1024:
                target = image.u32(table + 4 * len(targets))
                if target is None or not low <= target <= high:
                    break
                targets.append(target)
            if not targets:
                return jump.group(0)
            cases = b" ".join(b"case %du: goto L_%08X;" % (i, t) for i, t in enumerate(targets) if t in labels)
            return (b"{ uint32_t _jx = %s; _itail_tgt = (uint32_t)(MEM32(%s * 4 + 0x%08Xu)); "
                    b"switch (_jx) { %s default: goto _ljump; } }" % (register, register, table, cases))

        parts += [text[pos:match.end()], JUMP_TABLE.sub(table_jump, body)]
        pos = end.end()
    return b"".join(parts + [text[pos:]]) if parts else text


def ui_sampling_return_sites(text):
    """Retain original return addresses at the two guarded UI call sites."""
    sites = (
        (b"    RECOMP_CALL(sub_A08B8390); /* 0xA08B874C: call 0xa08b8390 */", b"0xA08B8751u"),
        (b"    RECOMP_ICALL(MEM32(esi + 0x78)); /* 0xA08C2BCC: call dword ptr [esi + 0x78] */", b"0xA08C2BCFu"),
    )
    for call, address in sites:
        if call not in text:
            continue
        if text.count(call) != 1:
            raise SystemExit("UI sampling call site is not unique")
        text = text.replace(call, b'#pragma push_macro("RECOMP_RETADDR")\n#undef RECOMP_RETADDR\n'
            b'#define RECOMP_RETADDR ' + address + b'\n' + call + b'\n#pragma pop_macro("RECOMP_RETADDR")', 1)
    return text


def resource_enumeration_source(local_xmm=True):
    """A separate clone keeps the original enumeration's PGO name and object intact."""
    if RELEASE is not None:
        return None
    source = GENERATED / "sims2-x86/sims2_A09A8000.c"
    text = source.read_bytes()
    match = re.search(rb"^void sub_A09AC040\(void\) \{.*?\n\}", text, re.M | re.S)
    if not match:
        raise ValueError("missing original DBPF enumeration")
    body = match.group().replace(b"void sub_A09AC040(void)", b"void sub_A09AC040_native(void)", 1)
    body = body.replace(b"\r\n", b"\n")
    anchor = b"L_A09AC120:\n"
    if body.count(anchor) != 1 or body.count(b"L_A09AC118:\n") != 1 or body.count(b"    RECOMP_REGS_LOCALS") != 1:
        raise ValueError("DBPF enumeration loop layout changed")
    body = body.replace(b"    RECOMP_REGS_LOCALS", b"    uint32_t _skip_try=0;\n    RECOMP_REGS_LOCALS", 1)
    # Physical production-VM/dispatch probes lose on one/two-row buckets.
    # Keep their exact original loop without invoking the guarded helper.
    body = body.replace(b"L_A09AC118:\n", b"L_A09AC118:\n    _skip_try=(edx>esi&&edx-esi>=64);\n", 1)
    injection = b'''L_A09AC120:
#ifndef RECOMP_TRACE
    if(_skip_try){ uint32_t _skipped=s2_resource_skip(esp,ebx,esi,MEM32(esp+0x1C));
      if(_skipped){
        esi+=_skipped*16;ecx=MEM32(esp+0x5C);edx=esp+0x44;eax=MEM32(ebx+4)&0xFFFFFF00u;
        _flag_k=FK_CMP;_flag_a=esi;_flag_b=MEM32(esp+0x1C);_cf=esi<_flag_b;
        if(esi==_flag_b)goto L_A09AC193;
      }else if(!(recomp_yield_hook&&g_backedges>=RECOMP_YIELD_EVERY-1))_skip_try=0;
    }
#endif
'''
    declarations = b'''#include "recomp_types.h"
void sub_A09A5500(void),sub_A09A37D0(void),sub_A09A5E20(void),sub_A09B2FB0(void),sub_A0824624(void),sub_A01443B0(void),sub_A082A797(void);
void void_lift_trap(uint32_t,const char *);
uint32_t s2_resource_skip(uint32_t,uint32_t,uint32_t,uint32_t);
'''
    result = declarations + body.replace(anchor, injection, 1) + b"\n"
    if local_xmm:
        result = localize_registers(result)
    target = OUT / "native-generated/sims2_resource_enum.c"
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.is_file() or target.read_bytes() != result:
        target.write_bytes(result)
    return target


def override_source(source, local_xmm):
    """The file to compile for a generated source and its extra flags: a
    patched copy under build/ when it defines one of NATIVE_OVERRIDES or (with
    local_xmm) a body that uses the SSE file, and with direct jump tables."""
    original = text = source.read_bytes()
    if RELEASE is None:
        if source.name == "recomp_dispatch.c":
            # This virtual method has no direct generated calls. Keep its old
            # symbol/PGO body as the exact OFF control, routing the guest VA here.
            entry = b"{0xA09AC040u, sub_A09AC040}"
            if text.count(entry) != 1:
                raise ValueError("DBPF enumeration dispatch layout changed")
            text = b"void sims2_resource_enumerate(void);\n" + text.replace(entry, b"{0xA09AC040u, sims2_resource_enumerate}", 1)
        elif b"RECOMP_CALL(sub_A09AC040)" in text:
            raise ValueError("DBPF enumeration gained a direct call; update native routing before building")
    for name in NATIVE_OVERRIDES:
        text = text.replace(b"\nvoid %s(void) {" % name.encode(), b"\nvoid %s_lifted(void) {" % name.encode(), 1)
    if source.name != "recomp_dispatch.c":
        module = source.parent.name[:-len("-x86")]
        if b"goto _ljump; }" in text:
            text = direct_jump_tables(text, module_image(module))
        if module == "sims2":
            text = ui_sampling_return_sites(text)
            text = prefetch_particles(text)
            text = skip_startup_intro(text)
            text = default_neighborhood_flythrough_off(text)
            if RELEASE is None:
                text = touch_camera_hover_guard(text)
            text = inline_dot(text)
        if local_xmm:
            text = localize_registers(text)
    if text == original:
        return source, []
    patched = OUT / "patched" / source.parent.name / source.name
    patched.parent.mkdir(parents=True, exist_ok=True)
    if not patched.is_file() or patched.read_bytes() != text:
        patched.write_bytes(text)
    return patched, ["-I" + source.parent.relative_to(ROOT).as_posix()]


def sha_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def available_modules():
    """Generated stages present on disk (PhysX joins once tools/lift_physx.py has run)."""
    return [m for m in MODULES if (GENERATED / f"{m}-x86/report.json").exists()]


def generated_sources():
    sources = []
    modules = available_modules()
    for name in modules:
        report = json.loads((GENERATED / f"{name}-x86/report.json").read_text())
        files = report.get("sources")
        if not isinstance(files, list) or not files:
            raise SystemExit(f"{name}: report.json has no source list")
        sources += [GENERATED / f"{name}-x86" / f for f in files]
    dispatch = GENERATED / "dispatch/recomp_dispatch.c"
    if dispatch.is_file():   # absent while a release is still being lifted (--compile-only)
        sources.append(dispatch)
    return sources


def host_sources():
    return sorted(p for p in (ROOT / "android/host").glob("*.c")
                  if RELEASE is None or p.name != "sims2_resource.c") + [ROOT / "android/launcher/native/zipmount.c"]


def header_digest(paths):
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(ROOT).as_posix().encode())
        digest.update(sha_file(path).encode())
    return digest.hexdigest()


def included_fragments(source, seen=None):
    """Hash local .inc dependencies without invalidating unrelated host objects."""
    seen = set() if seen is None else seen
    found = set()
    for name in re.findall(r'^\s*#\s*include\s*"([^"\n]+\.inc)"', source.read_text(encoding="utf-8"), re.M):
        path = (source.parent / name).resolve()
        if path in seen:
            continue
        if not path.is_file():
            raise FileNotFoundError(f"Included fragment missing: {path}")
        seen.add(path)
        found.add(path)
        found.update(included_fragments(path, seen))
    return sorted(found)


def compile_one(source, obj, flags, fingerprint, cache):
    rel = source.relative_to(ROOT).as_posix()
    entry = cache.get(rel)
    if entry and entry.get("fingerprint") == fingerprint and obj.is_file() and sha_file(obj) == entry.get("object_sha256"):
        return rel, "reused", 0.0, entry
    obj.parent.mkdir(parents=True, exist_ok=True)
    start = time.time()
    command = [str(CLANG)] + flags + ["-c", str(source), "-o", str(obj)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    elapsed = time.time() - start
    if result.returncode:
        raise RuntimeError(f"{rel}: clang failed ({result.returncode})\n{result.stderr[-4000:]}")
    if result.stderr.strip():
        print(f"[warn] {rel}:\n{result.stderr.strip()[-3000:]}", flush=True)
    return rel, "compiled", elapsed, {"fingerprint": fingerprint, "object_sha256": sha_file(obj)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 4))
    parser.add_argument("--host-only", action="store_true", help="Recompile host sources and relink only")
    parser.add_argument("--shared", action="store_true", help="Link libvoid.so for the APK instead of the executable")
    parser.add_argument("--global-regs", action="store_true",
                        help="Keep the integer registers in globals (upstream model) instead of RECOMP_LOCAL_REGS")
    parser.add_argument("--volatile-memory", action="store_true",
                        help="Volatile guest memory accesses (upstream) instead of RECOMP_PLAIN_MEMORY")
    parser.add_argument("--pgo-generate", action="store_true",
                        help="Instrument the generated code for a profile (build/arm64-pgogen; LLVM_PROFILE_FILE "
                             "names the output, the script command 'profile' writes it)")
    parser.add_argument("--pgo-use", metavar="PROFDATA", default=None,
                        help="Optimise the generated code with a merged profile (llvm-profdata merge); "
                             "default build/pgo/abb24fae-20261022.profdata when present")
    parser.add_argument("--no-pgo", action="store_true", help="Build without the profile")
    parser.add_argument("--compile-only", action="store_true", help="Compile the objects, do not link")
    parser.add_argument("--lto", action="store_true",
                        help="ThinLTO over the generated code and the host (build/arm64-lto): the hot small "
                             "functions inline across chunks")
    parser.add_argument("--trace", action="store_true",
                        help="Keep the ring of the last function entries in the generated code (crash dumps list them)")
    parser.add_argument("--rt-check", action="store_true", help="Assert GL/EGL calls run on the GLES render thread")
    parser.add_argument("--release", metavar="NAME",
                        help="Build another release of the game lifted by tools/lift_release.py "
                             "(generated/NAME, build/arm64-NAME, libvoid_NAME.so); never with the 2009 profile")
    args = parser.parse_args()
    from prepare_dependencies import prepare
    prepare("mojoshader", "dlmalloc")
    global GENERATED, RELEASE, MODULES, MODULE_IMAGES, NATIVE_OVERRIDES
    if args.release:
        GENERATED = ROOT / "generated" / args.release
        if (GENERATED / "release.json").is_file():
            RELEASE = json.loads((GENERATED / "release.json").read_text())
        elif args.compile_only:
            # Still lifting (tools/lift_release.py --only): the modules done so far.
            spec = json.loads((ROOT / "config/releases" / f"{args.release}.json").read_text())
            done = [m for m in spec["modules"] if (GENERATED / f"{m['prefix']}-x86/report.json").is_file()
                    and json.loads((GENERATED / f"{m['prefix']}-x86/report.json").read_text()).get("status") == "GENERATED"]
            reports = [json.loads((GENERATED / f"{m['prefix']}-x86/report.json").read_text()) for m in done]
            native = [f"sub_{int(a, 16):08X}" for r in reports for a in r["overrides"]["crt_memmove"]]
            native += [f"sub_{int(r['overrides']['mdct_backward'], 16):08X}" for r in reports if "mdct_backward" in r["overrides"]]
            RELEASE = {"modules": [m["prefix"] for m in done], "native_overrides": native,
                       "images": {m["prefix"]: r["input_image"] for m, r in zip(done, reports)}}
        else:
            raise SystemExit(f"run tools/lift_release.py {args.release} (or --finish) first")
        MODULES = tuple(RELEASE["modules"])
        MODULE_IMAGES = dict(RELEASE["images"])
        NATIVE_OVERRIDES = tuple(RELEASE["native_overrides"])
        args.no_pgo = True
    # The first location gains ~12% (42-51 fps against 38-45) from the
    # profile, the map's settled phase more; a build without it is an explicit
    # choice, since it recompiles every generated object.
    if args.pgo_use is None and not args.no_pgo and not args.pgo_generate and (ROOT / "build/pgo/abb24fae-20261022.profdata").is_file():
        args.pgo_use = str(ROOT / "build/pgo/abb24fae-20261022.profdata")
    if args.no_pgo:
        args.pgo_use = None
    # The default generated code keeps registers in locals and accesses guest
    # memory plainly (first location on the Redmi Note 8 Pro: 37-43 fps against
    # 33-39 with both off). The upstream modes build in their own directories
    # (build/arm64-globalregs, build/arm64-volatilemem, ...) for A/B runs.
    global OUT, GENERATED_FLAGS
    if args.trace:
        GENERATED_FLAGS = GENERATED_FLAGS + ["-DRECOMP_TRACE"]
    GENERATED_FLAGS = (GENERATED_FLAGS + ([] if args.global_regs else ["-DRECOMP_LOCAL_REGS"]) +
                       ([] if args.volatile_memory else ["-DRECOMP_PLAIN_MEMORY"]))
    upstream = [name for name, on in (("globalregs", args.global_regs), ("volatilemem", args.volatile_memory),
                                      ("pgogen", args.pgo_generate), ("lto", args.lto)) if on]
    if args.release:
        upstream.insert(0, args.release)
    if upstream:
        OUT = ROOT / ("build/arm64-" + "-".join(upstream))
    global HOST_FLAGS
    if args.lto:
        GENERATED_FLAGS = GENERATED_FLAGS + ["-flto=thin"]
        HOST_FLAGS = HOST_FLAGS + ["-flto=thin"]
    if args.pgo_generate:
        GENERATED_FLAGS = GENERATED_FLAGS + ["-fprofile-instr-generate"]
    if args.pgo_use:
        profile = Path(args.pgo_use).resolve()
        # The profile's digest joins the flags, so objects follow a new profile.
        GENERATED_FLAGS = GENERATED_FLAGS + ["-fprofile-instr-use=" + profile.as_posix(),
                                             "-DVOID_PGO_PROFILE_" + sha_file(profile)[:16]]
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    cache_path = OUT / "objects.json"
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    compiler_sha = sha_file(CLANG)
    runtime_headers = [ROOT / "android/runtime/recomp_types.h", ROOT / "android/runtime/recomp_sse_ext.h"]
    # A module's sources see the runtime and their own headers; the merged
    # dispatch table (last module) sees every module's headers.
    # Chunks declare what they use themselves (tools/liftlib.py), so only the
    # runtime headers key their objects; the dispatch table sees every header.
    module_digest = {name: header_digest(runtime_headers) for name in available_modules()}
    gen_digest = header_digest(runtime_headers + sorted(p for name in available_modules()
                                                        for p in (GENERATED / f"{name}-x86").glob("*.h")))
    release_headers = [GENERATED / "release_overrides.h"] if args.release and not args.compile_only else []
    host_digest = header_digest(runtime_headers + sorted((ROOT / "android/host").glob("*.h")) +
                                sorted((ROOT / "android/texconv").glob("*.h")) +
                                [ROOT / "reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c", ROOT / "android/launcher/native/zipmount.h"] + release_headers)
    jobs = []
    if not args.host_only:
        for source in generated_sources():
            module = source.parent.name
            obj = OUT / "obj" / module / (source.stem + ".o")
            digest = gen_digest if source.name == "recomp_dispatch.c" else module_digest[module[:-len("-x86")]]
            source, extra = override_source(source, local_xmm=not args.global_regs)
            flags = GENERATED_FLAGS + extra
            fp = hashlib.sha256(json.dumps([sha_file(source), digest, compiler_sha, flags]).encode()).hexdigest()
            jobs.append((source, obj, flags, fp))
    # This host specialization's clone is also required by --host-only links.
    native_enum = resource_enumeration_source(local_xmm=not args.global_regs)
    if native_enum is not None:
        obj = OUT / "obj/native-generated/sims2_resource_enum.o"
        fp = hashlib.sha256(json.dumps([sha_file(native_enum), module_digest["sims2"], compiler_sha, GENERATED_FLAGS]).encode()).hexdigest()
        jobs.append((native_enum, obj, GENERATED_FLAGS, fp))
    for name in MOJO_SOURCES:
        source = ROOT / MOJO_DIR / name
        obj = OUT / "obj/mojoshader" / (source.stem + ".o")
        fp = hashlib.sha256(json.dumps([sha_file(source), header_digest(sorted((ROOT / MOJO_DIR).glob("*.h"))),
                                        compiler_sha, MOJO_FLAGS]).encode()).hexdigest()
        jobs.append((source, obj, MOJO_FLAGS, fp))
    host_flags = HOST_FLAGS + (["-DVOID_ANDROID_APP"] if args.shared else [])
    if args.release:
        host_flags = host_flags + ["-DVOID_RELEASE", "-I" + GENERATED.relative_to(ROOT).as_posix()]
    host_dir = "obj/host-app" if args.shared else "obj/host"
    for source in [] if args.compile_only else host_sources():
        obj = OUT / host_dir / (source.stem + ".o")
        flags = host_flags + (["-DVOID_RT_CHECK"] if args.rt_check and source.stem.startswith("d3d9_") else [])
        fragments = included_fragments(source)
        inputs = [sha_file(source), host_digest, compiler_sha, flags]
        if fragments:
            inputs.append(header_digest(fragments))
        fp = hashlib.sha256(json.dumps(inputs).encode()).hexdigest()
        jobs.append((source, obj, flags, fp))
    # Largest first keeps the pool busy at the end.
    jobs.sort(key=lambda j: -j[0].stat().st_size)
    start = time.time()
    compiled = reused = 0
    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, s, o, f, fp, cache) for s, o, f, fp in jobs]
        for future in as_completed(futures):
            try:
                rel, status, elapsed, entry = future.result()
            except RuntimeError as error:
                failures.append(str(error))
                print(str(error), flush=True)
                continue
            cache[rel] = entry
            if status == "compiled":
                compiled += 1
                if elapsed > 20:
                    print(f"[slow] {rel}: {elapsed:.1f}s", flush=True)
            else:
                reused += 1
            if (compiled + reused) % 50 == 0:
                print(f"[progress] {compiled + reused}/{len(jobs)} ({time.time() - start:.0f}s)", flush=True)
                cache_path.write_text(json.dumps(cache, indent=1))
    cache_path.write_text(json.dumps(cache, indent=1))
    print(f"compiled {compiled}, reused {reused}, failed {len(failures)} in {time.time() - start:.0f}s", flush=True)
    if failures:
        sys.exit(1)
    if args.compile_only:
        return
    if args.host_only:
        # The generated objects as they stand (objects of sources a later lift
        # or a removed host file left behind are not linked).
        objects = [str(OUT / "obj" / s.parent.name / (s.stem + ".o")) for s in generated_sources()]
        objects += [str(o) for _, o, _, _ in jobs]
    else:
        objects = [str(o) for _, o, _, _ in jobs]
    response = OUT / "link.rsp"
    response.write_text("\n".join(o.replace("\\", "/") for o in objects))
    library = "libsims2.so"
    output = OUT / (library if args.shared else "sims2-arm64")
    link = [str(CLANG), "--target=" + TARGET, "-fuse-ld=lld", "-Wl,--gc-sections", "-Wl,-z,nostart-stop-gc",
            "-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384",
            "-Wl,--build-id=sha1", "@" + str(response), "-o", str(output), "-lm", "-lz", "-llog", "-ldl", "-lEGL", "-lGLESv3"]
    link += ["-shared", "-landroid", "-lOpenSLES", "-laaudio"] if args.shared else ["-pie"]
    if args.pgo_generate:
        link.append("-fprofile-instr-generate")
    # The code the device runs first and together: ~270 MB of lifted code on
    # 4 KB pages misses the instruction TLB unless the hot part is packed.
    if args.lto:
        cache = OUT / "thinlto-cache"
        link += ["-flto=thin", f"-Wl,--thinlto-jobs={args.jobs}", "-Wl,--thinlto-cache-dir=" + cache.as_posix(), "-O2"]
        if args.pgo_use:
            link.append("-fprofile-instr-use=" + Path(args.pgo_use).resolve().as_posix())
    order = ROOT / "build/pgo/order.txt"
    if order.is_file() and not args.pgo_generate:
        link += ["-Wl,--symbol-ordering-file=" + order.as_posix(), "-Wl,--no-warn-symbol-ordering"]
    start = time.time()
    result = subprocess.run(link, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        print(result.stderr[-6000:])
        sys.exit(1)
    print(f"linked {output} ({output.stat().st_size / 1e6:.1f} MB) in {time.time() - start:.0f}s")


if __name__ == "__main__":
    main()
