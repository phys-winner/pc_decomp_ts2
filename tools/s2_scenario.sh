#!/bin/bash
# Drive the shell build on the target from start into the first neighborhood
# (UI scale 200% layout: 1390x632 game pixels), optionally on to the
# Graphics/Performance Options panel or into the Cordial family's lot (live
# mode), and take a screenshot.
#   tools/s2_scenario.sh nhood|graphics|lot [OUT.png]      (extra environment: S2_ENV)
set -e
cd "$(dirname "$0")/.."
stage="${1:-nhood}"
out="${2:-build/live/scenario.png}"
S2="bash tools/s2.sh"
click() { $S2 send "cursor $1 $2"; sleep 0.8; $S2 send "bdown 0"; sleep 0.15; $S2 send "bup 0"; sleep "${3:-2}"; }
# The neighborhood's idle drift camera (CameraDriftCamEnabled) moves the lots
# under scripted clicks: off for this run only. The game reads userProps.xml
# at startup; the user's value goes back once the neighborhood is up.
export MSYS_NO_PATHCONV=1
ADB="${ADB:-C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
PROPS="/sdcard/Sims2/userdata/Documents/EA Games/The Sims 2 Legacy/Config/userProps.xml"
adb_shell() { "$ADB" -s "${S2_SERIAL:-192.168.1.103:5555}" shell "$@"; }
drift=$(adb_shell "grep -o 'CameraDriftCamEnabled\" type=\"0xcba908e1\">[a-z]*' '$PROPS'" | sed 's/.*>//' | tr -d '\r')
[ -n "$drift" ] && adb_shell "sed -i 's/\(CameraDriftCamEnabled\" type=\"0xcba908e1\">\)[a-z]*/\1false/' '$PROPS'"
$S2 start >/dev/null
# The chooser: wait for its frames (51 draws) to repeat, then pick the first neighborhood.
for i in $(seq 1 90); do
    sleep 5
    n=$($S2 log 40 | grep -c " 51 draws" || true)
    [ "$n" -ge 3 ] && break
done
click 510 300 4
click 653 548 5
# The neighborhood: wait for its frames (1000+ draws).
for i in $(seq 1 90); do
    sleep 5
    line=$($S2 log 20 | grep -E "draws|FATAL|signal" | tail -1)
    case "$line" in *FATAL*|*signal*) echo "$line"; exit 1;; esac
    d=$(echo "$line" | sed -n 's/.* \([0-9]*\) draws.*/\1/p')
    [ -n "$d" ] && [ "$d" -ge 1000 ] && break
done
sleep 5
[ -n "$drift" ] && adb_shell "sed -i 's/\(CameraDriftCamEnabled\" type=\"0xcba908e1\">\)[a-z]*/\1$drift/' '$PROPS'"
# Expansion pack notices (OK at 691,437) and the welcome (a white panel across
# row 473, its check button at 694,506), each clicked only while it shows: a
# click on the ground pans the camera. The fly-in can outlast the first shots.
seen=0
for i in $(seq 1 30); do
    $S2 shot build/live/probe.png >/dev/null
    if python tools/find_play.py --button build/live/probe.png 691 437; then click 691 437 1.5; seen=1; continue; fi
    if python tools/find_play.py --panel build/live/probe.png 473; then click 694 506 3; seen=2; continue; fi
    [ "$seen" = 2 ] && break
    sleep 2
done
if [ "$stage" = graphics ]; then
    click 120 600 2                                       # "..." options
    click 298 539 3                                       # Graphics/Performance
fi
if [ "$stage" = lot ]; then
    # A family lot: under a green plumbob, hovered until its family tooltip
    # shows, then clicked; Play on the family panel; the lot's first dialogs.
    play=
    for attempt in 1 2 3 4; do
        $S2 shot build/live/hood.png >/dev/null
        # S2_LOT_AT="X Y" picks the lot under a known plumbob (Belladonna Cove, the
        # camera where the fly-in leaves it: 1027 326 the Bella Park Condominiums,
        # the Clevelands, ~1300 draws; 1309 552 a house, ~450): the plumbobs spin,
        # and which of them the detection sees changes from run to run.
        points=$(python tools/find_play.py --plumbobs build/live/hood.png | tr ' ' ',')
        [ -n "${S2_LOT_AT:-}" ] && points=$(echo "$S2_LOT_AT" | tr ' ' ',')
        for point in $points; do
            x=${point%,*}; y=${point#*,}
            $S2 send "cursor $x $y"; sleep 2.5
            $S2 shot build/live/hover.png >/dev/null
            python tools/find_play.py --tooltip build/live/hover.png $x $y || continue
            $S2 send "bdown 0"; sleep 0.15; $S2 send "bup 0"; sleep 5
            $S2 shot build/live/panel.png >/dev/null
            play=$(python tools/find_play.py build/live/panel.png)
            break                                         # clicked: the camera may have moved
        done
        [ -n "$play" ] && break
    done
    [ -n "$play" ] || { echo "no family panel (build/live/hood.png, panel.png)"; exit 1; }
    click $play 5                                         # Play with this family
    for i in $(seq 1 120); do
        sleep 5
        line=$($S2 log 20 | grep -E "draws|FATAL|signal" | tail -1)
        case "$line" in *FATAL*|*signal*) echo "$line"; exit 1;; esac
        d=$(echo "$line" | sed -n 's/.* \([0-9]*\) draws.*/\1/p')
        [ -n "$d" ] && [ "$d" -ge 200 ] && [ "$d" -le 1000 ] && break   # a lot; the neighborhood draws 1500+
    done
    sleep 20
    # The aspiration tutorial (Continue at 1267,80, four steps that animate in)
    # and the game tips question (No at 741,376) hold the game paused.
    # They appear a few seconds into the lot: wait for the first, then stop
    # once none has shown for three looks in a row.
    hits=0; misses=0
    for i in $(seq 1 30); do
        $S2 shot build/live/probe.png >/dev/null
        if python tools/find_play.py --button build/live/probe.png 1267 80; then click 1267 80 2.5; hits=$((hits + 1)); misses=0
        elif python tools/find_play.py --button build/live/probe.png 741 376; then click 741 376 2.5; hits=$((hits + 1)); misses=0
        else
            misses=$((misses + 1))
            [ "$hits" -gt 0 ] && [ "$misses" -ge 3 ] && break
            [ "$misses" -ge 10 ] && break
            sleep 2
        fi
    done
    click 1359 108 1                                      # close the tip balloon
fi
$S2 shot "$out"
