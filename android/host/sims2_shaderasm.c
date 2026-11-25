/* D3DXAssembleShader (0xA001E89E, the statically linked D3DX; __stdcall
 * pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs,
 * `retn 1Ch`): the scene graph assembles each material's vertex and pixel
 * shader text when the material is created (cShaderProgramsManagerDX,
 * 0xA0175120), and it creates materials per object instance. On the Huawei
 * POT-LX1 one lot visit with a zoom-out, rotations and floor changes made
 * 2816 assemblies of 50 distinct texts: 1.7 s of the game thread, 6% of the
 * zoom-out and floor-change frames.
 *
 * Without an include handler the result depends only on the text, the
 * defines (D3DXMACRO name/definition pairs; the scene graph passes its
 * shader-model macros) and the flags, so a key seen before (compared in full)
 * gets a new ID3DXBuffer (D3DXCreateBuffer, 0xA002EAB9) holding the same
 * token stream, S_OK and, when asked for, no error buffer (what the original
 * returns after an assembly without messages). Anything else runs the lifted
 * body and, when it succeeds without messages, is remembered.
 * debug.s2.shadercache: "off" always assembles; "check" assembles every
 * repeat too and compares the token stream with the remembered one. */
#include "host.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

void sub_A001E89E_lifted(void);

#define ASM_CREATE_BUFFER 0xA002EAB9u     /* D3DXCreateBuffer(NumBytes, ppBuffer), __stdcall */
#define ASM_SLOTS 1024u
#define ASM_TEXT_LIMIT (64u << 10)
#define ASM_TOTAL_LIMIT (16u << 20)

typedef struct {
    uint64_t hash;
    uint32_t len, flags, size;
    int messages;                 /* 1 none (seen with ppErrorMsgs), 0 unknown */
    uint8_t *text, *tokens;
} asm_entry;

static asm_entry asm_cache[ASM_SLOTS];
static unsigned asm_calls, asm_hits, asm_entries, asm_mismatches;
static uint64_t asm_ns, asm_hit_ns, asm_bytes;

/* The key (malloc'd, the caller frees it): the text, then each define's name
 * and definition with their terminators, then an end mark. NULL when a define
 * is unreadable or too long. A yield inside the lifted body can let another
 * guest thread assemble meanwhile, so the key is not shared. */
static uint8_t *asm_key(uint32_t src, uint32_t len, uint32_t defines, uint32_t *key_len)
{
    uint32_t need = len + 2;
    for (uint32_t m = defines, n = 0; m; m += 8, ++n) {
        if (n > 256 || !void_vm_readable(m, 8)) return NULL;
        uint32_t name = MEM32(m), value = MEM32(m + 4);
        if (!name) break;
        for (uint32_t s = 0; s < 2; ++s) {
            uint32_t str = s ? value : name, l = 0;
            if (!str) { need += 2; continue; }
            while (l < 4096 && void_vm_readable(str + l, 1) && MEM8(str + l)) ++l;
            if (l == 4096 || !void_vm_readable(str + l, 1)) return NULL;
            need += l + 2;
        }
    }
    if (need > ASM_TEXT_LIMIT) return NULL;
    uint8_t *key = (uint8_t *)malloc(need);
    if (!key) return NULL;
    memcpy(key, GPTR(src), len);
    uint32_t at = len;
    for (uint32_t m = defines; m; m += 8) {
        uint32_t name = MEM32(m), value = MEM32(m + 4);
        if (!name) break;
        for (uint32_t s = 0; s < 2; ++s) {
            uint32_t str = s ? value : name;
            key[at++] = str ? 1 : 0;                 /* a NULL definition differs from "" */
            if (str) { uint32_t l = (uint32_t)strlen((const char *)GPTR(str)); memcpy(key + at, GPTR(str), l + 1); at += l + 1; }
            else key[at++] = 0;
        }
    }
    key[at++] = 0xFF;
    key[at++] = 0xFF;
    *key_len = at;
    return key;
}

static int asm_mode(void)
{
    static int mode = -1;   /* 0 off, 1 on, 2 check */
    if (mode < 0) {
        mode = 1;
#ifdef __ANDROID__
        char value[92] = "";
        __system_property_get("debug.s2.shadercache", value);
        if (!strcmp(value, "off") || !strcmp(value, "0")) mode = 0;
        else if (!strcmp(value, "check")) mode = 2;
#endif
        if (mode != 1) void_log("[shaderasm] cache mode %s", mode ? "check" : "off");
    }
    return mode;
}

static asm_entry *asm_find(uint64_t hash, uint32_t len, uint32_t flags, const uint8_t *text, int insert)
{
    for (uint32_t i = 0, at = (uint32_t)hash & (ASM_SLOTS - 1); i < ASM_SLOTS; ++i, at = (at + 1) & (ASM_SLOTS - 1)) {
        asm_entry *e = &asm_cache[at];
        if (!e->text) return insert ? e : NULL;
        if (e->hash == hash && e->len == len && e->flags == flags && !memcmp(e->text, text, len)) return e;
    }
    return NULL;
}

