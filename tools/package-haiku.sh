#!/usr/bin/env bash
# Builds the Haiku package (.hpkg) on Haiku. Run from the repository root.
set -euo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$BURROW_ROOT"
NAME=$(awk '$1 == "name" { print $2; exit }' resources/Burrow.PackageInfo)
VERSION=$(awk '$1 == "version" { print $2; exit }' resources/Burrow.PackageInfo)
ARCH=$(awk '$1 == "architecture" { print $2; exit }' resources/Burrow.PackageInfo)
FILE="$BURROW_ROOT/artifacts/$NAME-$VERSION-$ARCH.hpkg"
make -j6
sh openvpn/build.sh
STAGE=$(mktemp -d /tmp/burrow-package-XXXXXX)
trap 'rm -rf -- "$STAGE"' EXIT
DOCS="$STAGE/documentation/packages/burrow"
mkdir -p "$STAGE/apps" "$STAGE/bin" "$DOCS/openvpn" "$STAGE/data/deskbar/menu/Applications" \
    "$BURROW_ROOT/artifacts"
cp build-haiku/Burrow "$STAGE/apps/Burrow"
strip --strip-debug "$STAGE/apps/Burrow"
# GNU strip removes the appended Haiku resources; restore them.
xres -o "$STAGE/apps/Burrow" build-haiku/Burrow.rsrc
cp build-haiku/openvpn/burrow-openvpn "$STAGE/bin/burrow-openvpn"
cp resources/Burrow.PackageInfo "$STAGE/.PackageInfo"
cp README.md LICENSE "$DOCS/"
cp -R docs "$DOCS/"
# OpenVPN is GPL 2: ship how burrow-openvpn was made next to the binary.
cp -R openvpn/patches openvpn/build.sh openvpn/SOURCE.md "$DOCS/openvpn/"
ln -s ../../../../apps/Burrow "$STAGE/data/deskbar/menu/Applications/Burrow"
( cd "$STAGE" && mimeset --all -f --mimedb data/mime_db --mimedb /boot/system/data/mime_db apps/Burrow )
package create -C "$STAGE" "$FILE"
printf '%s\n' "$FILE"
