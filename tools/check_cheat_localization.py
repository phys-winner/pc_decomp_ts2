"""Check translated labels against original fixed-name getters, including dormant cheats."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import pefile
from check_touch import JDK, ROOT


def main():
    fixture = json.loads((ROOT / 'tools/fixtures/cheat-command-names.json').read_text(encoding='utf-8'))
    original = ROOT / fixture['source']
    assert hashlib.sha256(original.read_bytes()).hexdigest() == fixture['source_sha256']
    pe = pefile.PE(str(original))
    base = pe.OPTIONAL_HEADER.ImageBase
    for command in fixture['commands']:
        table, method = int(command['vtable'], 16), int(command['name_method'], 16)
        assert struct.unpack('<I', pe.get_data(table - base + 0x14, 4))[0] == method
        code = pe.get_data(method - base, 6)
        assert code[0] == 0xB8 and code[5] == 0xC3
        pointer = struct.unpack_from('<I', code, 1)[0]
        assert pe.get_string_at_rva(pointer - base).decode('ascii') == command['name']
    names = sorted({command['name'] for command in fixture['commands']}, key=str.lower)
    names += ['kaChing', 'motherLode', 'exit', 'x', 'h', '?']
    java = r'''package org.sims2.port;
public class CheatLocalizationCheck {
 public static void main(String[] args) {
  String[] names=NAME_ARRAY;
  for(String name:names) {
   String original="Exact syntax: <name:string> [on|off] -flag\nSecond line\tunchanged";
   CheatCommands.Command c=new CheatCommands.Command(name+"\t"+original);
   assert c.name.equals(name)&&c.help.equals(original):name;
   assert c.title(false).equals(name):name;
   assert c.title(true).endsWith(" — "+name):name;
   assert c.title(true).matches("(?s).*[А-Яа-яЁё].*"):name;
   assert c.help(true).contains(original)&&c.help(false).contains(original):name;
   assert c.help(true).contains("Синтаксис и справка оригинальной игры:"):name;
   assert c.group()>=0&&c.group()<CheatCommands.groups(true).length:name;
   assert new CheatCommands.Command(name.toUpperCase(java.util.Locale.ROOT)).title(true).contains(" — "):name;
   assert new CheatCommands.Command(name).help(true).matches("(?s).*[А-Яа-яЁё].*"):name;
  }
  assert new CheatCommands.Command("money").help(true).contains("Знак")||new CheatCommands.Command("money").help(true).contains("знак");
  assert new CheatCommands.Command("deleteAllObjects").help(true).contains("doors");
  assert new CheatCommands.Command("disablePuppyKittenAging").help(true).contains("on — запретить");
  assert new CheatCommands.Command("UnknownModCommand\tmod syntax").title(true).equals("UnknownModCommand");
  assert new CheatCommands.Command("UnknownModCommand\tmod syntax").help(true).contains("mod syntax");
  java.util.List<CheatCommands.Command> unique=CheatCommands.parse(new String[]{null,"","terrainType\tsyntax","terrainType\tsyntax","terrainType\tother syntax","aging\ton/off"});
  assert unique.size()==4&&unique.get(0).name.equals("aging");
  assert unique.get(1).help.equals("syntax")&&unique.get(2).help.equals("other syntax");
  assert unique.get(3).title(false).equals("Developer mode (testingcheatsenabled)");
  System.out.println("PASS original fixed-name getter coverage and "+names.length+" localized command/alias rows: exact syntax/help retained, missing help, case handling and future command fallback");
 }
}'''.replace('NAME_ARRAY', '{' + ','.join(json.dumps(name) for name in names) + '}')
    with tempfile.TemporaryDirectory(prefix='sims2-cheat-localization-') as folder:
        out = Path(folder)
        harness = out / 'CheatLocalizationCheck.java'
        harness.write_text(java, encoding='utf-8')
        subprocess.run([str(JDK / 'bin/javac.exe'), '--release', '8', '-encoding', 'UTF-8', '-d', str(out),
                        str(harness), str(ROOT / 'android/app/src/org/sims2/port/CheatCommands.java')], check=True)
        subprocess.run([str(JDK / 'bin/java.exe'), '-ea', '-cp', str(out),
                        'org.sims2.port.CheatLocalizationCheck'], check=True)


if __name__ == '__main__':
    main()
