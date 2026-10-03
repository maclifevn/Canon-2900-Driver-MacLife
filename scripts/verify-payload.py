#!/usr/bin/env python3
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET

project = pathlib.Path(__file__).resolve().parent.parent
expanded = pathlib.Path(sys.argv[1])
root = expanded / "OpenCAPT-component.pkg/Payload"
support = root / "Library/Application Support/Canon LBP2900 OpenCAPT"
manifest = json.loads((support / "BUILD-MANIFEST.json").read_text())
version = (project / "VERSION").read_text().strip()
assert manifest["version"] == version and manifest["deployment_target"] == "26.0"
paths = {
    "canon2900-rastertocapt": "usr/libexec/cups/filter/canon2900-rastertocapt",
    "canon2900usb": "usr/libexec/cups/backend/canon2900usb",
    "Canon-LBP2900-OpenCAPT.ppd": "Library/Printers/PPDs/Contents/Resources/Canon-LBP2900-OpenCAPT.ppd",
}
for name, relative in paths.items():
    path = root / relative
    assert hashlib.sha256(path.read_bytes()).hexdigest() == manifest["sha256"][name], name
    assert not path.is_symlink(), name
    if name.endswith(".ppd"):
        continue
    assert (path.stat().st_mode & 0o777) == 0o555, name
    assert subprocess.check_output(["/usr/bin/lipo", "-archs", str(path)], text=True).strip() == "arm64"
    subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(path)], check=True)
    if manifest.get("signing"):
        details = subprocess.run(["/usr/bin/codesign", "--display", "--verbose=4", str(path)],
                                 capture_output=True, text=True, check=True).stderr
        assert "Authority=" + manifest["signing"]["developer_id_application"] in details, name
        assert "(runtime)" in details and "Timestamp=" in details, name
    lines = subprocess.check_output(["/usr/bin/otool", "-L", str(path)], text=True).splitlines()[1:]
    dependencies = [line.strip().split(" (", 1)[0] for line in lines]
    assert "/usr/lib/libcups.2.dylib" in dependencies, name
    assert all(p.startswith(("/usr/lib/", "/System/Library/Frameworks/")) for p in dependencies), dependencies
    minimum = subprocess.check_output(["/usr/bin/vtool", "-show-build", str(path)], text=True)
    assert "minos 26.0" in minimum, minimum

with tarfile.open(support / "Corresponding-Source.tar.gz") as archive:
    members = archive.getmembers()
    assert all(m.isfile() and not m.name.startswith("/") and ".." not in pathlib.PurePosixPath(m.name).parts for m in members)
    names = [m.name for m in members]
    for suffix in ["/LICENSE", "/scripts/build.sh", "/third_party/source/cups-4bd119b.tar.gz", "/third_party/captdriver/src/rastertocapt.c"]:
        assert any(name.endswith(suffix) for name in names), suffix

allowed_files = set(paths.values())
allowed_files.update("Library/Application Support/Canon LBP2900 OpenCAPT/" + name for name in [
    "BUILD-MANIFEST.json", "Corresponding-Source.tar.gz", "LICENSE-GPL-3.0.txt", "CUPS-LICENSE.txt", "CUPS-NOTICE.txt",
    "README.md", "SOURCE.md", "THIRD_PARTY_NOTICES.md", "INSTALL.md", "uninstall.sh",
])
actual_files = {str(path.relative_to(root)) for path in root.rglob("*") if path.is_file() or path.is_symlink()}
assert actual_files == allowed_files, actual_files.symmetric_difference(allowed_files)
bom = expanded / "OpenCAPT-component.pkg/Bom"
bom_lines = subprocess.check_output(["/usr/bin/lsbom", "-f", "-p", "fmug", str(bom)], text=True).splitlines()
bom_files = set()
for line in bom_lines:
    relative, mode, uid, gid = line.split("\t")
    relative = relative.removeprefix("./")
    assert relative in allowed_files, f"Unexpected BOM file: {relative}"
    assert uid == "0" and gid == "0", f"BOM ownership: {line}"
    expected_mode = 0o644
    if relative in [paths["canon2900-rastertocapt"], paths["canon2900usb"]]:
        expected_mode = 0o555
    elif relative.endswith("/uninstall.sh"):
        expected_mode = 0o755
    assert int(mode, 8) == stat.S_IFREG | expected_mode, f"BOM mode: {line}"
    bom_files.add(relative)
assert bom_files == allowed_files, bom_files.symmetric_difference(allowed_files)
info = ET.parse(expanded / "OpenCAPT-component.pkg/PackageInfo").getroot()
assert info.attrib["identifier"] == "org.opencapt.lbp2900.macos"
assert info.attrib["version"] == version
for name in ["preinstall", "postinstall"]:
    assert (expanded / "OpenCAPT-component.pkg/Scripts" / name).read_bytes() == (project / "package/scripts" / name).read_bytes()
assert not any(path.name.startswith("._") for path in expanded.rglob("*"))
print("Payload verified: ARM64, min macOS 26.0, signatures, system libraries, exact files, scripts and corresponding source.")
