"""Build the port's resource overlay with patched game packages and pinned PE
images. The host serves these copies at their original game paths.

The Graphics/Performance Options panel (ui.package, type 0, instance
0x49060F03, groups 0xA99D8A11 for 1024x768 and up and 0x08000600 for
800x600) gets three option groups in its own style where the PC-only
controls were (Window Size list, Refresh Rate list, Smooth Edges slider, all
kept in the tree but hidden for the panel's code):

    UI Scale          100% 150% 200% 300%        radio:s2UIScale
    Frame Rate Limit  Off, 30 and display rates radio:s2FrameRateLimit
    Render Scale      50% 75% 100% 125% 150%     radio:s2RenderScale

The panel base class binds the windows with IDs 0xA0, 0xA1, ... (searched
recursively, until one is missing or is not a GZWinGen) whose caption reads
"radio:<property>:<type>:<values>" to that property of the game's user
property set (Config/userProps.xml). The graphics panel's Object Hiding
group held 0xA0-0xA2; it moves to 0xB0-0xB2 (its controller follows through
config/Sims2EP9.patches.json) and the new groups take 0xA0-0xA2. The host
sees the values change through cGZPropertySetString::SetProperty
(android/host/sims2_settings.c). The savedProps.txt overlay adds those keys
to the game's save list, so Apply keeps the chosen values across restarts.

    python tools/build_overlay.py [--out build/overlay]
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dbpf  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
GAME = Path(r"C:\Games\The Sims 2 Legacy")
PANEL_INSTANCE = 0x49060F03
GROUPS = (0xA99D8A11, 0x08000600)
CAPTURE_PANELS = (0x49001020, 0x49000000, 0x4905F85B)
CAPTURE_BACKDROP = (0x856DDBAC, 0x499DB772, 0xA9600001)
CAPTURE_ART_RECTS = ((38, 139, 64, 159), (69, 143, 89, 160))
PORT_STRING_INSTANCE = 0xFF00
# captionres group 0x7f232750 is Options.package, with one-based string indices.
PORT_STRING_PACKAGE = Path("EP9/TSData/Res/Text/Options.package")
# STR# IDs differ from the registry language IDs; verified against original text resources.
PORT_LABELS = {
    1: ("UI Scale", "Frame Rate Limit", "Render Scale", "Off"),
    2: ("UI Scale", "Frame Rate Limit", "Render Scale", "Off"),
    3: ("Échelle interface", "Limite d'IPS", "Échelle rendu", "Aucune"),
    4: ("UI-Skalierung", "FPS-Limit", "Render-Skalierung", "Aus"),
    5: ("Scala interfaccia", "Limite FPS", "Scala rendering", "No"),
    6: ("Escala interfaz", "Límite de FPS", "Escala renderizado", "No"),
    7: ("Interfaceschaal", "FPS-limiet", "Renderschaal", "Uit"),
    8: ("Brugerfladeskala", "FPS-grænse", "Renderingsskala", "Fra"),
    9: ("Gränssnittsskala", "FPS-gräns", "Renderingsskala", "Av"),
    10: ("Grensesnittskala", "FPS-grense", "Gjengivelsesskala", "Av"),
    11: ("Käyttöliittymä", "FPS-raja", "Renderöintiskaala", "Pois"),
    13: ("Масштаб UI", "Предел FPS", "Масштаб рендера", "Нет"),
    14: ("Escala interface", "Limite de FPS", "Escala de imagem", "Não"),
    15: ("UI倍率", "FPS上限", "描画倍率", "なし"),
    16: ("Skala interfejsu", "Limit FPS", "Skala renderowania", "Brak"),
    17: ("界面缩放", "帧率上限", "渲染缩放", "关闭"),
    18: ("介面縮放", "影格率上限", "繪製縮放", "關閉"),
    19: ("ขนาดส่วนติดต่อ", "จำกัดเฟรมเรต", "ขนาดเรนเดอร์", "ปิด"),
    20: ("인터페이스 배율", "프레임 제한", "렌더링 배율", "끄기"),
    26: ("Měřítko rozhraní", "Limit FPS", "Měřítko vykreslení", "Vyp."),
    28: ("Felület mérete", "FPS-korlát", "Renderelés mérete", "Ki"),
    35: ("Escala interface", "Limite de FPS", "Escala de imagem", "Não"),
}

OPTIONS = [
    # id, property, title, values, labels
    (0xA0, "s2UIScale", "UI Scale", (100, 150, 200, 300), ("100%", "150%", "200%", "300%")),
    # Runtime supplies the display's rates before the original binder hides unused buttons.
    (0xA1, "s2FrameRateLimit", "Frame Rate Limit", (0, 30, 60), ("Off", "30", "60") + ("",) * 7),
    (0xA2, "s2RenderScale", "Render Scale", (50, 75, 100, 125, 150), ("50%", "75%", "100%", "125%", "150%")),
]
# The panel's Object Hiding group held 0xA0-0xA2 (a flat rectangle the binder
# cannot take, which ended the binding there); it moves to 0xB0-0xB2, and the
# controller code with it (config/Sims2EP9.patches.json).
RENUMBERED = {0xA0: 0xB0, 0xA1: 0xB1, 0xA2: 0xB2}
# The containers they replace, by caption, in column order.
REPLACED = ("screensize", "refresh rates", "Smooth Edges")

FLAGS = ("winflag_visible=yes winflag_enabled=yes winflag_moveable=yes winflag_sortable=no winflag_pbuff=no "
         "winflag_pbufftrans={t} winflag_pbufferase={t} winflag_alphablend={a} winflag_acceptfocus={f} "
         "winflag_mousetrans=no winflag_ignoremouse=no")


def group_markup(option, area, indent="   "):
    ident, prop, title, values, labels = option
    x0, y0, x1, y1 = area
    caption = f"radio:{prop}:int:" + ":".join(str(v) for v in values)
    title_resource = f" captionres={{7f232750,{PORT_STRING_INSTANCE:04x}{ident - 0xA0 + 1:04x}}}"
    lines = [
        f"{indent}<LEGACY clsid=GZWinGen iid=IGZWinGen id=0x{ident:08x} area=({x0},{y0},{x1},{y1}) fillcolor=(0,0,0) "
        f'caption="{caption}" ' + FLAGS.format(t="no", a="no", f="yes") +
        " blttype=tiled userdata=0 moveable=no sizeable=no defaultkeys=no closevisible=no gobackvisible=no "
        "minmaxvisible=no closedisabled=no gobackdisabled=no minmaxdisabled=no titlebar=no fill=no outline=no "
        "paint=yes sidebar=no gutters=(0,0) >",
        f"{indent}<CHILDREN>",
        f"{indent}   <LEGACY clsid=GZWinText iid=IGZWinText area=(0,0,{x1 - x0 + 17},26) fillcolor=(0,128,0) "
        f'caption="{title}"' + title_resource + ' ' + FLAGS.format(t="yes", a="no", f="yes") +
        " font=OptionsText align=leftbottom notify=no wrapped=yes opaque=no forecolor=(5,6,113) bkgcolor=(0,0,0) "
        "gutters=(2,2) textoffsets=(0,0) >",
    ]
    # Six FPS choices use two columns so the group stays above the next setting.
    columns = 2 if prop == "s2FrameRateLimit" else 1
    width = 60 if columns == 2 else 83
    for k, label in enumerate(labels):
        left, top = 62 * (k % columns), 26 + 16 * (k // columns)
        label_resource = f" captionres={{7f232750,{PORT_STRING_INSTANCE:04x}0004}}" if label == "Off" else ""
        lines.append(
            f"{indent}   <LEGACY clsid=GZWinBtn iid=IGZWinBtn id=0x{k + 1:08x} area=({left},{top},{left + width},{top + 16}) "
            f'fillcolor=(192,192,192) caption="{label}"' + label_resource + ' ' + FLAGS.format(t="no", a="yes", f="no") +
            " image={499db772,ac76e165} font=OptionsText colorfontnormal=(5,6,113) colorfontdisabled=(84,85,105) "
            "colorfonthilited=(204,214,255) colorfontnormalbkg=(0,0,0) colorfontdisabledbkg=(0,0,0) "
            "colorfonthilitedbkg=(0,0,0) toggle=off triggerondown=off showcaption=yes fill=yes autosize=no "
            "wrapcaption=no shiftcaption=no tips=no tipsdelay=no tipstimeout=no style=radiocheck gutters=(0,2,0,2) "
            'tiptext="" tipoffsets=(0,0) tipflag=0x01000000 align=center btnclicksnd={00000000,ff391ba9} >')
    lines.append(f"{indent}</CHILDREN>")
    return lines


def patch_panel(text):
    """The panel script with the three replaced containers hidden and the new
    groups added at their places (same column x, same top, 107 px tall)."""
    lines = text.split("\r\n")
    areas = []
    for caption in REPLACED:
        for i, line in enumerate(lines):
            if line.lstrip().startswith("<LEGACY clsid=GZWinFlatRect") and f'caption="{caption}"' in line:
                m = re.search(r"area=\((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)", line)
                x0, y0, x1, y1 = (int(v) for v in m.groups())
                areas.append((x0, y0, x1, y0 + 107))
                lines[i] = line.replace("winflag_visible=yes", "winflag_visible=no", 1)
                break
        else:
            raise SystemExit(f"panel has no container {caption!r}")
    # Object Hiding: its container and its two buttons take their new IDs.
    for i, line in enumerate(lines):
        if 'caption="Object Hiding"' in line and "id=0x000000a0 " in line:
            lines[i] = line.replace("id=0x000000a0 ", "id=0x000000b0 ")
            for j in range(i + 1, min(i + 8, len(lines))):
                for a, b in RENUMBERED.items():
                    lines[j] = lines[j].replace(f"id=0x{a:08x} ", f"id=0x{b:08x} ")
            break
    else:
        raise SystemExit("panel has no Object Hiding group at 0xA0")
    rest = "\r\n".join(lines)
    if any(f"id=0x{o[0]:08x} " in rest for o in OPTIONS) or any(f"id=0x{b:08x} " in text for b in RENUMBERED.values()):
        raise SystemExit("the IDs the port uses are taken in the panel")
    end = max(i for i, line in enumerate(lines) if line.strip() == "</CHILDREN>")
    added = [l for option, area in zip(OPTIONS, areas) for l in group_markup(option, area)]
    return "\r\n".join(lines[:end] + added + lines[end:])


def patch_capture_buttons(text):
    """Keep controller IDs while hiding the desktop capture controls."""
    lines = text.splitlines(keepends=True)
    count = 0
    for i, line in enumerate(lines):
        if 'clsid=GZWinBtn ' not in line or not any(
                f'tiptext="{label}"' in line for label in ('Camera Snapshot - C', 'Video Capture - V')):
            continue
        for before, after in (('winflag_visible=yes', 'winflag_visible=no'),
                              ('winflag_enabled=yes', 'winflag_enabled=no'),
                              ('winflag_ignoremouse=no', 'winflag_ignoremouse=yes')):
            line = line.replace(before, after, 1)
        lines[i] = line
        count += 1
    if count != 2:
        raise ValueError(f'expected two capture buttons, found {count}')
    return ''.join(lines)


def patch_capture_backdrop(data):
    """Remove the town puck's baked capture art using its adjacent blue gradient."""
    if len(data) != 135368 or data[:18] != bytes.fromhex('000002000000000000000000b300bd002008'):
        raise ValueError('expected the 179x189 uncompressed BGRA town puck TGA')
    result = bytearray(data)
    # This TGA stores bottom row first; resource rectangles use screen coordinates.
    def pixel(x, y):
        return 18 + ((188 - y) * 179 + x) * 4
    for x0, y0, x1, y1 in CAPTURE_ART_RECTS:
        width = x1 - x0 + 1
        for y in range(y0, y1):
            left, right = pixel(x0 - 1, y), pixel(x1, y)
            for x in range(x0, x1):
                at, distance = pixel(x, y), x - x0 + 1
                if data[at + 3] != 255:
                    raise ValueError('capture artwork must lie inside the opaque puck')
                for channel in range(3):
                    result[at + channel] = (data[left + channel] * (width - distance) +
                                            data[right + channel] * distance + width // 2) // width
    return bytes(result)


def rewrite_package(source, out, replacements, *, additions=None):
    """source with the resources in `replacements` ({(type, group, instance):
    bytes}) replaced and stored uncompressed; every other resource copied as is."""
    p = dbpf.Package(source)
    header = bytearray(p.data[:96])
    blobs, index, directory = bytearray(), [], []
    for e in p.entries:
        if e["type"] == dbpf.DIR_TYPE:
            continue
        key3 = (e["type"], e["group"], e["instance"])
        if key3 in replacements:
            blob, usize = replacements.pop(key3), None
        else:
            blob = p.raw(e)
            usize = p.compressed.get(p.key(e))
        index.append((e["type"], e["group"], e["instance"], 96 + len(blobs), len(blob)))
        if usize is not None:
            directory.append(struct.pack("<IIII", e["type"], e["group"], e["instance"], usize))
        blobs += blob
    if replacements:
        raise SystemExit(f"resources not found: {replacements.keys()}")
    for key3, blob in (additions or {}).items():
        if any(tuple(e[:3]) == key3 for e in index):
            raise ValueError(f"added resource already exists: {key3}")
        index.append((*key3, 96 + len(blobs), len(blob)))
        blobs += blob
    dir_blob = b"".join(directory)
    index.append((dbpf.DIR_TYPE, dbpf.DIR_TYPE, 0x286B1F03, 96 + len(blobs), len(dir_blob)))
    blobs += dir_blob
    table = b"".join(struct.pack("<IIIII", *e) for e in index)
    struct.pack_into("<IIII", header, 32, 7, len(index), 96 + len(blobs), len(table))
    struct.pack_into("<III", header, 48, 0, 0, 0)     # no holes
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(header) + bytes(blobs) + table)


