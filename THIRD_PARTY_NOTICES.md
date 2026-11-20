# Third-party notices

The port retains the existing notices in submodule source files and port patches and packages
component licenses in `assets/licenses` inside the APK. Dependency hashes and
origins are recorded in [dependencies.json](dependencies.json).

| Component | License / notice location |
| --- | --- |
| pcrecomp tools and runtime | MIT; `reference/pcrecomp/LICENSE`. The submodule plus `patches/pcrecomp.patch` preserves local lifter changes on base revision `904ac3129d0c9d05efbeec19a1dce6abbe7f5d26`. |
| MojoShader | Zlib; `reference/mojoshader/LICENSE.txt`. |
| glslang (Khronos Group) | BSD-3-Clause, with some files under other permissive licenses. Compiled from the pinned Android NDK's shaderc sources into `libsims2.so` for the native Vulkan renderer's run-time shader translations; the license headers of the compiled files are collected into the APK's `assets/licenses/glslang.txt`. |
| dlmalloc | MIT-0; notice at the beginning of `reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c`. |
| ASTC encoder | Apache-2.0; `reference/astcenc/LICENSE.txt`. |
| etcpak | BSD-3-Clause; `reference/etcpak/LICENSE.txt`. Its bundled `bcdec.h` is MIT OR Unlicense. |
| FidelityFX FSR 1 | MIT; retained in `android/host/fsr1.h`. |
| Android NDK libc++ | Apache-2.0 WITH LLVM-exception; linked statically into `libsims2.so` (glslang is C++). The LLVM exception covers object code embedded this way. `libc++_shared.so` (the adrenotools libraries' runtime) is packaged with the NDK `NOTICE` as `assets/licenses/libcxx-ndk.txt`. |
| adrenotools and linkernsbypass | BSD-2-Clause; the pinned runtime licenses are copied into the APK (`assets/licenses/adrenotools.txt`, `linkernsbypass.txt`). They load the app-local Vulkan drivers of the native Vulkan renderer. |
| Mesa Turnip community binary | See the distribution limitation below and the APK's `assets/licenses/turnip-mesa.txt`. |
| Mesa PanVK (mali-panvk, patched Frostpunk build) | Pinned component notices in the APK's `assets/licenses/PanVK-JimVulkan-NOTICE.txt`. |

The bundled launcher is a source snapshot of the common launcher used by this
family of ports. Its source is retained in `android/launcher`; no separate
license grant is inferred for code without an explicit license notice.

## Known distribution limitation

The pinned Mesa Turnip 26.0.0 R8 community asset does not contain a complete
component license manifest. The packaged `turnip-mesa.txt` records that
limitation; it is not a replacement for all applicable component licenses. A
complete notice manifest and the corresponding third-party runtime
sources/build instructions must be resolved before public binary distribution.

## Game content

Original game files are excluded from the source repository. Recompiled game
code and resource overlays are derived from the original game; the licenses of
the tools and runtime do not relicense that content.
