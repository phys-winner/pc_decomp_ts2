#!/bin/bash
# The benchmarks on the installed APK (screen on, the app in front: the
# phone's real clocks): tools/s2_bench.sh (neighborhood) and, with "lot",
# tools/s2_bench_lot.sh, with the port's frame rate limit lifted for the run
# (userProps.xml backed up and restored: vsync, 120 Hz, still caps). The app
# is force-stopped and the screen put to sleep afterwards.
#   tools/s2_apk_bench.sh [nhood|lot] [SECONDS...]
set -e
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
SERIAL="${S2_SERIAL:-192.168.1.103:5555}"
cd "$(dirname "$0")/.."
PROPS="/sdcard/Sims2/userdata/Documents/EA Games/The Sims 2 Legacy/Config/userProps.xml"
shell() { "$ADB" -s "$SERIAL" shell "$@"; }
what="${1:-nhood}"
shift || true
echo "foreground: $(shell "dumpsys activity top | grep -m1 ACTIVITY" | sed 's/^ *//' | cut -c1-90)"
backup="$PROPS.bench-backup-$$"
saved=0
restore() {
    local status=$?
    trap - EXIT
    if [ "$saved" = 1 ]; then shell "mv -f '$backup' '$PROPS'" || status=1; fi
    S2_APK=1 bash tools/s2.sh stop || status=1
    exit "$status"
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
shell "test ! -e '$backup' && cp '$PROPS' '$backup'"
saved=1
python - "$ADB" "$SERIAL" "$PROPS" <<'PY'
import re
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET

adb, serial, path = sys.argv[1:]
text = subprocess.check_output([adb, '-s', serial, 'shell', f'cat {shlex.quote(path)}'], text=True)
text = re.sub(r'<AnyUint32\b[^>]*\bkey="s2FrameRateLimit"[^>]*>.*?</AnyUint32\s*>', '', text, flags=re.S)
close = '</cGZPropertySetString>'
if text.count(close) != 1:
    sys.exit('invalid userProps.xml: expected one property-set closing tag')
text = text.replace(close, '<AnyUint32 key="s2FrameRateLimit" type="0xeb61e4f7">0</AnyUint32>\n' + close)
ET.fromstring(text)
temp = path + '.bench-new'
subprocess.run([adb, '-s', serial, 'shell',
                f'cat > {shlex.quote(temp)} && mv -f {shlex.quote(temp)} {shlex.quote(path)}'],
               input=text, text=True, check=True)
PY
export S2_APK=1 S2_KEEP=0
if [ "$what" = lot ]; then bash tools/s2_bench_lot.sh "$@"; else bash tools/s2_bench.sh "$@"; fi
