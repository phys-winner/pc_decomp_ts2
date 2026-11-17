"""Summarize existing GLES window statistics without inventing frame percentiles.

python tools/summarize_frame_log.py baseline.log candidate.log --start-frame 6000
Only whole logged windows inside the requested frame range are included.
Query/upload windows are retained independently; their boundaries differ from GL.
"""
import argparse
import json
from pathlib import Path
import re

NUMBER = r'[-+0-9.eE]+'
FRAME = re.compile(
    r'\[gl\] frame (?P<frame>\d+): (?P<fps>' + NUMBER + r') fps; per frame '
    r'(?P<wall_ms>' + NUMBER + r') ms wall, (?P<cpu_ms>' + NUMBER + r') ms cpu'
    r'(?:, (?P<render_ms>' + NUMBER + r') ms render \((?P<rt_wait_ms>' + NUMBER +
    r') ms waited, (?P<syncs>' + NUMBER + r') syncs\))?, '
    r'(?P<machine_wait_ms>' + NUMBER + r') ms machine wait, (?P<draws>' + NUMBER +
    r') draws, (?P<swap_ms>' + NUMBER + r') ms swap; cadence max (?P<cadence_max_ns>\d+) ns, '
    r'over90 (?P<over90>\d+)/(?P<frames>\d+)(?:; gpu (?P<gpu_ms>' + NUMBER + r') ms)?')
QUERY = re.compile(
    r'\[rt\] queries per frame: (' + NUMBER + r') occlusion, (' + NUMBER +
    r') event, (' + NUMBER + r') pending; (' + NUMBER + r') handed over, (' + NUMBER +
    r') cached, (' + NUMBER + r') answered unasked; (' + NUMBER + r') ms asleep')
BUFFER = re.compile(r'\[d3d9\] buffers: (' + NUMBER + r') uploads, (' + NUMBER +
                    r') KB per frame \((' + NUMBER + r') KB outside the written ranges in full uploads\)')
OUTLIER = re.compile(
    r'\[gl-frame\] frame (?P<frame>\d+) mono (?P<mono_ns>\d+) cadence (?P<cadence_ns>\d+) '
    r'gamecpu (?P<cpu_ns>\d+) rendercpu (?P<render_ns>\d+) machinewait (?P<machine_wait_ns>\d+) '
    r'queuewait (?P<rt_wait_ns>\d+) syncs (?P<syncs>\d+) swap (?P<swap_ns>\d+) draws (?P<draws>\d+)')
RESOURCE_MODE = re.compile(r'\[resource-skip\] (?P<mode>on|off) rows (?P<rows>\d+) '
                           r'batches (?P<batches>\d+) dispatch (?P<dispatch>.*)')


def parse(text):
    windows, resources = [], []
    last_frame, segment = None, 0
    for line_number, line in enumerate(text.splitlines(), 1):
        match = FRAME.search(line)
        if match:
            fields = match.groupdict()
            item = {key: (None if value is None else int(value) if key in
                    ('frame', 'cadence_max_ns', 'over90', 'frames') else float(value))
                    for key, value in fields.items()}
            if last_frame is not None and item['frame'] < last_frame:
                segment += 1
            last_frame = item['frame']
            item.update(segment=segment, line=line_number,
                        first_frame=item['frame'] - item['frames'] + 1)
            windows.append(item)
            continue
        match = QUERY.search(line)
        if match:
            keys = ('occlusion', 'event', 'pending', 'recorded', 'cached', 'answered_idle', 'sleep_ms')
            resources.append(dict(kind='queries', line=line_number, segment=segment,
                                  after_frame=last_frame, **dict(zip(keys, map(float, match.groups())))))
        match = BUFFER.search(line)
        if match:
            keys = ('uploads', 'kb', 'spare_kb')
            resources.append(dict(kind='uploads', line=line_number, segment=segment,
                                  after_frame=last_frame, **dict(zip(keys, map(float, match.groups())))))
    return windows, resources


def parse_outliers(text):
    outliers, modes = [], []
    last_frame, segment, mode = None, 0, None
    for line_number, line in enumerate(text.splitlines(), 1):
        window, sample = FRAME.search(line), OUTLIER.search(line)
        observed = window or sample
        if observed:
            frame = int(observed['frame'])
            if last_frame is not None and frame < last_frame:
                segment += 1
                mode = None
            last_frame = frame
        if sample:
            item = {key: int(value) for key, value in sample.groupdict().items()}
            item.update(line=line_number, segment=segment, resource_skip=mode)
            outliers.append(item)
        selected = RESOURCE_MODE.search(line)
        if selected:
            mode = selected['mode']
            modes.append(dict(line=line_number, segment=segment, after_frame=last_frame,
                              mode=mode, rows=int(selected['rows']), batches=int(selected['batches']),
                              dispatch=selected['dispatch']))
    return outliers, modes


