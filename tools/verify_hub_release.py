#!/usr/bin/env python3
"""Verify the single normal package published to The FM-1 Firmware Hub."""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "web"))
from make_site import product_of  # noqa: E402


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_hub_release.py PACKAGE.fwsc")

    package = Path(sys.argv[1])
    raw = package.read_bytes()
    header = (ROOT / "firmware" / "src" / "rdx_version.h").read_text(encoding="utf-8")
    match = re.search(r'^#define RDX_IDENTITY "([^"]+)"$', header, re.MULTILINE)
    if not match:
        raise SystemExit("RDX_IDENTITY is missing")

    expected = match.group(1)
    actual = product_of(raw)
    if actual != expected:
        raise SystemExit(f"package identity {actual!r}, expected {expected!r}")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit("Felucca update loader marker is missing")
    if package.name != "Felucca-RDX.fwsc":
        raise SystemExit(f"unexpected Hub asset name: {package.name}")

    digest = hashlib.sha256(raw).hexdigest()
    print(f"ok: {package} ({len(raw)} bytes, {actual}, sha256 {digest})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

