#!/bin/bash
# Drive the ARM64 build on the target device (realme RMX3851, 192.168.1.103:5555).
#   tools/s2.sh push                 stop the game, install build/arm64/sims2-arm64 (hash-checked)
#   tools/s2.sh binaries             copy the original EP9/TSBin files from the installed distribution
#   tools/s2.sh overlay              stage resources for the command-line host (APK resources are bundled)
#   tools/s2.sh run [SECONDS] [ARGS] run in the foreground with a timeout, print the log tail
#   tools/s2.sh start [ARGS]         start in the background (live input via S2_INPUT_LIVE)
#   tools/s2.sh send 'key 27' ...    append live input commands (android/host/script.c)
#   tools/s2.sh shot [OUT.png]       dump the next frame and fetch it as PNG
#   tools/s2.sh log [N]              last N lines of the host log
#   tools/s2.sh bt                   symbolize the [bt] stacks of the last run.log
#   tools/s2.sh wait [DRAWS] [SECS]   until a frame shows DRAWS+ draws (or a trap)
#   tools/s2.sh nhood                start and open Pleasantview from the chooser
#   tools/s2.sh stop
# S2_APK=1 drives the installed APK instead (tools/build_game_apk.py --install):
# start launches the activity (screen woken) with <data>/live.txt as its command
# file, stop force-stops it and puts the screen back to sleep; send, shot, log
# and wait work alike. The screen is shared: look at `dumpsys activity top` first.
# Only /data/local/tmp/sims2 and /sdcard/Sims2 are ours on this shared device.
set -e
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
SERIAL="${S2_SERIAL:-192.168.1.103:5555}"
DIR=/data/local/tmp/sims2
LIVE=$DIR/live.txt
DATA=/sdcard/Sims2/userdata
FRAMES=$DATA/frames
BIN=sims2-arm64
BUILD=${S2_BUILD:-build/arm64}
APK_PACKAGE=org.sims2.port
if [ "${S2_APK:-0}" = 1 ]; then
    LIVE=$DATA/live.txt         # the APK reads it when present at start (script.c)
    BIN=$APK_PACKAGE
fi
cd "$(dirname "$0")/.."
shell() { "$ADB" -s "$SERIAL" shell "$@"; }
case "$1" in
push)
    shell "kill \$(pidof $BIN) 2>/dev/null; sleep 1" || true
    "$ADB" -s "$SERIAL" push $BUILD/$BIN $DIR/$BIN | tail -1
    shell "chmod 755 $DIR/$BIN"
    local_sum=$(sha256sum $BUILD/$BIN | cut -c1-16)
    remote_sum=$(shell "sha256sum $DIR/$BIN" | cut -c1-16)
    [ "$local_sum" = "$remote_sum" ] || { echo "push failed: device has $remote_sum, local $local_sum"; exit 1; }
    ;;
binaries)
    shell "mkdir -p /sdcard/Sims2/game/EP9/TSBin"
    for name in Sims2EP9.exe ijl15.dll; do
        "$ADB" -s "$SERIAL" push "${S2_GAME:-C:/Games/The Sims 2 Legacy}/EP9/TSBin/$name" /sdcard/Sims2/game/EP9/TSBin/ | tail -1
    done
    ;;
overlay)
    if [ "${S2_APK:-0}" = 1 ]; then
        echo "Port resources ship in the APK. Rebuild/install tools/build_game_apk.py."; exit 0
    fi
    "$ADB" -s "$SERIAL" push build/overlay/. "$DIR/overlay/" | tail -1
    ;;
run)
    secs="${2:-60}"
    shift 2 2>/dev/null || shift $#
    shell "cd $DIR && rm -rf $FRAMES; env ${S2_ENV:-} timeout $secs ./$BIN ${S2_FLAGS:-} --args '$*' > run.log 2>&1; echo exit=\$?; tail -${S2_TAIL:-40} run.log"
    ;;
start)
    shift
    if [ "${S2_APK:-0}" = 1 ]; then
        shell "am force-stop $APK_PACKAGE; rm -f $LIVE; touch $LIVE; rm -rf $FRAMES; input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard; am start -n $APK_PACKAGE/.GameActivity >/dev/null"
        echo started
        exit 0
    fi
    shell "kill \$(pidof $BIN) 2>/dev/null; rm -f $LIVE; touch $LIVE; rm -rf $FRAMES"
    shell "cd $DIR && (env ${S2_ENV:-} VOID_INPUT_LIVE=$LIVE nohup ./$BIN ${S2_FLAGS:-} --args '$*' > live.log 2>&1 &)"
    echo started
    ;;
send)
    shift
    for command in "$@"; do shell "echo '$command' >> $LIVE"; done
    ;;
shot)
    out="${2:-build/live-shot.png}"
    before=$(shell "ls $FRAMES 2>/dev/null | wc -l")
    shell "echo shot >> $LIVE"
    for i in $(seq 1 240); do
        now=$(shell "ls $FRAMES 2>/dev/null | wc -l")
        [ "$now" != "$before" ] && break
        sleep 0.25
    done
    sleep 0.3
    newest=$(shell "ls -t $FRAMES 2>/dev/null | head -1" | tr -d '\r')
    [ -n "$newest" ] || { echo "no frame dumped (is the game presenting?)"; exit 1; }
    mkdir -p build/live
    "$ADB" -s "$SERIAL" pull "$FRAMES/$newest" build/live/last.bmp >/dev/null 2>&1
    python -c "
from PIL import Image
import sys
Image.open('build/live/last.bmp').convert('RGB').save(sys.argv[1])
" "$out"
    echo "$out"
    ;;
log)
    shell "tail -${2:-30} $DATA/sims2.log"
    ;;
wait)
    # wait [DRAWS] [SECONDS]: until a frame report shows DRAWS+ draws, the
    # game traps, or the process ends
    want="${2:-45}"; limit="${3:-600}"
    for i in $(seq 1 $((limit / 5))); do
        line=$(shell "grep -E 'FATAL|signal|draws' $DATA/sims2.log | tail -1" | tr -d '\r')
        case "$line" in *FATAL*|*signal*) echo "$line"; exit 1;; esac
        n=$(echo "$line" | sed -n 's/.* \([0-9]*\) draws.*/\1/p')
        [ -n "$n" ] && [ "$n" -ge "$want" ] && { echo "$line"; exit 0; }
        [ -z "$(shell "pidof $BIN" | tr -d '\r')" ] && { echo "process ended"; exit 1; }
        sleep 5
    done
    echo "timeout"; exit 1
    ;;
nhood)
    # start, then open the first neighborhood (Pleasantview) from the chooser
    "$0" start "${@:2}"
    "$0" wait 45 900
    sleep 25
    "$0" send "click 455 345 0"
    sleep 6
    "$0" send "click 598 592 0"
    echo "opening Pleasantview"
    ;;
bt)
    "$ADB" -s "$SERIAL" pull $DIR/run.log build/run.log >/dev/null 2>&1 || true
    python tools/symbolize_bt.py build/run.log
    ;;
stop)
    if [ "${S2_APK:-0}" = 1 ]; then
        shell "am force-stop $APK_PACKAGE; rm -f $LIVE; input keyevent KEYCODE_SLEEP" || true
        exit 0
    fi
    shell "kill \$(pidof $BIN) 2>/dev/null" || true
    ;;
*)
    sed -n 2,15p "$0"
    ;;
esac
