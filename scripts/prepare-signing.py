#!/usr/bin/env python3
"""Developer ID-sign copies of the tested binaries without recompiling them."""
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

project = pathlib.Path(__file__).resolve().parent.parent
identity = sys.argv[1]
if not identity.startswith("Developer ID Application:"):
    raise SystemExit("Use a Developer ID Application identity for the binaries.")
source = project / "build/arm64"
destination = project / "build/notarize/arm64"
destination.mkdir(parents=True, exist_ok=True)
manifest = json.loads((source / "BUILD-MANIFEST.json").read_text())
names = ["canon2900-rastertocapt", "canon2900usb", "Canon-LBP2900-OpenCAPT.ppd"]


def section(path, name):
    output = subprocess.check_output(["/usr/bin/otool", "-s", "__TEXT", name, str(path)], text=True)
    return [line for line in output.splitlines() if re.match(r"^[0-9a-fA-F]{8,}\s", line)]


for name in names:
    original = source / name
    assert hashlib.sha256(original.read_bytes()).hexdigest() == manifest["sha256"][name], name
    signed = destination / name
    if signed.exists():
        signed.chmod(0o644)
    shutil.copyfile(original, signed)
    if name.endswith(".ppd"):
        signed.chmod(0o644)
        continue
    subprocess.run([
        "/usr/bin/codesign", "--force", "--sign", identity,
        "--options", "runtime", "--timestamp", str(signed),
    ], check=True)
    signed.chmod(0o555)
    subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(signed)], check=True)
    details = subprocess.run(["/usr/bin/codesign", "--display", "--verbose=4", str(signed)],
                             capture_output=True, text=True, check=True).stderr
    assert "Authority=" + identity in details and "(runtime)" in details and "Timestamp=" in details, details
    for name_of_section in ["__text", "__const", "__cstring"]:
        assert section(original, name_of_section) == section(signed, name_of_section), name_of_section
    print(name + ": Developer ID signature, hardened runtime and timestamp verified; code/data unchanged.", flush=True)

# Verify actual startup after enabling hardened runtime. Neither call prints.
empty = project / "build/notarize/empty.ras"
empty.write_bytes(b"")
rejected = subprocess.run([str(destination / names[0]), "1", "tester", "Input check", "1", "", str(empty)],
                          capture_output=True, timeout=5)
assert rejected.returncode == 1 and any(message in rejected.stderr for message in [b"no pages in job", b"unable to open CUPS raster"]), rejected.stderr
discovery = subprocess.run([str(destination / names[1])], capture_output=True, timeout=20)
assert discovery.returncode == 0, discovery.stderr
print("Signed filter input rejection and native USB discovery passed; no print job sent.", flush=True)

manifest["unsigned_binary_sha256"] = {name: manifest["sha256"][name] for name in names if not name.endswith(".ppd")}
manifest["sha256"] = {name: hashlib.sha256((destination / name).read_bytes()).hexdigest() for name in names}
manifest["signing"] = {
    "developer_id_application": identity,
    "hardened_runtime": True,
    "secure_timestamp": True,
    "code_and_data_unchanged": True,
    "startup_checks": "input rejection and USB discovery passed; no print job sent",
}
(destination / "BUILD-MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("Signed artifacts prepared: " + str(destination), flush=True)
