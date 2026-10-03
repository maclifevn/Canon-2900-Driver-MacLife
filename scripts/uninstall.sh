#!/bin/sh
set -eu

PKG_ID=org.opencapt.lbp2900.macos
QUEUE=Canon_LBP2900_OpenCAPT
if [ "$(/usr/bin/id -u)" -ne 0 ]; then
    echo "Run: sudo /bin/sh uninstall.sh" >&2
    exit 1
fi
if ! /usr/sbin/pkgutil --pkg-info "$PKG_ID" >/dev/null 2>&1; then
    echo "This package has no installed receipt; nothing removed." >&2
    exit 1
fi

if /usr/bin/lpstat -p "$QUEUE" >/dev/null 2>&1; then
    if ! /usr/bin/grep -q '^\*% OpenCAPT-Package: org.opencapt.lbp2900.macos$' \
        "/etc/cups/ppd/$QUEUE.ppd"; then
        echo "Queue ownership changed; uninstall stopped to preserve it." >&2
        exit 1
    fi
    /usr/bin/cancel -a "$QUEUE" >/dev/null 2>&1 || true
    /usr/sbin/lpadmin -x "$QUEUE"
fi

/bin/rm -f /usr/libexec/cups/filter/canon2900-rastertocapt \
    /usr/libexec/cups/backend/canon2900usb \
    /Library/Printers/PPDs/Contents/Resources/Canon-LBP2900-OpenCAPT.ppd
/bin/rm -rf "/Library/Application Support/Canon LBP2900 OpenCAPT"
/usr/sbin/pkgutil --forget "$PKG_ID" >/dev/null
echo "Canon LBP2900 OpenCAPT removed."
