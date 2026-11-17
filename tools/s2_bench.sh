#!/bin/bash
# A repeatable frame-time benchmark on the target: the shell build into the
# first neighborhood (tools/s2_scenario.sh nhood: the camera where the fly-in
# leaves it, the drift camera off), then the mean of the frame reports (one
# every ~5 s) over SECONDS (default 30). The prime core's clock and the
# hottest of its sensors are sampled alongside: the SoC throttles under a
# sustained load (cpu7 2150 MHz cap near 90 C), so compare runs started cool.
# The game is stopped afterwards unless S2_KEEP=1.
#   tools/s2_bench.sh [SECONDS]        (extra environment: S2_ENV, S2_BUILD for the push)
set -e
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
SERIAL="${S2_SERIAL:-192.168.1.103:5555}"
cd "$(dirname "$0")/.."
seconds="${1:-30}"
prime_state() {
    "$ADB" -s "$SERIAL" shell 'f=/sys/devices/system/cpu/cpu7/cpufreq; t=0;
        for z in /sys/class/thermal/thermal_zone*; do case $(cat $z/type) in cpu-2-*) v=$(cat $z/temp); [ $v -gt $t ] && t=$v;; esac; done;
        echo "cpu7 $(( $(cat $f/scaling_cur_freq) / 1000 )) MHz (cap $(( $(cat $f/scaling_max_freq) / 1000 ))), $(( t / 1000 )) C"'
}
echo "before: $(prime_state)"
S2_ENV="${S2_ENV:-S2_FPS_LIMIT=0}" bash tools/s2_scenario.sh nhood build/live/bench.png >/dev/null
sleep 5
bash tools/s2.sh send "cursor 695 316"          # away from every edge and window
"$ADB" -s "$SERIAL" shell "f=/sys/devices/system/cpu/cpu7/cpufreq; for i in \$(seq $seconds); do cat \$f/scaling_cur_freq; sleep 1; done" |
    python -c "
import sys
v = [int(x) / 1000 for x in sys.stdin.read().split()]
print('cpu7 clock during the run: mean %.0f MHz, min %.0f, max %.0f' % (sum(v) / len(v), min(v), max(v)) if v else 'no clock samples')
"
echo "after: $(prime_state)"
bash tools/s2.sh log 400 | grep " draws" | tail -n "$(( seconds / 5 ))" | python -c "
import re, sys
rows = [re.search(r'([\d.]+) fps; per frame ([\d.]+) ms wall, ([\d.]+) ms cpu.*?(\d+) draws', l) for l in sys.stdin]
rows = [tuple(float(x) for x in m.groups()) for m in rows if m]
if not rows:
    sys.exit('no frame reports')
n = len(rows)
print('samples %d  fps %.1f  wall %.2f ms  cpu %.2f ms  draws %.0f' % (n, sum(r[0] for r in rows) / n,
      sum(r[1] for r in rows) / n, sum(r[2] for r in rows) / n, sum(r[3] for r in rows) / n))
"
[ "${S2_KEEP:-0}" = 1 ] || bash tools/s2.sh stop
