"""Read-only, bounded FreePlay object-control inventory from the pinned original ELF.

No IDA database or whole-binary disassembly is created. Widget IDs, binding calls,
closure RTTI/vtables and callback code are independently retained as compact evidence.
"""
import hashlib
import io
import json
import re
import struct
from pathlib import Path

import capstone
from elftools.elf.elffile import ELFFile
import dbpf

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT / 'mobile_app/lib/arm64-v8a/libapprunner.so'
OUT = ROOT / 'build/test/freeplay-controls'
EXPECTED = 'f11c59c6389a711eaa19fc758e141e09425866cb1ae099d4cb6a186cb58e6a37'


def main():
    data = ORIGINAL.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    assert digest == EXPECTED, 'The inventory is pinned to the supplied original library'
    elf = ELFFile(io.BytesIO(data))
    segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    reloc = {r['r_offset']: r['r_addend'] for r in elf.get_section_by_name('.rela.dyn').iter_relocations()
             if r['r_info_type'] == 1027}

    def read(addr, count):
        for segment in segments:
            if segment['p_vaddr'] <= addr < addr + count <= segment['p_vaddr'] + segment['p_filesz']:
                offset = segment['p_offset'] + addr - segment['p_vaddr']
                return data[offset:offset + count]
        raise ValueError(hex(addr))

    def pointer(addr):
        return reloc.get(addr, struct.unpack('<Q', read(addr, 8))[0])

    def string(addr):
        return read(addr, 256).split(b'\0')[0].decode('ascii')

    def closure_name(vtable):
        return string(pointer(pointer(vtable - 8) + 8))

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    hdr = elf.get_section_by_name('.eh_frame_hdr')
    raw = hdr.data()
    assert raw[:4] == bytes.fromhex('011b033b')
    count = struct.unpack_from('<I', raw, 8)[0]
    functions = [hdr['sh_addr'] + struct.unpack_from('<i', raw, 12 + i * 8)[0] for i in range(count)]

    def disasm(lo, hi):
        return [dict(address=hex(ins.address), mnemonic=ins.mnemonic, operands=ins.op_str)
                for ins in md.disasm(read(lo, hi - lo), lo)]

    assert string(0xA7A3FF) == 'N2im3app16BuildModeArcToolE'
    assert pointer(0x48478A0) == 0x48479A0 and pointer(0x48479A8) == 0xA7A3FF
    assert pointer(0x48478A8 + 8 * 8) == 0x2A69980
    controls = [
        # ID, string address (None for an inline string), binding call, closure vtable.
        ('BTN_ACCEPT', 0x7BD8E4, 0x2A6A0F0, 0x4847C58),
        ('BTN_DECLINE', 0x88AF02, 0x2A6A198, 0x4847CD8),
        ('BTN_INVENTORY', 0x8111A5, 0x2A6A240, 0x4847D58),
        ('BTN_ROTATE', 0x8ECC5A, 0x2A6A2E4, 0x4847DD8),
        ('BTN_SELL', None, 0x2A6A384, 0x4847E58),
        ('BTN_RETURN', 0x898E0A, 0x2A6A428, 0x4847ED8),
        ('BTN_DUPLICATE', 0x79B7D9, 0x2A6AF2C, 0x4848358),
        ('BTN_ACCEPT_DUPLICATE', 0x8BD16C, 0x2A6B09C, 0x48483D8),
    ]
    result = []
    for control, addr, call, vtable in controls:
        if addr:
            assert string(addr) == control
        binding = disasm(call, call + 4)
        assert binding[0]['mnemonic'] == 'bl' and binding[0]['operands'] == '#0x264f56c'
        name = closure_name(vtable)
        assert 'BuildModeArcTool7OnEvent' in name
        invoke = pointer(vtable + 6 * 8)
        index = functions.index(invoke)
        result.append(dict(control=control, string_address=hex(addr) if addr else 'inline immediate BTN_SELL',
                           bind_call=hex(call), binder='0x264f56c', closure_vtable=hex(vtable), closure_rtti=name,
                           callback=hex(invoke), callback_disasm=disasm(invoke, functions[index + 1])))
    # The compiler constructs BTN_SELL inline at the binding site, rather than a standalone rodata string.
    inline = int.from_bytes(b'BTN_SELL', 'little')
    assert inline == 0x4C4C45535F4E5442
    sell_disasm = disasm(0x2A6A33C, 0x2A6A388)
    assert sum(ins['mnemonic'] == 'movk' and ins['operands'].startswith('x9,') for ins in sell_disasm) == 3
    # The confirm target creates closures explicitly named OnConfirmClicked in the original RTTI.
    assert 'BuildModeArcTool16OnConfirmClicked' in closure_name(0x4848458)
    camera_fields = {'cameraToggleVisible': '0x7caf81', 'onTapCameraToggle': '0x7ebafa'}
    for name, address in camera_fields.items():
        assert string(int(address, 16)) == name
    assert string(0x8AF6B8) == 'use_side_camera_in_bm'
    camera_callback = disasm(0x2A4F518, 0x2A4F618)
    assert any(i['mnemonic'] == 'bl' and i['operands'] == '#0x25289f0' for i in camera_callback)
    assert any(i['mnemonic'] == 'bl' and i['operands'] == '#0x2525e6c' for i in camera_callback)
    parking_fields = {'parking_space_index': '0x8bb49a', 'in_parking_space': '0x78d105',
                      'OBJECT_ROAD_PATH_APARTMENT': '0x801d74', 'car_facing': '0x83e912',
                      'car_offset_x': '0x8af4ab', 'car_offset_z': '0x82619c'}
    for name, address in parking_fields.items():
        assert string(int(address, 16)) == name
    original_ui = Path(r'C:\Games\The Sims 2 Legacy\EP9\TSData\Res\UI\ui.package')
    package = dbpf.Package(original_ui)
    sims2_controls = []
    for instance, wanted in [(0x49060003, [0xA5, 0xA4, 0xAE, 0xAB, 0xA1, 0xA2, 0xA3, 0xAF]),
                             (0x49060004, [0xCC607031, 0xCC607032, 0xAE, 0xCC607030, 0xCC607033, 0xCC607034, 0xCC607036, 0xCC6070AF]),
                             (0x49064800, [0x14000000])]:
        entries = [entry for entry in package.entries if entry['type'] == 0 and entry['group'] == 0xA99D8A11 and entry['instance'] == instance]
        assert len(entries) == 1
        markup = package.read(entries[0]).decode('latin-1')
        for ident in wanted:
            lines = [line.strip() for line in markup.splitlines() if re.search(r'\bid=0x%08x\b' % ident, line, re.I)]
            assert len(lines) == 1, (hex(instance), hex(ident))
            attributes = {match[0]: match[1] or match[2] for match in re.findall(r'([a-z_]+)=(?:"([^"]*)"|([^ >]+))', lines[0])}
            sims2_controls.append(dict(resource=dict(type='0x0', group='0xa99d8a11', instance=hex(instance)),
                                       attributes={key: attributes.get(key) for key in ['id', 'clsid', 'area', 'caption', 'tiptext', 'winflag_visible', 'winflag_enabled']}))
    payload = dict(original=str(ORIGINAL), size=len(data), sha256=digest,
                   class_rtti='0xa7a3ff', class_vtable='0x48478a8', event_handler='0x2a69980',
                   object_controls=result, inline_sell_binding_disasm=sell_disasm,
                   confirm_handler='0x2a6d740', decline_handler='0x2a6fab0', inventory_handler='0x2a71a40',
                   sell_handler='0x2a708dc', return_handler='0x2a714f4',
                   camera_databind_fields=camera_fields,
                   camera_side_view=dict(callback='0x2a4f518', generated_setter='0x2a47754', model_setter='0x25289f0',
                                         preference_setter='0x236245c', preference='use_side_camera_in_bm', preference_address='0x8af6b8',
                                         mode_switcher='0x2525e6c', setup_disasm=disasm(0x2A45730, 0x2A45754),
                                         callback_disasm=camera_callback, model_setter_disasm=disasm(0x25289F0, 0x2528A2C),
                                         preference_setter_disasm=disasm(0x2362474, 0x23624D4),
                                         camera_switcher_disasm=disasm(0x2525F00, 0x2525F90)),
                   return_handler_disasm=disasm(0x2A714F4, 0x2A715A8),
                   return_scope=dict(scope='Vehicle/parking-specific return, not generic furniture inventory or Undo',
                                     leaf='0x2abac38', parking_transform='0x22bc070', reposition='0x2aa9908',
                                     anchors=parking_fields,
                                     leaf_disasm=disasm(0x2ABAC38, 0x2ABAE9C),
                                     parking_transform_disasm=disasm(0x22BC070, 0x22BC348),
                                     type_guard='Runtime StringId0x451 tested via0x213ae3c; exact localized type name unresolved'),
                   sims2_original_ui=dict(source=str(original_ui), sha256=hashlib.sha256(original_ui.read_bytes()).hexdigest(),
                                          controls=sims2_controls,
                                          limits='Original visible controls, not a claim that top-down/45-degree presets equal the FreePlay side camera or that Sim Inventory equals a global furniture bank.'),
                   limits='Widget bindings and callback ABI are static evidence. No screenshot/active visibility claim; exact side-view projection profile is not reconstructed; no economy, inventory-model or construction-feature transplantation claim.')
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'result.json').write_text(json.dumps(payload, indent=2), encoding='utf-8')
    print(f'PASS: {len(result)} bound BuildModeArcTool buttons, closure RTTI/callback chains, camera fields; {OUT / "result.json"}')


if __name__ == '__main__':
    main()
