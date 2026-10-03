#!/usr/bin/env python3
"""Bundle corresponding source, licenses and offline build tools."""
import gzip
import os
import pathlib
import tarfile

root = pathlib.Path(__file__).resolve().parent.parent
version = (root / "VERSION").read_text().strip()
dist = pathlib.Path(os.environ.get("OPENCAPT_DIST_DIR", str(root / "dist")))
dist.mkdir(parents=True, exist_ok=True)
destination = dist / ("Canon-LBP2900-OpenCAPT-" + version + "-source.tar.gz")
prefix = "Canon-LBP2900-OpenCAPT-" + version
entries = []
for name in ["VERSION", "LICENSE", "Makefile", "README.md", "SOURCE.md", "sources.lock",
             "THIRD_PARTY_NOTICES.md", "scripts", "package", "docs", "tests", "patches", "third_party"]:
    path = root / name
    entries.extend([path] if path.is_file() else sorted(path.rglob("*")))

with destination.open("wb") as stream:
    with gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for path in entries:
                if not path.is_file() or path.name.startswith("._") or path.name == ".DS_Store" or "__pycache__" in path.parts:
                    continue
                info = archive.gettarinfo(str(path), prefix + "/" + str(path.relative_to(root)))
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = 0
                info.pax_headers = {}
                info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                with path.open("rb") as source:
                    archive.addfile(info, source)
print(destination)
