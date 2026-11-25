"""Prepare the launcher base for a game's APK build.

    python tools/launcher.py prepare --spec GAME/android/port.json --manifest GAME/android/app/AndroidManifest.xml --out BUILD/launcher
    python tools/launcher.py check --spec GAME/android/port.json
    python tools/launcher.py probe --package org.example.port.launcher --out BUILD/lib/arm64-v8a

prepare writes into --out:
  src/<package>/*.java        the base's sources in the game's own Java package (app.javaPackage, by
                              default "<manifest package>.launcher"): the base is a template, every
                              game gets its own copy at build time
  AndroidManifest.xml         the game's manifest with its two markers expanded:
                                <!-- launcher:manifest -->     permissions and <queries> (inside <manifest>;
                                                               the vendors' game tools unless app.gameModeNotice is false)
                                <!-- launcher:application -->  the launcher's activities and services
                              (with a minSdkVersion below 30 also the storage permissions of Android 8-10
                              and requestLegacyExternalStorage for Android 10)
  assets/launcher/port.json   the specification the launcher reads at run time
  assets/launcher/build.json  versionName, versionCode, commit and build time
and prints a JSON summary. A build script imports prepare() instead. Standard library only.

probe compiles native/vulkan_probe.c into libportprobe.so with the Android NDK for the game's
launcher package: the Vulkan driver check behind the DXVK renderers (Probe.java). build_probe()
defaults to arm64-v8a with 16 KB pages; abi="arm32" is armeabi-v7a with 4 KB pages. A build script
calls build_probe() and puts the library beside the game's own.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "android/src/port/launcher"
TEMPLATE_PACKAGE = "port.launcher"
PROBE_SOURCE = ROOT / "native/vulkan_probe.c"
PROBE_LIBRARY = "libportprobe.so"
LANGUAGES = ("en", "ru", "de", "pl")
KINDS = ("gles", "vulkan", "turnip", "panvk")
PROBES = ("dxvk", "version")   # what the driver check asks of a Vulkan mode (Renderers.java)
BC_FORMATS = set("DXT1 DXT2 DXT3 DXT4 DXT5 DXT1_SRGB DXT3_SRGB DXT5_SRGB BC1_RGB BC1_RGBA BC2 BC3 BC4 BC4_SNORM BC5 BC5_SNORM BC6H_UFLOAT BC6H_SFLOAT BC7".split())
BC_FORMATS.update(f"{name}_SRGB" for name in ("BC1_RGB", "BC1_RGBA", "BC2", "BC3", "BC7"))
ETC_FORMATS = {"ETC2_RGB", "ETC2_RGB_A1", "ETC2_RGBA", "EAC_R11", "EAC_R11_SNORM", "EAC_RG11", "EAC_RG11_SNORM"}
ETC_FORMATS.update(f"{name}_SRGB" for name in ("ETC2_RGB", "ETC2_RGB_A1", "ETC2_RGBA"))
ASTC_FORMATS = {f"ASTC_{size}{suffix}" for size in "4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12".split()
                for suffix in ("", "_SRGB")}
# An ASTC conversion may write one- and two-channel data (BC4, BC5 and their signed forms) as EAC
# R11 / RG11: ASTC has no signed formats (Prey's normal maps are BC5_SNORM). astcFormats then names
# those EAC formats too, so the offer requires what the cache really holds.
EAC_FORMATS = {"EAC_R11", "EAC_R11_SNORM", "EAC_RG11", "EAC_RG11_SNORM"}
OPTION_TYPES = ("bool", "choice", "text")
FPS_COUNTER = {
    "key": "fps_counter", "type": "bool", "default": "0",
    "title": {"en": "FPS counter", "ru": "Счётчик FPS", "de": "FPS-Zähler", "pl": "Licznik FPS"},
}
ANDROID = "http://schemas.android.com/apk/res/android"


class SpecError(ValueError):
    pass


def text_ok(value):
    if isinstance(value, str):
        return bool(value)
    return isinstance(value, dict) and value and all(isinstance(k, str) and isinstance(v, str) for k, v in value.items())


def validate(spec):
    """Raises SpecError naming everything wrong with a port.json."""
    errors = []

    def need(condition, message):
        if not condition:
            errors.append(message)

    def theme_colours(theme, path, keys):
        need(isinstance(theme, dict), f"{path} must be an object")
        if isinstance(theme, dict):
            need(theme.get("playParticleStyle", "magic") in ("magic", "snow", "embers", "water", "plumbob", "stars"),
                 f"{path}.playParticleStyle must be magic, snow, embers, water, plumbob or stars")
            for key in keys:
                if key in theme:
                    need(isinstance(theme[key], str) and re.fullmatch(r"#[0-9a-fA-F]{6}", theme[key]),
                         f"{path}.{key} must be an opaque #RRGGBB colour")

    theme_colours(spec.get("theme", {}), "theme", ("playGlow", "playParticles"))
    need(spec.get("schema") == 1, "schema must be 1")
    app = spec.get("app") or {}
    need(isinstance(app.get("label"), str) and app.get("label"), "app.label is required")
    need(type(app.get("performanceHud", False)) is bool, "app.performanceHud must be true or false")
    for key in ("title", "subtitle"):
        if key in app:
            need(text_ok(app[key]), f"app.{key} must be a text or a map of texts")
    github = app.get("github", "")
    need(not app.get("gogInstaller") or (isinstance(app["gogInstaller"], str) and re.fullmatch(r"lib[a-zA-Z0-9_]+\.so", app["gogInstaller"])),
         "app.gogInstaller must name a bundled native reader lib….so")
    need(github == "" or re.fullmatch(r"[\w.-]+/[\w.-]+", github or ""), "app.github must be OWNER/NAME or empty")
    for language in app.get("languages", ["en"]):
        need(language in LANGUAGES, f"app.languages: {language} is not one of {', '.join(LANGUAGES)}")
    for entry in app.get("licenses", []):
        path = entry.get("path") if isinstance(entry, dict) else entry
        need(isinstance(path, str) and path and ".." not in path.split("/") and not path.startswith("/"),
             f"app.licenses: {entry!r} must be an asset path or {{\"path\", \"title\"}}")
        if isinstance(entry, dict) and "title" in entry:
            need(text_ok(entry["title"]), f"app.licenses: {path}: title must be a text or a map of texts")
    need(type(app.get("hyperosNotice", False)) is bool, "app.hyperosNotice must be true or false")
    need(type(app.get("gameModeNotice", True)) is bool, "app.gameModeNotice must be true or false")
    package = app.get("javaPackage", "")
    need(package == "" or re.fullmatch(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+", package), "app.javaPackage must be a Java package")
    renderers = spec.get("renderers") or {}
    textures = spec.get("textures") or {}
    for key, allowed in (("formats", BC_FORMATS), ("etc2Formats", ETC_FORMATS), ("astcFormats", ASTC_FORMATS | EAC_FORMATS)):
        if key in textures:
            values = textures[key]
            need(isinstance(values, list) and all(isinstance(v, str) and v in allowed for v in values),
                 f"textures.{key} must list known format names ({', '.join(sorted(allowed))})")
    if "conversionRequired" in textures:
        need(isinstance(textures["conversionRequired"], bool), "textures.conversionRequired must be true or false")
        need(not textures["conversionRequired"] or textures.get("backend"),
             "textures.conversionRequired needs textures.backend (a converter)")
    modes = [m.get("id") for m in renderers.get("modes", [])]
    for mode in renderers.get("modes", []):
        need(isinstance(mode.get("id"), str) and mode.get("id"), "renderers.modes[].id is required")
        need(isinstance(mode.get("aliases", []), list) and all(isinstance(a, str) and a for a in mode.get("aliases", [])),
             f"renderers.modes[{mode.get('id')}].aliases must be a list of earlier ids")
        taken = [m.get("id") for m in renderers.get("modes", [])] +                 [a for m in renderers.get("modes", []) if m is not mode for a in m.get("aliases", []) if isinstance(m.get("aliases", []), list)]
        need(not any(a in taken for a in mode.get("aliases", []) if isinstance(mode.get("aliases", []), list)),
             f"renderers.modes[{mode.get('id')}].aliases: an alias repeats another mode's id or alias")
        need(mode.get("kind", "gles") in KINDS, f"renderer {mode.get('id')}: kind must be one of {', '.join(KINDS)}")
        if "vulkan" in mode:
            need(re.fullmatch(r"\d+\.\d+", str(mode["vulkan"])), f"renderer {mode.get('id')}: vulkan must look like 1.3")
        if "gles" in mode:
            need(mode.get("kind", "gles") == "gles" and mode["gles"] in ("3.0", "3.1", "3.2"),
                 f"renderer {mode.get('id')}: gles must be 3.0, 3.1 or 3.2 on a GLES mode")
        if "textureCache" in mode:
            need(mode["textureCache"] in ("read", "originals", "incompatible"),
                 f"renderer {mode.get('id')}: textureCache must be read, originals or incompatible")
        if "adreno" in mode:
            need(isinstance(mode["adreno"], list) and len(mode["adreno"]) == 2, f"renderer {mode.get('id')}: adreno must be [min, max]")
        if mode.get("kind") in ("turnip", "panvk"):
            need(isinstance(mode.get("driver"), str) and mode.get("driver", "").endswith(".so"),
                 f"renderer {mode.get('id')}: a {mode.get('kind')} mode names its driver library (\"driver\": \"lib….so\")")
        if mode.get("kind") == "panvk":
            need("vulkan" in mode, f"renderer {mode.get('id')}: a panvk mode is checked by its \"vulkan\" version: give it")
        if "needsBC" in mode:
            need(isinstance(mode["needsBC"], bool), f"renderer {mode.get('id')}: needsBC must be true or false")
        if "maintenance1" in mode:
            need(mode.get("kind") in ("vulkan", "turnip", "panvk") and type(mode["maintenance1"]) is bool,
                 f"renderer {mode.get('id')}: maintenance1 is a Vulkan requirement and must be true or false")
        if "vulkanRequires" in mode:
            verified = {"androidSurface", "swapchain", "graphicsQueue", "robustBufferAccess"}
            requirements = mode["vulkanRequires"]
            need(mode.get("probe") == "version" and isinstance(requirements, list)
                 and all(isinstance(value, str) and value in verified for value in requirements),
                 f"renderer {mode.get('id')}: vulkanRequires must list native probe capabilities")
        if "probe" in mode:
            need(mode["probe"] in PROBES, f"renderer {mode.get('id')}: probe must be one of {', '.join(PROBES)}")
            if mode["probe"] == "version":
                need(mode.get("kind", "gles") in ("vulkan", "turnip", "panvk"),
                     f"renderer {mode.get('id')}: probe \"version\" is for a Vulkan mode (the port's own renderer)")
                need("vulkan" in mode, f"renderer {mode.get('id')}: probe \"version\" checks the mode's \"vulkan\" version: give it")
        if "option" in mode:
            switches = [o.get("key") for o in spec.get("options", []) if o.get("type", "bool") == "bool"]
            need(mode["option"] in switches, f"renderer {mode.get('id')}: option must name a bool option of \"options\"")
    if modes:
        need(renderers.get("default", modes[0]) in modes, "renderers.default must be one of the modes")
    if "failureWindowSeconds" in renderers:
        # How long after the first presented frame a GPU device loss still counts as the renderer's failure
        # (RendererFailure.java: the launcher offers the next renderer); later it is an ordinary crash.
        window = renderers["failureWindowSeconds"]
        need(type(window) is int and 0 <= window <= 600,
             "renderers.failureWindowSeconds must be an integer from 0 to 600 (seconds after the first presented frame)")
    distributions = spec.get("distributions") or []
    need(distributions, "at least one distribution is required")
    ids = set()
    for d in distributions:
        did = d.get("id")
        if "originalTextureRenderers" in d:
            ids_using_originals = d["originalTextureRenderers"]
            need(isinstance(ids_using_originals, list) and all(m in d.get("renderers", modes) for m in ids_using_originals),
                 f"distribution {did}: originalTextureRenderers must name this edition's renderers")
        if "textureFormats" in d:
            formats = d["textureFormats"]
            need(isinstance(formats, list) and all(isinstance(f, str) and f in BC_FORMATS for f in formats),
                 f"distribution {did}: textureFormats must list known source formats")
        theme_colours(d.get("theme", {}), f"distribution {did}: theme",
                      ("primaryFill", "accentDark", "playGlow", "playParticles", "playFill"))
        need(isinstance(did, str) and re.fullmatch(r"[a-z0-9_]+", did or ""), f"distribution id {did!r} must be [a-z0-9_]+")
        need(did not in ids, f"distribution {did} is listed twice")
        ids.add(did)
        need(isinstance(d.get("activity"), str) and "." in d.get("activity", ""), f"distribution {did}: activity must be a class name")
        for mode in d.get("renderers", []):
            need(mode in modes, f"distribution {did}: renderer {mode} is not in renderers.modes")
        parts = d.get("parts") or []
        need(parts, f"distribution {did}: parts are required")
        part_ids = set()
        for p in parts:
            pid = p.get("id")
            need(isinstance(pid, str) and re.fullmatch(r"[a-z0-9_]+", pid or ""), f"{did}: part id {pid!r} must be [a-z0-9_]+")
            need(pid not in part_ids, f"{did}: part {pid} is listed twice")
            part_ids.add(pid)
            group = p.get("folderGroup", "")
            need(isinstance(group, str) and (not group or re.fullmatch(r"[a-z0-9_]+", group)),
                 f"{did}/{pid}: folderGroup must be [a-z0-9_]+")
            if "folderTitle" in p:
                need(text_ok(p["folderTitle"]), f"{did}/{pid}: folderTitle must be a text or a map of texts")
            folders = p.get("userFolders", {})
            need(isinstance(folders, dict), f"{did}/{pid}: userFolders must be an object")
            if isinstance(folders, dict):
                seen_folders = set()
                for path, title in folders.items():
                    need(isinstance(path, str) and path and "\\" not in path and not path.startswith("/")
                         and not re.match(r"^[A-Za-z]:", path) and all(ord(c) >= 32 for c in path)
                         and all(part not in ("", ".", "..") for part in path.split("/")),
                         f"{did}/{pid}: userFolders path {path!r} must be a relative directory")
                    need(title is None or text_ok(title), f"{did}/{pid}: userFolders {path!r} title must be text or null")
                    if isinstance(path, str):
                        folded = path.casefold()
                        need(folded not in seen_folders, f"{did}/{pid}: userFolders repeats {path!r} ignoring case")
                        seen_folders.add(folded)
            need(p.get("required"), f"{did}/{pid}: required is empty")
            for path in p.get("required", []) + p.get("needed", []):
                need(isinstance(path, str) and ".." not in path.split("/") and not path.startswith("/"),
                     f"{did}/{pid}: {path!r} must be a relative path")
    groups = {}
    for d in distributions:
        for p in d.get("parts", []):
            group = p.get("folderGroup", "")
            if not group:
                continue
            definition = (p.get("id"), p.get("folder"), p.get("fixed", False), p.get("mount", False))
            need(not p.get("mount", False), f"folder group {group}: mounted archives are not supported")
            need(group not in ids, f"folder group {group} must not collide with a distribution id")
            need(group not in groups or groups[group] == definition, f"folder group {group}: members must use the same part id, folder and fixed setting")
            groups[group] = definition
    for o in spec.get("options", []):
        if "rendererSetting" in o:
            need(type(o["rendererSetting"]) is bool and (not o["rendererSetting"] or o.get("type", "bool") == "bool"),
                 f"option {o.get('key')}: rendererSetting is for a bool renderer switch")
        need(o.get("type", "bool") in OPTION_TYPES, f"option {o.get('key')}: type must be one of {', '.join(OPTION_TYPES)}")
        need(re.fullmatch(r"[a-z0-9_]+", o.get("key", "")), f"option key {o.get('key')!r} must be [a-z0-9_]+")
        minimum = o.get("defaultMinRamGiB", 0)
        need(type(minimum) is int and 0 <= minimum <= 1024,
             f"option {o.get('key')}: defaultMinRamGiB must be an integer from 0 to 1024")
        need(not minimum or o.get("type", "bool") == "bool",
             f"option {o.get('key')}: defaultMinRamGiB requires a bool option")
        need(type(o.get("arm64Only", False)) is bool,
             f"option {o.get('key')}: arm64Only must be a boolean")
        for only in o.get("only", []):
            need(only in ids, f"option {o.get('key')}: unknown distribution {only}")
        if o.get("type") == "choice":
            need(o.get("choices"), f"option {o.get('key')}: choices are required")
        if "offer" in o:
            need(o.get("type", "bool") == "bool" and text_ok(o["offer"]),
                 f"option {o.get('key')}: offer is the question of a bool option (a text or a map of texts)")
    for key, value in (spec.get("hints") or {}).items():
        need(key in ("gameFiles", "options") and text_ok(value), f"hints.{key}: a text or a map of texts for gameFiles or options")
    folders = spec.get("folders", [])
    need(isinstance(folders, list), "folders must be an array")
    for f in folders if isinstance(folders, list) else []:
        path = f.get("path", "") if isinstance(f, dict) else ""
        need(isinstance(f, dict) and text_ok(f.get("title")) and (f.get("hint") is None or text_ok(f.get("hint"))),
             "folders: each needs a title (text or map of texts) and an optional hint")
        need(isinstance(path, str) and re.match(r"^\{(storage|files|private)\}/", path) and "\\" not in path
             and not any(ord(c) < 32 for c in path) and ":" not in path
             and all(part not in ("", ".", "..") for part in path.split("/")[1:]),
             f"folders: path {path!r} must start with {{storage}}/, {{files}}/ or {{private}}/ and stay inside it")
    for c in (spec.get("cleanup") or {}).get("categories", []):
        need(c.get("id") and c.get("globs"), "cleanup categories need an id and globs")
        for glob in c.get("globs", []):
            need(".." not in glob.split("/"), f"cleanup {c.get('id')}: {glob!r} must stay inside the game folder")
    if errors:
        raise SpecError("port.json:\n  " + "\n  ".join(errors))


def manifest_attribute(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*=\s*"([^"]*)"', text)
    return match.group(1) if match else None


def java_package(spec, manifest_text):
    package = (spec.get("app") or {}).get("javaPackage", "")
    if package:
        return package
    application = manifest_attribute(manifest_text, "package")
    if not application:
        raise SpecError("the manifest has no package attribute; set app.javaPackage")
    return application + ".launcher"


def render_sources(package, out):
    """The base's sources with the game's package; returns the written files."""
    target = Path(out) / package.replace(".", "/")
    target.mkdir(parents=True, exist_ok=True)
    written = []
    for source in sorted(SOURCES.glob("*.java")):
        text = source.read_text(encoding="utf-8")
        line = f"package {TEMPLATE_PACKAGE};"
        if not text.startswith(line):
            raise SpecError(f"{source.name} does not start with '{line}'")
        path = target / source.name
        path.write_text(f"package {package};" + text[len(line):], encoding="utf-8", newline="\n")
        written.append(path)
    return written


def min_sdk(text):
    """The manifest's minSdkVersion (1 when it names none, as Android reads it)."""
    match = re.search(r'android:minSdkVersion\s*=\s*"(\d+)"', text)
    return int(match.group(1)) if match else 1


