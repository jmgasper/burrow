#!/usr/bin/env bash
# Cross-builds Burrow for the ROCK 5 ITX (Haiku arm64) on the Linux workstation
# and packages it as artifacts/burrow-<version>-arm64.hpkg.
#
# Uses the fork's arm64 cross toolchain and host tools (build/arm64), the
# Summit arm64 sysroot and its static OpenSSL 3 (build/summit-arm64/deps, which
# carries the Haiku arm64 armcap SIGBUS fix). The board has no OpenSSL, LZO or
# LZ4 packages, so burrow-openvpn links OpenSSL statically and leaves out LZO
# and LZ4 compression, which AWS Client VPN does not use.
set -euo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$BURROW_ROOT"
WORK=/mnt/HaikuWork
CROSS=$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku
TOOLS=$WORK/build/arm64/objects/linux/x86_64/release/tools
SYSROOT=${BURROW_ARM64_SYSROOT:-$WORK/build/summit-arm64/sysroot}
DEPS=${BURROW_ARM64_DEPS:-$WORK/build/summit-arm64/deps}
OUT=$BURROW_ROOT/build-arm64
JOBS=${JOBS:-8}

for required in "$CROSS-g++" "$TOOLS/rc/rc" "$TOOLS/xres" "$TOOLS/resattr/resattr" \
        "$TOOLS/package/package" "$SYSROOT/boot/system/develop/lib/libbe.so" \
        "$DEPS/lib/libssl.a" "$DEPS/lib/libcrypto.a"; do
    if [[ ! -e $required ]]; then
        echo "Missing $required" >&2
        exit 1
    fi
done

# --- burrow-openvpn ---------------------------------------------------------
VERSION=2.6.13
SHA256=730ad13ab0d8efda24b7e8e0df660e4c358ad89105aca47f2bf26da44d8ab020
TARBALL=$WORK/cache/openvpn-$VERSION.tar.gz
SRC=$OUT/openvpn/openvpn-$VERSION
mkdir -p "$OUT/openvpn" "$WORK/cache"
if [[ ! -f $TARBALL ]]; then
    curl -sSfL -o "$TARBALL.part" \
        "https://github.com/OpenVPN/openvpn/archive/refs/tags/v$VERSION.tar.gz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c - >/dev/null
patches=$(ls openvpn/patches/0*.patch* | sort)
stamp=$( (cat $patches; echo arm64-static-openssl) | sha256sum | cut -d' ' -f1)
if [[ ! -f $SRC/.burrow-patches || "$(cat "$SRC/.burrow-patches")" != "$stamp" ]]; then
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar xzf "$TARBALL" -C "$SRC" --strip-components=1
    for patch in $patches; do
        patch -d "$SRC" -p1 -s < "$patch"
    done
    (cd "$SRC" && autoreconf -fi >/dev/null 2>&1)
    # The host's ifconfig and route must not leak into the paths OpenVPN runs.
    (cd "$SRC" && ./configure -q --host=aarch64-unknown-haiku --build=x86_64-pc-linux-gnu \
        CC="$CROSS-gcc --sysroot=$SYSROOT" \
        OPENSSL_CFLAGS="-I$DEPS/include" \
        OPENSSL_LIBS="$DEPS/lib/libssl.a $DEPS/lib/libcrypto.a" \
        LDFLAGS="-lnetwork" \
        IFCONFIG=/boot/system/bin/ifconfig ROUTE=/boot/system/bin/route \
        NETSTAT=/boot/system/bin/netstat IPROUTE=/bin/false \
        --disable-lzo --disable-lz4 --disable-plugins --disable-pkcs11 \
        --disable-debug --disable-unit-tests >/dev/null)
    echo "$stamp" > "$SRC/.burrow-patches"
fi
make -C "$SRC" -j"$JOBS" >/dev/null
cp "$SRC/src/openvpn/openvpn" "$OUT/burrow-openvpn"
"$CROSS-strip" "$OUT/burrow-openvpn"

# --- Burrow -----------------------------------------------------------------
make -s -j"$JOBS" BUILD=build-arm64 TARGET_OS=Haiku CXX="$CROSS-g++ --sysroot=$SYSROOT" objects
objects=$(ls build-arm64/src/core/*.o build-arm64/src/ui/*.o build-arm64/src/main.o)
"$CROSS-g++" --sysroot="$SYSROOT" -o build-arm64/Burrow $objects \
    -lbe -ltracker -lnetwork -Wl,--export-dynamic
"$TOOLS/rc/rc" -I resources -o build-arm64/Burrow.rsrc resources/Burrow.rdef
"$TOOLS/xres" -o build-arm64/Burrow build-arm64/Burrow.rsrc

# --- package ----------------------------------------------------------------
NAME=$(awk '$1 == "name" { print $2; exit }' resources/Burrow.PackageInfo)
PKGVERSION=$(awk '$1 == "version" { print $2; exit }' resources/Burrow.PackageInfo)
FILE=$BURROW_ROOT/artifacts/$NAME-$PKGVERSION-arm64.hpkg
STAGE=$(mktemp -d "$WORK/tmp/burrow-arm64-XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
DOCS=$STAGE/documentation/packages/burrow
mkdir -p "$STAGE/apps" "$STAGE/bin" "$DOCS/openvpn" "$STAGE/data/deskbar/menu/Applications" \
    "$BURROW_ROOT/artifacts"
cp build-arm64/Burrow "$STAGE/apps/Burrow"
"$CROSS-strip" --strip-debug "$STAGE/apps/Burrow"
# GNU strip drops the appended resources; packagefs serves Tracker's icon and
# signature from attributes, which mimeset would write on Haiku.
"$TOOLS/xres" -o "$STAGE/apps/Burrow" build-arm64/Burrow.rsrc
"$TOOLS/resattr/resattr" -O -o "$STAGE/apps/Burrow" build-arm64/Burrow.rsrc
cp build-arm64/burrow-openvpn "$STAGE/bin/burrow-openvpn"
chmod 755 "$STAGE/apps/Burrow" "$STAGE/bin/burrow-openvpn"
cp README.md LICENSE "$DOCS/"
cp -R docs "$DOCS/"
cp -R openvpn/patches openvpn/build.sh openvpn/SOURCE.md "$DOCS/openvpn/"
cp tools/build-arm64.sh "$DOCS/openvpn/"
ln -s ../../../../apps/Burrow "$STAGE/data/deskbar/menu/Applications/Burrow"
# arm64 build: OpenSSL is linked in and LZO/LZ4 are left out, so only Haiku is required.
awk '
    /^architecture / { print "architecture arm64"; next }
    /^requires \{/   { print; print "    haiku >= r1~beta6"; inreq = 1; next }
    inreq && /^\}/   { print; inreq = 0; next }
    inreq            { next }
                     { print }
' resources/Burrow.PackageInfo > "$STAGE/.PackageInfo"
rm -f "$FILE"
"$TOOLS/package/package" create -C "$STAGE" "$FILE" >/dev/null
sha256sum "$FILE"
