#!/usr/bin/env bash
# qemu-smoke.sh — boot the ARC kernel in QEMU and verify it reaches
# userspace (init prints "init: starting" only from user mode, so it
# proves the syscall/thread path works).
#
# Used by CI (.github/workflows/ci.yml).  Also fine for local use:
#
#   tools/qa/qemu-smoke.sh            # amd64 (builds disk.img if missing)
#   tools/qa/qemu-smoke.sh i386       # i386   (builds disk.img with i386 kernel)
#   tools/qa/qemu-smoke.sh arm64      # arm64 in QEMU virt (direct kernel boot)
#
# Exit codes: 0 = boot OK, 1 = boot failed, 2 = usage / missing artifacts.
set -euo pipefail

ARCH="${1:-amd64}"
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
    timeout 90 "$@" > "$LOG" 2>&1 || true
    echo "=== boot log tail ==="
    tail -25 "$LOG"
}

case "$ARCH" in
    amd64|i386)
        if [ ! -x "$REPO_ROOT/arc.elf" ]; then
            echo "arc.elf not found — run: make ARCH=$ARCH" >&2
            exit 2
        fi

        echo "=== building boot disk (tools/qa/mkdisk.sh) ==="
        "$REPO_ROOT/tools/qa/mkdisk.sh"

        if [ "$ARCH" = "amd64" ]; then
            QEMU=qemu-system-x86_64
        else
            QEMU=qemu-system-i386
        fi
        echo "=== qemu boot ($ARCH) ==="
        run_qemu "$QEMU" -machine q35 \
            -drive "file=$REPO_ROOT/disk.img,format=raw,if=ide" \
            -serial stdio -no-reboot -m 64

        check_marker "arc kernel $ARCH"
        check_marker "arc: BSD layer initialized"
        check_marker "init: starting (pid=1)"
        check_no_panic
        ;;

    arm64)
        if [ ! -f "$REPO_ROOT/arc.bin" ]; then
            echo "arc.bin not found — run: make ARCH=arm64 arc.bin" >&2
            exit 2
        fi

        echo "=== qemu boot (arm64 virt) ==="
        # -nic none: without it QEMU instantiates a default virtio-net
        # NIC whose option rom (efi-virtio.rom, shipped by the separate
        # ipxe-qemu package) may be missing and aborts the whole boot.
        run_qemu qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a57 \
            -m 64 -kernel "$REPO_ROOT/arc.bin" \
            -display none -serial stdio -nic none

        check_marker "arc kernel arm64"
        check_marker "arc: BSD layer initialized"
        check_no_panic
        ;;

    *)
        echo "usage: $0 [amd64|i386|arm64]" >&2
        exit 2
        ;;
esac

if [ "$fail" -ne 0 ]; then
    echo "=== boot log (full) ==="
    cat "$LOG"
    exit 1
fi

echo "=== boot OK ($ARCH) ==="
