#!/bin/sh
# Builds Burrow's OpenVPN in the Haiku guest: OpenVPN 2.6.13 with the
# HaikuPorts platform patches, the AWS Client VPN patch and Burrow's own
# patches, in that order. Output: build-haiku/openvpn/burrow-openvpn.
# Usage (from the checkout root, in Haiku): sh openvpn/build.sh
set -e
VERSION=2.6.13
SHA256=730ad13ab0d8efda24b7e8e0df660e4c358ad89105aca47f2bf26da44d8ab020
ROOT=$(pwd)
OUT=$ROOT/build-haiku/openvpn
TARBALL=$OUT/openvpn-$VERSION.tar.gz
SRC=$OUT/openvpn-$VERSION

mkdir -p "$OUT"
if [ ! -f "$TARBALL" ]; then
	curl -sSfL -o "$TARBALL.part" \
		"https://github.com/OpenVPN/openvpn/archive/refs/tags/v$VERSION.tar.gz"
	mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c - >/dev/null

# Always start from pristine sources so patch changes take effect.
patches=$(ls "$ROOT"/openvpn/patches/0*.patch* | sort)
stamp=$(cat $patches | sha256sum | cut -d' ' -f1)
if [ ! -f "$SRC/.burrow-patches" ] || [ "$(cat "$SRC/.burrow-patches")" != "$stamp" ]; then
	rm -rf "$SRC"
	mkdir -p "$SRC"
	tar xzf "$TARBALL" -C "$SRC" --strip-components=1
	for patch in $patches; do
		echo "applying $(basename "$patch")"
		patch -d "$SRC" -p1 -s < "$patch"
	done
	(cd "$SRC" && autoreconf -fi >/dev/null 2>&1)
	(cd "$SRC" && LDFLAGS="-lnetwork" ./configure -q \
		--disable-plugin-auth-pam --disable-plugin-down-root \
		--disable-pkcs11 \
		--disable-debug >/dev/null)
	echo "$stamp" > "$SRC/.burrow-patches"
fi
make -C "$SRC" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
cp "$SRC/src/openvpn/openvpn" "$OUT/burrow-openvpn"
strip "$OUT/burrow-openvpn"
"$OUT/burrow-openvpn" --version | head -1
