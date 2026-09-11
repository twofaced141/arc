#!/usr/bin/env bash
# qemu-smp.sh — boot ARC with multiple CPUs and verify SMP bringup.
#
#   tools/qa/qemu-smp.sh [amd64|arm64] [ncpus]
#
# Checks:
#   - smp: N/M APs online (M == ncpus-1)
#   - IPI_RESCHEDULE delivered (amd64) / smp selftest PASS
#   - no panic output
# Exit codes: 0 = SMP OK, 1 = failed, 2 = usage/missing artifacts.
set -euo pipefail

ARCH="${1:-amd64}"
NCPUS="${2:-4}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

fail=0

check_marker() {
    if grep -qF "$1" "$LOG"; then
        echo "OK: $1"
    else
        echo "FAIL: missing marker: $1"
        fail=1
    fi
}

check_no_panic() {
    if grep -qiE "panic|double fault|fatal error|kernel oops|general protection" "$LOG"; then
        echo "FAIL: panic-like output in boot log:"
        grep -iE "panic|double fault|fatal error|kernel oops|general protection" "$LOG" | head -5
        fail=1
    else
        echo "OK: no panic output"
    fi
}

run_qemu() {
    timeout 120 "$@" > "$LOG" 2>&1 || true
    echo "=== boot log tail ==="
    tail -30 "$LOG"
}

case "$ARCH" in
    amd64)
        if [ ! -x "$REPO_ROOT/arc.elf" ]; then
            echo "arc.elf not found — run: make ARCH=amd64" >&2
            exit 2
        fi
        echo "=== building boot disk (tools/qa/mkdisk.sh) ==="
        "$REPO_ROOT/tools/qa/mkdisk.sh"
        echo "=== qemu boot (amd64 -smp $NCPUS) ==="
        run_qemu qemu-system-x86_64 -machine q35 \
            -smp "$NCPUS" \
            -drive "file=$REPO_ROOT/disk.img,format=raw,if=ide" \
            -serial stdio -no-reboot -m 128

        check_marker "arc kernel amd64"
        check_marker "smp: $((NCPUS-1))/$((NCPUS-1)) APs online"
        check_marker "smp: IPI_RESCHEDULE delivered to $((NCPUS-1))/$((NCPUS-1)) CPUs"
        check_marker "smp: selftest PASS"
        check_marker "init: starting (pid=1)"
        check_no_panic
        ;;

    arm64)
        if [ ! -f "$REPO_ROOT/arc.bin" ]; then
            echo "arc.bin not found — run: make ARCH=arm64 arc.bin" >&2
            exit 2
        fi
        echo "=== qemu boot (arm64 virt -smp $NCPUS) ==="
        run_qemu qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a57 \
            -smp "$NCPUS" -m 128 -kernel "$REPO_ROOT/arc.bin" \
            -display none -serial stdio -nic none

        check_marker "arc kernel arm64"
        check_marker "APs online"
        check_marker "smp: selftest PASS"
        check_no_panic
        ;;

    *)
        echo "usage: $0 [amd64|arm64] [ncpus]" >&2
        exit 2
        ;;
esac

if [ "$fail" -ne 0 ]; then
    echo "=== boot log (full) ==="
    cat "$LOG"
    exit 1
fi

echo "=== SMP boot OK ($ARCH -smp $NCPUS) ==="
