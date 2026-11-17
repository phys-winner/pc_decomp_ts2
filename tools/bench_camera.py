"""Measure FPS while holding camera keys in a running APK; leaves settings alone.

    python tools/bench_camera.py --serial abb24fae --seconds 60 --out build/perf/camera.log
    python tools/bench_camera.py --serial SERIAL --rotation --target-fps 90 --seconds 120
    python tools/bench_camera.py --self-test

The log reports interval averages rounded to 0.1 FPS; their frame counts and
FPS imply elapsed time for the weighted result. New native builds also report
the worst present-start cadence in integer nanoseconds. This is app cadence,
not display scanout. Older builds cannot establish an individual-frame minimum.
Verify the scene and actual camera movement separately with screenshots or observation.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DATA = '/sdcard/Sims2/userdata'
LIVE = DATA + '/live.txt'
LOG = DATA + '/sims2.log'
KEYS = ('27', '25')  # right, left: short opposing holds keep the view near its start
REPORT = re.compile(r'\[gl\] frame (\d+): ([\d.]+) fps; per frame ([\d.]+) ms wall')
CADENCE = re.compile(r'; cadence max (\d+) ns, over90 (\d+)/(\d+)')
MARKER = '[live] hud 0 0 0'
PID_POLL_SECONDS = 2.0


def read_pid(shell):
    """One retry of this read only; failed commands and input writes are not retried."""
    try:
        return shell('pidof org.sims2.port').strip(), False
    except subprocess.TimeoutExpired:
        return shell('pidof org.sims2.port').strip(), True


def live_input_logged(text, canonical_path):
    prefix = '[live] reading commands from '
    paths = {LIVE, canonical_path} - {''}
    return any(line.startswith(prefix) and line[len(prefix):].strip() in paths for line in text.splitlines())


def intervals(text):
    """Intervals with both endpoints between the two camera markers."""
    markers = 0
    previous = None
    rows = []
    for line in text.splitlines():
        if line.strip() == MARKER:
            markers += 1
            if markers == 2:
                break
            continue
        if markers != 1:
            continue
        if 'FATAL' in line or re.search(r'\bsignal (?:\d+|SIG)', line):
            raise ValueError('game failed during camera benchmark: ' + line)
        match = REPORT.search(line)
        if not match:
            continue
        frame, fps, wall = int(match[1]), float(match[2]), float(match[3])
        if fps <= 0 or wall <= 0:
            raise ValueError('invalid frame report: ' + line)
        if previous is not None:
            if frame <= previous:
                raise ValueError('frame counter did not advance; log may span different runs')
            row = {'frame': frame, 'frames': frame - previous, 'fps': fps, 'wall_ms': wall}
            cadence = CADENCE.search(line)
            if cadence:
                worst, over90, count = map(int, cadence.groups())
                if (worst <= 0 or count != row['frames'] or not 0 <= over90 <= count or
                        (worst > 1_000_000_000 // 90) != (over90 > 0)):
                    raise ValueError('invalid cadence report: ' + line)
                row.update(max_frame_ns=worst, frames_over90_budget=over90, cadence_frames=count)
            rows.append(row)
        previous = frame
    if markers != 2:
        raise ValueError('camera markers were not both processed; is live input active?')
    if not rows:
        raise ValueError('no complete frame-report interval during camera movement')
    return rows


def summarize(rows):
    frames = sum(row['frames'] for row in rows)
    wall = sum(row['frames'] / row['fps'] for row in rows)
    stats = {'intervals': len(rows), 'frames': frames, 'seconds': wall,
             'fps': frames / wall, 'min_interval_fps': min(row['fps'] for row in rows)}
    if all('max_frame_ns' in row for row in rows):
        worst = max(row['max_frame_ns'] for row in rows)
        stats.update(max_frame_ns=worst, min_present_cadence_fps=1e9 / worst,
                     frames_over90_budget=sum(row['frames_over90_budget'] for row in rows),
                     cadence_frames=sum(row['cadence_frames'] for row in rows))
    return stats


def require_coverage(stats, requested_seconds):
    required = max(0, requested_seconds - 12)
    if stats['seconds'] < required:
        raise ValueError(f"INCONCLUSIVE: only ~{stats['seconds']:.2f} s of complete frame intervals; "
                         f"at least {required:g} s required for a {requested_seconds:g} s run")


def camera_keys(rotation):
    # Shift+comma is the game's continuous camera rotation shortcut.
    return (('BC',), ('10',)) if rotation else (KEYS, ())


def self_test():
    calls = []
    def transient(command):
        calls.append(command)
        if len(calls) == 1:
            raise subprocess.TimeoutExpired(command, 15)
        return b'1234\n'
    assert read_pid(transient) == (b'1234', True) and len(calls) == 2
    for error in (subprocess.TimeoutExpired('pidof', 15), RuntimeError('command failed')):
        calls.clear()
        def failed(command):
            calls.append(command)
            raise error
        try:
            read_pid(failed)
        except type(error):
            assert len(calls) == (2 if isinstance(error, subprocess.TimeoutExpired) else 1)
        else:
            raise AssertionError('accepted a failed PID read')
    calls.clear()
    def empty(command):
        calls.append(command)
        return b''
    assert read_pid(empty) == (b'', False) and len(calls) == 1
    canonical = '/storage/emulated/0/Sims2/userdata/live.txt'
    assert live_input_logged('[live] reading commands from ' + canonical, canonical)
    assert live_input_logged('[live] reading commands from ' + LIVE, canonical)
    assert not live_input_logged('[live] reading commands from /sdcard/Other/live.txt', canonical)
    assert not live_input_logged('[live] reading commands from ', '')
    report = lambda frame, fps, wall: f'[gl] frame {frame}: {fps} fps; per frame {wall} ms wall'
    text = '\n'.join([report(50, 1, 1000), MARKER, report(100, 10, 100),
                      report(400, 60, 16.7), report(1000, 120, 8.3), MARKER, report(1100, 1, 1000)])
    rows = intervals(text)
    assert [row['frames'] for row in rows] == [300, 600]
    stats = summarize(rows)
    assert abs(stats['seconds'] - 10) < 1e-8 and stats['min_interval_fps'] == 60
    assert abs(stats['fps'] - 90) < 1e-8
    assert camera_keys(True) == (('BC',), ('10',))
    assert camera_keys(False) == (KEYS, ())
    assert stats['fps'] >= 90 and stats['min_interval_fps'] < 90
    cadence_text = '\n'.join([MARKER, report(100, 120, 8.3),
        report(700, 120, 8.3) + '; cadence max 11111112 ns, over90 1/600', MARKER])
    cadence_stats = summarize(intervals(cadence_text))
    assert cadence_stats['fps'] == 120 and cadence_stats['min_present_cadence_fps'] < 90
    assert cadence_stats['frames_over90_budget'] == 1 and cadence_stats['cadence_frames'] == 600
    assert 11111111 * 90 <= 1e9 < 11111112 * 90
    for bad_suffix in ('; cadence max 11111112 ns, over90 0/600',
                       '; cadence max 11111111 ns, over90 1/600',
                       '; cadence max 8333333 ns, over90 0/599'):
        bad = '\n'.join([MARKER, report(100, 120, 8.3), report(700, 120, 8.3) + bad_suffix, MARKER])
        try:
            intervals(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted inconsistent per-frame cadence counts')
    capped = summarize([{'frames': 300, 'fps': 60.0, 'wall_ms': 16.7}])
    assert capped['fps'] == 60.0 and capped['seconds'] == 5.0
    require_coverage({'seconds': 48}, 60)
    try:
        require_coverage({'seconds': 5}, 60)
    except ValueError:
        pass
    else:
        raise AssertionError('accepted one short interval as a sustained 60-second run')
    for bad in [MARKER, '\n'.join([MARKER, report(100, 60, 16.7), MARKER]),
                '\n'.join([MARKER, report(100, 60, 16.7), report(99, 60, 16.7), MARKER])]:
        try:
            intervals(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted missing boundaries or non-advancing counters')
    print('bench_camera: storage aliases, interval boundaries, frame counters, weighted aggregation and coverage PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', default=os.environ.get('S2_SERIAL'))
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--rotation', action='store_true', help='hold Shift+comma for continuous camera rotation')
    parser.add_argument('--target-fps', type=float, default=60, help='required mean and minimum interval FPS (default: 60)')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.serial:
        parser.error('--serial or S2_SERIAL is required')
    if not 12 <= args.seconds <= 3600:
        parser.error('--seconds must be between 12 and 3600')
    if not 0 < args.target_fps <= 1000:
        parser.error('--target-fps must be greater than 0 and at most 1000')
    keys, modifiers = camera_keys(args.rotation)
    poll_seconds = PID_POLL_SECONDS if args.rotation else 0.5
    adb = os.environ.get('ADB', str(Path(os.environ.get('LOCALAPPDATA', r'C:\Users\Xilonen\AppData\Local')) /
                                   'Android/Sdk/platform-tools/adb.exe'))
    def shell(command):
        result = subprocess.run([adb, '-s', args.serial, 'shell', command], capture_output=True, timeout=15)
        if result.returncode:
            raise RuntimeError(f'adb command failed: {command}\n{result.stderr.decode(errors="replace")}')
        return result.stdout
    def send(commands):
        # Commands are generated here; the only writable path is our live file.
        shell("printf '%s\\n' " + ' '.join("'" + command + "'" for command in commands) + f' >> {LIVE}')
    pid_read_retries = 0
    def pid():
        nonlocal pid_read_retries
        value, retried = read_pid(shell)
        pid_read_retries += retried
        return value
    original_pid = pid()
    if not original_pid:
        raise RuntimeError('org.sims2.port is not running')
    canonical_live = shell(f'test -f {LIVE} && readlink -f {LIVE}').decode().strip()
    live_log = shell(f"grep -F '[live] reading commands from ' {LOG} || true").decode(errors='replace')
    if not live_input_logged(live_log, canonical_live):
        raise RuntimeError('APK did not initialize live input from ' + LIVE)
    offset = int(shell(f'wc -c < {LOG}').strip())
    name = re.sub(r'[^A-Za-z0-9_.-]', '_', args.serial)
    out = args.out or ROOT / 'build/perf' / f'camera-{name}-{time.strftime("%Y%m%d-%H%M%S")}.log'
    out.parent.mkdir(parents=True, exist_ok=True)
    segment = b''
    released = False
    print(f'Holding camera keys for {args.seconds:g} s on {args.serial}; log: {out}', flush=True)
    host_start = time.monotonic()
    try:
        send(['down ' + key for key in modifiers] + ['down ' + keys[0], 'hud'])
        deadline = time.monotonic() + args.seconds
        direction = 0
        while time.monotonic() < deadline:
            time.sleep(min(poll_seconds, max(0, deadline - time.monotonic())))
            if pid() != original_pid:
                raise RuntimeError('APK process ended or restarted during benchmark')
            if len(keys) > 1 and time.monotonic() < deadline:
                following = (direction + 1) % len(keys)
                send(['up ' + keys[direction], 'down ' + keys[following]])
                direction = following
        send(['hud'] + ['up ' + key for key in keys + modifiers])
        # Wait for the script thread to consume the end marker before reading.
        for attempt in range(30):
            segment = shell(f'tail -c +{offset + 1} {LOG}')
            if segment.decode(errors='replace').count(MARKER) >= 2:
                break
            time.sleep(0.1)
    finally:
        try:
            send(['up ' + key for key in keys + modifiers])
            released = True
        finally:
            full_log = shell(f'cat {LOG}')
            out.write_bytes(full_log)
    if not released:
        raise RuntimeError('could not release camera keys')
    if pid() != original_pid or len(full_log) < offset:
        raise RuntimeError('APK/log changed during benchmark')
    if not segment:
        segment = full_log[offset:]
    rows = intervals(segment.decode(errors='replace'))
    stats = summarize(rows)
    stats.update(serial=args.serial, camera_keys=list(keys), modifier_keys=list(modifiers), rotation=args.rotation,
                 target_fps=args.target_fps, requested_seconds=args.seconds, log=str(out.resolve()),
                 pid_read_timeout_retries=pid_read_retries, pid_poll_seconds=poll_seconds,
                 host_operation_seconds=time.monotonic() - host_start,
                 required_coverage_seconds=max(0, args.seconds - 12),
                 coverage_sufficient=stats['seconds'] >= max(0, args.seconds - 12), interval_reports=rows)
    result = out.with_suffix('.json')
    result.write_text(json.dumps(stats, indent=2) + '\n', encoding='utf-8')
    print(f"{stats['intervals']} complete intervals, {stats['frames']} frames in ~{stats['seconds']:.2f} s: "
          f"weighted {stats['fps']:.2f} FPS; minimum reported interval {stats['min_interval_fps']:.1f} FPS")
    print('Elapsed time is inferred from interval FPS rounded to 0.1.')
    if 'max_frame_ns' in stats:
        print(f"Worst present-start cadence {stats['max_frame_ns'] / 1e6:.6f} ms "
              f"({stats['min_present_cadence_fps']:.3f} FPS); "
              f"{stats['frames_over90_budget']}/{stats['cadence_frames']} frames over the 90 FPS budget. "
              'This measures app cadence, not display scanout.')
    else:
        print('This build has no per-frame cadence telemetry; individual-frame minimum FPS is not measured.')
    print('Verify the scene and actual camera movement separately; this tool checks FPS while camera keys are held.')
    require_coverage(stats, args.seconds)
    passed = stats['fps'] >= args.target_fps and stats['min_interval_fps'] >= args.target_fps
    if 'max_frame_ns' in stats:
        passed = passed and stats['max_frame_ns'] * args.target_fps <= 1e9
    print(f'{"PASS" if passed else "BELOW TARGET"}: {args.target_fps:g} FPS while camera keys are held; {result}')
    return 0 if passed else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(2)
