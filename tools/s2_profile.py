"""Profile the installed APK with simpleperf and summarize lifted functions per phase.

    python -X utf8 tools/s2_profile.py record --serial SERIAL --out build/X/startup --seconds 110 --start
    python -X utf8 tools/s2_profile.py record --serial SERIAL --out build/X/town --seconds 60 \\
        --send "click 455 345 0"
    python -X utf8 tools/s2_profile.py report build/X/startup.data [--thread TID|NAME] [--from S] [--to S]
        [--top N] [--inclusive] [--timeline]

record: app-scoped cpu-cycles with frame-pointer call chains (the lifted code keeps
frame pointers), optionally starting the game fresh (force-stop first; the live
command file is replaced only while the app is stopped). The native log and a
logcat excerpt are pulled next to the profile. Live commands (android/host/script.c)
are appended to <data>/live.txt after the recorder starts.

report: symbolizes against the local unstripped build/arm64/libsims2.so (same
build-id as the APK's library), names lifted functions with their RTTI class
(build/rtti_methods.json, tools/rtti_classes.py --dump) and prints per-thread CPU,
self and inclusive time. Times are seconds since the first sample.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'org.sims2.port'
DATA = '/sdcard/Sims2/userdata'
LIVE = DATA + '/live.txt'
NDK = Path(os.environ.get('ANDROID_NDK', r'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018'))
ADB = os.environ.get('ADB', str(Path(os.environ.get('LOCALAPPDATA', r'C:\Users\Xilonen\AppData\Local'))
                                / 'Android/Sdk/platform-tools/adb.exe'))


class Device:
    def __init__(self, serial):
        self.base = [ADB, '-s', serial]

    def shell(self, command, check=True, timeout=60, input=None):
        result = subprocess.run(self.base + ['shell', command], capture_output=True, timeout=timeout, input=input)
        if check and result.returncode:
            raise RuntimeError(f'adb shell {command!r}: {result.stderr.decode(errors="replace")}')
        return result.stdout.decode(errors='replace')

    def private(self, command, **kwargs):
        return self.shell(f'run-as {PACKAGE} sh -c {shlex.quote(command)}', **kwargs)

    def pid(self):
        value = self.shell(f'pidof {PACKAGE}', check=False).strip()
        return int(value) if value.isdigit() else None

    def fetch_private(self, remote, local):
        with open(local, 'wb') as out:
            result = subprocess.run(self.base + ['exec-out', 'run-as', PACKAGE, 'cat', remote], stdout=out,
                                    stderr=subprocess.PIPE, timeout=600)
        if result.returncode:
            raise RuntimeError(f'fetch {remote}: {result.stderr.decode(errors="replace")}')


def record(args):
    device = Device(args.serial)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    name = out.name
    remote = f'files/{name}.data'
    if args.start:
        device.shell(f'am force-stop {PACKAGE}')
        time.sleep(0.5)
        commands = ''.join(c + '\n' for c in args.startup_command)
        device.shell(f'rm -f {LIVE}; cat > {LIVE}', input=commands.encode())
        device.shell('logcat -c', check=False)
        device.shell('input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard', check=False)
        extras = ''.join(f' --es {shlex.quote(k)} {shlex.quote(v)}' for k, v in (e.split('=', 1) for e in args.extra))
        t0 = time.monotonic()
        device.shell(f'am start -n {PACKAGE}/.GameActivity{extras}')
        pid = None
        while pid is None and time.monotonic() - t0 < 20:
            pid = device.pid()
            if pid is None:
                time.sleep(0.05)
        if pid is None:
            raise SystemExit('the game did not start')
    else:
        pid = device.pid()
        if pid is None:
            raise SystemExit('the game is not running')
        t0 = time.monotonic()
    device.private(f'rm -f {remote}', check=False)
    event = '-e cpu-clock --trace-offcpu' if args.offcpu else '-e cpu-cycles'
    command = (f'/system/bin/simpleperf record -p {pid} {event} --call-graph fp -f {args.freq} -m {args.mmap_pages} '
               f'--duration {args.seconds} -o {remote}')
    log = open(str(out) + '.record.log', 'wb')
    recorder = subprocess.Popen(device.base + ['shell', f'run-as {PACKAGE} {command}'], stdout=log,
                                stderr=subprocess.STDOUT)
    started = time.monotonic() - t0
    marks = {'pid': pid, 'recorder_started_s': started, 'sent': []}
    time.sleep(1.0)
    for command in args.send:
        if command.startswith('sleep '):
            time.sleep(float(command.split()[1]))
            continue
        device.shell(f"printf '%s\\n' {shlex.quote(command)} >> {LIVE}")
        marks['sent'].append([command, time.monotonic() - t0])
    recorder.wait(timeout=args.seconds + 120)
    log.close()
    marks['recorder_exit'] = recorder.returncode
    device.fetch_private(remote, str(out) + '.data')
    device.private(f'rm -f {remote}', check=False)
    with open(str(out) + '.sims2.log', 'wb') as f:
        f.write(device.shell(f'cat {DATA}/sims2.log', check=False).encode())
    with open(str(out) + '.logcat.txt', 'wb') as f:
        f.write(subprocess.run(device.base + ['logcat', '-d', '-v', 'monotonic', '-s', 'Sims2:*', 'ActivityTaskManager:I'],
                               capture_output=True).stdout)
    marks['lib'] = device.shell(f'pm path {PACKAGE}').strip()
    Path(str(out) + '.marks.json').write_text(json.dumps(marks, indent=1))
    print(json.dumps(marks, indent=1))


def symfs_for(data_path, lib_dso):
    """A symfs tree where the device library path resolves to the local unstripped library."""
    symfs = ROOT / 'build/perf/symfs'
    target = symfs / lib_dso.lstrip('/')
    local = ROOT / 'build/arm64/libsims2.so'
    if not target.is_file() or target.stat().st_size != local.stat().st_size or \
            target.stat().st_mtime < local.stat().st_mtime:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            target.unlink()
        try:
            os.link(local, target)
        except OSError:
            import shutil
            shutil.copyfile(local, target)
    return symfs


def report(args):
    sys.path.insert(0, str(NDK / 'simpleperf'))
    from simpleperf_report_lib import ReportLib
    rtti_path = ROOT / 'build/rtti_methods.json'
    rtti = json.loads(rtti_path.read_text()) if rtti_path.is_file() else {}

    def label(sym):
        if sym.startswith('sub_') and sym[4:12] in rtti:
            n = rtti[sym[4:12]][0]
            return '%s [%s]' % (sym, n[4:n.index('@@')] + n[n.index('@@') + 2:] if '@@' in n else n)
        return sym

    # Find the library's device path first (one pass over a few samples).
    lib = None
    probe = ReportLib()
    probe.SetRecordFile(args.data)
    probe.SetLogSeverity('error')
    for _ in range(20000):
        s = probe.GetNextSample()
        if s is None:
            break
        dso = probe.GetSymbolOfCurrentSample().dso_name
        if dso.endswith('/libsims2.so'):
            lib = dso
            break
    probe.Close()
    lib_reader = ReportLib()
    lib_reader.SetLogSeverity('error')
    if lib:
        lib_reader.SetSymfs(str(symfs_for(args.data, lib)))
    lib_reader.SetRecordFile(args.data)
    if args.offcpu:
        lib_reader.SetTraceOffCpuMode(args.offcpu)
    threads = collections.Counter()
    names = {}
    self_t = collections.Counter()
    incl_t = collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    timeline = collections.defaultdict(collections.Counter)
    first = None
    total = 0
    want = args.thread
    focus = args.focus
    while True:
        s = lib_reader.GetNextSample()
        if s is None:
            break
        if first is None:
            first = s.time
        t = (s.time - first) / 1e9
        period = s.period
        threads[s.tid] += period
        names[s.tid] = s.thread_comm
        timeline[int(t // args.bucket)][s.tid] += 1
        if args.start_at is not None and t < args.start_at:
            continue
        if args.end_at is not None and t > args.end_at:
            continue
        if want and not (str(s.tid) == want or s.thread_comm == want):
            continue
        sym = lib_reader.GetSymbolOfCurrentSample()
        chain = lib_reader.GetCallChainOfCurrentSample()
        frames = [sym.symbol_name] + [chain.entries[i].symbol.symbol_name for i in range(chain.nr)]
        if focus:
            if focus not in frames:
                continue
            frames = frames[:frames.index(focus) + 1]
        total += period
        self_t[frames[0]] += period
        seen = set()
        for i, f in enumerate(frames):
            if f in seen:
                continue
            seen.add(f)
            incl_t[f] += period
            if args.callers and f == args.callers and i + 1 < len(frames):
                callers[f][frames[i + 1]] += period
    lib_reader.Close()
    grand = sum(threads.values()) or 1
    print(f'samples weight total {grand:.3e}; selected {total:.3e} ({100 * total / grand:.1f}%)')
    print('threads:')
    for tid, v in threads.most_common(12):
        print(f'  {tid:>6} {names[tid]:<18} {100 * v / grand:6.2f}%')
    if args.timeline:
        top_threads = [tid for tid, _ in threads.most_common(5)]
        print('timeline (%% of one CPU per thread, from sample counts at --freq %d; bucket %gs):' % (args.freq, args.bucket))
        print('   t  ' + ' '.join(f'{str(t)[-5:]+names[t][:5]:>10}' for t in top_threads) + '      all')
        for b in sorted(timeline):
            c = timeline[b]
            scale = 100.0 / (args.freq * args.bucket)
            print(f'{b * args.bucket:5.0f} ' + ' '.join(f'{c[t] * scale:10.1f}' for t in top_threads) + f'{sum(c.values()) * scale:9.1f}')
    total = total or 1
    print(f'\nself (top {args.top}):')
    for sym, v in self_t.most_common(args.top):
        print(f'  {100 * v / total:6.2f}%  {label(sym)}')
    if args.inclusive:
        print(f'\ninclusive (top {args.top}):')
        for sym, v in incl_t.most_common(args.top):
            print(f'  {100 * v / total:6.2f}%  {label(sym)}')
    if args.callers:
        print(f'\ncallers of {args.callers}:')
        for sym, v in callers[args.callers].most_common(args.top):
            print(f'  {100 * v / total:6.2f}%  {label(sym)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    r = sub.add_parser('record')
    r.add_argument('--serial', default=os.environ.get('S2_SERIAL'), required=os.environ.get('S2_SERIAL') is None)
    r.add_argument('--out', required=True)
    r.add_argument('--seconds', type=float, default=60)
    r.add_argument('--freq', type=int, default=500)
    r.add_argument('--start', action='store_true', help='force-stop and start the game fresh')
    r.add_argument('--startup-command', action='append', default=[], help='live command present at start')
    r.add_argument('--extra', action='append', default=[], help='KEY=VALUE string extra for am start')
    r.add_argument('--send', action='append', default=[], help="live command after the recorder starts ('sleep S' waits)")
    r.add_argument('--offcpu', action='store_true', help='cpu-clock with off-CPU time (needs tracepoint access)')
    r.add_argument('--mmap-pages', type=int, default=128, help='per-CPU buffer pages; the default 1024 needs a perf_event_mlock_kb property an app cannot set')
    p = sub.add_parser('report')
    p.add_argument('data')
    p.add_argument('--thread')
    p.add_argument('--from', dest='start_at', type=float)
    p.add_argument('--to', dest='end_at', type=float)
    p.add_argument('--top', type=int, default=40)
    p.add_argument('--inclusive', action='store_true')
    p.add_argument('--timeline', action='store_true')
    p.add_argument('--bucket', type=float, default=2.0)
    p.add_argument('--freq', type=int, default=500, help='the recording frequency (timeline scale)')
    p.add_argument('--callers', help='list the direct callers of this symbol')
    p.add_argument('--focus', help='only samples under this symbol (frames above it dropped)')
    p.add_argument('--offcpu', choices=('on-cpu', 'off-cpu', 'on-off-cpu', 'mixed-on-off-cpu'))
    args = parser.parse_args()
    (record if args.command == 'record' else report)(args)


if __name__ == '__main__':
    main()
