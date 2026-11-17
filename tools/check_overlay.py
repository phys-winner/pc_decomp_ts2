"""Check that adding saved graphics keys preserves the game's save whitelist."""
from pathlib import Path
import tempfile
import re
import hashlib
import xml.etree.ElementTree as ET

from build_overlay import (GAME, OPTIONS, GROUPS, PANEL_INSTANCE, CAPTURE_PANELS, CAPTURE_BACKDROP,
                           CAPTURE_ART_RECTS, dbpf, patch_panel, patch_capture_buttons, patch_capture_backdrop,
                           patch_saved_props, write_saved_props, patch_camera_defaults, write_camera_defaults,
                           patch_graphics_queries, write_graphics_queries, port_strings,
                           PORT_STRING_INSTANCE, PORT_STRING_PACKAGE, rewrite_package)


def main():
    from check_neighborhood_warning import entries
    strings = port_strings()
    rows = entries(strings)
    by_language = {lang: [text for code, text, _ in rows if code == lang] for lang, _, _ in rows}
    assert len(rows) == 88 and len(by_language) == 22
    assert [lang for lang, _, _ in rows] == sorted(lang for lang, _, _ in rows)
    assert by_language[1] == ['UI Scale', 'Frame Rate Limit', 'Render Scale', 'Off']
    assert by_language[13][1] == 'Предел FPS' and by_language[4][0] == 'UI-Skalierung'
    assert by_language[16][2] == 'Skala renderowania'
    original_strings = GAME / PORT_STRING_PACKAGE
    key = (0x53545223, 0xFFFFFFFF, PORT_STRING_INSTANCE)
    with tempfile.TemporaryDirectory(prefix='sims2-port-strings-') as directory:
        target = Path(directory) / PORT_STRING_PACKAGE.name
        original = dbpf.Package(original_strings)
        # Resolve original captions through the same package and one-based indices
        # used below. Merely finding translations in an unrelated package hid the
        # previous failure: all four port labels fell back to English on device.
        def table(package, instance, language):
            entry = next(e for e in package.entries
                         if (e['type'], e['group'], e['instance']) == (0x53545223, 0xFFFFFFFF, instance))
            return [text for code, text, _ in entries(package.read(entry)) if code == language]
        assert table(original, 0x81, 1)[1 - 1] == 'Graphics/Performance Options'
        assert table(original, 0x80, 1)[3 - 1] == 'Off'
        rewrite_package(original_strings, target, {}, additions={key: strings})
        actual = dbpf.Package(target)
        ui = dbpf.Package(GAME / 'EP9/TSData/Res/UI/ui.package')
        for group in GROUPS:
            entry = next(e for e in ui.entries
                         if (e['type'], e['group'], e['instance']) == (0, group, PANEL_INSTANCE))
            original_panel = ui.read(entry).decode('latin-1')
            assert 'caption="Graphics/Performance Options" captionres={7f232750,00810001}' in original_panel
            assert 'caption="Off" captionres={7f232750,00800003}' in original_panel
            patched_panel = patch_panel(original_panel)
            refs = re.findall(r'caption="([^"]*)" captionres=\{7f232750,([0-9a-f]{8})\}', patched_panel)
            port_refs = [(caption, int(address, 16) & 0xffff) for caption, address in refs
                         if int(address, 16) >> 16 == PORT_STRING_INSTANCE]
            assert len(port_refs) == 4
            for caption, index in port_refs:
                assert 1 <= index <= 4
                assert table(actual, PORT_STRING_INSTANCE, 1)[index - 1] == caption
                for language, labels in by_language.items():
                    assert table(actual, PORT_STRING_INSTANCE, language)[index - 1] == labels[index - 1]
        assert len(actual.entries) == len(original.entries) + 1
        old_entries = {tuple(e[k] for k in ('type','group','instance')): e for e in original.entries if e['type'] != dbpf.DIR_TYPE}
        for e in actual.entries:
            current = tuple(e[k] for k in ('type','group','instance'))
            if current == key: assert actual.read(e) == strings
            elif e['type'] != dbpf.DIR_TYPE: assert actual.raw(e) == original.raw(old_entries[current])
        try:
            rewrite_package(original_strings, target, {}, additions={next(iter(old_entries)): strings})
        except ValueError:
            pass
        else:
            raise AssertionError('duplicate additional resource accepted')
    assert all(labels != by_language[1] for language, labels in by_language.items() if language not in (1, 2))
    assert by_language[17][0] == '界面缩放' and by_language[18][0] == '介面縮放'
    assert by_language[28][0] == 'Felület mérete' and by_language[35][0] == 'Escala interface'
    print('port STR#: all 21 game languages / 22 STR# IDs; Options namespace and one-based caption references; original bytes retained and duplicate-resource rejection PASS')
    package = dbpf.Package(GAME / 'EP9/TSData/Res/UI/ui.package')
    capture_entries = [e for e in package.entries if e['type'] == 0 and e['instance'] in CAPTURE_PANELS]
    assert len(capture_entries) == 3
    for entry in capture_entries:
        original = package.read(entry).decode('latin-1')
        patched = patch_capture_buttons(original)
        assert patch_capture_buttons(patched) == patched
        before, after = original.splitlines(), patched.splitlines()
        changed = [(a, b) for a, b in zip(before, after) if a != b]
        assert len(changed) == 2 and len(before) == len(after)
        for a, b in changed:
            assert 'winflag_visible=no' in b and 'winflag_enabled=no' in b and 'winflag_ignoremouse=yes' in b
            assert re.search(r'id=0x[0-9a-f]+', a)[0] == re.search(r'id=0x[0-9a-f]+', b)[0]
    print('capture controls: all six buttons hidden/disabled, IDs retained, idempotent PASS')
    entry = next(e for e in package.entries if (e['type'], e['group'], e['instance']) == CAPTURE_BACKDROP)
    original = package.read(entry)
    assert hashlib.sha256(original).hexdigest() == '3808e79be54bd57a11347e947279269813dadab9ee55b9b64e5eb4f287a89196'
    patched = patch_capture_backdrop(original)
    assert len(patched) == len(original) and patch_capture_backdrop(patched) == patched
    assert patched[:18] == original[:18] and patched[-26:] == original[-26:]
    changed = set()
    for y in range(189):
        for x in range(179):
            at = 18 + ((188 - y) * 179 + x) * 4
            assert patched[at + 3] == original[at + 3]
            if patched[at:at + 4] != original[at:at + 4]:
                assert any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in CAPTURE_ART_RECTS)
                changed.add((x, y))
    assert len(changed) > 800
    for x, y, expected_rgb in ((44, 144, (131, 137, 194)), (76, 151, (127, 133, 189))):
        at = 18 + ((188 - y) * 179 + x) * 4
        assert patched[at:at + 3][::-1] == bytes(expected_rgb)
        assert patched[at:at + 3] != original[at:at + 3]
    transparent_icon = bytearray(original)
    transparent_icon[18 + ((188 - 144) * 179 + 44) * 4 + 3] = 0
    for bad in (b'', original + b'\0', bytes(transparent_icon)):
        try:
            patch_capture_backdrop(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid or nonopaque capture backdrop accepted')
    print(f'capture backdrop: {len(changed)} pixels in two icon rectangles only; all alpha/header/footer/other pixels unchanged, pixel oracle/idempotence/format guards PASS')
    for group in GROUPS:
        entry = next(e for e in package.entries
                     if (e['type'], e['group'], e['instance']) == (0, group, PANEL_INSTANCE))
        lines = patch_panel(package.read(entry).decode('latin-1')).splitlines()
        start = next(i for i, line in enumerate(lines) if 'caption="radio:s2FrameRateLimit:' in line)
        assert 'caption="radio:s2FrameRateLimit:int:0:30:60"' in lines[start]
        bounds = tuple(map(int, re.search(r'area=\(([^)]+)\)', lines[start])[1].split(',')))
        buttons = []
        for line in lines[start + 1:]:
            if '</CHILDREN>' in line:
                break
            if 'clsid=GZWinBtn ' in line:
                rect = tuple(map(int, re.search(r'area=\(([^)]+)\)', line)[1].split(',')))
                buttons.append((int(re.search(r'id=0x([0-9a-f]+)', line)[1], 16),
                                re.search(r'caption="([^"]*)"', line)[1], rect))
        assert [(i, label) for i, label, _ in buttons] == list(enumerate(('Off', '30', '60') + ('',) * 7, 1))
        for i, (_, _, rect) in enumerate(buttons):
            x0, y0, x1, y1 = rect
            assert 0 <= x0 < x1 <= bounds[2] - bounds[0] and 26 <= y0 < y1 <= bounds[3] - bounds[1]
            assert y1 - y0 == 16
            for _, _, other in buttons[:i]:
                assert x1 <= other[0] or other[2] <= x0 or y1 <= other[1] or other[3] <= y0
    print('FPS overlay: both variants reserve ten contained, nonoverlapping radios for display-specific rates PASS')
    names = [option[1].encode('ascii') for option in OPTIONS]
    original = (GAME / 'EP9/TSData/Res/Config/savedProps.txt').read_bytes()
    for source in (original, b'# saved keys\nactiveDeviceNum', b'# comment\r\n' + names[0] + b'\r\n'):
        result = patch_saved_props(source)
        assert result.startswith(source)
        keys = [line.partition(b'#')[0].strip() for line in result.splitlines()]
        assert all(keys.count(name) == 1 for name in names)
        assert patch_saved_props(result) == result
        if b'\r\n' in source:
            assert b'\n' not in result.replace(b'\r\n', b'')
    originals = sorted(GAME.glob('*/TSData/Res/Config/savedProps.txt'))
    assert len(originals) == 16, len(originals)
    with tempfile.TemporaryDirectory(prefix='sims2-saved-props-') as directory:
        out = Path(directory)
        targets = write_saved_props(GAME, out)
        assert {p.relative_to(out) for p in targets} == {p.relative_to(GAME) for p in originals}
        for source, target in zip(originals, targets):
            before, after = source.read_bytes(), target.read_bytes()
            assert after.startswith(before), source
            keys = [line.partition(b'#')[0].strip() for line in after.splitlines()]
            assert all(keys.count(name) == 1 for name in names), source
            assert patch_saved_props(after) == after, source
        first = {p.relative_to(out): p.read_bytes() for p in targets}
        assert {p.relative_to(out): p.read_bytes() for p in write_saved_props(GAME, out)} == first
    print('savedProps overlay: all 16 pack paths covered, original bytes preserved, three keys once, idempotent PASS')
    globals = sorted(GAME.glob('*/TSData/Res/Config/globalProps.xml'))
    assert len(globals) == 16, len(globals)
    with tempfile.TemporaryDirectory(prefix='sims2-camera-defaults-') as directory:
        out = Path(directory)
        targets = write_camera_defaults(GAME, out)
        assert {p.relative_to(out) for p in targets} == {p.relative_to(GAME) for p in globals}
        for source, target in zip(globals, targets):
            before, after = source.read_bytes(), target.read_bytes()
            assert patch_camera_defaults(after) == after
            changes = []
            for key in ('edgeScrolling', 'CameraDriftCamEnabled'):
                original_node = ET.fromstring(before).find(f"./AnyBoolean[@key='{key}']")
                assert original_node is not None and original_node.text == 'true', source
                assert ET.fromstring(after).find(f"./AnyBoolean[@key='{key}']").text == 'false'
                pattern = rb'(<AnyBoolean\b[^>]*\bkey="' + key.encode() + rb'"[^>]*>)(true)(</AnyBoolean>)'
                changes.append(pattern)
            expected = before
            for pattern in changes:
                expected = re.sub(pattern, lambda m: m[1] + b'false' + m[3], expected)
            assert after == expected, source
        saved = write_saved_props(GAME, out)
        graphics = write_graphics_queries(GAME, out)
        originals = sorted(GAME.glob('*/TSData/Res/Config/Graphics Rules.sgr'))
        assert len(graphics) == len(originals) == 16
        for source, target in zip(originals, graphics):
            before, after = source.read_bytes(), target.read_bytes()
            assert after.startswith(before)
            assert after.splitlines()[-1] == b'boolProp queriesOK true'
            assert patch_graphics_queries(after) == after
        assert len(saved) + len(targets) + len(graphics) + 1 == 49 <= 64
        assert not list(out.glob('*/TSData/Res/UserData/**/userProps.xml'))
    sample = b'<cGZPropertySetString>\r\n  <AnyBoolean key="edgeScrolling" type="x"> true </AnyBoolean>\r\n<AnyBoolean type="y" key="CameraDriftCamEnabled">\nfalse\n</AnyBoolean>\r\n</cGZPropertySetString>'
    patched = patch_camera_defaults(sample)
    assert patched == sample.replace(b'> true <', b'> false <')
    assert patch_camera_defaults(patched) == patched
    for bad in (b'', sample.replace(b'edgeScrolling', b'unknown'), sample + sample):
        try:
            patch_camera_defaults(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('missing/duplicate camera default accepted')
    print('camera defaults: all 16 originals preserve every other byte, false only, format/idempotence/missing guards PASS')
    for original in (b'# rules', b'# rules\r\n', b'boolProp useShaders true\n'):
        patched = patch_graphics_queries(original)
        assert patched.startswith(original) and patched.splitlines()[-1] == b'boolProp queriesOK true'
        assert patch_graphics_queries(patched) == patched
        if b'\r\n' in original:
            assert b'\n' not in patched.replace(b'\r\n', b'')
    print('EVENT query rules: all 16 Config paths, exact append, no existing bytes changed, LF/CRLF/idempotence, 49 <= 64 overlays PASS')


if __name__ == '__main__':
    main()
