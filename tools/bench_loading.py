"""Capture loading observations and app-scoped CPU profiles, without changing settings.

    python tools/bench_loading.py startup --serial abb24fae --outprefix build/perf/startup-b --startup-input late
    python tools/bench_loading.py town --serial abb24fae --outprefix build/perf/town-b --click 625 534
    python tools/bench_loading.py lot --serial abb24fae --outprefix build/perf/lot-b --click X Y --paused-save
    python tools/bench_loading.py --self-test

Startup requires the app to be absent; town/lot require its selected Play panel.
Click coordinates are native logical-game coordinates, not physical pixels.
Lot detection requires a save made while paused and a visible scene viewport.
Readiness is bounded by
screenshots, not an exact transition or proof all background loading finished.
Private run-as profile files are retained for recovery; no game is force-stopped.
"""
import argparse
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'org.sims2.port'
LIVE = '/sdcard/Sims2/userdata/live.txt'
NATIVE_LOG = '/sdcard/Sims2/userdata/sims2.log'
REFERENCES = {
    'startup': (ROOT/'build/live/abb24fae-final-load-state.png', (790,345,1690,400)),
    'town': (ROOT/'build/live/abb24fae-final-town-ready.png', (1000,20,1230,90)),
}
PAUSE_PIXELS = ((300,5),(5,300),(1000,5))
# Exclude the pause border, action queue, portraits and bottom HUD. A pointer
# over the otherwise black scene must not count as a rendered lot.
LOT_SCENE_ROI = (320,160,2320,700)
LOT_SCENE_CHANNEL_MIN = 12
LOT_SCENE_FRACTION_MIN = .05


def classify(image, phase):
    image = image.convert('RGB')
    if image.size != (2400,1080):
        raise ValueError(f'expected 2400x1080 screenshot, got {image.size}')
    if phase == 'lot':
        pixels = [image.getpixel(p) for p in PAUSE_PIXELS]
        paused = all(r>200 and g<60 and b<60 for r,g,b in pixels)
        red,green,blue = image.crop(LOT_SCENE_ROI).split()
        histogram = ImageChops.lighter(ImageChops.lighter(red,green),blue).histogram()
        visible_fraction = sum(histogram[LOT_SCENE_CHANNEL_MIN+1:])/sum(histogram)
        return {'ready': paused and visible_fraction>=LOT_SCENE_FRACTION_MIN,
                'pause_pixels': pixels, 'paused_hud': paused,
                'scene_nonblack_fraction': visible_fraction}
    reference, roi = REFERENCES[phase]
    with Image.open(reference) as ref:
        difference = ImageChops.difference(image.crop(roi),ref.convert('RGB').crop(roi))
    mae = sum(ImageStat.Stat(difference).mean)/3
    return {'ready': mae<12, 'roi_mae': mae}


def readiness_bounds(observations):
    positive = next((o for o in observations if o['ready']), None)
    if positive is None:
        return None
    negatives = [o for o in observations if not o['ready'] and o['end_seconds']<=positive['end_seconds']]
    return {'lower_seconds': negatives[-1]['begin_seconds'] if negatives else 0,
            'upper_seconds': positive['end_seconds'],
            'last_negative': negatives[-1] if negatives else None, 'first_positive': positive,
            'precision': 'screenshot acquisition bounds; not an exact transition'}


def record_stats(text):
    duration = re.search(r'Recorded for ([\d.]+) seconds',text)
    counts = re.search(r'Samples recorded:\s*([\d,]+)\. Samples lost:\s*([\d,]+)',text)
    return {'recorded_seconds':float(duration[1]) if duration else None,
            'raw_recorded_samples':int(counts[1].replace(',','')) if counts else None,
            'raw_lost_samples':int(counts[2].replace(',','')) if counts else None}


def sample_stats(lines):
    threads = {}; current = None; decoded = 0; trailer = {}
    def flush(row):
        nonlocal decoded
        if row is None or 'tid' not in row or 'cycles' not in row:
            return
        decoded += 1
        q = threads.setdefault(row['tid'], {'name':row.get('name',''), 'samples':0, 'cycles':0})
        q['samples'] += 1; q['cycles'] += row['cycles']
    for line in lines:
        if line.startswith('sample:'):
            flush(current); current = {}; continue
        if line.startswith('lost_situation:'):
            flush(current); current = None
        key, _, value = line.strip().partition(':'); value = value.strip()
        if current is not None:
            if key=='thread_id': current['tid']=int(value)
            elif key=='thread_name': current['name']=value
            elif key=='event_count': current['cycles']=int(value)
        elif key in ('sample_count','lost_count'):
            trailer[key]=int(value)
    flush(current)
    return {'decoded_samples':decoded, 'decoded_trailer':trailer, 'threads':threads,
            'count_scope':'Raw record-tool counts and decoded counts are separate; cycles are event_count sums by exact TID.'}


