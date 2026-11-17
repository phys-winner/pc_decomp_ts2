#!/bin/bash
# Per-thread CPU time and context switches of the running shell build over
# SECONDS (default 10): which threads run, how often each sleeps (voluntary)
# and is preempted (involuntary), and how many threads came and went.
#   tools/s2_threads.sh [SECONDS]
set -e
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
SERIAL="${S2_SERIAL:-192.168.1.103:5555}"
BIN="${S2_BIN:-sims2-arm64}"
cd "$(dirname "$0")/.."
secs="${1:-10}"
"$ADB" -s "$SERIAL" shell "P=\$(pidof $BIN); snap() { for t in /proc/\$P/task/*; do echo \"\$(basename \$t)|\$(cat \$t/comm 2>/dev/null)|\$(cat \$t/stat 2>/dev/null | sed 's/.*) //')|\$(grep -E '^(non)?voluntary' \$t/status 2>/dev/null | tr '\n' ' ')\"; done; }; snap > /data/local/tmp/sims2/threads1.txt; sleep $secs; snap > /data/local/tmp/sims2/threads2.txt"
"$ADB" -s "$SERIAL" pull /data/local/tmp/sims2/threads1.txt build/threads1.txt >/dev/null
"$ADB" -s "$SERIAL" pull /data/local/tmp/sims2/threads2.txt build/threads2.txt >/dev/null
python - "$secs" <<'EOF'
import sys
secs = float(sys.argv[1])
def load(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("|")
        if len(parts) < 4 or not parts[2]:
            continue
        f = parts[2].split()
        ticks = int(f[11]) + int(f[12])        # utime + stime (fields 14, 15 of stat)
        cpu = int(f[36]) if len(f) > 36 else -1  # field 39: last CPU
        sw = parts[3].split()
        vol = int(sw[1]) if len(sw) > 1 else 0
        invol = int(sw[3]) if len(sw) > 3 else 0
        out[parts[0]] = (parts[1], ticks, vol, invol, cpu)
    return out
a, b = load("build/threads1.txt"), load("build/threads2.txt")
rows = []
for tid, (name, t2, v2, i2, cpu) in b.items():
    if tid in a:
        _, t1, v1, i1, _ = a[tid]
        rows.append((t2 - t1, tid, name, (v2 - v1) / secs, (i2 - i1) / secs, cpu))
rows.sort(reverse=True)
print("threads: %d then %d (%d new, %d gone)" % (len(a), len(b), len(set(b) - set(a)), len(set(a) - set(b))))
for ticks, tid, name, vol, invol, cpu in rows[:14]:
    print("%7s %-16s %5.1f%% cpu  %7.0f sleeps/s  %7.0f preemptions/s  last cpu %d" % (tid, name, ticks / secs, vol, invol, cpu))
EOF