def summarize(text, first=None, last=None):
    all_windows, resources = parse(text)
    outliers, modes = parse_outliers(text)
    outliers = [item for item in outliers if (first is None or item['frame'] >= first) and
                (last is None or item['frame'] <= last)]
    windows = [w for w in all_windows if w['frames'] and
               (first is None or w['first_frame'] >= first) and
               (last is None or w['frame'] <= last)]
    frames = sum(w['frames'] for w in windows)
    means = {}
    for key in ('wall_ms', 'cpu_ms', 'render_ms', 'rt_wait_ms', 'syncs',
                'machine_wait_ms', 'draws', 'swap_ms', 'gpu_ms'):
        observed = [w for w in windows if w[key] is not None]
        count = sum(w['frames'] for w in observed)
        means[key] = sum(w[key] * w['frames'] for w in observed) / count if count else None
    elapsed_ms = sum(w['wall_ms'] * w['frames'] for w in windows)
    selected_lines = [w['line'] for w in windows]
    if selected_lines:
        # Context only: resource records have different, unlogged frame boundaries.
        previous_lines = [w['line'] for w in all_windows if w['line'] < min(selected_lines)]
        low = max(previous_lines, default=0)
        resources = [r for r in resources if low < r['line'] <= max(selected_lines)]
    else:
        resources = []
    return {
        'window_count': len(windows), 'covered_frames': frames,
        'estimated_wall_seconds': elapsed_ms / 1000,
        'weighted_fps': frames * 1000 / elapsed_ms if elapsed_ms else None,
        'means_per_frame': means,
        'max_observed_cadence_ms': max((w['cadence_max_ns'] / 1e6 for w in windows), default=None),
        'over_90hz_budget_count': sum(w['over90'] for w in windows),
        'over_90hz_budget_fraction': sum(w['over90'] for w in windows) / frames if frames else None,
        'windows': windows, 'resource_context': resources,
        'qa_over_50ms_count': len(outliers),
        'qa_over_50ms_max': max((item['cadence_ns'] / 1e6 for item in outliers), default=None),
        'qa_outliers': outliers, 'resource_skip_boundaries': modes,
        'limits': [
            'Window averages are weighted by reported cadence counts; printed values are rounded.',
            'Regular windows do not provide individual durations or p95/p99.',
            'QA gl-frame samples contain only cadences above 50 ms; their absence does not prove the QA logger was enabled.',
            'QA monotonic timestamps and CPU/render/wait deltas are nanoseconds, counts are integers; gl-frame timing spans the preceding frame.',
            'over90 means above 11.111 ms, not a general long-frame threshold.',
            'Main and render CPU overlap; do not add them to infer frame wall time.',
            'rt_wait_ms is the game thread waiting for the render queue, including frame pacing.',
            'Query/upload records have independent boundaries and are context, not same-frame attribution.',
            'Upload bytes/calls do not measure GL upload stalls; readbacks are only within total syncs.',
            'GPU timer values must be validated by the renderer/device, not inferred valid by this parser.'
        ]
    }


def self_test():
    template = ('[gl] frame {frame}: 50.0 fps; per frame {wall} ms wall, 3.0 ms cpu'
                '{render}, 1.0 ms machine wait, 100 draws, 2.0 ms swap; cadence max '
                '{maximum} ns, over90 {over}/{frames}{gpu}\n')
    a = template.format(frame=100, wall=20, render=', 4.0 ms render (5.0 ms waited, 2.0 syncs)',
                        maximum=30000000, over=100, frames=100, gpu='; gpu 6.0 ms')
    b = template.format(frame=300, wall=10, render='', maximum=25000000, over=10, frames=200, gpu='')
    report = summarize(a + b)
    assert report['covered_frames'] == 300 and report['estimated_wall_seconds'] == 4
    assert report['weighted_fps'] == 75 and report['max_observed_cadence_ms'] == 30
    assert report['means_per_frame']['wall_ms'] == 40 / 3
    assert report['means_per_frame']['render_ms'] == 4 and report['means_per_frame']['gpu_ms'] == 6
    assert summarize(a + b, first=101)['window_count'] == 1
    assert summarize(a + b, last=200)['covered_frames'] == 100
    assert parse(a + b + a)[0][-1]['segment'] == 1
    assert summarize('')['weighted_fps'] is None
    event = ('[resource-skip] off rows 128 batches 4 dispatch native wrapper\n'
             '[gl-frame] frame 201 mono 9000000000 cadence 70000000 gamecpu 61000000 '
             'rendercpu 5000000 machinewait 1000000 queuewait 2000000 syncs 0 swap 3000000 draws 400\n')
    result = summarize(a + event + b)
    assert result['qa_over_50ms_count'] == 1 and result['qa_over_50ms_max'] == 70
    assert result['qa_outliers'][0]['resource_skip'] == 'off'
    assert result['qa_outliers'][0]['mono_ns'] == 9000000000
    assert result['resource_skip_boundaries'][0]['rows'] == 128
    assert summarize(a + event, last=200)['qa_over_50ms_count'] == 0
    assert parse_outliers(a + event + a + event)[0][-1]['segment'] == 1
    print('PASS frame-log summary: weighted windows, optional fields, filters, restarts, QA outliers and mode boundaries')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='*', type=Path)
    parser.add_argument('--start-frame', type=int)
    parser.add_argument('--end-frame', type=int)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.logs:
        parser.error('provide at least one engine log or --self-test')
    result = {str(p): summarize(p.read_text(encoding='utf-8', errors='replace'),
                               args.start_frame, args.end_frame) for p in args.logs}
    payload = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding='utf-8')
    else:
        print(payload, end='')


if __name__ == '__main__':
    main()
