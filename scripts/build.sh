#!/bin/sh
set -eu

PROJECT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$PROJECT_DIR/build/arm64"
SRC="$PROJECT_DIR/third_party/captdriver/src"
VERSION="$(/usr/bin/tr -d '[:space:]' < "$PROJECT_DIR/VERSION")"
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export MACOSX_DEPLOYMENT_TARGET=26.0
export COPYFILE_DISABLE=1

if [ "$(uname -m)" != arm64 ]; then
    echo "This release is built for Apple Silicon (arm64)." >&2
    exit 1
fi
. "$PROJECT_DIR/sources.lock"
for entry in "$CAPT_SHA256:$CAPT_ARCHIVE" "$CUPS_SHA256:$CUPS_ARCHIVE"; do
    expected="${entry%%:*}"
    archive="${entry#*:}"
    actual="$(shasum -a 256 "$PROJECT_DIR/third_party/source/$archive" | awk '{print $1}')"
    [ "$actual" = "$expected" ] || { echo "Source checksum mismatch: $archive" >&2; exit 1; }
done

mkdir -p "$OUT"
rm -f "$OUT/canon2900-rastertocapt" "$OUT/canon2900usb" \
    "$OUT/Canon-LBP2900-OpenCAPT.ppd" "$OUT/BUILD-MANIFEST.json"
echo "Building the native CAPT filter (deployment target macOS 26.0)..."
clang -arch arm64 -mmacosx-version-min=26.0 -std=c99 -O2 -Wall -Wextra \
    -D_DARWIN_C_SOURCE -I"$SRC" \
    "$SRC/rastertocapt.c" "$SRC/capt-command.c" "$SRC/capt-status.c" \
    "$SRC/generic-ops.c" "$SRC/printer.c" "$SRC/paper.c" \
    "$SRC/hiscoa-common.c" "$SRC/hiscoa-compress.c" "$SRC/prn_lbp2900.c" \
    -lcups -o "$OUT/canon2900-rastertocapt" >"$OUT/filter-build.log" 2>&1 || {
    cat "$OUT/filter-build.log" >&2; exit 1;
}

# Build CUPS in a temporary path without spaces; its upstream makefiles assume
# unquoted build paths. No system CUPS files or services are installed/replaced.
TASK_TMP="${TMPDIR:-/tmp}"
TASK_BUILD="$(mktemp -d "${TASK_TMP%/}/canon2900-build.XXXXXX")"
trap 'rm -rf "$TASK_BUILD"' EXIT HUP INT TERM
mkdir -p "$TASK_BUILD/cups"
tar -xzf "$PROJECT_DIR/third_party/source/$CUPS_ARCHIVE" \
    -C "$TASK_BUILD/cups" --strip-components=1
patch -s -d "$TASK_BUILD/cups" -p1 < "$PROJECT_DIR/patches/cups-backend-name.patch"
JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
echo "Building the native Darwin USB backend..."
if ! (
    cd "$TASK_BUILD/cups"
    ./configure --prefix="$TASK_BUILD/stage" --with-components=libcups \
        --with-tls=no --with-dnssd=no --disable-dbus --disable-libusb \
        CC=/usr/bin/clang CFLAGS='-arch arm64 -mmacosx-version-min=26.0 -O2' \
        LDFLAGS='-arch arm64 -mmacosx-version-min=26.0' &&
    make -C cups -j"$JOBS" libcups.a libcups.2.dylib &&
    make -C backend -j"$JOBS" usb
) >"$OUT/backend-build.log" 2>&1; then
    tail -n 60 "$OUT/backend-build.log" >&2
    exit 1
fi
cp "$TASK_BUILD/cups/backend/usb" "$OUT/canon2900usb"
install_name_tool -change "$TASK_BUILD/stage/lib/libcups.2.dylib" \
    /usr/lib/libcups.2.dylib "$OUT/canon2900usb"
codesign -f -s - "$OUT/canon2900-rastertocapt" "$OUT/canon2900usb"
chmod 0555 "$OUT/canon2900-rastertocapt" "$OUT/canon2900usb"

echo "Generating the printer description..."
ppdc -d "$TASK_BUILD/ppd" "$SRC/canon-lbp.drv" >"$OUT/ppd-build.log" 2>&1 || {
    cat "$OUT/ppd-build.log" >&2; exit 1;
}
sed \
    -e "s/^\*FileVersion:.*/\*FileVersion: \"$VERSION\"/" \
    -e 's/^\*PCFileName:.*/\*PCFileName: "C2900OC.PPD"/' \
    -e 's/^\*ModelName:.*/\*ModelName: "Canon LBP2900"/' \
    -e 's/^\*ShortNickName:.*/\*ShortNickName: "Canon LBP2900"/' \
    -e 's/^\*NickName:.*/\*NickName: "Canon LBP2900"/' \
    -e 's/application\/vnd.cups-raster 1 rastertocapt/application\/vnd.cups-raster 1 canon2900-rastertocapt/' \
    "$TASK_BUILD/ppd/CanonLBP-2900-3000.ppd" > "$OUT/Canon-LBP2900-OpenCAPT.ppd"
echo '*% OpenCAPT-Package: org.opencapt.lbp2900.macos' >> "$OUT/Canon-LBP2900-OpenCAPT.ppd"

/usr/bin/python3 "$PROJECT_DIR/scripts/manifest.py"
echo "Native binaries and PPD built in: $OUT"
