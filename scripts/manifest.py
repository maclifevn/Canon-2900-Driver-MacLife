#!/usr/bin/env python3
import hashlib
import json
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "build/arm64"
files = ["canon2900-rastertocapt", "canon2900usb", "Canon-LBP2900-OpenCAPT.ppd"]
manifest = {
    "version": (root / "VERSION").read_text().strip(),
    "architecture": "arm64",
    "deployment_target": "26.0",
    "target_macos": [26, 27],
    "release_status": "experimental; physical printing not verified for this build",
    "build_host_macos": subprocess.check_output(["/usr/bin/sw_vers", "-productVersion"], text=True).strip(),
    "build_host_os_build": subprocess.check_output(["/usr/bin/sw_vers", "-buildVersion"], text=True).strip(),
    "compiler": subprocess.check_output(["/usr/bin/clang", "--version"], text=True).splitlines()[0],
    "sha256": {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in files},
}
(out / "BUILD-MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")
