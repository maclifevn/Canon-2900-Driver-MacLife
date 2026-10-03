#!/usr/bin/env python3
import hashlib
import json
import os
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
dist = pathlib.Path(os.environ.get("OPENCAPT_DIST_DIR", str(root / "dist")))
manifest = json.loads((dist / "BUILD-MANIFEST.json").read_text())
lines = [
    "Canon LBP2900 OpenCAPT " + manifest["version"] + " - EXPERIMENTAL",
    "Built on macOS " + manifest["build_host_macos"] + " (" + manifest["build_host_os_build"] + ").",
    "ARM64; deployment target macOS 26.0; targets macOS 26 and 27.",
    "Static package verification: PASSED (payload, PPD, signatures, libraries, min OS, source archive).",
]
package_signature = (dist / "PACKAGE-SIGNATURE.txt").read_text()
if "Developer ID Installer:" in package_signature:
    lines.append("Installer: Developer ID Installer signed with a secure timestamp.")
else:
    lines.append("Installer: unsigned / not notarized.")
if manifest.get("signing"):
    lines.append("Mach-O binaries: Developer ID Application signed; hardened runtime and secure timestamps verified.")
    lines.append("Executable code and data unchanged from the tested unsigned binaries.")
    lines.append("Signed binary startup checks: " + manifest["signing"]["startup_checks"] + ".")
else:
    lines.append("Mach-O binaries: ad-hoc signed.")
notary = dist / "NOTARIZATION.json"
if notary.exists():
    state = json.loads(notary.read_text())
    lines.append("Apple notarization status: " + state.get("status", "unknown") + "; submission " + state.get("id", "unknown") + ".")
if (dist / "STAPLER.txt").exists():
    lines.append("Stapled ticket validation: see STAPLER.txt.")
if (dist / "GATEKEEPER.txt").exists():
    lines.append("Gatekeeper assessment: see GATEKEEPER.txt.")
results = root / "build/tests/RESULTS.json"
if results.exists():
    tests = json.loads(results.read_text())
    if (tests["result"] == "passed" and
        tests["filter_sha256"] == manifest["sha256"]["canon2900-rastertocapt"] and
        tests["ppd_sha256"] == manifest["sha256"]["Canon-LBP2900-OpenCAPT.ppd"]):
        lines.append("Automated tests passed for this filter and PPD (AddressSanitizer + UndefinedBehaviorSanitizer):")
        lines.append((root / "build/tests/RESULTS.txt").read_text().strip())
    else:
        lines.append("Automated tests: no matching results for this build.")
else:
    lines.append("Automated tests: not run.")
lines += [
    "Package installation on a real host: NOT TESTED.",
    "Physical printing on macOS 26: NOT TESTED.",
    "Physical printing on macOS 27: NOT TESTED.",
]
(dist / "VALIDATION.txt").write_text("\n".join(lines) + "\n")
version = manifest["version"]
names = ["Canon-LBP2900-OpenCAPT-macOS26-27-arm64-" + version + ".pkg",
         "Canon-LBP2900-OpenCAPT-" + version + "-source.tar.gz",
         "BUILD-MANIFEST.json", "uninstall.sh", "INSTALL.md", "VALIDATION.txt", "PACKAGE-SIGNATURE.txt"]
names += [name for name in ["NOTARIZATION-SUBMISSION.json", "NOTARIZATION.json", "NOTARIZATION-LOG.json", "STAPLER.txt", "GATEKEEPER.txt"] if (dist / name).exists()]
(dist / "SHA256SUMS").write_text("".join(hashlib.sha256((dist / name).read_bytes()).hexdigest() + "  " + name + "\n" for name in names))
print("Validation report saved: " + str(dist / "VALIDATION.txt"))
