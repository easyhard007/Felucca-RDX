#!/usr/bin/env python3
"""Import the Reface DX sound banks and hot-path lookup tables.

The source data is the GPL-3.0-or-later RDX_new/PicoFaceDX port used by the
CoreS3 reference project.  The generated header is plain C so the AC79 build
does not need C++, libm, or dynamic initialisation.
"""
from __future__ import annotations

import argparse
import math
import re
from pathlib import Path


PATCH_SIZE = 150
FACTORY_EXCLUDE = {"LegendEP1", "*PJ CP-70D", "DynaLd BD6"}


def syx_messages(data: bytes, path: Path) -> list[bytes]:
    """Return complete F0..F7 messages and reject stray/truncated bytes."""
    out: list[bytes] = []
    at = 0
    while at < len(data):
        if data[at] != 0xF0:
            raise SystemExit(f"{path}: stray byte {data[at]:02X} at {at}")
        try:
            end = data.index(0xF7, at + 1)
        except ValueError:
            raise SystemExit(f"{path}: unterminated SysEx message at {at}") from None
        out.append(data[at:end + 1])
        at = end + 1
    return out


def reface_patch(path: Path) -> tuple[str, list[int]]:
    """Decode a Yamaha reface DX seven-message 241-byte bulk dump."""
    blocks: dict[tuple[int, int, int], bytes] = {}
    messages = syx_messages(path.read_bytes(), path)
    if len(messages) != 7:
        raise SystemExit(f"{path}: expected 7 SysEx messages, found {len(messages)}")
    for msg in messages:
        if len(msg) < 13 or msg[:2] != b"\xF0\x43" or msg[3:5] != b"\x7F\x1C" or msg[7] != 0x05:
            raise SystemExit(f"{path}: unsupported Yamaha/re-face DX bulk header")
        count = (msg[5] << 7) | msg[6]
        if count < 4 or len(msg) != count + 9:
            raise SystemExit(f"{path}: invalid bulk byte count {count}")
        # Yamaha checksum covers model, three-byte address and the data.
        if sum(msg[7:-1]) & 0x7F:
            raise SystemExit(f"{path}: invalid Yamaha checksum at address {msg[8]:02X}:{msg[9]:02X}:{msg[10]:02X}")
        blocks[(msg[8], msg[9], msg[10])] = msg[11:-2]
    if blocks.get((0x0E, 0x0F, 0), b"x") or blocks.get((0x0F, 0x0F, 0), b"x"):
        raise SystemExit(f"{path}: missing reface DX bulk start/end blocks")
    common = blocks.get((0x30, 0, 0))
    ops = [blocks.get((0x31, op, 0)) for op in range(4)]
    if common is None or len(common) != 38 or any(op is None or len(op) != 28 for op in ops):
        raise SystemExit(f"{path}: expected one 38-byte common and four 28-byte operator blocks")
    patch = list(common + b"".join(op for op in ops if op is not None))
    if len(patch) != PATCH_SIZE:
        raise SystemExit(f"{path}: decoded patch is {len(patch)} bytes")
    name = bytes(patch[:10]).decode("ascii", errors="strict").rstrip(" \0")
    if not name:
        raise SystemExit(f"{path}: empty voice name")
    return name, patch