def expand_manifest(text, spec, package):
    """The game's manifest with the launcher's permissions, queries and components in place."""
    app = spec.get("app") or {}
    for marker in ("<!-- launcher:manifest -->", "<!-- launcher:application -->"):
        if text.count(marker) != 1:
            raise SpecError(f"the manifest needs exactly one {marker}")
    launchers = re.findall(r'<category\s+android:name="android\.intent\.category\.LAUNCHER"', text)
    if launchers:
        raise SpecError("another activity is the home-screen entry (category LAUNCHER): the launcher takes it")
    declared = set(re.findall(r'<uses-permission\s+android:name="([^"]+)"', text))
    permissions = ["android.permission.POST_NOTIFICATIONS", "android.permission.FOREGROUND_SERVICE",
                   "android.permission.FOREGROUND_SERVICE_DATA_SYNC", "android.permission.WAKE_LOCK"]
    legacy = []
    if app.get("allFilesAccess", True):
        permissions.insert(0, "android.permission.MANAGE_EXTERNAL_STORAGE")
        if min_sdk(text) < 30:
            # Android 8-10 have no "All files access": there the launcher asks for these
            # (Storage.requestAllFiles), which only works when the manifest declares them.
            legacy = ["android.permission.READ_EXTERNAL_STORAGE", "android.permission.WRITE_EXTERNAL_STORAGE"]
    if (app.get("github") and app.get("updates", True)) or any(
            m.get("kind") == "turnip" for m in (spec.get("renderers") or {}).get("modes", [])):
        permissions.append("android.permission.INTERNET")
    if app.get("localNetwork"):
        permissions.append("android.permission.ACCESS_LOCAL_NETWORK")
    head = ["  <!-- Added by the launcher base (tools/launcher.py). -->"]
    head += [f'  <uses-permission android:name="{name}" />' for name in permissions if name not in declared]
    head += [f'  <uses-permission android:name="{name}" android:maxSdkVersion="29" />' for name in legacy if name not in declared]
    if legacy and "requestLegacyExternalStorage" not in text:
        # Android 10 gives a target SDK of 29 or later scoped storage unless the app asks for the
        # legacy view: game files are read by path. Android 11+ ignores the flag (All files access).
        at = text.rfind("<application", 0, text.index("<!-- launcher:application -->"))
        if at < 0:
            raise SpecError("<!-- launcher:application --> must be inside <application>")
        at += len("<application")
        text = text[:at] + ' android:requestLegacyExternalStorage="true"' + text[at:]
    head += [
        "  <queries>",
        '    <intent><action android:name="android.intent.action.OPEN_DOCUMENT_TREE" /></intent>',
        '    <intent><action android:name="android.intent.action.OPEN_DOCUMENT" /><data android:mimeType="*/*" /></intent>',
        '    <intent><action android:name="android.intent.action.VIEW" /><data android:scheme="https" /></intent>',
        '    <package android:name="com.google.android.documentsui" />',
        '    <package android:name="com.android.documentsui" />',
    ]
    if app.get("gameModeNotice", True):
        # The game-mode tip (HyperOs.java) opens the vendor's game tool by the package's launch
        # intent: getLaunchIntentForPackage sees a package only when it is queried.
        head += ['    <package android:name="com.miui.securitycenter" />',
                 '    <package android:name="com.samsung.android.game.gamehome" />',
                 '    <package android:name="com.oplus.games" />']
    head.append("  </queries>")
    process = (app.get("processes") or {}).get("launcher", ":launcher")
    where = f' android:process="{process}"' if process else ""
    probe = (app.get("processes") or {}).get("probe", ":probe")
    vulkan = any(m.get("kind") in ("vulkan", "turnip", "panvk") for m in (spec.get("renderers") or {}).get("modes", []))
    changes = "orientation|screenSize|smallestScreenSize|screenLayout|keyboardHidden|keyboard|navigation|uiMode|density"
    body = [
        "    <!-- The launcher base (tools/launcher.py): the home-screen entry. -->",
        f'    <activity android:name="{package}.LauncherActivity" android:exported="true"{where}',
        '        android:theme="@android:style/Theme.Material.NoActionBar"',
        f'        android:configChanges="{changes}">',
        "      <intent-filter>",
        '        <action android:name="android.intent.action.MAIN" />',
        '        <category android:name="android.intent.category.LAUNCHER" />',
        "      </intent-filter>",
        "    </activity>",
        f'    <activity android:name="{package}.FileBrowserActivity" android:exported="false"{where}',
        '        android:theme="@android:style/Theme.Material.NoActionBar"',
        f'        android:configChanges="{changes}" />',
        f'    <service android:name="{package}.Work$WorkService" android:exported="false"{where}',
        '        android:foregroundServiceType="dataSync" />',
    ]
    if vulkan:
        body += [
            "    <!-- The Vulkan driver check (libportprobe.so): a driver that crashes takes only this process. -->",
            f'    <service android:name="{package}.Probe" android:exported="false" android:process="{probe}" />',
        ]
    text = text.replace("<!-- launcher:manifest -->", "\n".join(head).lstrip())
    return text.replace("<!-- launcher:application -->", "\n".join(body).lstrip())


