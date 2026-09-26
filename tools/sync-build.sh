#!/usr/bin/env bash
# Sync sources to the Burrow VM and build there, printing compiler errors only.
# Usage: tools/sync-build.sh [make-target] [max-lines]
set -uo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$BURROW_ROOT"
TARGET=${1:-all}
# Everything the build and the packaging step read, so `make package` also works
# in the guest.
tar -czf - --exclude=__pycache__ Makefile README.md LICENSE docs src tests tools resources openvpn 2>/dev/null |
    bash tools/haiku.sh "mkdir -p /boot/home/burrow && tar xzf - --warning=no-timestamp -C /boot/home/burrow 2>/dev/null; cd /boot/home/burrow && make -j6 $TARGET 2>&1 | grep -E -B1 -A4 'error|Error|undefined|warning: unused' | head -${2:-80}; echo \"exit: \${PIPESTATUS[0]}\""
