"""Sum the [iostat] lines of a native log or logcat over a time window.

    python -X utf8 tools/iostat_sum.py LOG [--from T] [--to T]

T is the logcat monotonic timestamp (first column of `logcat -v monotonic`);
without timestamps (sims2.log) every line counts. Prints, for the main thread
and for all threads, calls and seconds per family plus the main thread's CPU
seconds and the wall time covered (2 s per report)."""
import argparse
import collections
import re

FAMILY = re.compile(r'(\w+) (\d+) ([\d.]+)(?: ([\d.]+)MB)?')


def parse(part):
    out = {}
    for name, calls, secs, mb in FAMILY.findall(part):
        out[name] = (int(calls), float(secs), float(mb or 0))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--from', dest='start', type=float)
    ap.add_argument('--to', dest='end', type=float)
    a = ap.parse_args()
    main_sum = collections.defaultdict(lambda: [0, 0.0, 0.0])
    all_sum = collections.defaultdict(lambda: [0, 0.0, 0.0])
    cpu = wall = 0.0
    for line in open(a.log, encoding='utf-8', errors='replace'):
        if '[iostat]' not in line:
            continue
        stamp = line.split()[0]
        try:
            t = float(stamp)
        except ValueError:
            t = None
        if t is not None and ((a.start is not None and t < a.start) or (a.end is not None and t > a.end)):
            continue
        body = line.split('[iostat]', 1)[1]
        m = re.match(r'\s*([\d.]+) s main cpu ([\d.]+) \|(.*)\| all(.*)', body)
        if not m:
            continue
        wall += float(m.group(1))
        cpu += float(m.group(2))
        for target, part in ((main_sum, m.group(3)), (all_sum, m.group(4))):
            for k, (c, s, mb) in parse(part).items():
                target[k][0] += c
                target[k][1] += s
                target[k][2] += mb
    print(f'wall covered {wall:.1f} s, main thread cpu {cpu:.1f} s')
    for title, table in (('main', main_sum), ('all', all_sum)):
        print(title + ':')
        for k, (c, s, mb) in sorted(table.items(), key=lambda kv: -kv[1][1]):
            extra = f' {mb:.0f} MB' if mb else ''
            print(f'  {k:<9} {c:>7} calls {s:7.2f} s{extra}')


if __name__ == '__main__':
    main()