def startup_commands(mode):
    # Preserve the B baseline's one-second sleeps; key itself holds for 100 ms.
    # Late mode targets approximately 0/8/12 s after the live reader starts.
    pauses = ('1','1') if mode=='early' else ('7.9','3.9')
    return f'key 1B\nsleep {pauses[0]}\nkey 1B\nsleep {pauses[1]}\nkey 1B\n'


def profiler_signal(pid,remote,signal):
    if not re.fullmatch(r'[1-9]\d*',str(pid)) or not re.fullmatch(r'files/load-(startup|town|lot)-[0-9a-f]{12}',remote) or signal not in ('INT','TERM'):
        raise ValueError('invalid owned-profiler PID, private prefix, or signal')
    return (f'if [ -e /proc/{pid}/cmdline ]; then '
            f'cmdline="$(tr \'\\000\' \' \' < /proc/{pid}/cmdline)"; '
            f'case "$cmdline" in *simpleperf*{remote}*) kill -{signal} {pid};; '
            '* ) echo "refusing to signal an unrelated PID" >&2; exit 3;; esac; fi')


def self_test():
    fixtures = {
        'startup': (('abb24fae-startup-baseline-ready.png',True),('abb24fae-final-town-load.png',False)),
        'town': (('abb24fae-b-final-town-start.png',True),('abb24fae-b-final-town-end.png',True),
                 ('abb24fae-final-town-load.png',False),('abb24fae-startup-baseline-ready.png',False)),
        'lot': (('abb24fae-lot-baseline-now.png',True),('abb24fae-save-reload.png',True),
                ('abb24fae-lot-baseline-loading.png',False),('abb24fae-b-final-town-end.png',False)),
    }
    for phase, cases in fixtures.items():
        for filename, expected in cases:
            with Image.open(ROOT/'build/live'/filename) as image: result=classify(image,phase)
            assert result['ready']==expected,(phase,filename,result)
            print(f'PASS {phase}: {filename}: {result}')
    # These historical "ready" captures expose why a paused HUD alone is not
    # scene readiness. Preserve the original artifacts and timing records.
    for filename,expected in (
        ('build/perf/abb24fae-load-b2-lot.ready.png',False),
        ('build/perf/abb24fae-load-final-lot.ready.png',True),
        ('build/perf/abb24fae-accept-lot.ready.png',False),
        ('build/live/abb24fae-accept-family-wide-settled.png',True),
    ):
        with Image.open(ROOT/filename) as image:result=classify(image,'lot')
        assert result['ready']==expected,(filename,result)
        print(f'PASS lot scene: {filename}: {result}')
    black_hud=Image.new('RGB',(2400,1080))
    for pixel in PAUSE_PIXELS:black_hud.putpixel(pixel,(255,0,0))
    assert not classify(black_hud,'lot')['ready']
    black_hud.paste((255,255,255),(1200,300,1232,332))
    assert not classify(black_hud,'lot')['ready'], 'pointer over black viewport counted as scene'
    assert not classify(Image.new('RGB',(2400,1080),(200,0,0)),'lot')['ready']
    assert not classify(Image.new('RGB',(2400,1080),(255,60,0)),'lot')['ready']
    assert readiness_bounds([]) is None
    rows=[{'begin_seconds':1,'end_seconds':2,'ready':False},
          {'begin_seconds':3,'end_seconds':4,'ready':False},
          {'begin_seconds':5,'end_seconds':6,'ready':True}]
    assert readiness_bounds(rows)['lower_seconds']==3 and readiness_bounds(rows)['upper_seconds']==6
    assert readiness_bounds(rows[:2]) is None
    assert record_stats('Recorded for 29.99 seconds. Samples recorded: 46,646. Samples lost: 0.')=={
        'recorded_seconds':29.99,'raw_recorded_samples':46646,'raw_lost_samples':0}
    stats=sample_stats(('sample:\nthread_id: 1\nthread_name: game\nevent_count: 11\n'
                        'sample:\nthread_id: 2\nthread_name: game\nevent_count: 7\n'
                        'sample:\nthread_id: 1\nthread_name: game\nevent_count: 5\n'
                        'lost_situation:\n sample_count: 3\n lost_count: 0\n').splitlines())
    assert stats['decoded_samples']==3 and stats['threads'][1]['cycles']==16
    assert stats['threads'][2]['cycles']==7 and stats['decoded_trailer']['sample_count']==3
    assert startup_commands('early').count('key 1B')==3 and 'sleep 1\n' in startup_commands('early')
    assert 'sleep 7.9' in startup_commands('late') and 'sleep 3.9' in startup_commands('late')
    assert 'kill -INT 123' in profiler_signal(123,'files/load-startup-0123456789ab','INT')
    for pid,remote,signal in [('123; kill 4','files/load-lot-0123456789ab','INT'),(123,'files/../other','INT'),(123,'files/load-town-0123456789ab','KILL')]:
        try:profiler_signal(pid,remote,signal)
        except ValueError:pass
        else:raise AssertionError('unsafe profiler identifier accepted')
    print('PASS: actual positive/negative images, strict pixel thresholds, timing bounds, per-TID weights, raw/decoded parsers, input schedules')


