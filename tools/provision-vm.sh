#!/usr/bin/env bash
# Installs what Burrow needs in a fresh test VM overlay (see docs/VM.md).
set -euo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$BURROW_ROOT"
bash tools/haiku.sh 'set -e
# Stock OpenVPN for comparison, plus what our patched build links against.
pkgman install -y openvpn lzo_devel lz4_devel libtool >/dev/null
# Write crash reports instead of opening the debugger.
mkdir -p ~/config/settings/system/debug_server
echo "default_action report" > ~/config/settings/system/debug_server/settings
mkdir -p /boot/home/burrow
echo provisioned'
