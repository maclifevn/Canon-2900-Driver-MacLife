#!/bin/sh
set -eu
PROJECT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="$(/usr/bin/tr -d '[:space:]' < "$PROJECT_DIR/VERSION")"
DIST="${OPENCAPT_DIST_DIR:-$PROJECT_DIR/dist}"
PKG="$DIST/Canon-LBP2900-OpenCAPT-macOS26-27-arm64-$VERSION.pkg"
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
for script in "$PROJECT_DIR"/scripts/*.sh "$PROJECT_DIR"/package/scripts/*; do
    sh -n "$script"
done
(cd "$DIST" && shasum -a 256 -c SHA256SUMS)
TASK_TMP="${TMPDIR:-/tmp}"
VERIFY_DIR="$(mktemp -d "${TASK_TMP%/}/canon2900-verify.XXXXXX")"
trap 'rm -rf "$VERIFY_DIR"' EXIT HUP INT TERM
pkgutil --expand-full "$PKG" "$VERIFY_DIR/expanded"
/usr/bin/python3 "$PROJECT_DIR/scripts/verify-payload.py" "$VERIFY_DIR/expanded"
ROOT="$VERIFY_DIR/expanded/OpenCAPT-component.pkg/Payload"
# The unpacked tree belongs to the build user. Installed filter ownership and
# permissions are checked against the package BOM by verify-payload.py.
cupstestppd -I filters -R "$ROOT" "$ROOT/Library/Printers/PPDs/Contents/Resources/Canon-LBP2900-OpenCAPT.ppd"
pkgutil --check-signature "$PKG" > "$DIST/PACKAGE-SIGNATURE.txt" 2>&1 || true
cat "$DIST/PACKAGE-SIGNATURE.txt"
/usr/bin/python3 "$PROJECT_DIR/scripts/validation-report.py"
echo "Static package verification passed. Physical printing remains unverified."
