# The Sims 2 Legacy for ARM64 Android

Development build: **0.1.1-dev**.

An unofficial Android port of the PC version of **The Sims 2 Legacy Collection**.
The original x86 executable is statically recompiled to C and built for ARM64,
with a Win32 compatibility runtime, OpenGL ES and optional DXVK renderers,
touch controls, gamepad input and an Android launcher.

Project repository: [phys-winner/pc_decomp_ts2](https://github.com/phys-winner/pc_decomp_ts2).

## Compatible game version

This project is compatible with **The Sims 2 Legacy Collection, Steam build
18015495**, released on **10 April 2025**:
[The Sims 2 Legacy Patch Notes #8 on SteamDB](https://steamdb.info/patchnotes/18015495/).
The port requires the matching original executable and DLL. Compatibility with
other executable revisions, older disc releases, Ultimate Collection and mods
has not been established.

| Required file in the installed game | SHA-256 |
| --- | --- |
| `EP9/TSBin/Sims2EP9.exe` | `7b65c2e8cb3100368d3e10db422768d99ef07169a7cee3aa30e6908aefd0ea88` |
| `EP9/TSBin/ijl15.dll` | `41d37bfb0a39017497e0aa373dc01f71ac9c5046c75f7cd269bee52112a5f3a1` |

Use your own installed copy of the game. The source repository excludes original
EXE/DLL files, game packages, generated game code, saves and personal settings.
The APK requires the full game installation; it does not include that installation.
Port resource overlays are generated from the user's original files during the build
and embedded in the APK.

## Requirements

- An **ARM64 Android** device running **Android 10 / API 29 or later**.
- **OpenGL ES 3.2**. The app targets API 37 and includes only `arm64-v8a` libraries.
- A complete installation of the compatible Legacy Collection, including `Base`,
  expansion and stuff-pack folders, and the original `EP9/TSBin` files.
- Enough storage for the original game, saves, optional converted textures and
  the app's extracted native libraries. Texture conversion requires extra space.
- Storage access granted through the launcher; Android 11+ uses All files access.

Native libraries and APK packaging use 16 KiB alignment. Renderer and upscaler
requirements are additional to the minimum Android version.

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

The engine reads the original EXE/DLL directly from the selected game installation.
Port resources are installed into private app storage automatically. A separate
`lifted` folder or manually copied resource overlay is unnecessary.

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

**Adaptive interface is on by default.** Its checkbox in the launcher and in-game
port settings saves the choice for the next launch. Original UI relayout and
thumbnail callbacks remain synchronous and can cause stalls.

## Renderers

| Mode | Requirements and behavior |
| --- | --- |
| OpenGL ES | Available on supported GLES 3.2 devices. Includes render scaling, optional UI FSR 1, FXAA, MSAA and spatial upscaling. |
| DXVK (Vulkan) | Android 11+ and a system Vulkan 1.3 driver with the required features, including the game's texture and query support. |
| DXVK (Turnip) | Android 11+, supported Adreno 6xx/7xx hardware and the bundled Mesa Turnip driver. The driver runs inside the app. |

The launcher probes the driver before offering Vulkan modes. With no saved choice,
it prefers verified system DXVK, then verified Turnip, then OpenGL ES. A saved
explicit choice takes priority. Driver capability checks do not guarantee correct
rendering: system DXVK showed incorrect character poses and family thumbnails on
the tested Adreno 735 device, while Turnip passed those scenarios. Adreno 6xx
Turnip uses system-memory rendering by default to avoid a reproduced queue hang.
OpenGL ES can be faster on that hardware.

Optional **FSR 3.1** uses the Snowapril Android SDK upscaler and optical flow,
requires Android 15 / API 35 or later plus suitable Vulkan/AHardwareBuffer
support, and falls back to FSR 1 when initialization or dispatch fails.
**Frame generation is not implemented.** The GLES filters and their settings do
not apply to DXVK.

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
- The pinned DXVK Native/adrenotools/Turnip binary inputs described below.
- A pristine checkout of the pinned FSR 3 Android source revision.
- A signing keystore at `build/android/debug.keystore`. Keep the existing key
  for updates. A new development installation may use its own key with alias
  `androiddebugkey` and the Android development passwords expected by the packager.
  Keys are never tracked by Git.

The shared launcher source is included in `android/launcher`. Set `PORT_LAUNCHER`
to use a separate compatible launcher tree. Third-party source dependencies
are pinned Git submodules under `reference`: pcrecomp, MojoShader, ASTC encoder,
etcpak, dlmalloc, DXVK and FSR 3 Android. The small port changes for pcrecomp,
MojoShader and the retained dlmalloc notice live in `patches/`.
Dependency origins, revisions and binary hashes are recorded in
[`dependencies.json`](dependencies.json).

The DXVK packager imports pinned native libraries; it does not build those
third-party libraries from source. Supply `libdxvk_d3d9.so`, the adrenotools/hook
libraries, `libs2turnip.so` and the license files under `reference/dxvk-runtime`
as required by `tools/build_dxvk.py`. These are local build inputs excluded from
Git. The DXVK headers come from its pinned `include/native/directx` submodule; the C++ runtime comes
from the NDK. All imported runtime binaries are checked against their SHA-256 pins.
The Turnip asset URL and component source revisions are in `dependencies.json`.

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

The FSR build downloads its pinned ARM64 static-library archive when it is absent
and verifies its hash. Internet access is needed for that first download. The
community Turnip binary has an incomplete component license manifest; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) before distributing artifacts.

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
The APK packager also builds resource overlays, texture conversion, the Vulkan
probe, the DXVK bridge and the isolated FSR module.

Existing workspaces can retain their selected PGO profile and symbol order:
`tools/build_android.py --shared --jobs 12` uses
`build/pgo/abb24fae-20261022.profdata` and `build/pgo/order.txt` when present.
These local optimization inputs are excluded from Git. A fresh checkout can
build with the explicit `--no-pgo` command above.

Installation is optional and requires an explicit target:

```powershell
python -X utf8 -B tools/build_game_apk.py --skip-native --install --serial DEVICE_SERIAL
```

## Local release artifacts

Version `0.1.1-dev` and version code `3` are stored in
`android/app/AndroidManifest.xml`. Increase the code for subsequent published updates.

Package a published release from its reviewed commit so that the launcher's
`assets/launcher/build.json` records that source commit. Name the artifact
**`pub/sims2-0.1.1-dev-COMMIT.apk`**, where `COMMIT`
is the first eight characters of the Git commit, with a matching `.apk.sha256`.
For a local build with uncommitted changes, append `-dirty` to the commit suffix.

```powershell
python -X utf8 -B tools/build_game_apk.py --skip-native
$sims2Commit = git rev-parse --short=8 HEAD
if (git status --porcelain) { $sims2Commit += "-dirty" }
$sims2ApkName = "sims2-0.1.1-dev-$sims2Commit.apk"
New-Item -ItemType Directory -Force pub | Out-Null
Copy-Item build/android-game/sims2.apk (Join-Path pub $sims2ApkName)
$sims2Hash = (Get-FileHash (Join-Path pub $sims2ApkName) -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Encoding ascii (Join-Path pub "$sims2ApkName.sha256") "$sims2Hash  $sims2ApkName"
```

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

## Credits and third-party code

This is an independent project and is not affiliated with or endorsed by
Electronic Arts, Maxis or Valve. The Sims and related game content belong to
their respective owners. Third-party notices and retained license locations are
listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Those licenses do
not grant redistribution rights to the original game or generated game code.