def numbers(text: str, name: str, count: int) -> list[float]:
    m = re.search(rf"\b{name}\s*\[\s*{count}\s*\]\s*=\s*\{{(.*?)\}}", text, re.S)
    if not m:
        raise SystemExit(f"cannot find {name}[{count}]")
    body = re.sub(r"//.*", "", m.group(1))
    vals = [float(x) for x in re.findall(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?", body, re.I)]
    if len(vals) != count:
        raise SystemExit(f"{name}: expected {count} values, found {len(vals)}")
    return vals


def c_array(name: str, ctype: str, vals: list[int], cols: int = 8) -> str:
    rows = []
    for i in range(0, len(vals), cols):
        rows.append("    " + ", ".join(str(v) for v in vals[i:i + cols]))
    return f"static const {ctype} {name}[{len(vals)}] = {{\n" + ",\n".join(rows) + "\n};\n"


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("reference", type=Path, help="components/rdx_reface directory")
    ap.add_argument("output", type=Path)
    ap.add_argument("--selected", type=Path,
                    default=Path(__file__).resolve().parents[1] / "assets/rdx/selected",
                    help="directory containing the Selected .syx sound set")
    args = ap.parse_args()

    const_text = (args.reference / "include/rdx_reface/dx_engine/RDX_Constants.h").read_text(encoding="utf-8")
    preset_text = (args.reference / "src/rdx_presets.cpp").read_text(encoding="utf-8")
    aeg_level = numbers(const_text, "AEG_LEVEL", 192)
    peg_speed = numbers(const_text, "PEG_SPEED", 128)
    feedback = numbers(const_text, "FEEDBACK_K", 128)
    velocity = numbers(const_text, "VELO_SENS", 128)
    lfo_speed = numbers(const_text, "LFO_SPEED", 128)
    am_depth = numbers(const_text, "AM_DEPTH", 128)
    pm_depth = numbers(const_text, "PM_DEPTH", 128)
    delay_ms = numbers(const_text, "DELAY_TIME_MS", 128)

    entries = re.findall(r'\{\s*"([^"]+)"\s*,\s*\{([^{}]+)\}\s*\}', preset_text)
    if len(entries) != 35:
        raise SystemExit(f"expected 35 presets, found {len(entries)}")
    source_factory: list[tuple[str, list[int]]] = []
    for name, body in entries:
        data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)]
        if len(data) != 150:
            raise SystemExit(f"{name}: expected 150 bytes, found {len(data)}")
        source_factory.append((name, data))
    missing = FACTORY_EXCLUDE - {name for name, _ in entries}
    if missing:
        raise SystemExit(f"factory exclusions not found: {', '.join(sorted(missing))}")

    selected_paths = sorted(args.selected.glob("*.syx"))
    if not selected_paths:
        raise SystemExit(f"{args.selected}: no Selected .syx files found")
    selected = [reface_patch(path) for path in selected_paths]
    patches = source_factory + selected
    factory_index = [i for i, (name, _) in enumerate(source_factory) if name not in FACTORY_EXCLUDE]
    selected_index = list(range(len(source_factory), len(patches)))
    visible_index = factory_index + selected_index
    if len({name for name, _ in patches}) != len(patches):
        raise SystemExit("duplicate RDX voice name across Factory and Selected")

    gain = []
    for level in range(192):
        v = 0.0 if level == 0 else 10.0 ** (-((127 - level) * 48.0) / (20.0 * 127.0)) - 0.003981071705533
        gain.append(max(0, min(131071, round(v * 32768))))
    env_target = [round(v * 65536) for v in aeg_level[:128]]
    attack_step = [max(1, round(4.1 * v / 44100.0 * 65536)) for v in peg_speed]
    decay_step = [max(1, round(0.27 * v / 44100.0 * 65536)) for v in peg_speed]
    fb_q16 = [round(v * 65536) for v in feedback]
    vel_q15 = [round(v * 32768) for v in velocity]
    detune_q16 = [round((1.00033913 ** (i - 64)) * 65536) for i in range(128)]
    lfo_inc = [round(v / 44100.0 * (1 << 32)) for v in lfo_speed]
    peg_up = [max(1, round(v / 44100.0 * 65536)) for v in peg_speed]
    peg_down = [max(1, round(v * 1.1 / 44100.0 * 65536)) for v in peg_speed]
    am_q15 = [round(v * 32768) for v in am_depth]
    pm_q16 = [round(v * 65536) for v in pm_depth]
    lfo_delay = [0 if i == 0 else round(v * 44.1) for i, v in enumerate(delay_ms)]
    porta_samples = [round(v * 2.5 * 44100) for v in am_depth]
    # One octave in 1/16-semitone steps.  eng_rdx.c interpolates between
    # adjacent entries, then shifts complete octaves, so pitch EG/LFO/glide
    # need no pow() in the audio path.
    pitch_ratio = [round((2.0 ** (i / 192.0)) * 65536) for i in range(192)]
    fixed_inc = []
    for coarse_group in range(4):
        for fine in range(128):
            hz = (10.0 ** coarse_group) * (9.772 ** (fine / 99.0))
            fixed_inc.append(min(0xFFFFFFFF, round(hz / 44100.0 * (1 << 32))))

    out = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later\n",
        " * Generated from the CoreS3 RDX_new/PicoFaceDX-derived implementation\n",
        " * and user-provided Yamaha reface DX SysEx sound dumps.  Do not hand-edit. */\n",
        "#ifndef FELUCCA_RDX_DATA_H\n#define FELUCCA_RDX_DATA_H\n#include <stdint.h>\n\n",
        f"#define RDX_FACTORY_COUNT {len(factory_index)}u\n",
        f"#define RDX_SELECTED_COUNT {len(selected)}u\n",
        f"#define RDX_NPRESETS {len(visible_index)}u\n",
        f"#define RDX_FIRST_VISIBLE {visible_index[0]}u\n",
        f"#define RDX_NPATCHES {len(patches)}u\n#define RDX_PATCH_SIZE {PATCH_SIZE}u\n",
        "typedef struct { char name[12]; uint8_t patch[RDX_PATCH_SIZE]; } rdx_preset_data_t;\n",
        "static const rdx_preset_data_t RDX_PATCHES[RDX_NPATCHES] = {\n",
    ]
    for name, data in patches:
        rows = []
        for i in range(0, len(data), 15):
            rows.append("            " + ", ".join(f"0x{x:02X}" for x in data[i:i + 15]))
        out.append(f"    {{{c_string(name)}, {{\n" + ",\n".join(rows) + "\n    }},\n")
    out.append("};\n\n")
    out.append("#define RDX_FOR_EACH_PATCH(X) \\\n")
    for i in range(len(patches)):
        out.append(f"    X({i})" + (" \\\n" if i + 1 < len(patches) else "\n"))
    out.append("\n")
    out.append("#define RDX_FOR_EACH_PRESET(X) \\\n")
    for at, i in enumerate(visible_index):
        out.append(f"    X({i})" + (" \\\n" if at + 1 < len(visible_index) else "\n"))
    out.append("\n")
    out.append("typedef struct { const char *name; const uint8_t *preset; uint8_t count; } rdx_sound_set_t;\n")
    out.append("static const uint8_t RDX_SET_FACTORY[RDX_FACTORY_COUNT] = {\n    " +
               ", ".join(str(i) for i in factory_index) + "\n};\n")
    out.append("static const uint8_t RDX_SET_SELECTED[RDX_SELECTED_COUNT] = {\n    " +
               ", ".join(str(i) for i in selected_index) + "\n};\n")
    out.append("static const rdx_sound_set_t RDX_SOUND_SETS[] = {\n")
    out.append('    {"Factory", RDX_SET_FACTORY, RDX_FACTORY_COUNT},\n')
    out.append('    {"Selected", RDX_SET_SELECTED, RDX_SELECTED_COUNT}\n};\n\n')
    out.append(c_array("RDX_GAIN_Q15", "uint32_t", gain))
    out.append(c_array("RDX_ENV_TARGET_Q16", "uint32_t", env_target))
    out.append(c_array("RDX_ATTACK_STEP_Q16", "uint32_t", attack_step))
    out.append(c_array("RDX_DECAY_STEP_Q16", "uint16_t", decay_step))
    out.append(c_array("RDX_FEEDBACK_Q16", "uint16_t", fb_q16))
    out.append(c_array("RDX_VELOCITY_Q15", "uint16_t", vel_q15))
    out.append(c_array("RDX_DETUNE_Q16", "uint32_t", detune_q16))
    out.append(c_array("RDX_LFO_INC", "uint32_t", lfo_inc))
    out.append(c_array("RDX_PEG_UP_Q16", "uint16_t", peg_up))
    out.append(c_array("RDX_PEG_DOWN_Q16", "uint16_t", peg_down))
    out.append(c_array("RDX_AM_DEPTH_Q15", "uint16_t", am_q15))
    out.append(c_array("RDX_PM_DEPTH_Q16", "uint32_t", pm_q16))
    out.append(c_array("RDX_LFO_DELAY_SAMPLES", "uint16_t", lfo_delay))
    out.append(c_array("RDX_PORTA_SAMPLES", "uint32_t", porta_samples))
    out.append(c_array("RDX_PITCH_RATIO_Q16", "uint32_t", pitch_ratio))
    out.append(c_array("RDX_FIXED_INC", "uint32_t", fixed_inc, 4))
    out.append("#endif\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("".join(out), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