/* The bytes of an ID3DXBuffer (vtable: +12 GetBufferPointer, +16 GetBufferSize, __stdcall). */
static int asm_buffer(uint32_t buffer, uint32_t *pointer, uint32_t *size)
{
    if (!buffer || !void_vm_readable(buffer, 4) || !void_vm_readable(MEM32(buffer), 20)) return 0;
    *size = void_call_guest(MEM32(MEM32(buffer) + 16), 1, &buffer, 1);
    *pointer = void_call_guest(MEM32(MEM32(buffer) + 12), 1, &buffer, 1);
    return *size <= ASM_TOTAL_LIMIT && (!*size || void_vm_readable(*pointer, *size));
}

void sub_A001E89E(void)
{
    const uint32_t sp = g_esp;
    const uint32_t src = MEM32(sp + 4), len = MEM32(sp + 8), defines = MEM32(sp + 12), include = MEM32(sp + 16);
    const uint32_t flags = MEM32(sp + 20), shader = MEM32(sp + 24), errors = MEM32(sp + 28);
    const int mode = asm_mode();
    const uint64_t started = void_monotonic_ns();
    asm_entry *e = NULL;
    uint64_t hash = 0;
    uint8_t *text = NULL;
    uint32_t key_len = 0;
    if (mode && !include && !(flags & ~3u) && len && len <= ASM_TEXT_LIMIT && void_vm_readable(src, len) &&
        (!shader || void_vm_writable(shader, 4)) && (!errors || void_vm_writable(errors, 4)) &&
        (text = asm_key(src, len, defines, &key_len)) != NULL) {
        hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < key_len; ++i) hash = (hash ^ text[i]) * 0x100000001B3ull;
        e = asm_find(hash, key_len, flags, text, 0);
        if (e && errors && e->messages != 1) e = NULL;      /* its messages are not known: assemble */
    }
    ++asm_calls;
    if (e && mode == 1) {
        /* The original's result for this text: a new buffer with its tokens. */
        uint32_t result = 0;
        if (shader) {
            MEM32(shader) = 0;
            uint32_t args[2] = { e->size, shader };
            result = void_call_guest(ASM_CREATE_BUFFER, 2, args, 1);
            uint32_t pointer, size;
            if ((int32_t)result >= 0) {
                if (!asm_buffer(MEM32(shader), &pointer, &size) || size != e->size) {
                    void_log("[shaderasm] D3DXCreateBuffer(%u) gave an unexpected buffer", e->size);
                    void_lift_trap(0xA001E89Eu, "shader cache buffer");
                }
                if (size) memcpy(GPTR(pointer), e->tokens, size);
                result = 0;
            }
        }
        if (errors) MEM32(errors) = 0;
        g_eax = result;
        g_esp = sp + 32;                                  /* retn 1Ch */
        ++asm_hits;
        asm_hit_ns += void_monotonic_ns() - started;
    } else {
        sub_A001E89E_lifted();
        const uint32_t result = g_eax, ecx = g_ecx, edx = g_edx;
        const uint32_t fk = g_flag_k, fa = g_flag_a, fb = g_flag_b, fc = g_flag_cf;
        uint32_t pointer, size;
        if (text && (int32_t)result >= 0 && shader && asm_buffer(MEM32(shader), &pointer, &size)) {
            const uint8_t *tokens = (const uint8_t *)GPTR(pointer);
            if (e) {   /* check mode: the remembered tokens against this assembly */
                if (size != e->size || memcmp(tokens, e->tokens, size)) {
                    if (++asm_mismatches <= 8) void_log("[shaderasm] check: %u-byte text assembled to %u bytes, remembered %u differ",
                                                         len, size, e->size);
                } else {
                    ++asm_hits;
                }
            } else if (asm_entries < ASM_SLOTS * 3 / 4 && asm_bytes + key_len + size <= ASM_TOTAL_LIMIT &&
                       (e = asm_find(hash, key_len, flags, text, 1)) != NULL && !e->text) {
                e->text = (uint8_t *)malloc(key_len);
                e->tokens = (uint8_t *)malloc(size ? size : 1);
                if (e->text && e->tokens) {
                    memcpy(e->text, text, key_len);
                    memcpy(e->tokens, tokens, size);
                    e->hash = hash; e->len = key_len; e->flags = flags; e->size = size;
                    e->messages = errors && !MEM32(errors) ? 1 : 0;
                    asm_bytes += key_len + size;
                    ++asm_entries;
                } else {
                    free(e->text); free(e->tokens);
                    e->text = e->tokens = NULL;
                }
            } else if (e && e->text && errors && !MEM32(errors)) {
                e->messages = 1;
            }
        }
        if (text && !e && errors && (int32_t)result >= 0 && !MEM32(errors)) {
            asm_entry *known = asm_find(hash, key_len, flags, text, 0);
            if (known) known->messages = 1;
        }
        g_eax = result; g_ecx = ecx; g_edx = edx;
        g_flag_k = fk; g_flag_a = fa; g_flag_b = fb; g_flag_cf = fc;   /* as the lifted body returned */
    }
    free(text);
    asm_ns += void_monotonic_ns() - started;
    if (asm_calls <= 2 || !(asm_calls & 255))
        void_log("[shaderasm] %u assemblies, %u from %u remembered texts (%llu KiB), %.1f ms in all, %.1f ms for those%s",
                 asm_calls, asm_hits, asm_entries, (unsigned long long)(asm_bytes >> 10), asm_ns / 1e6, asm_hit_ns / 1e6,
                 mode == 2 ? " (check: assembled anyway)" : "");
}
