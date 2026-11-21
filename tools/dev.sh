#!/usr/bin/env bash
# Single entry point for all device interaction. Always targets DEV_SERIAL (tools/device.env), never bare adb.
#
#   tools/dev.sh devices                 list attached devices (no serial needed)
#   tools/dev.sh info                    M0 facts: model, kernel, page size, PAC, root, VA bits
#   tools/dev.sh push <local> [name]     copy to $DEV_DIR/<name>, chmod 755
#   tools/dev.sh run <name> [args...]    run $DEV_DIR/<name> as root, prints output + "[exit N]"
#   tools/dev.sh strace <name> [args...] same under strace (needs $DEV_DIR/strace pushed)
#   tools/dev.sh logcat [args...]        adb logcat -d (default: last 200 lines)
#   tools/dev.sh pull <remote> [local]
#   tools/dev.sh root '<cmd>'            arbitrary root shell command (destructive patterns refused)
#   tools/dev.sh setenforce0
set -euo pipefail
cd "$(dirname "$0")/.."
DEV_DIR=/data/local/tmp/ioswine

[ "${1:-}" = devices ] && exec adb devices -l
[ -f tools/device.env ] && . tools/device.env
: "${DEV_SERIAL:?set DEV_SERIAL in tools/device.env (see tools/dev.sh devices)}"
adb -s "$DEV_SERIAL" get-state >/dev/null 2>&1 || { echo "device $DEV_SERIAL not attached" >&2; exit 3; }
A=(adb -s "$DEV_SERIAL")

DENY='reboot|fastboot|bootloader|recovery|\bdd\b|mkfs|wipe|flash|/dev/block|sys\.powerctl|\bformat\b|blockdev|remount|\bsgdisk\b|\bparted\b|\bfdisk\b|rm -rf /($| )'
su_run() {  # run a command as root, keep exit code
    local cmd=$1
    if echo "$cmd" | grep -qiE "$DENY"; then echo "dev.sh: refused (destructive pattern): $cmd" >&2; exit 4; fi
    "${A[@]}" shell "su -c 'sh -c \"$cmd; echo [exit \\\$?]\"'"
}

sub=${1:-}; shift || true
case "$sub" in
info)
    "${A[@]}" shell 'echo model=$(getprop ro.product.model) device=$(getprop ro.product.device) android=$(getprop ro.build.version.release)
        echo kernel=$(uname -r) arch=$(uname -m)
        echo pagesize=$(getconf PAGESIZE)
        grep -m1 -E "^Features" /proc/cpuinfo
        grep -E "^CPU part" /proc/cpuinfo | sort | uniq -c
        for f in paca pacg; do grep -qw $f /proc/cpuinfo && echo "$f=yes" || echo "$f=NO"; done
        su -c "id; getenforce; zcat /proc/config.gz 2>/dev/null | grep -E \"CONFIG_ARM64_(VA_BITS|PTR_AUTH|PAGE_SHIFT|.*K_PAGES)=\"; grep -c . /proc/self/maps" 2>&1 | head -20' ;;
push)
    src=$1; name=${2:-$(basename "$1")}
    "${A[@]}" shell "mkdir -p $DEV_DIR"
    "${A[@]}" push "$src" "$DEV_DIR/$name" >/dev/null && "${A[@]}" shell "chmod 755 $DEV_DIR/$name" && echo "pushed $DEV_DIR/$name" ;;
run)
    name=$1; shift; su_run "cd $DEV_DIR && ./$name $*" ;;
strace)
    name=$1; shift; su_run "cd $DEV_DIR && ./strace -f ./$name $*" ;;
logcat)
    if [ $# -eq 0 ]; then "${A[@]}" logcat -d -t 200; else "${A[@]}" logcat -d "$@"; fi ;;
pull)
    "${A[@]}" pull "$1" "${2:-.}" ;;
root)
    su_run "$1" ;;
setenforce0)
    su_run "setenforce 0; getenforce" ;;
*)
    sed -n '2,13p' "$0"; exit 2 ;;
esac
