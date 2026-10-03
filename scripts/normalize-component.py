#!/usr/bin/env python3
"""Remove build-host AppleDouble metadata before productbuild signs/archives.

macOS 27 pkgbuild emits com.apple.provenance AppleDouble records even when
COPYFILE_DISABLE is set. Rebuild only the unsigned component's archives/BOM;
retain Apple's PackageInfo and the actual payload bytes and file modes.
"""
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

component, payload, scripts = map(pathlib.Path, sys.argv[1:])
expanded = component.parent / "normalized-component"
subprocess.run(["/usr/sbin/pkgutil", "--expand", str(component), str(expanded)], check=True)
rows = subprocess.check_output(["/usr/bin/lsbom", str(expanded / "Bom")], text=True).splitlines()
rows = [row for row in rows if not any(part.startswith("._") for part in pathlib.PurePosixPath(row.split("\t", 1)[0]).parts)]
filelist = component.parent / "clean-bom.txt"
filelist.write_text("\n".join(rows) + "\n")
subprocess.run(["/usr/bin/mkbom", "-i", str(filelist), str(expanded / "Bom")], check=True)

for source, name in [(payload, "Payload"), (scripts, "Scripts")]:
    output = expanded / name
    if output.is_dir():
        import shutil
        shutil.rmtree(output)
    subprocess.run([
        "/usr/bin/tar", "--format=cpio", "--no-mac-metadata", "--no-xattrs",
        "--uid", "0", "--gid", "0", "-czf", str(output), "-C", str(source), ".",
    ], check=True)

info = ET.parse(expanded / "PackageInfo")
info.getroot().find("payload").set("numberOfFiles", str(len(rows)))
size = sum((path.stat().st_size + 1023) // 1024 for path in payload.rglob("*") if path.is_file())
info.getroot().find("payload").set("installKBytes", str(size))
info.write(expanded / "PackageInfo", encoding="utf-8", xml_declaration=True)
output = component.parent / "clean-component.pkg"
subprocess.run([
    "/usr/bin/xar", "--compression=none", "-cf", str(output),
    "Bom", "PackageInfo", "Payload", "Scripts",
], cwd=expanded, check=True)
output.replace(component)
print("Component normalized: payload and BOM contain only the driver/support files.")
