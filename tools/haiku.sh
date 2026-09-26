#!/usr/bin/env bash
# Runs a command inside Burrow's Haiku test VM over SSH (port 2228).
set -euo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec ssh -i "$BURROW_ROOT/.vm/id_ed25519" -p 2228 \
    -o IdentitiesOnly=yes -o BatchMode=yes \
    -o UserKnownHostsFile="$BURROW_ROOT/.vm/known_hosts" -o StrictHostKeyChecking=accept-new \
    -o ConnectTimeout=10 -o ServerAliveInterval=30 user@127.0.0.1 "$@"
