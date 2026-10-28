#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the Felucca-RDX source-level browser emulator into build/emu."""

import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "emu"


def main():
    header = (ROOT / "firmware" / "src" / "rdx_version.h").read_text(encoding="utf-8")
    match = re.search(r'^#define\s+RDX_VERSION\s+"([^"]+)"\s*$', header, re.M)
    if not match:
        raise SystemExit("Cannot read RDX_VERSION from firmware/src/rdx_version.h")
    version = match.group(1)
    emcc = shutil.which("emcc")
    if not emcc:
        raise SystemExit("emcc is required to build the browser emulator")

    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        emcc, "-O2", "-ffp-contract=off", "-std=gnu99", "-w",
        "-DFELUCCA_RDX=1", "-DFELUCCA_FM6_ONLY=1", "-DFELUCCA_CPU2=0",
        "-Ibuild/gen", "-Ifirmware/src", "--no-entry", "-sSTANDALONE_WASM",
        "-sTOTAL_STACK=1048576", "-sINITIAL_MEMORY=33554432", "-sFILESYSTEM=0",
        "-o", str(OUT / "felucca.wasm"), "web/emu/felucca_web.c",
    ], cwd=ROOT, check=True)

    html = (ROOT / "web" / "emu" / "index.html").read_text(encoding="utf-8")
    html = html.replace("@VERSION@", f"Felucca-RDX {version}").replace("@PREVIEW@", "")
    (OUT / "index.html").write_text(html, encoding="utf-8")
    shutil.copy2(ROOT / "web" / "emu" / "worklet.js", OUT / "worklet.js")
    fonts = OUT / "fonts"
    fonts.mkdir(exist_ok=True)
    for name in ("DotGothic16-subset.woff", "OFL.txt"):
        shutil.copy2(ROOT / "web" / "emu" / "fonts" / name, fonts / name)
    print(f"emulator: {OUT} (Felucca-RDX {version})")


if __name__ == "__main__":
    main()
