#!/bin/sh
set -eu
PROJECT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="$(/usr/bin/tr -d '[:space:]' < "$PROJECT_DIR/VERSION")"
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
APP_ID="${DEVELOPER_ID_APPLICATION:-Developer ID Application: Anh Tuan Vu Nguyen (CF5PGH3KGK)}"
export DEVELOPER_ID_INSTALLER="${DEVELOPER_ID_INSTALLER:-Developer ID Installer: Anh Tuan Vu Nguyen (CF5PGH3KGK)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-MNotch-notary}"
export OPENCAPT_ARTIFACTS_DIR="$PROJECT_DIR/build/notarize/arm64"
export OPENCAPT_DIST_DIR="$PROJECT_DIR/dist/notarized"
PKG="$OPENCAPT_DIST_DIR/Canon-LBP2900-OpenCAPT-macOS26-27-arm64-$VERSION.pkg"

IDENTITIES="$(/usr/bin/security find-identity -v)"
for identity in "$APP_ID" "$DEVELOPER_ID_INSTALLER"; do
    if ! printf '%s\n' "$IDENTITIES" | /usr/bin/grep -Fq "\"$identity\""; then
        echo "Missing signing certificate and private key: $identity" >&2
        echo "See docs/NOTARIZE.md; no submission made and existing releases preserved." >&2
        exit 2
    fi
done

/usr/bin/python3 "$PROJECT_DIR/scripts/prepare-signing.py" "$APP_ID"
mkdir -p "$OPENCAPT_DIST_DIR"
# Results below must describe this newly signed package, not a previous run.
rm -f "$OPENCAPT_DIST_DIR/NOTARIZATION-SUBMISSION.json" \
    "$OPENCAPT_DIST_DIR/NOTARIZATION.json" "$OPENCAPT_DIST_DIR/NOTARIZATION-LOG.json" \
    "$OPENCAPT_DIST_DIR/STAPLER.txt" "$OPENCAPT_DIST_DIR/GATEKEEPER.txt"
"$PROJECT_DIR/scripts/package.sh"
"$PROJECT_DIR/scripts/verify.sh"

xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" \
    --output-format json > "$OPENCAPT_DIST_DIR/NOTARIZATION-SUBMISSION.json"
SUBMISSION_ID="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$OPENCAPT_DIST_DIR/NOTARIZATION-SUBMISSION.json")"
echo "Uploaded to Apple; submission: $SUBMISSION_ID"
xcrun notarytool wait "$SUBMISSION_ID" --keychain-profile "$NOTARY_PROFILE" \
    --timeout 10m --output-format json > "$OPENCAPT_DIST_DIR/NOTARIZATION.json"
xcrun notarytool log "$SUBMISSION_ID" --keychain-profile "$NOTARY_PROFILE" \
    "$OPENCAPT_DIST_DIR/NOTARIZATION-LOG.json"
/usr/bin/python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); print("Apple status:",s["status"]); sys.exit(0 if s["status"] == "Accepted" else 1)' \
    "$OPENCAPT_DIST_DIR/NOTARIZATION.json"

xcrun stapler staple "$PKG"
xcrun stapler validate "$PKG" > "$OPENCAPT_DIST_DIR/STAPLER.txt" 2>&1
spctl --assess --type install --verbose=4 "$PKG" > "$OPENCAPT_DIST_DIR/GATEKEEPER.txt" 2>&1
# Stapling changes the package digest. Refresh checksums before re-verifying.
/usr/bin/python3 "$PROJECT_DIR/scripts/validation-report.py"
"$PROJECT_DIR/scripts/verify.sh"
cat "$OPENCAPT_DIST_DIR/STAPLER.txt" "$OPENCAPT_DIST_DIR/GATEKEEPER.txt"
echo "Notarized, stapled and Gatekeeper-accepted: $PKG"