def patch_saved_props(data):
    keys = {line.partition(b'#')[0].strip() for line in data.splitlines()}
    missing = [option[1].encode('ascii') for option in OPTIONS if option[1].encode('ascii') not in keys]
    if not missing:
        return data
    newline = b'\r\n' if b'\r\n' in data else b'\n'
    separator = b'' if data.endswith((b'\n', b'\r')) else newline
    return data + separator + newline + b'# Android port graphics options' + newline + newline.join(missing) + newline


def write_saved_props(game, out):
    # The game picks the first existing Config search path, which can be any installed pack.
    sources = sorted(game.glob('*/TSData/Res/Config/savedProps.txt'))
    if not sources:
        raise SystemExit(f'no saved property lists under {game}')
    targets = []
    for source in sources:
        target = out / source.relative_to(game)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(patch_saved_props(source.read_bytes()))
        targets.append(target)
    return targets


def patch_camera_defaults(data):
    for key in (b'edgeScrolling', b'CameraDriftCamEnabled'):
        pattern = rb'(<AnyBoolean\b[^>]*\bkey="' + key + rb'"[^>]*>\s*)(true|false)(\s*</AnyBoolean>)'
        data, count = re.subn(pattern, lambda m: m[1] + b'false' + m[3], data)
        if count != 1:
            raise ValueError(f'expected one boolean camera default {key.decode()}, found {count}')
    return data


