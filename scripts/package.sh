#!/bin/sh
set -eu

PROJECT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ARTIFACTS="${OPENCAPT_ARTIFACTS_DIR:-$PROJECT_DIR/build/arm64}"
STAGE="$PROJECT_DIR/build/package"
PAYLOAD="$STAGE/root"
RESOURCES="$STAGE/resources"
SCRIPT_DIR="$STAGE/scripts"
SUPPORT="$PAYLOAD/Library/Application Support/Canon LBP2900 OpenCAPT"
VERSION="$(/usr/bin/tr -d '[:space:]' < "$PROJECT_DIR/VERSION")"
DIST="${OPENCAPT_DIST_DIR:-$PROJECT_DIR/dist}"
PKG="$DIST/Canon-LBP2900-OpenCAPT-macOS26-27-arm64-$VERSION.pkg"
SOURCE="$DIST/Canon-LBP2900-OpenCAPT-$VERSION-source.tar.gz"
export COPYFILE_DISABLE=1
export COPY_EXTENDED_ATTRIBUTES_DISABLE=1
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

if [ -n "${DEVELOPER_ID_INSTALLER:-}" ]; then
    case "$DEVELOPER_ID_INSTALLER" in
        'Developer ID Installer:'*) ;;
        *) echo "A .pkg requires a Developer ID Installer identity." >&2; exit 1 ;;
    esac
fi

for name in canon2900-rastertocapt canon2900usb Canon-LBP2900-OpenCAPT.ppd BUILD-MANIFEST.json; do
    [ -f "$ARTIFACTS/$name" ] || { echo "Missing $name. Run make build first." >&2; exit 1; }
done
/usr/bin/python3 "$PROJECT_DIR/scripts/source-archive.py"
rm -rf "$STAGE"
mkdir -p "$PAYLOAD/usr/libexec/cups/filter" "$PAYLOAD/usr/libexec/cups/backend" \
    "$PAYLOAD/Library/Printers/PPDs/Contents/Resources" "$SUPPORT" \
    "$RESOURCES" "$SCRIPT_DIR"
cp -X "$ARTIFACTS/canon2900-rastertocapt" "$PAYLOAD/usr/libexec/cups/filter/"
cp -X "$ARTIFACTS/canon2900usb" "$PAYLOAD/usr/libexec/cups/backend/"
cp -X "$ARTIFACTS/Canon-LBP2900-OpenCAPT.ppd" "$PAYLOAD/Library/Printers/PPDs/Contents/Resources/"
cp -X "$ARTIFACTS/BUILD-MANIFEST.json" "$SUPPORT/"
cp -X "$SOURCE" "$SUPPORT/Corresponding-Source.tar.gz"
cp -X "$PROJECT_DIR/LICENSE" "$SUPPORT/LICENSE-GPL-3.0.txt"
cp -X "$PROJECT_DIR/third_party/licenses/CUPS-LICENSE.txt" "$SUPPORT/"
cp -X "$PROJECT_DIR/third_party/licenses/CUPS-NOTICE.txt" "$SUPPORT/"
cp -X "$PROJECT_DIR/README.md" "$PROJECT_DIR/SOURCE.md" "$PROJECT_DIR/THIRD_PARTY_NOTICES.md" "$SUPPORT/"
cp -X "$PROJECT_DIR/docs/INSTALL.md" "$SUPPORT/"
cp -X "$PROJECT_DIR/scripts/uninstall.sh" "$SUPPORT/uninstall.sh"
cp -X "$PROJECT_DIR/package/scripts/preinstall" "$PROJECT_DIR/package/scripts/postinstall" "$SCRIPT_DIR/"
cp -X "$PROJECT_DIR/package/resources/"*.html "$RESOURCES/"
cp -X "$PROJECT_DIR/LICENSE" "$RESOURCES/license.txt"
chmod -R u+w "$PAYLOAD" "$RESOURCES" "$SCRIPT_DIR"
chmod 0555 "$PAYLOAD/usr/libexec/cups/filter/canon2900-rastertocapt" "$PAYLOAD/usr/libexec/cups/backend/canon2900usb"
chmod 0755 "$SUPPORT/uninstall.sh" "$SCRIPT_DIR/preinstall" "$SCRIPT_DIR/postinstall"
sed "s/@VERSION@/$VERSION/g" "$PROJECT_DIR/package/Distribution.xml.in" > "$STAGE/Distribution.xml"

# Apple packaging helpers can refuse output on an external workspace volume.
# Build the archives in the permitted temporary directory, then copy the final
# artifacts back. The payload remains a local staging tree throughout.
TASK_TMP="${TMPDIR:-/tmp}"
TASK_PACKAGE="$(mktemp -d "${TASK_TMP%/}/canon2900-package.XXXXXX")"
trap 'rm -rf "$TASK_PACKAGE"' EXIT HUP INT TERM

pkgbuild --root "$PAYLOAD" --scripts "$SCRIPT_DIR" \
    --identifier org.opencapt.lbp2900.macos --version "$VERSION" \
    --install-location / --ownership recommended "$TASK_PACKAGE/OpenCAPT-component.pkg"
/usr/bin/python3 "$PROJECT_DIR/scripts/normalize-component.py" \
    "$TASK_PACKAGE/OpenCAPT-component.pkg" "$PAYLOAD" "$SCRIPT_DIR"
if [ -n "${DEVELOPER_ID_INSTALLER:-}" ]; then
    productbuild --distribution "$STAGE/Distribution.xml" --resources "$RESOURCES" \
        --package-path "$TASK_PACKAGE" --sign "$DEVELOPER_ID_INSTALLER" --timestamp \
        "$TASK_PACKAGE/$(basename "$PKG")"
else
    productbuild --distribution "$STAGE/Distribution.xml" --resources "$RESOURCES" \
        --package-path "$TASK_PACKAGE" "$TASK_PACKAGE/$(basename "$PKG")"
fi
cp -X "$TASK_PACKAGE/$(basename "$PKG")" "$PKG"
cp -X "$ARTIFACTS/BUILD-MANIFEST.json" "$DIST/BUILD-MANIFEST.json"
cp -X "$PROJECT_DIR/scripts/uninstall.sh" "$DIST/uninstall.sh"
cp -X "$PROJECT_DIR/docs/INSTALL.md" "$DIST/INSTALL.md"
(cd "$DIST" && shasum -a 256 "$(basename "$PKG")" "$(basename "$SOURCE")" \
    BUILD-MANIFEST.json uninstall.sh INSTALL.md > SHA256SUMS)
echo "Built: $PKG"
