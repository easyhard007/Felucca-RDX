#!/usr/bin/env python3
"""Import the Reface DX factory bank and hot-path lookup tables.

The source data is the GPL-3.0-or-later RDX_new/PicoFaceDX port used by the
CoreS3 reference project.  The generated header is plain C so the AC79 build
does not need C++, libm, or dynamic initialisation.
"""
from __future__ import annotations

import argparse
import math
import re
from pathlib import Path


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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("reference", type=Path, help="components/rdx_reface directory")
    ap.add_argument("output", type=Path)
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
    presets: list[tuple[str, list[int]]] = []
    for name, body in entries:
        data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)]
        if len(data) != 150:
            raise SystemExit(f"{name}: expected 150 bytes, found {len(data)}")
        presets.append((name, data))

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
        " * Generated from the CoreS3 RDX_new/PicoFaceDX-derived implementation.\n",
        " * Source preset copyright: 2026 Michi71.  Do not hand-edit. */\n",
        "#ifndef FELUCCA_RDX_DATA_H\n#define FELUCCA_RDX_DATA_H\n#include <stdint.h>\n\n",
        "#define RDX_NPRESETS 35u\n#define RDX_PATCH_SIZE 150u\n",
        "typedef struct { char name[12]; uint8_t patch[RDX_PATCH_SIZE]; } rdx_factory_t;\n",
        "static const rdx_factory_t RDX_FACTORY[RDX_NPRESETS] = {\n",
    ]
    for name, data in presets:
        rows = []
        for i in range(0, len(data), 15):
            rows.append("            " + ", ".join(f"0x{x:02X}" for x in data[i:i + 15]))
        out.append(f'    {{"{name}", {{\n' + ",\n".join(rows) + "\n    }},\n")
    out.append("};\n\n")
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
