# Third-party notices

The port retains the existing notices in submodule source files and port patches and packages
component licenses in `assets/licenses` inside the APK. Dependency hashes and
origins are recorded in [dependencies.json](dependencies.json).

| Component | License / notice location |
| --- | --- |
| pcrecomp tools and runtime | MIT; `reference/pcrecomp/LICENSE`. The submodule plus `patches/pcrecomp.patch` preserves local lifter changes on base revision `904ac3129d0c9d05efbeec19a1dce6abbe7f5d26`. |
| MojoShader | Zlib; `reference/mojoshader/LICENSE.txt`. |
| dlmalloc | MIT-0; notice at the beginning of `reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c`. |
| ASTC encoder | Apache-2.0; `reference/astcenc/LICENSE.txt`. |
| etcpak | BSD-3-Clause; `reference/etcpak/LICENSE.txt`. Its bundled `bcdec.h` is MIT OR Unlicense. |
| FidelityFX FSR 1 | MIT; retained in `android/host/fsr1.h`. |
| Snowapril FSR 3 Android / AMD FidelityFX | MIT; pinned checkout's `reference/fsr3-android/sdk/LICENSE.txt`, copied into the APK. No frame generation is implemented by this port. |
| DXVK Native | Zlib; `reference/dxvk/LICENSE`. MinGW-w64 DirectX header notices are retained in `reference/dxvk/include/native/directx`. |
| adrenotools and linkernsbypass | BSD-2-Clause; pinned local runtime licenses copied into the APK. |
| Android NDK libc++ | Apache-2.0 WITH LLVM-exception; NDK `NOTICE`, copied into the APK. |
| Mesa Turnip community binary | See the distribution limitation below and the APK's `assets/licenses/turnip-mesa.txt`. |
| Mesa PanVK (mali-panvk, patched Frostpunk build) | Pinned component notices in the APK's `assets/licenses/PanVK-JimVulkan-NOTICE.txt`. |

The bundled launcher is a source snapshot of the common launcher used by this
family of ports. Its source is retained in `android/launcher`; no separate
license grant is inferred for code without an explicit license notice.

## Known distribution limitation

The pinned Mesa Turnip 26.0.0 R8 community asset does not contain a complete
component license manifest. The existing `Turnip-LICENSE-NOTE.txt` explicitly
records that limitation. The APK packages this note, but it is not a replacement
for all applicable component licenses. The current build is prepared locally;
a complete notice manifest and the corresponding modified third-party runtime
sources/build instructions must be resolved before public binary distribution.

Original game files are excluded from the source repository. Recompiled game
code and resource overlays are derived from the original game; the licenses of
the tools and runtime do not relicense that content.