def write_camera_defaults(game, out):
    sources = sorted(game.glob('*/TSData/Res/Config/globalProps.xml'))
    if not sources:
        raise SystemExit(f'no global property defaults under {game}')
    targets = []
    for source in sources:
        target = out / source.relative_to(game)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(patch_camera_defaults(source.read_bytes()))
        targets.append(target)
    return targets


def patch_graphics_queries(data):
    # EVENT queries use the host's GLsync path instead of a GPU pixel read per resource release.
    newline = b'\r\n' if b'\r\n' in data else b'\n'
    rule = b'boolProp queriesOK true'
    suffix = newline + b'# Android port: EVENT queries are supported by GLsync' + newline + rule + newline
    if data.endswith(suffix):
        return data
    separator = b'' if data.endswith((b'\n', b'\r')) else newline
    return data + separator + suffix


def write_graphics_queries(game, out):
    sources = sorted(game.glob('*/TSData/Res/Config/Graphics Rules.sgr'))
    if not sources:
        raise SystemExit(f'no graphics rules under {game}')
    targets = []
    for source in sources:
        target = out / source.relative_to(game)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(patch_graphics_queries(source.read_bytes()))
        targets.append(target)
    return targets


def patch_neighborhood_warning(data):
    # The English original says missing associated neighborhoods, not missing installed packs.
    old = ('Похоже, некоторые дополнения еще не установлены на вашем компьютере, поэтому ваши подопечные не могут посетить новые учатски. '
           'Каждое дополнение необходимо сопоставить хотя бы с одним городком. Чтобы сделать это, на экране выбора городка нажмите кнопку со знаком "+", которая находится в левом верхнем углу экрана.').encode('utf-8')
    new = ('У некоторых установленных дополнений еще нет районов, связанных с этим городком. Чтобы ваши подопечные могли посещать новые места, '
           'добавьте хотя бы один район соответствующего дополнения. Для этого нажмите кнопку выбора района со знаком "+" в левом верхнем углу экрана.').encode('utf-8')
    if data.count(old) != 1:
        raise ValueError('expected one original Russian associated-neighborhood warning')
    return data.replace(old, new, 1)


