# Port launcher base

A template launcher that every static-recompilation port of this family builds into its own
APK: game files from a folder or a ZIP (unpacked, or played straight from a stored ZIP), several
distributions, renderer choice (OpenGL ES, DXVK, Turnip, also with the player's own Turnip driver package) with
capability checks, cleanup of unused files,
texture conversion, GitHub update checks passed on to the game, the port's version and GitHub link.
English, Russian, German and Polish. Target SDK 37.

On Qualcomm/Adreno devices, the renderer section can download Turnip drivers for DXVK from the
same seven GitHub repositories as Frostpunk. Players can browse the ten newest releases, read their
notes and choose a ZIP package or .so library. Downloads use the standard progress panel, cancellation
and foreground service, with a 256 MiB limit; the selected driver is checked again before use.
Release lists are cached for six hours (with an offline fallback), and downloaded libraries are cached
by SHA-256 for offline reuse. Resetting to the built-in driver keeps the cache; About offers deletion
of unused external drivers and preserves the active one. No extra port.json settings are needed for a
`turnip` mode; the manifest generator adds Internet access even when APK updates are disabled.

The launcher and game editions share one Android task and one Recents card. Game activities use
`singleTop` and inherit the application's task affinity; native libraries still run in their separate
processes. Opening the launcher or game removes duplicate tasks left by earlier APKs.

The base's Java sources live in the template package `port.launcher` and are rewritten into the game's own
package (`<application id>.launcher`) at build time; a game describes itself in `android/port.json`.

Game cards show readiness and Play; paths, folder/archive selection and rechecking belong only to the
folding Game files section. Missing data opens that section once with gently pulsing import buttons;
manual folding is respected until the data state or selected edition changes. Finding the data folds
the guided section again. A ready Play button has a game-coloured shimmer and rising particles,
with optional palettes for individual editions. These effects
respect the system animation setting and stop redrawing when disabled, hidden or out of focus.

Driver checks leave the cards in place while they run, so tapping and scrolling do not wait for
the check to finish. Background results refresh only the requested sections after the current
touch gesture ends; unrelated settings and cards stay attached.

| Path | What |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | Decisions, analysis of the mature ports, architecture, status (Russian) |
| [docs/INTEGRATION.md](docs/INTEGRATION.md) | Adding the base to a game; port.json reference; the UT3 example (Russian) |
| [docs/UPDATES.md](docs/UPDATES.md) | Update checks through GitHub releases (Russian) |
| [docs/ICONS.md](docs/ICONS.md) | Icon requirements and checks (Russian) |
| [docs/PLAY_BUTTON.md](docs/PLAY_BUTTON.md) | Play button colours, particles, animation and per-edition palettes (Russian) |
| `android/src/port/launcher/` | The launcher (Java 8 language level, no Gradle, no AndroidX) |
| `native/` | `port_env.h` (what the launcher passes to native code), `zipmount.c` (stored-ZIP file layer), `vulkan_probe.c` (the Vulkan driver probe run in `:probe`) |
| `tools/launcher.py` | Renders the sources, expands the manifest markers, writes assets, builds the probe library |
| `tools/icons.py` | Checks adaptive icons (layers, safe zone) |
| `tools/update_manifest.py`, `ci/update-manifest.yml` | Builds `update.json` from releases and attaches it to the latest one |
| `examples/homm5.port.json` | A multi-distribution specification |

```powershell
python tests/run_tests.py                                   # Java selfcheck, zipmount, probe (NDK), tools
python tools/launcher.py check --spec GAME/android/port.json
python tools/icons.py check GAME/android/app/res
python tools/update_manifest.py --repo OWNER/NAME --out update.json --state versions.json
```

Heroes of Might and Magic V (three editions, Android 8+) builds on the base as well; see docs/INTEGRATION.md §8.

Status: the base and the Unreal Tournament 3 integration build and run on the test Pixel 10a (Android 17,
Mali-G715): first start without the game's library in `:launcher`, permissions, folder and ZIP import with the
system picker and the built-in browser, copy and move, the Vulkan probe, languages, Play and back.
