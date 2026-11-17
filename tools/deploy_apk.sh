#!/bin/bash
# Put the game on a device: the APK (build/android-game/sims2.apk, tools/build_game_apk.py)
# with its storage permissions and every pack's TSData (/sdcard/Sims2/game, ~12 GB).
# Original PE files come from EP9/TSBin; adaptation resources ship in the APK.
# The profile (/sdcard/Sims2/userdata) is the game's own: it fills it on its
# first start. Files already there with the same size are skipped by adb push
# --sync, so a second run only completes an interrupted one.
#   tools/deploy_apk.sh SERIAL [PACK...]    (default: every pack; e.g. SERIAL Base EP9)
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
S="${1:?serial}"
shift
[ "$#" -gt 0 ] || set -- Base EP9 EP1 EP2 EP3 SP1 SP2 EP4 EP5 SP4 SP5 EP6 SP6 EP7 SP7 EP8
packs=("$@")
cd "$(dirname "$0")/.."
GAME="${S2_GAME:-C:/Games/The Sims 2 Legacy}"
for pack in "${packs[@]}"; do
    case "$pack" in Base|EP[1-9]|SP[124567]) ;; *) echo "unknown pack: $pack" >&2; exit 1;; esac
    [ -d "$GAME/$pack/TSData" ] || { echo "missing game data: $GAME/$pack/TSData" >&2; exit 1; }
done
# Check final file growth before changing the device. Existing matching-size
# files need no extra storage; reserve space for the profile and APK install.
python - "$ADB" "$S" "$GAME" "${packs[@]}" <<'PY'
from pathlib import Path
import subprocess
import sys
import zipfile

adb, serial, game, *packs = sys.argv[1:]
apk = Path('build/android-game/sims2.apk')
with zipfile.ZipFile(apk) as archive:
    install_bytes = apk.stat().st_size * 2 + sum(f.file_size for f in archive.infolist())
sources = {}
def add_tree(local, remote):
    local = Path(local)
    if not local.is_dir():
        sys.exit(f'missing source directory: {local}')
    for path in local.rglob('*'):
        if path.is_file():
            sources[f'{remote}/{path.relative_to(local).as_posix()}'] = path.stat().st_size
for pack in packs:
    add_tree(Path(game) / pack / 'TSData', f'/sdcard/Sims2/game/{pack}/TSData')
for name in ('Sims2EP9.exe', 'ijl15.dll'):
    path = Path(game) / 'EP9/TSBin' / name
    if not path.is_file():
        sys.exit(f'missing game binary: {path}')
    sources[f'/sdcard/Sims2/game/EP9/TSBin/{name}'] = path.stat().st_size

def shell(command):
    return subprocess.check_output([adb, '-s', serial, 'shell', command], text=True)
df = shell('df -Pk /sdcard').splitlines()
try:
    available = int(df[-1].split()[3]) * 1024
except (IndexError, ValueError):
    sys.exit(f'cannot read device free space: {df}')
existing = {}
for line in shell("find /sdcard/Sims2/game -type f -exec stat -c '%s %n' {} + 2>/dev/null; true").splitlines():
    size, _, name = line.partition(' ')
    if size.isdigit():
        existing[name] = int(size)
growth = sum(max(0, size - existing.get(name, 0)) for name, size in sources.items())
required = growth + install_bytes + 1024**3
print(f'Storage: {available / 1024**3:.2f} GiB free, {required / 1024**3:.2f} GiB needed '
      f'(includes 1 GiB for the profile; packs: {", ".join(packs)})', flush=True)
if available < required:
    sys.exit('insufficient storage; free space or select fewer packs before deploying')
PY
shell() { "$ADB" -s "$S" shell "$@"; }
start=$(date +%s)
"$ADB" -s "$S" install -r build/android-game/sims2.apk | tail -1
shell "pm grant org.sims2.port android.permission.READ_EXTERNAL_STORAGE; pm grant org.sims2.port android.permission.WRITE_EXTERNAL_STORAGE" || true
shell "mkdir -p /sdcard/Sims2/game/EP9/TSBin /sdcard/Sims2/userdata"
for name in Sims2EP9.exe ijl15.dll; do
    "$ADB" -s "$S" push --sync "$GAME/EP9/TSBin/$name" /sdcard/Sims2/game/EP9/TSBin/ | tail -1
done
for pack in "${packs[@]}"; do
    t=$(date +%s)
    shell "mkdir -p /sdcard/Sims2/game/$pack"
    "$ADB" -s "$S" push --sync "$GAME/$pack/TSData" "/sdcard/Sims2/game/$pack/" | tail -1
    echo "$pack in $(( $(date +%s) - t )) s"
done
echo "deployed to $S in $(( $(date +%s) - start )) s: $(shell 'du -sh /sdcard/Sims2/game | cut -f1') of game data"
