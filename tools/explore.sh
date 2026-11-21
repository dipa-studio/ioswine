#!/system/bin/sh
# explore.sh - device-side crash census for Aidoku (run via devq as uid shell, ~135 s budget).
# Starts ioswine-loader, drives the presenter with random taps/swipes/back, restarts the app
# after every exit, and prints every crash/exception line with a run number.
# usage: sh ./explore.sh [seed] [seconds]
SEED=${1:-1}
BUDGET=${2:-125}
DIR=/data/local/tmp/ioswine
cd "$DIR" || exit 1
mkdir -p home/aidoku/tmp explore
RANDOM=$SEED
# Never leave loaders behind (devq's timeout kills this script; orphans then fight over the presenter).
cleanup() { pkill -9 -f "ioswine-loader Aidoku" 2>/dev/null; }
trap cleanup EXIT INT TERM HUP
cleanup
START=$(date +%s)
run=0

app_env() {
    env IOSWINE_RENDERS_LOCALLY=1 HOME=$DIR/home/aidoku CFFIXED_USER_HOME=$DIR/home/aidoku \
        TMPDIR=$DIR/home/aidoku/tmp/ IOSWINE_TOUCH=1 IOSWINE_TOUCH_DIRECT=1 IOSWINE_TOUCH_TRACE=1 \
        IOSWINE_IOKIT=1 IOSWINE_SNAPSHOT=1 IOSWINE_SNAPSHOT_MS=50 IOSWINE_SNAPSHOT_SCALE=2 IOSWINE_NO_ANIM=1 \
        IOSWINE_FAKEBOARD=1 IOSWINE_DSC=$DIR/dsc/dyld_shared_cache_arm64e "$@"
}

# Screen is 1440x3200. Tab bar y~3165, nav bar buttons y~190, content rows y 600..2900.
presenter_focused() {
    dumpsys window 2>/dev/null | grep -m1 mCurrentFocus | grep -q org.ioswine.surface
}

monkey_step() {
    # Safety: never touch anything unless the presenter window has focus.
    if ! presenter_focused; then
        echo "[monkey] presenter not focused, refocusing, no input sent"
        am start -n org.ioswine.surface/android.app.NativeActivity >/dev/null 2>&1
        return
    fi
    r=$((RANDOM % 90))
    if [ $r -lt 35 ]; then
        x=$(( (RANDOM % 5) * 288 + 144 )); input tap $x 3165; echo "[monkey] tab $x"
    elif [ $r -lt 50 ]; then
        x=$(( 1050 + (RANDOM % 3) * 120 )); input tap $x 190; echo "[monkey] navbtn $x"
    elif [ $r -lt 80 ]; then
        y=$(( 600 + RANDOM % 2300 )); input tap 700 $y; echo "[monkey] row $y"
    else
        input swipe 700 2400 700 900 300; echo "[monkey] swipe-up"
    fi
}

while [ $(( $(date +%s) - START )) -lt $BUDGET ]; do
    run=$((run + 1))
    cleanup; sleep 1
    LOG=explore/run-$SEED-$run.log
    app_env ./ioswine-loader Aidoku.app/Aidoku > "$LOG" 2>&1 &
    PID=$!
    # wait for first frame (max 40 s)
    w=0
    while [ $w -lt 40 ] && kill -0 $PID 2>/dev/null && ! grep -q "rc=0" "$LOG"; do sleep 1; w=$((w + 1)); done
    while kill -0 $PID 2>/dev/null && [ $(( $(date +%s) - START )) -lt $BUDGET ]; do
        monkey_step >> "$LOG"
        sleep 1.5
    done
    kill -9 $PID 2>/dev/null
    wait $PID 2>/dev/null
    RC=$?
    frames=$(grep -c "rc=0" "$LOG")
    echo "=== run $SEED-$run rc=$RC frames=$frames"
    grep -a -E "lsk CRASH|Terminating app|reason:|\[nsexception\]" "$LOG" | head -5
    grep -a "\[monkey\]" "$LOG" | tail -4 | sed 's/^/  last: /'
done
echo "=== explore done seed=$SEED runs=$run"
