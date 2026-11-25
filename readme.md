# The Sims 2 Legacy for Android

Development build: **0.3-dev**.

An unofficial Android port of the PC version of **The Sims 2 Legacy Collection**.
The original x86 executable is statically recompiled to C and built for ARM64 and ARMv7,
with a Win32 compatibility runtime, native OpenGL ES and Vulkan renderers,
touch controls, gamepad input and an Android launcher.

Project repository: [phys-winner/pc_decomp_ts2](https://github.com/phys-winner/pc_decomp_ts2).

## Compatible game version

This project is compatible with **The Sims 2 Legacy Collection, Steam build
18015495**, released on **10 April 2025**:
[The Sims 2 Legacy Patch Notes #8 on SteamDB](https://steamdb.info/patchnotes/18015495/).
The APK bundles the matching original executable and DLL in `assets/runtime.zip`. Compatibility with
other executable revisions, older disc releases, Ultimate Collection and mods
has not been established.

| Original PC file used by the build | SHA-256 |
| --- | --- |
| `EP9/TSBin/Sims2EP9.exe` | `7b65c2e8cb3100368d3e10db422768d99ef07169a7cee3aa30e6908aefd0ea88` |
| `EP9/TSBin/ijl15.dll` | `41d37bfb0a39017497e0aa373dc01f71ac9c5046c75f7cd269bee52112a5f3a1` |

Use your own installed copy of the game. The source repository excludes original
EXE/DLL files, game packages, generated game code, saves and personal settings.
The APK requires the data packages from your game installation.
Port resource overlays are generated from the user's original files during the build
and embedded in the APK.

## Requirements

- Android **10 / API 29 or later** on ARM64 or an experimental ARMv7 device with Thumb-2, NEON and 4 KiB pages.
- **OpenGL ES 3.2**. The app targets API 37 and includes `arm64-v8a` and `armeabi-v7a` libraries.
- The data from a compatible Legacy Collection installation, including `Base`
  and the expansion and stuff-pack folders.
- Enough storage for the original game, saves, optional converted textures and
  the app's native libraries. The APK carries both builds of the engine as XZ
  in `assets/native`. On first use it unpacks the selected engine into the app's
  files (about 191 MB for ARM64 or 154 MB for ARMv7). Texture conversion requires extra space.
- Storage access granted through the launcher; Android 11+ uses All files access.

The APK is aligned for 16 KiB pages. ARM64 native libraries use 16 KiB alignment,
and ARMv7 native libraries use 4 KiB alignment. Renderer requirements are additional
to the minimum Android version.

## Install and play

1. Install the APK from `pub` on the Android device.
2. Copy the installed PC game folder to the device, for example
   `/sdcard/Sims2/game`, preserving the `Base`, `EP1` through `EP9` and `SP`
   directory structure. The launcher can also import a ZIP of that folder.
3. Open **The Sims 2** and grant the requested storage access.
4. Select the installed game folder or its ZIP in the launcher's Game files section.
   Select the folder containing `Base` and `EP9`, rather than either subfolder.
5. For a ZIP stored without compression, choose **Play from the archive** in the
   import dialog. Keep the ZIP in its selected location. The launcher creates a
   small file index; the engine reads game files directly from the archive. A
   few compressed entries may be unpacked into a separate sidecar (up to 256 MiB);
   a fully compressed archive must be unpacked instead.
6. Choose the language and renderer and press **Play** when the launcher reports
   that the files are ready. Texture conversion works with folders and mounted ZIP archives. The launcher
   checks only one texture package; full discovery runs only when conversion starts,
   in the background with progress and cancellation. For ZIP installations the
   converted cache stays in the separate sidecar; the original ZIP is never changed.

The launcher accepts game files on a mounted SD card. Select the installed
folder or ZIP from the card in **Game files**. For FAT32 cards, copy the game
folder because FAT32 cannot hold a single ZIP of 4 GiB or more.

The engine reads its bundled EXE and DLL from private app storage, where it also
installs the port overlays. A game folder or ZIP containing the data packages
is sufficient.

The ZIP is read-only. New or modified installation files are stored separately
in the app's sidecar; original data is copied there only when actually written.
User-profile templates are copied into the writable profile on first launch,
just as for an unpacked installation. Reads, profile copies and their flushes
release guest execution so that the audio decoder can refill its stream.

The default writable profile is `/sdcard/Sims2/userdata`. Back up this directory
before replacing the app or experimenting with game modifications. Preserve the
APK signing key when building updates; a different certificate cannot update
an existing installation in place.

## Controls and settings

- Tap to interact. In Live Mode a tap checks a small area for nearby targets.
  A single object opens its actions immediately. With several objects, choose
  one from the list, which also includes the ground if the tap area hits it.
  A tap on the ground alone opens its actions directly.
- Drag one finger to pan the town or lot camera. Pinch to zoom, twist to rotate,
  and move two fingers vertically to tilt.
- Drag a Buy/Build catalog object onto the lot to preview it. The on-screen
  rotate, confirm and cancel buttons control placement; releasing a drag does
  not purchase the object. A brief two-finger world tap cancels selection.
- Hold an ordinary interface element for a right click. Hold a catalog thumbnail
  to show its description. Swipe lists, text, catalogs and category strips.
- Hold the **...** control to open grouped cheats and the editable command console.
- The **?** control in the original Options screen opens interface settings and
  gesture help. Interface scale is available in the game's Graphics Options.
- Android gamepad support includes a left-stick cursor, right-stick camera,
  trigger zoom, shoulder rotation and selection/placement buttons.
- The launcher selects game language, resolution, frame limit and supported
  renderer options. The game supports 21 language choices; the launcher UI
  supports English, Russian, German and Polish.

- Loading does not wait for the loading screens' animations: on PC the town and
  lot screens hold every loading step until their progress animation ends (about
  320 ms a step). The animations still play; on the Redmi Note 8 Pro Veronaville
  loads in 6.4-6.9 s instead of 10.2-10.6 s and a lot in 28.9 s instead of
  31.0-31.4 s. The startup screen is bound by its real work and is unchanged.
- The launcher's **Mods folder** card shows where The Sims 2 reads `.package`
  mods: `Sims2/userdata/Documents/EA Games/The Sims 2 Legacy/Downloads` in shared
  storage (subfolders work too); the launcher creates it.

**Adaptive interface is on by default.** Its checkbox in the launcher and in-game
port settings saves the choice for the next launch. **Regular UI layouts for mods**
selects the layouts targeted by most UI mods on the next launch, including when
the current UI scale would otherwise choose the compact layouts.

Family and lot thumbnails appear as their slots are filled between frames.
An individual uncached family portrait can still stall a frame.

## TS2 Extender

Enable **TS2 Extender** in the launcher's options to use the ARM64 adaptation of
[TS2 Extender 1.0.0 for Legacy Collection](https://github.com/LazyDuchess/TS2-Extender/releases/tag/1.0.0).
The switch defaults to off. The game reads the switch and the INI at startup.

- **TS2Extender.ini** opens the editor. It saves to `Sims2/userdata/TS2Extender.ini`
  in shared storage. The editor starts with the imported INI, or with the
  [upstream field names and defaults](https://github.com/LazyDuchess/TS2-Extender/blob/1.0.0/files_lc/TSBin/TS2Extender.ini).
  A saved profile INI takes precedence over the copy in the ZIP.
- **Import LC ZIP** accepts the original Legacy Collection archive and stores it
  as `Sims2/userdata/TS2Extender.zip`. While enabled, the game reads the date,
  timing and wall cutaway packages from it. The OFB uniforms fix uses its
  companion package when `FixOFBUniform` is enabled and the game loads custom
  content. Choose **Enable Custom Content** in the game's startup dialog and
  restart if the game reports that custom content is disabled.
  The port uses a file of the same name in the ordinary Downloads folder
  if the player has installed their own copy.
- On Android, `Console` enables verbose logging in `sims2.log`.
  `UIScaleResolution` sets the reference height for the port's UI scale
  when `UIScale` is enabled. The two **Sims 3 Camera** sensitivity values
  apply to the rotation and tilt gestures. **Splash** uses the image from the
  imported archive while the game starts.
- `ExtendedLua` enables additional libraries in the game's existing Lua VM.
  Mods that require the extender's additional Lua functions and script loader
  still require the Windows version. The port uses its own graphics settings
  interface and its statically compiled game code.

QA profiles keep their own INI and ZIP. The launcher switch also applies to
QA runs unless the test specifies `--es qa_ts2_extender 0` or
`--es qa_ts2_extender 1`. QA launches always skip both intro movies.

## Renderers

| Mode | Requirements and behavior |
| --- | --- |
| OpenGL ES | The port's native OpenGL ES 3.2 renderer; the default. |
| Vulkan | The port's native Vulkan 1.1 renderer, offered when the launcher's probe finds a Vulkan 1.1 driver. |

Both renderers (`android/render`) draw the game's Direct3D 9 calls directly: the
fixed-function pipeline runs as two port shaders driven by constants. The game's
shaders come from a shader library built with the APK (`tools/build_shaderlib.py`,
from shader bytecode captured on a device); a shader the library lacks, such as a
mod's, is translated on the device when first used (Vulkan keeps the compiled
SPIR-V in the app's cache, OpenGL ES the linked program binaries). DXVK and its
Vulkan drivers (Turnip, PanVK) are no longer part of the port.

The launcher's render options apply to both renderers: game resolution (the
render scale), render upscaling (bilinear or FSR 1), FXAA, scene MSAA (2x or 4x)
and FSR 1 for interface textures. FSR 3.1 was removed because it was too slow on
phones; a profile that chose it now uses FSR 1. `ProcessVertices` and multiple
render targets work in both renderers.

Where the GPU cannot sample the game's DXT textures (Mali, and OpenGL ES on
Adreno), both renderers decode them to RGBA8 while loading unless the launcher's
**Textures** card has converted them to ETC2 or ASTC 4x4: the converted levels then
go to the GPU as they are, 4-8 times smaller. On a Redmi Note 8 Pro lot the
textures took 121 MB (ETC2) or 127 MB (ASTC) instead of 201 MB, and the process
85-88 MB (OpenGL ES) or 126-167 MB (Vulkan) less memory. ASTC keeps more detail;
ETC2 converts faster and takes less storage. Where the GPU samples DXT (BC), the
textures stay DXT and the card offers no conversion. The earlier Direct3D 9 -> OpenGL ES layer
remains in the code as the `legacy` renderer for comparison (the QA intent extra
`renderer legacy`); the launcher does not offer it.

## Audio

The game's own mixer, EA SND inside the original executable, renders straight
into an AAudio stream in the low-latency performance mode. DirectSound is not
emulated: SND's PC output driver is replaced natively (`android/host/sims2_audio.c`),
and the stream's data callback drains a FIFO that SND fills on demand
(`android/host/audio.c`). SND mixes at the device's output rate when it lies in
SND's 11025–48000 Hz range (normally 48 kHz), so the port does not resample.
The FIFO's reserve adapts: an underrun widens it at once by how late the mixer
was, and quiet periods narrow it again. A 10 Hz high-pass at the FIFO removes
the constant offset SND leaves while the game is paused (it holds paused voices
at their last sample), so stopping or starting the stream does not click.

SND's service thread is urgent for the guest machine: the holder hands over at
its next loop back-edge, and the owner of SND's lock is served first while the
mixer waits for it. A D3D call waiting for the render thread (a
`GetRenderTargetData` screen grab, `ProcessVertices`) lends the machine to the
mixer, wherever the mixer waits in the queue, or to the owner of the lock the
mixer waits for until that owner releases it; the borrower parks its ticket,
runs, hands the machine back, and the other threads keep waiting for the
lender. A waiter sleeps on its own condition, and a release wakes only the
owner of the next ticket. Slow host work does not keep the machine: cache
write-back and truncation in `SetEndOfFile`/`FlushFileBuffers`, the unlink in
`DeleteFile`, registry writes (a writer thread, flushed before a pause or an
exit), and the memory report's `mincore`; the native QFS decoder and window
search offer the machine like the loops they replace. An urgent wait over 20 ms
logs a `[machine]` line with the holder's function, its last import or method,
the threads ahead of the mixer, the loans so far and the holder's host program
counter and return addresses (sampled with a signal); a `[machine] last 10 s`
line counts slow handoffs.

The stream closes in the background and reopens on return; the game's sounds
stay paused meanwhile. A changed output route (headphones, Bluetooth, an output
capture) reopens the stream, and a failed open is retried after 20 ms, doubling
up to 2 s. A new stream's first 200 ms (its prefill) do not count as underruns
and do not grow the device buffer. A low-latency stream whose callback stops
twice is reopened in Android's normal mode until the route changes. Each
session log has a 10-second `[audio]` summary with the latency, underruns, the
offset per channel and the mixer's response time, and a `[latency]` line with
the age of the mix when it is heard (from SND's commit to the stream's
presentation timestamp); `sims2.audio.log` in the profile records stream
events.

## Development status

Representative development checks cover startup, town and household loading,
CAS, Buy/Build/Live navigation, saving/loading, touch input, audio and app resume
on selected Mali, Adreno and PowerVR devices. These checks do not establish
whole-game completion or compatibility with every phone, GPU, locale or mod.
First-use shader compilation, original UI callbacks and asset loading can still
stall. This is a development release; the APK has debugging disabled
(`android:debuggable="false"`). Developer tools that use `run-as`, including
`--push-lib`, require a separate debuggable APK.

## Build requirements

The current build scripts target **Windows with PowerShell** and require:

- Python 3.13 or later, Git, and the packages in `requirements.txt`.
- Android SDK build-tools **36.1.0**, platform **android-37.0**, platform-tools,
  and Android NDK **27.2.12479018**.
- JDK 24; set `JAVA_HOME` to its installation directory.
- The compatible original game installed at `C:/Games/The Sims 2 Legacy`.
  `tools/build_overlay.py` currently uses this fixed path. If you keep the game
  elsewhere, update its `GAME` constant and `port.json` consistently.
- A signing keystore at `build/android/debug.keystore`. Keep the existing key
  for updates. A new development installation may use its own key with alias
  `androiddebugkey` and the Android development passwords expected by the packager.
  Keys are never tracked by Git.

The shared launcher source is included in `android/launcher`. Set `PORT_LAUNCHER`
to use a separate compatible launcher tree. Third-party source dependencies
are pinned Git submodules under `reference`: pcrecomp, MojoShader, ASTC encoder,
etcpak and dlmalloc. The small port changes for pcrecomp,
MojoShader and the retained dlmalloc notice live in `patches/`.
Dependency origins, revisions and binary hashes are recorded in
[`dependencies.json`](dependencies.json).

The native Vulkan renderer compiles its shaders with glslang, built from the
pinned NDK's shaderc sources (`tools/build_android.py`); no other GPU runtime
is imported.

Initialize dependencies after cloning (or clone with `--recurse-submodules`):

```powershell
git submodule update --init --recursive
python -X utf8 -B tools/prepare_dependencies.py
```

The preparation tool verifies every submodule commit, applies each port patch
once and checks its resulting source hash. Repeating it is safe; unexpected
local edits or a different revision are rejected without overwriting them.
The native, lifter and texture-converter builders also prepare the dependencies
they use. `reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c` retains exactly the
allocator implementation used by this port; ASTC library sources are under
`reference/astcenc/Source`.

See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) before distributing artifacts.

## Generate and build

Run from the repository root:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt

$env:ANDROID_SDK = Join-Path $env:LOCALAPPDATA "Android\Sdk"
$env:ANDROID_NDK = Join-Path $env:ANDROID_SDK "ndk\27.2.12479018"
$env:JAVA_HOME = Read-Host "JDK 24 installation directory"

Copy-Item port.example.json port.json
New-Item -ItemType Directory -Force original | Out-Null
Copy-Item "C:/Games/The Sims 2 Legacy/EP9/TSBin/Sims2EP9.exe" original/
Copy-Item "C:/Games/The Sims 2 Legacy/EP9/TSBin/ijl15.dll" original/

python -X utf8 -B tools/lift_sims2.py --jobs 12
python -X utf8 -B tools/build_android.py --shared --jobs 12 --no-pgo
python -X utf8 -B tools/build_android.py --abi arm32 --shared --jobs 12
python -X utf8 -B tools/build_game_apk.py --skip-native
```

Create or restore `build/android/debug.keystore` before packaging. For a new
independent development key only:

```powershell
New-Item -ItemType Directory -Force build/android | Out-Null
& "$env:JAVA_HOME/bin/keytool.exe" -genkeypair -keystore build/android/debug.keystore `
    -alias androiddebugkey -storepass android -keypass android `
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug"
```

The lifter verifies the original PE hashes and uses the committed function and
switch catalogs. Generated C and checked PE images are local outputs under
`generated/`. The APK is written to **`build/android-game/sims2.apk`**.
The APK packager also builds resource overlays, texture conversion and the Vulkan
probe.

Existing workspaces can retain their selected PGO profile and symbol order:
`tools/build_android.py --shared --jobs 12` uses
`build/pgo/abb24fae-20261022.profdata` and `build/pgo/order.txt` when present.
These local optimization inputs are excluded from Git. A fresh checkout can
build with the explicit `--no-pgo` command above.

The native renderers' shader library is optional: without it they translate every
game shader on the device when first used. The game builds its shaders from its
own material text at run time, so the library is made from bytecode a device saw:
run the game once with `adb shell setprop debug.s2.shaderdump 1` (it writes the
shaders the library lacks into the profile's `shaderdump` folder), then

```powershell
python -X utf8 -B tools/build_shaderlib.py capture DEVICE_SERIAL userdata
python -X utf8 -B tools/build_shaderlib.py build
```

The capture is kept in `generated/shaders/capture` (private, like the lifted
code); `tools/build_game_apk.py` rebuilds the library when its inputs change and
packs it into the APK. Shaders it lacks, such as a mod's, are still translated on
the device. The host translator needs Visual Studio's C tools (for `clang-cl` of
the NDK); the SPIR-V comes from the NDK's `glslc`.

Installation is optional and requires an explicit target:

```powershell
python -X utf8 -B tools/build_game_apk.py --skip-native --install --serial DEVICE_SERIAL
```

Use a separate QA profile for device tests. The game skips both startup movies
when launched with a `userdata-qa-*` profile.

```powershell
adb -s DEVICE_SERIAL shell am start -n org.sims2.port/.GameActivity --es userdata userdata-qa-touch --es renderer gles
```

## Local release artifacts

Version `0.3.1-dev` and version code `8` are stored in
`android/app/AndroidManifest.xml`. Increase the code for subsequent published updates.

Package a published release from its reviewed commit so that the launcher's
`assets/launcher/build.json` records that source commit. Name the artifact
**`pub/sims2-0.3.1-dev-COMMIT.apk`**, where `COMMIT`
is the first eight characters of the Git commit, with a matching `.apk.sha256`.
For a local build with uncommitted changes, append `-dirty` to the commit suffix.

```powershell
$env:PORT_LAUNCHER = (Resolve-Path android/launcher).Path
python -X utf8 -B tools/build_game_apk.py --skip-native
$sims2Commit = git rev-parse --short=8 HEAD
if (git status --porcelain) { $sims2Commit += "-dirty" }
$sims2ApkName = "sims2-0.3.1-dev-$sims2Commit.apk"
New-Item -ItemType Directory -Force pub | Out-Null
Copy-Item build/android-game/sims2.apk (Join-Path pub $sims2ApkName)
$sims2Hash = (Get-FileHash (Join-Path pub $sims2ApkName) -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Encoding ascii (Join-Path pub "$sims2ApkName.sha256") "$sims2Hash  $sims2ApkName"
```

The release APK includes both ABIs. Android selects the app process ABI, and
the app unpacks that ABI's engine on first use. The ARM32 variant encodes
textures to ETC2. The bundled Vulkan drivers are for ARM64. Pass `--abi arm32`
or `--abi arm64` to `tools/build_game_apk.py` for a single-ABI development build.

`pub`, build outputs, originals, signing keys and machine-specific `port.json`
remain outside Git. This procedure creates local artifacts only.

## Focused host checks

```powershell
python -X utf8 -B tools/check_dependencies.py
python -X utf8 -B tools/check_game_assets.py
python -X utf8 -B tools/check_game_preferences.py
python -X utf8 -B tools/check_renderer_defaults.py
python -X utf8 -B tools/check_port_options.py
python -X utf8 -B tools/check_option_warning.py
python -X utf8 -B tools/check_overlay.py
python -X utf8 -B tools/check_apk_profile.py
```

The integrated native ZIP check can be built with
`python -X utf8 -B tools/build_host_test.py tools/test_zip_game.c --output build/test/zip-game/check-zip-game --wrap void_module_exe_base --wrap fsync`
and run on a selected ARM64 device. It covers offsets above 4 GiB, bounded and
overlapped reads, independent seeks, directory metadata, mappings, profile
copies, copy-on-write and guest audio progress during a slow flush.

Additional `tools/check_*.py` scripts cover native runtime, rendering and input
contracts. Some require generated modules, original game files or a selected
physical device; they are development tools rather than a universal test suite.
`tools/check_audio_streaming.py` runs the actual audio output and SND driver C
against a simulated AAudio device (MSVC and the NDK's `clang-cl`);
`tools/check_machine_yield.py` builds an ARM64 binary for the machine hand-over,
including the audio producer's urgency and the loans to it, to run on a device;
`tools/check_file_audio_yield.py` does the same for file calls that release
the machine.

## License

Original source code written for this port and owned by the project authors is
licensed under the [GNU General Public License, version 3 only](LICENSE)
(`GPL-3.0-only`). Third-party components and game content are subject to their
respective rights holders' terms.

## Credits and third-party code

This is an independent project and is not affiliated with or endorsed by
Electronic Arts, Maxis or Valve. The Sims and related game content belong to
their respective owners. Third-party notices and retained license locations are
listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Those licenses do
not grant redistribution rights to the original game or generated game code.