def git_commit(folder):
    try:
        result = subprocess.run(["git", "-C", str(folder), "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True, timeout=20)
        return result.stdout.strip() if result.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def write_assets(spec, manifest_text, out, game_root):
    folder = Path(out) / "launcher"
    folder.mkdir(parents=True, exist_ok=True)
    modes = (spec.get("renderers") or {}).get("modes", [])
    if any(m.get("kind", "gles") == "gles" and "gles" not in m for m in modes):
        versions = [node.get(f"{{{ANDROID}}}glEsVersion", "") for node in ElementTree.fromstring(manifest_text).findall("uses-feature")]
        minimum = max((int(v, 0) for v in versions if re.fullmatch(r"0[xX][0-9a-fA-F]+|[1-9][0-9]*", v)), default=0)
        if minimum:
            gl = f"{minimum >> 16}.{minimum & 0xffff}"
            spec = dict(spec, renderers=dict(spec["renderers"], modes=[
                dict(m, gles=gl) if m.get("kind", "gles") == "gles" and "gles" not in m else m for m in modes]))
    if spec["app"].get("performanceHud"):
        options = spec.get("options", [])
        if not any(option["key"] == FPS_COUNTER["key"] for option in options):
            spec = dict(spec, options=[*options, FPS_COUNTER])
    (folder / "port.json").write_text(json.dumps(spec, ensure_ascii=False, indent=1) + "\n", encoding="utf-8", newline="\n")
    build = {
        "versionName": manifest_attribute(manifest_text, "android:versionName") or "",
        "versionCode": int(manifest_attribute(manifest_text, "android:versionCode") or 0),
        "commit": git_commit(game_root),
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    (folder / "build.json").write_text(json.dumps(build, indent=1) + "\n", encoding="utf-8", newline="\n")
    return build


def ndk_root():
    """The Android NDK: ANDROID_NDK_HOME / ANDROID_NDK_ROOT when it has the unified sysroot (r19+;
    an older one cannot build the probe: no <dlfcn.h> beside its clang), else the newest in the SDK's ndk folder."""
    for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if os.environ.get(name) and any(Path(os.environ[name]).glob("toolchains/llvm/prebuilt/*/sysroot/usr/include/dlfcn.h")):
            return Path(os.environ[name])
    sdk = next((Path(os.environ[n]) for n in ("ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_SDK")
                if os.environ.get(n) and Path(os.environ[n]).is_dir()), Path.home() / "AppData/Local/Android/Sdk")
    versions = sorted((sdk / "ndk").glob("*"), key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)])
    return versions[-1] if versions else None


def ndk_clang(ndk=None):
    ndk = Path(ndk) if ndk else ndk_root()
    if ndk is None:
        raise SpecError("no Android NDK: set ANDROID_NDK_HOME")
    for host in ("windows-x86_64", "linux-x86_64", "darwin-x86_64"):
        for name in ("clang.exe", "clang"):
            clang = ndk / "toolchains/llvm/prebuilt" / host / "bin" / name
            if clang.is_file():
                return clang
    raise SpecError(f"{ndk}: no NDK clang")


def jni_class(package):
    """The JNI name of the launcher's Probe class in the game's package."""
    return (package + ".Probe").replace(".", "/")


def build_probe(out, package, ndk=None, api=30, abi="arm64"):
    """Compiles libportprobe.so (the Vulkan driver check) into the folder out. arm64-v8a is 16 KB
    aligned; armeabi-v7a (abi="arm32") is 4 KB. Returns its path. The Java class it registers with
    is the game's. The default abi keeps every other game on arm64."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    library = out / PROBE_LIBRARY
    if abi == "arm32":
        target, page = f"armv7a-linux-androideabi{api}", "4096"
    elif abi == "arm64":
        target, page = f"aarch64-linux-android{api}", "16384"
    else:
        raise SpecError(f"probe abi must be arm64 or arm32, not {abi}")
    command = [str(ndk_clang(ndk)), f"--target={target}", "-std=c11", "-O2", "-fPIC", "-shared",
               "-Wall", "-Wextra", "-Werror", f'-DPORT_PROBE_CLASS="{jni_class(package)}"',
               f"-Wl,-z,max-page-size={page}", "-Wl,--build-id=sha1", "-o", str(library), str(PROBE_SOURCE), "-ldl"]
    subprocess.run(command, check=True)
    return library


def prepare(spec_path, manifest_path, out):
    spec_path, manifest_path, out = Path(spec_path), Path(manifest_path), Path(out)
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    validate(spec)
    manifest_text = manifest_path.read_text(encoding="utf-8")
    package = java_package(spec, manifest_text)
    for name in ("src", "assets"):
        stale = out / name
        if stale.exists():
            for path in sorted(stale.rglob("*"), reverse=True):
                path.unlink() if path.is_file() else path.rmdir()
    sources = render_sources(package, out / "src")
    manifest = out / "AndroidManifest.xml"
    expanded = expand_manifest(manifest_text, spec, package)
    try:
        ElementTree.fromstring(expanded.encode("utf-8"))
    except ElementTree.ParseError as failure:
        raise SpecError(f"{manifest_path}: the manifest is not well-formed XML: {failure}") from failure
    manifest.write_text(expanded, encoding="utf-8", newline="\n")
    build = write_assets(spec, manifest_text, out / "assets", spec_path.parent)
    vulkan = any(m.get("kind") in ("vulkan", "turnip", "panvk") for m in (spec.get("renderers") or {}).get("modes", []))
    return {"package": package, "sources": [str(p) for p in sources], "manifest": str(manifest),
            "assets": str(out / "assets"), "build": build, "probe": vulkan}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("prepare")
    p.add_argument("--spec", required=True, type=Path)
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    c = commands.add_parser("check")
    c.add_argument("--spec", required=True, type=Path)
    b = commands.add_parser("probe")
    b.add_argument("--package", required=True, help="the launcher's Java package, e.g. org.example.port.launcher")
    b.add_argument("--out", required=True, type=Path)
    b.add_argument("--ndk", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "check":
            validate(json.loads(args.spec.read_text(encoding="utf-8")))
            print(f"{args.spec}: valid")
        elif args.command == "probe":
            print(build_probe(args.out, args.package, args.ndk))
        else:
            print(json.dumps(prepare(args.spec, args.manifest, args.out), indent=2))
    except (SpecError, json.JSONDecodeError, subprocess.CalledProcessError) as failure:
        sys.exit(str(failure))


if __name__ == "__main__":
    main()
