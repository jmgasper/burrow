#!/usr/bin/env bash
# Starts Burrow's Haiku R1/beta6 test VM (VNC 127.0.0.1:5908, SSH 2228).
#
# The workspace drive has no room for a full disk clone, so the VM boots
# TasAmp's beta6 disk read-only through a throwaway qcow2 overlay. The overlay
# lives in BURROW_VM_TMPDIR and is recreated whenever the base disk changed;
# run tools/provision-vm.sh after a fresh overlay. See docs/VM.md.
set -euo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$BURROW_ROOT"
BASE=${BURROW_VM_BASE:-$(cd .. && pwd)/tasamp/.vm/work.qcow2}
TMP=${BURROW_VM_TMPDIR:-/mnt/HaikuWork/tmp/burrow-vm}
OVERLAY=$TMP/overlay.qcow2
STAMP=$TMP/base.stamp

if [[ -S .vm/qmp.sock ]] && python3 tools/vm.py status >/dev/null 2>&1; then
    echo 'The Burrow VM is already running.'
    exit 0
fi
if [[ -f .vm/qemu.pid ]] && kill -0 "$(cat .vm/qemu.pid)" 2>/dev/null; then
    echo 'VM process exists but QMP is not answering; inspect the existing process.' >&2
    exit 1
fi
if [[ ! -f $BASE ]]; then
    echo "Missing base disk $BASE (set BURROW_VM_BASE)." >&2
    exit 1
fi

mkdir -p "$TMP"
free_kb=$(df -Pk "$TMP" | awk 'NR == 2 {print $4}')
if (( free_kb < 3 * 1024 * 1024 )); then
    echo "Less than 3 GiB free for the overlay in $TMP; set BURROW_VM_TMPDIR." >&2
    exit 1
fi

# The overlay is only valid on top of the exact base it was created from.
stamp=$(stat -c '%s %Y' "$BASE")
if [[ -f $OVERLAY && "$(cat "$STAMP" 2>/dev/null)" != "$stamp" ]]; then
    echo 'Base disk changed since the overlay was made; starting from a fresh overlay.'
    rm -f "$OVERLAY"
fi
if [[ ! -f $OVERLAY ]]; then
    qemu-img create -q -f qcow2 -b "$BASE" -F qcow2 "$OVERLAY"
    echo "$stamp" > "$STAMP"
    echo 'Fresh overlay: run tools/provision-vm.sh once the guest is up.'
fi

rm -f .vm/qmp.sock
exec qemu-system-x86_64 -enable-kvm -cpu host -m "${BURROW_VM_MEMORY:-6144}" -smp "${BURROW_VM_CPUS:-6}" \
    -drive file="$OVERLAY",format=qcow2,if=ide,index=0 \
    -nic user,model=e1000,hostfwd=tcp:127.0.0.1:2228-:22 \
    -device qemu-xhci -device usb-tablet -vga std -display none \
    -vnc 127.0.0.1:8 -qmp unix:.vm/qmp.sock,server=on,wait=off \
    -pidfile .vm/qemu.pid -serial file:.vm/serial.log -daemonize
