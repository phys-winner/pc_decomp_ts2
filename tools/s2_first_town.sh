#!/bin/bash
# The first load of Pleasantview from a pristine copy of the neighborhoods, timed:
# restores <userdata>/.../Neighborhoods from /data/local/tmp/sims2/nh-pristine
# (made once with cp -r while the game was stopped), starts the game, opens
# Pleasantview from the chooser (UI scale 300%: the chooser fits 1300x600) and
# waits for the first welcome dialog. Prints the phase times; logs go to OUT.*.
#   tools/s2_first_town.sh SERIAL OUT [--trace]
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB="${ADB:-adb}"
S="${1:?serial}"
OUT="${2:?output prefix}"
TRACE="${3:-}"
shell() { "$ADB" -s "$S" shell "$@"; }
U="/sdcard/Sims2/userdata/Documents/EA Games/The Sims 2 Legacy"
shell "am force-stop org.sims2.port"
shell "test -d /data/local/tmp/sims2/nh-pristine/Neighborhoods" || { echo "no pristine neighborhoods backup" >&2; exit 1; }
shell "rm -rf '$U/Neighborhoods' && cp -r /data/local/tmp/sims2/nh-pristine/Neighborhoods '$U/'"
shell "rm -f /sdcard/Sims2/userdata/iotrace /sdcard/Sims2/userdata/iotrace.log"
[ "$TRACE" = "--trace" ] && shell "touch /sdcard/Sims2/userdata/iotrace"
shell "rm -f /sdcard/Sims2/userdata/live.txt; touch /sdcard/Sims2/userdata/live.txt; logcat -c; am start -n org.sims2.port/.GameActivity >/dev/null"
start=$(date +%s)
until "$ADB" -s "$S" logcat -d -s Sims2:I | grep -q "ui-font\] clone originalSize=20"; do
    sleep 2
    [ $(( $(date +%s) - start )) -gt 400 ] && { echo "no chooser" >&2; exit 1; }
done
echo "chooser after $(( $(date +%s) - start )) s"
sleep 3
for c in "click 942 276 0" "sleep 2" "click 465 284 0" "sleep 2" "click 608 532 0"; do
    case "$c" in sleep*) sleep "${c#sleep }";; *) shell "echo '$c' >> /sdcard/Sims2/userdata/live.txt";; esac
done
play=$(date +%s)
until "$ADB" -s "$S" logcat -d -s Sims2:I | grep -q "fit\] modal open"; do
    sleep 3
    [ $(( $(date +%s) - play )) -gt 900 ] && { echo "no town" >&2; break; }
done
echo "town after $(( $(date +%s) - play )) s"
"$ADB" -s "$S" logcat -d -v monotonic -s Sims2:I > "$OUT.logcat.txt"
sleep 3
[ "$TRACE" = "--trace" ] && "$ADB" -s "$S" pull /sdcard/Sims2/userdata/iotrace.log "$OUT.iotrace.log" >/dev/null && shell "rm -f /sdcard/Sims2/userdata/iotrace /sdcard/Sims2/userdata/iotrace.log"
grep -E "live\] click 608|fit\] modal open" "$OUT.logcat.txt" | head -2