class Device:
    def __init__(self,serial):
        adb=os.environ.get('ADB',str(Path(os.environ.get('LOCALAPPDATA',r'C:\Users\Xilonen\AppData\Local'))/'Android/Sdk/platform-tools/adb.exe'))
        self.command=[adb,'-s',serial]
    def shell(self,command,*,check=True,timeout=15,input=None):
        result=subprocess.run(self.command+['shell',command],input=input,capture_output=True,timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f'adb failed: {command}\n{result.stderr.decode(errors="replace")}')
        return result.stdout
    def private(self,command,**kwargs):
        return self.shell(f'run-as {PACKAGE} sh -c {shlex.quote(command)}',**kwargs)
    def pid(self):
        value=self.shell(f'pidof {PACKAGE}',check=False).decode().strip()
        if not value:return None
        if not re.fullmatch(r'[1-9]\d*',value):raise RuntimeError('expected one app PID, got '+value)
        return int(value)
    def screenshot(self,timeout=15):
        result=subprocess.run(self.command+['exec-out','screencap','-p'],capture_output=True,timeout=timeout)
        if result.returncode:raise RuntimeError(result.stderr.decode(errors='replace'))
        return result.stdout
    def fetch(self,remote,local):
        with local.open('wb') as output:
            result=subprocess.run(self.command+['exec-out','run-as',PACKAGE,'cat',remote],stdout=output,stderr=subprocess.PIPE,timeout=60)
        if result.returncode:raise RuntimeError(f'fetch {remote}: '+result.stderr.decode(errors='replace'))


