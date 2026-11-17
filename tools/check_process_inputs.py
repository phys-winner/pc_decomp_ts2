"""Check actual ProcessVertices stream bounds and GL binding on AArch64.

Uses mocked GL calls; no device or graphics context. Requires Unicorn and
pyelftools. The game captures use nonzero strides; multi-vertex constant
streams are explicitly unsupported until a constant-attribute path is added.
"""
import subprocess
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0
from elftools.elf.elffile import ELFFile
from build_android import CLANG, ROOT

out = ROOT / 'build/test/processvertices/inputs'
out.mkdir(parents=True, exist_ok=True)
gl = (ROOT / 'android/host/d3d9_gl.c').read_text()
helpers = gl[gl.index('typedef struct { GLint size; GLenum type; GLboolean norm; } attr_format;'):gl.index('static int bgra_of(')]
start = gl.index('int d3d_process_inputs(')
actual = gl[start:gl.index('\n}\n', start) + 3]
source = '#include "d3d9_internal.h"\n' + helpers + actual + r'''
static d3d_device device;
static d3d_decl decl;
static d3d_buffer buffer;
static uint32_t calls, stride_seen;
static uintptr_t offset_seen;
void glBindBuffer(GLenum target, GLuint object) { ++calls; }
void glEnableVertexAttribArray(GLuint index) { ++calls; }
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *pointer) {
    ++calls; stride_seen = stride; offset_seen = (uintptr_t)pointer;
}
uint32_t selftest(void) {
    MOJOSHADER_attribute attribute = {MOJOSHADER_USAGE_POSITION, 0, "vs_v0"};
    MOJOSHADER_parseData pd = {0}; pd.attribute_count = 1; pd.attributes = &attribute;
    device.st.decl = &decl; device.st.streams[0] = &buffer;
    decl.count = 1; decl.elements[0].type = 2; /* FLOAT3 */
    buffer.size = 12;
    /* One vertex may use GLES stride zero safely. */
    if (!d3d_process_inputs(&device,&pd,0,1,1) || calls != 3 || stride_seen || offset_seen) return 1;
    calls = 0;
    if (d3d_process_inputs(&device,&pd,0,2,0) || d3d_process_inputs(&device,&pd,0,2,1) || calls) return 2;
    device.st.stream_stride[0] = 12; buffer.size = 24;
    if (!d3d_process_inputs(&device,&pd,0,2,1) || calls != 3 || stride_seen != 12 || offset_seen) return 3;
    calls = 0;
    if (d3d_process_inputs(&device,&pd,1,2,1) || calls) return 4;
    if (!d3d_process_inputs(&device,&pd,1,1,1) || offset_seen != 12) return 5;
    /* Combined stream/element/source offsets and an exact buffer-end read. */
    device.st.stream_stride[0] = 16; device.st.stream_offset[0] = 8;
    decl.elements[0].offset = 4; buffer.size = 40;
    if (!d3d_process_inputs(&device,&pd,1,1,1) || offset_seen != 28 || stride_seen != 16) return 6;
    if (d3d_process_inputs(&device,&pd,1,2,0)) return 7;
    buffer.size = UINT32_MAX;
    if (d3d_process_inputs(&device,&pd,UINT32_MAX,UINT32_MAX,0)) return 8;
    device.st.stream_stride[0] = UINT32_MAX;
    if (d3d_process_inputs(&device,&pd,UINT32_MAX,UINT32_MAX,0)) return 9;
    device.st.stream_stride[0] = 16; device.st.streams[0] = NULL;
    if (d3d_process_inputs(&device,&pd,0,1,0)) return 10;
    device.st.streams[0] = &buffer; decl.elements[0].stream = 16;
    if (d3d_process_inputs(&device,&pd,0,1,0)) return 11;
    decl.elements[0].stream = 0; decl.elements[0].type = 17;
    if (d3d_process_inputs(&device,&pd,0,1,0)) return 12;
    return 0;
}
'''
(out / 'check.c').write_text(source)
binary = out / 'check.elf'
subprocess.run([str(CLANG), '--target=aarch64-linux-android30', '-O2', '-std=gnu11', '-ffreestanding',
                '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-nostdlib', '-no-pie',
                '-Wl,-e,selftest', '-Wl,--build-id=none', '-Iandroid/runtime', '-Iandroid/host',
                str(out / 'check.c'), '-o', str(binary)], cwd=ROOT, check=True)
mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
with binary.open('rb') as stream:
    elf = ELFFile(stream)
    for seg in elf.iter_segments():
        if seg['p_type'] != 'PT_LOAD':
            continue
        lo = seg['p_vaddr'] & ~4095
        hi = (seg['p_vaddr'] + seg['p_memsz'] + 4095) & ~4095
        mu.mem_map(lo, hi - lo)
        mu.mem_write(seg['p_vaddr'], seg.data())
    entry = elf['e_entry']
mu.mem_map(0x10000000, 0x40000)
mu.reg_write(UC_ARM64_REG_SP, 0x1003FFF0)
mu.reg_write(UC_ARM64_REG_LR, 0xF0000000)
mu.emu_start(entry, 0xF0000000, timeout=30000000)
assert mu.reg_read(UC_ARM64_REG_PC) == 0xF0000000, 'stream-input check timed out'
assert mu.reg_read(UC_ARM64_REG_X0) == 0, f'stream-input check failed: {mu.reg_read(UC_ARM64_REG_X0)}'
print('PASS: actual AArch64 input helper: stride-zero rejection, one vertex, packed stride, offsets, '
      'exact end, overrun/overflow, missing/invalid streams and declarations; mocked GL binding')
