#!/usr/bin/env python3
"""Offline validation. Never installs a package or sends a printer job."""
import os
import hashlib
import json
import pathlib
import socket
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "build/tests"
out.mkdir(parents=True, exist_ok=True)
src = root / "third_party/captdriver/src"
logs = []
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

def run(args, expected=0, timeout=60, **kwargs):
    process = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=env, timeout=timeout, **kwargs)
    if process.returncode != expected:
        (out / "last-failure.log").write_bytes(process.stderr)
        raise RuntimeError("Command failed: " + str(args) + "\n" + process.stderr.decode(errors="replace")[-3000:])
    return process

def log(message):
    logs.append(message)
    print(message, flush=True)

cc = ["/usr/bin/clang", "-arch", "arm64", "-std=c99", "-O1", "-g",
      "-D_DARWIN_C_SOURCE", "-fsanitize=address,undefined", "-I", src]
run(cc + [root / "tests/compressor.c", src / "hiscoa-compress.c", src / "hiscoa-common.c",
          root / "third_party/captdriver/tests/hiscoa-decompress.c", "-o", out / "compressor"])
log(run([out / "compressor"]).stdout.decode().strip())

driver_sources = [src / (name + ".c") for name in ["capt-command", "capt-status", "generic-ops",
                  "printer", "paper", "hiscoa-common", "hiscoa-compress", "prn_lbp2900"]]
run(cc + [root / "tests/filter-unit.c"] + driver_sources + ["-lcups", "-o", out / "filter-unit"])
log(run([out / "filter-unit"]).stdout.decode().strip())

run(cc + ["-DCAPT_MAX_POLLS=2", root / "tests/status-unit.c", src / "capt-status.c", "-o", out / "status-unit"])
log(run([out / "status-unit"]).stdout.decode().strip())
failure = run([out / "status-unit", "stuck"], expected=1)
assert b"timed out waiting" in failure.stderr
log("Status polling: persistent busy returns failure instead of false success")

binary = root / "build/arm64/canon2900-rastertocapt"
for name, data in [("empty.ras", b""), ("invalid.ras", b"Not a CUPS raster file\n")]:
    fixture = out / name
    fixture.write_bytes(data)
    result = run([binary, "1", "tester", "Input rejection test", "1", "", fixture], expected=1, timeout=5)
    assert b"no pages in job" in result.stderr or b"unable to open CUPS raster" in result.stderr
log("Built filter: empty and invalid jobs fail before USB communication")

# A small, generated PDF checks Apple's real PDF-to-raster path with our PPD.
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.276 841.89] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.276 841.89] /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]
for page in (1, 2):
    text = ("BT /F1 18 Tf 50 780 Td (Canon LBP2900 OpenCAPT - page %d) Tj ET\n" % page).encode()
    objects.append(b"<< /Length " + str(len(text)).encode() + b" >>\nstream\n" + text + b"endstream")
pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
offsets = [0]
for index, obj in enumerate(objects, 1):
    offsets.append(len(pdf))
    pdf += str(index).encode() + b" 0 obj\n" + obj + b"\nendobj\n"
xref = len(pdf)
pdf += ("xref\n0 %d\n0000000000 65535 f \n" % len(offsets)).encode()
for offset in offsets[1:]:
    pdf += ("%010d 00000 n \n" % offset).encode()
pdf += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(offsets), xref)).encode()
fixture = out / "two-pages.pdf"
fixture.write_bytes(pdf)
result = run(["/usr/sbin/cupsfilter", "-p", root / "build/arm64/Canon-LBP2900-OpenCAPT.ppd",
              "-m", "application/vnd.cups-raster", "-o", "PageSize=A4", "-o", "Resolution=600dpi", fixture])
raster = out / "two-pages.ras"
raster.write_bytes(result.stdout)
(out / "rasterization.log").write_bytes(result.stderr)
log(run([out / "filter-unit", raster]).stdout.decode().strip())

run(cc + [root / "tests/usb-session-unit.c", "-lcups", "-o", out / "usb-session-unit"])


def start_cups_process(args, backchannel, sidechannel, **kwargs):
    # Map the test pipes to the same descriptors as a real CUPS filter/backend.
    setup = """
import os, sys
sources = [int(value) for value in sys.argv[1:3]]
copies = [os.dup(fd) for fd in sources]
for source, dest in zip(copies, (3, 4)):
    os.dup2(source, dest)
    os.set_inheritable(dest, True)
for fd in set(sources + copies) - {3, 4}:
    os.close(fd)
os.execv(sys.argv[3], sys.argv[3:])
"""
    return subprocess.Popen([sys.executable, "-c", setup, str(backchannel), str(sidechannel),
                             *map(str, args)], pass_fds=(backchannel, sidechannel), env=env, **kwargs)


with tempfile.TemporaryDirectory(prefix="capt-test-", dir="/tmp") as temp, socket.socket(socket.AF_UNIX) as listener:
    path = str(pathlib.Path(temp) / "usb.sock")
    listener.bind(path)
    listener.listen()
    daemon = subprocess.Popen([out / "usb-session-unit", "serve", str(listener.fileno())],
                              pass_fds=(listener.fileno(),), env=env)
    try:
        for index in range(1, 9):
            read_fd, write_fd = os.pipe()
            filter_side, backend_side = socket.socketpair()
            with filter_side, backend_side, tempfile.TemporaryFile() as backend_log:
                filter_process = start_cups_process(
                    [binary, index, "tester", "Sequential USB job", 1, "", raster],
                    read_fd, filter_side.fileno(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )
                backend_process = start_cups_process(
                    [out / "usb-session-unit", "submit", path], write_fd, backend_side.fileno(),
                    stdin=filter_process.stdout, stderr=backend_log,
                )
                os.close(read_fd)
                os.close(write_fd)
                filter_process.stdout.close()
                filter_side.close()
                backend_side.close()
                try:
                    _, filter_log = filter_process.communicate(timeout=15)
                    backend_code = backend_process.wait(timeout=5)
                    backend_log.seek(0)
                    backend_output = backend_log.read()
                    assert filter_process.returncode == backend_code == 0, (filter_log, backend_output)
                    assert b"PAGE: 1 1\n" in filter_log and b"PAGE: 2 1\n" in filter_log
                    assert f"Mock USB job {index}\n".encode() in backend_output
                    if index == 2:
                        assert b"discarded 3 stale USB reply bytes" in filter_log
                except subprocess.TimeoutExpired as error:
                    backend_log.seek(0)
                    raise RuntimeError((error.stderr, backend_log.read())) from error
                finally:
                    for process in (filter_process, backend_process):
                        if process.poll() is None:
                            process.kill()
                            process.wait()
    finally:
        daemon.terminate()
        daemon.wait(timeout=5)
log("USB session relay: eight consecutive two-page jobs passed with fragmented, BCD and stale replies")

log("Physical printing: NOT TESTED. No package was installed and no printer job was sent.")
(out / "RESULTS.txt").write_text("\n".join(logs) + "\n")
(out / "RESULTS.json").write_text(json.dumps({
    "filter_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "ppd_sha256": hashlib.sha256((root / "build/arm64/Canon-LBP2900-OpenCAPT.ppd").read_bytes()).hexdigest(),
    "sanitizers": ["address", "undefined"],
    "result": "passed",
    "physical_printing": "not tested",
}, indent=2) + "\n")
