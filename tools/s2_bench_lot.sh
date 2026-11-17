#!/bin/bash
# A live-mode benchmark on the target: the shell build into the first family
# lot (tools/s2_scenario.sh lot: tutorial and tips dismissed, the game left
# paused), the mean frame report over PAUSED seconds there, then the game at
# speed 1 (key '1') for RUNNING seconds after a short settle. The prime core's
# clock is sampled through both (the SoC throttles; compare runs started
# cool). The game is stopped afterwards unless S2_KEEP=1.
#   tools/s2_bench_lot.sh [PAUSED] [RUNNING]       (extra environment: S2_ENV, S2_KEEP, S2_PROFILE=1,
#                                                   S2_LOT=condo|house: tools/s2_scenario.sh S2_LOT_AT)
set -e
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
SERIAL="${S2_SERIAL:-192.168.1.103:5555}"
cd "$(dirname "$0")/.."
paused="${1:-30}"
running="${2:-40}"
clock() {   # sample cpu7's clock for $1 seconds, print the mean
    "$ADB" -s "$SERIAL" shell "f=/sys/devices/system/cpu/cpu7/cpufreq; for i in \$(seq $1); do cat \$f/scaling_cur_freq; sleep 1; done" |
        python -c "
import sys
v = [int(x) / 1000 for x in sys.stdin.read().split()]
print('%.0f' % (sum(v) / len(v)) if v else '?')
"
}
mean() {    # the mean of the last N frame reports
    bash tools/s2.sh log 400 | grep " draws" | tail -n "$1" | python -c "
import re, sys
rows = [re.search(r'([\d.]+) fps; per frame ([\d.]+) ms wall, ([\d.]+) ms cpu.*?(\d+) draws', l) for l in sys.stdin]
rows = [tuple(float(x) for x in m.groups()) for m in rows if m]
if not rows:
    sys.exit('no frame reports')
n = len(rows)
print('samples %d  fps %.1f  wall %.2f ms  cpu %.2f ms  draws %.0f' % (n, sum(r[0] for r in rows) / n,
      sum(r[1] for r in rows) / n, sum(r[2] for r in rows) / n, sum(r[3] for r in rows) / n))
"
}
case "${S2_LOT:-}" in
condo) export S2_LOT_AT="1027 326";;
house) export S2_LOT_AT="1309 552";;
esac
S2_ENV="${S2_ENV:-S2_FPS_LIMIT=0}" bash tools/s2_scenario.sh lot build/live/bench_lot.png >/dev/null
sleep 5
mhz=$(clock "$paused")
echo "paused:  $(mean $(( paused / 5 )))  cpu7 $mhz MHz"
bash tools/s2.sh send "key 31"
sleep 10
mhz=$(clock "$running")
echo "speed 1: $(mean $(( running / 5 )))  cpu7 $mhz MHz"
bash tools/s2.sh shot build/live/bench_lot_running.png >/dev/null
if [ "${S2_PROFILE:-0}" = 1 ]; then
    # 15 s of the main thread at speed 1 -> build/pgo/perf_lot.samples.txt
    "$ADB" -s "$SERIAL" shell 'P=$(pidof sims2-arm64); cd /data/local/tmp/sims2 && simpleperf record -t $P --call-graph fp -f 1000 --duration 15 -o perf_lot.data >/dev/null 2>&1; simpleperf report-sample -i perf_lot.data --show-callchain > perf_lot.txt 2>/dev/null'
    "$ADB" -s "$SERIAL" pull /data/local/tmp/sims2/perf_lot.txt build/pgo/perf_lot.samples.txt | tail -1
    "$ADB" -s "$SERIAL" shell 'rm -f /data/local/tmp/sims2/perf_lot.data /data/local/tmp/sims2/perf_lot.txt'
    echo "profiled: $(mean 3)"
fi
[ "${S2_KEEP:-0}" = 1 ] || bash tools/s2.sh stop