def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('phase',nargs='?',choices=('startup','town','lot'))
    parser.add_argument('--serial',default=os.environ.get('S2_SERIAL'))
    parser.add_argument('--outprefix',type=Path)
    parser.add_argument('--click',nargs=2,type=int,metavar=('X','Y'))
    parser.add_argument('--startup-input',choices=('early','late'),default='early')
    parser.add_argument('--paused-save',action='store_true',help='selected lot was saved paused; required for paused-HUD plus visible-scene detection')
    parser.add_argument('--timeout',type=float,default=90,help='capture wall-clock limit, at most 90 seconds')
    parser.add_argument('--self-test',action='store_true')
    args=parser.parse_args()
    if args.self_test:self_test();return 0
    if not args.phase or not args.serial or not args.outprefix:parser.error('phase, --serial and --outprefix are required')
    if not 0<args.timeout<=90:parser.error('--timeout must be greater than zero and at most 90')
    if args.phase!='startup' and not args.click:parser.error('town/lot require --click in logical-game coordinates')
    if args.phase=='startup' and args.click:parser.error('startup does not accept --click')
    if args.phase=='lot' and not args.paused_save:parser.error('lot requires --paused-save: selected save must have been saved paused')
    prefix=args.outprefix.resolve();prefix.parent.mkdir(parents=True,exist_ok=True)
    def artifact(suffix):return Path(str(prefix)+suffix)
    if artifact('.timing.json').exists() or artifact('.frames').exists():raise RuntimeError('output prefix already exists')
    frames=artifact('.frames');frames.mkdir()
    token='load-'+args.phase+'-'+uuid.uuid4().hex[:12]
    remote='files/'+token
    device=Device(args.serial)
    state={'phase':args.phase,'serial':args.serial,'condition':'fresh process; warm OS cache' if args.phase=='startup' else 'running app; selected native Play panel; warm OS cache',
           'startup_input':args.startup_input if args.phase=='startup' else None,'paused_save_required':args.phase=='lot',
           'timeout_seconds':args.timeout,'profile':'app-scoped all threads; cpu-cycles; 1000 Hz; frame-pointer callchains; max 90 s',
           'remote_prefix':remote,'marker_prefix':token,'observations':[],'status':'preparing','errors':[],
           'readiness_rule':{'roi':REFERENCES[args.phase][1],'mae_less_than':12} if args.phase in REFERENCES else {
               'criterion':'paused HUD plus nonblack scene viewport, version 2',
               'pixels':PAUSE_PIXELS,'R_greater_than':200,'G_B_less_than':60,
               'scene_roi':LOT_SCENE_ROI,'scene_max_channel_greater_than':LOT_SCENE_CHANNEL_MIN,
               'scene_fraction_at_least':LOT_SCENE_FRACTION_MIN},
           'precision':'screenshot acquisition bounds; no exact transition or completion of all background work is claimed'}
    def save():
        temporary=artifact('.timing.json.tmp');temporary.write_text(json.dumps(state,indent=2)+'\n');os.replace(temporary,artifact('.timing.json'))
    handles=[];processes=[];private_jobs=[];profile=None;amstart=None;started=None
    def launch(command,logfile):
        handle=logfile.open('wb');handles.append(handle)
        proc=subprocess.Popen(command,stdout=handle,stderr=subprocess.STDOUT);processes.append(proc);return proc
    def start_private(command,pid_suffix,logfile):
        # exec retains this shell's exact PID; no other process is ever signalled.
        script=f'echo $$ > {remote+pid_suffix}; exec {command}'
        proc=launch(device.command+['shell',f'run-as {PACKAGE} sh -c {shlex.quote(script)}'],logfile)
        private_jobs.append((proc,pid_suffix))
        return proc,pid_suffix
    def stop_private(proc,pid_suffix,signal='INT'):
        if proc is None:return
        for _ in range(20):
            raw=device.private(f'cat {remote+pid_suffix}',check=False).decode().strip()
            if re.fullmatch(r'[1-9]\d*',raw):break
            if proc.poll() is not None and not raw:return
            time.sleep(.05)
        else:raise RuntimeError('own profiler PID file was not written')
        device.private(profiler_signal(raw,remote,signal))
    save()
    try:
        existing=device.pid()
        if args.phase=='startup' and existing:raise RuntimeError('startup requires the app process to be absent; quit it normally first')
        if args.phase!='startup' and not existing:raise RuntimeError('town/lot require a running app with the selected Play panel')
        if args.phase!='startup':
            native=device.shell(f'cat {NATIVE_LOG}').decode(errors='replace')
            if '[live] reading commands from ' not in native:raise RuntimeError('native live input is not active')
        before=device.screenshot();artifact('.before.png').write_bytes(before)
        if args.phase!='startup' and classify(Image.open(io.BytesIO(before)),args.phase)['ready']:
            raise RuntimeError('preflight screenshot already satisfies readiness; selected Play panel is required')
        logcat=launch(device.command+['logcat','-v','monotonic','-T','1','-s','Sims2:I','S2Load:I','ActivityTaskManager:I','ActivityManager:I'],artifact('.logcat.txt'))
        if args.phase=='startup':
            # Only a stopped app may have its input file replaced.
            if device.pid() is not None:raise RuntimeError('app started during preflight; refusing to replace live input')
            device.shell(f'cat > {LIVE}',input=startup_commands(args.startup_input).encode())
        started=time.monotonic();deadline=started+args.timeout
        state['host_capture_begin_monotonic']=started;state['status']='capturing';save()
        device.shell(f'log -t S2Load {token}_capture_begin')
        state['host_action_send_begin_seconds']=time.monotonic()-started
        if args.phase=='startup':
            amstart=launch(device.command+['shell','am','start','-W','-n',PACKAGE+'/.GameActivity'],artifact('.am-start.txt'))
            while (pid:=device.pid()) is None:
                if time.monotonic()>=deadline:raise TimeoutError('no app PID before capture deadline')
                if amstart.poll() not in (None,0):raise RuntimeError('am start failed; see .am-start.txt')
                time.sleep(.05)
        else:
            pid=existing
        state['pid']=pid;state['host_pid_observed_seconds']=time.monotonic()-started;save()
        record=f'/system/bin/simpleperf record -p {pid} --call-graph fp -f 1000 --duration 90 -o {remote}.data > {remote}.record.log 2>&1'
        profile,pid_suffix=start_private(record,'.pid',artifact('.profile-helper.log'))
        state['host_profile_requested_seconds']=time.monotonic()-started
        if args.phase!='startup':
            device.shell(f"printf '%s\\n' 'click {args.click[0]} {args.click[1]}' >> {LIVE}")
        state['host_action_send_end_seconds']=time.monotonic()-started;save()
        artifact('.threads.txt').write_bytes(device.shell(f'ps -T -p {pid}'))
        while time.monotonic()<deadline:
            if device.pid()!=pid:raise RuntimeError('app exited or restarted during capture')
            begin=time.monotonic()-started
            png=device.screenshot(timeout=max(.01,min(10,deadline-time.monotonic())))
            end=time.monotonic()-started
            image=Image.open(io.BytesIO(png));row=classify(image,args.phase)
            path=frames/f'{len(state["observations"]):03d}.png';path.write_bytes(png)
            row.update(begin_seconds=begin,end_seconds=end,screenshot=str(path))
            state['observations'].append(row);state['ready_bounds']=readiness_bounds(state['observations']);save()
            print(f'{args.phase}: screenshot {end:.3f} s: {row}',flush=True)
            if row['ready']:
                artifact('.ready.png').write_bytes(png);state['status']='ready';break
            artifact('.last-negative.png').write_bytes(png)
            if profile.poll() is not None:raise RuntimeError('profiler ended before readiness; inspect .record.log')
            time.sleep(max(0,min(.5,deadline-time.monotonic())))
        else:raise TimeoutError('readiness was not observed within the wall-clock limit')
    except (Exception,KeyboardInterrupt) as error:
        state['status']='timeout' if isinstance(error,(TimeoutError,subprocess.TimeoutExpired)) else 'failed'
        state['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        if started is not None:
            state['host_capture_end_seconds']=time.monotonic()-started
            try:device.shell(f'log -t S2Load {token}_capture_end_{state["status"]}')
            except Exception as error:state['errors'].append('end marker: '+str(error))
        if profile is not None:
            try:
                stop_private(profile,'.pid');profile.wait(timeout=20)
            except Exception as error:
                state['errors'].append('stop profiler: '+str(error))
                try:stop_private(profile,'.pid','TERM');profile.wait(timeout=5)
                except Exception as cleanup_error:state['errors'].append('profiler cleanup: '+str(cleanup_error))
            try:
                if profile.poll() is None:raise RuntimeError('recorder is still active; refusing to decode an unfinished file')
                report,pid_suffix=start_private(f'/system/bin/simpleperf report-sample -i {remote}.data --show-callchain > {remote}.samples.txt 2> {remote}.report.log',
                                                '.report.pid',artifact('.report-helper.log'))
                try:report.wait(timeout=60)
                finally:
                    if report.poll() is None:stop_private(report,'.report.pid','TERM')
                if report.returncode:state['errors'].append('report-sample exit '+str(report.returncode))
            except Exception as error:state['errors'].append('report-sample: '+str(error))
            for suffix in ('.data','.record.log','.samples.txt','.report.log','.pid'):
                try:device.fetch(remote+suffix,artifact(suffix))
                except Exception as error:state['errors'].append(str(error))
            if artifact('.record.log').exists():state['record_stats']=record_stats(artifact('.record.log').read_text(errors='replace'))
            if artifact('.samples.txt').exists():
                with artifact('.samples.txt').open(encoding='utf-8',errors='replace') as lines:state['sample_stats']=sample_stats(lines)
            # A failed SIGINT must not leave this tool's recorder behind.
            try:
                if profile.poll() is None:stop_private(profile,'.pid','TERM')
            except Exception as error:state['errors'].append('profiler cleanup: '+str(error))
        for proc,pid_suffix in private_jobs:
            try:
                if proc.poll() is None:stop_private(proc,pid_suffix,'TERM');proc.wait(timeout=5)
            except Exception as error:state['errors'].append('private helper cleanup: '+str(error))
        for proc in processes:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
        for handle in handles:handle.close()
        state['ready_bounds']=readiness_bounds(state['observations']);save()
    print(json.dumps({'status':state['status'],'ready_bounds':state.get('ready_bounds'),'errors':state['errors'],'timing':str(artifact('.timing.json'))},indent=2))
    return 0 if state['status']=='ready' and not state['errors'] else 2


if __name__=='__main__':
    raise SystemExit(main())