def port_strings():
    localized = bytearray(b"Android port options".ljust(64, b"\0") + struct.pack("<HH", 0xFFFD, 4 * len(PORT_LABELS)))
    for language, labels in PORT_LABELS.items():
        for label in labels:
            text = label.encode("utf-8")
            localized += bytes([language]) + text + b"\0\0"
    return bytes(localized)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=str(ROOT / "build/overlay"))
    args = parser.parse_args()
    strings = PORT_STRING_PACKAGE
    rewrite_package(GAME / strings, Path(args.out) / strings, {},
                    additions={(0x53545223, 0xFFFFFFFF, PORT_STRING_INSTANCE): port_strings()})
    rel = Path("EP9/TSData/Res/UI/ui.package")
    source = GAME / rel
    p = dbpf.Package(source)
    replacements = {}
    for group in GROUPS:
        entry = next(e for e in p.entries if (e["type"], e["group"], e["instance"]) == (0, group, PANEL_INSTANCE))
        text = p.read(entry).decode("latin-1")
        patched = patch_panel(text)
        replacements[(0, group, PANEL_INSTANCE)] = patched.encode("latin-1")
        dump = ROOT / f"research/ui/graphics_{group:08X}_{PANEL_INSTANCE:08X}.patched.txt"
        dump.parent.mkdir(parents=True, exist_ok=True)
        dump.write_bytes(patched.encode("latin-1"))
    capture_entries = [e for e in p.entries if e['type'] == 0 and e['instance'] in CAPTURE_PANELS]
    if len(capture_entries) != 3:
        raise ValueError(f'expected three capture panels, found {len(capture_entries)}')
    for entry in capture_entries:
        key = (entry['type'], entry['group'], entry['instance'])
        replacements[key] = patch_capture_buttons(p.read(entry).decode('latin-1')).encode('latin-1')
    backdrop = next(e for e in p.entries if (e['type'], e['group'], e['instance']) == CAPTURE_BACKDROP)
    replacements[CAPTURE_BACKDROP] = patch_capture_backdrop(p.read(backdrop))
    out = Path(args.out) / rel
    rewrite_package(source, out, dict(replacements))
    check = dbpf.Package(out)
    for key3, data in replacements.items():
        e = next(e for e in check.entries if (e["type"], e["group"], e["instance"]) == key3)
        assert check.read(e) == data
    print(f"{out}: {len(check.entries)} resources, panel groups {[hex(g) for g in GROUPS]} patched")
    targets = write_saved_props(GAME, Path(args.out))
    print(f'{len(targets)} pack saved property lists: port graphics options added')
    targets = write_camera_defaults(GAME, Path(args.out))
    print(f'{len(targets)} pack global property defaults: edge scrolling and camera drift disabled')
    targets = write_graphics_queries(GAME, Path(args.out))
    print(f'{len(targets)} pack graphics rules: native EVENT queries enabled')
    text_rel = Path('EP9/TSData/Res/Text/Neighborhood.package')
    strings = dbpf.Package(GAME / text_rel)
    entry = next(e for e in strings.entries if (e['type'], e['group'], e['instance']) == (0x53545223, 0xFFFFFFFF, 205))
    patched = patch_neighborhood_warning(strings.read(entry))
    rewrite_package(GAME / text_rel, Path(args.out) / text_rel, {(entry['type'], entry['group'], entry['instance']): patched})
    print('Russian neighborhood hint: installed packs need associated districts')
    modules = json.loads((ROOT / 'port.example.json').read_text(encoding='utf-8'))['modules']
    for module in modules:
        relative = Path(module['source'])
        original = GAME / relative
        data = original.read_bytes()
        if hashlib.sha256(data).hexdigest() != module['sha256']:
            raise SystemExit(f'{original} does not match the supported Legacy Collection image')
        target = Path(args.out) / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    count = sum(p.is_file() for p in Path(args.out).rglob('*'))
    if count > 64:
        raise SystemExit(f'{count} overlay files exceed the host capacity of 64')
    print(f'{count} overlay files fit the host capacity of 64')


if __name__ == "__main__":
    main()
