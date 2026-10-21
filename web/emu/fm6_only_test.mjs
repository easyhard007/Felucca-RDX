// SPDX-License-Identifier: GPL-3.0-only
// Regression for state that exists on a physical FM-1 but not in a fresh browser simulator:
// a saved FAV filter and an autosave whose selected track used another engine.
import fs from "fs";
import { storeSectors, restoreSectors } from "./worklet.js";

const bytes = fs.readFileSync(process.argv[2] || "build/emu/felucca.wasm");
let failed = false;
const check = (name, ok) => {
  console.log(`fm6-only: ${name.padEnd(66)} ${ok ? "ok" : "FAIL"}`);
  failed ||= !ok;
};

async function boot(saved = null) {
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const ex = instance.exports;
  ex._initialize();
  ex.web_nor_erase();
  if (saved) restoreSectors(new Uint8Array(ex.memory.buffer, ex.web_nor(), ex.web_nor_size()), saved);
  ex.web_boot();
  const render = (ms) => {
    let peak = 0;
    for (let left = Math.round(ms * 44.1); left > 0;) {
      const n = Math.min(left, 128);
      ex.web_render(n);
      const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), n);
      const r = new Float32Array(ex.memory.buffer, ex.web_out_r(), n);
      for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(l[i]), Math.abs(r[i]));
      left -= n;
    }
    return peak;
  };
  render(700);
  return { ex, render, sectors: () => storeSectors(new Uint8Array(ex.memory.buffer, ex.web_nor(), ex.web_nor_size())) };
}

const legacy = await boot();
legacy.ex.web_test_make_legacy_state();
const first = await boot(legacy.sectors());
check("first upgrade clears FAV and exposes all 8 factory presets", first.ex.web_test_preset_total() === 8);
check("first upgrade skips the incompatible autosave once",
      [0, 1, 2, 3].every((k) => first.ex.web_test_track_engine(k) === 12) &&
      [0, 4, 3, 2].every((p, k) => first.ex.web_test_track_preset(k) === p));

const second = await boot(first.sectors());
const secondEngines = [0, 1, 2, 3].map((k) => second.ex.web_test_track_engine(k));
const secondPresets = [0, 1, 2, 3].map((k) => second.ex.web_test_track_preset(k));
check("a later restore migrates a stored non-FM6 track to engine 12",
      secondEngines.every((e) => e === 12) && secondPresets[0] === 7 && second.ex.web_test_preset_total() === 8);
if (!secondEngines.every((e) => e === 12) || secondPresets[0] !== 7)
  console.log(`fm6-only: restored engines=${secondEngines.join(",")} presets=${secondPresets.join(",")}`);

const names = ["TINE EP", "BELL", "FM BASS", "BRASS", "PAD", "MARIMBA", "ORGAN", "PLUCK"];
for (let p = 0; p < names.length; p++) {
  const d = await boot();
  d.ex.web_test_fm6_preset(p);
  d.ex.web_keys(1 << 7);
  const peak = d.render(500);
  d.ex.web_keys(0);
  d.render(500);
  check(`${names[p]} renders audible audio (peak ${peak.toFixed(3)})`, peak > 0.005);
}

console.log(failed ? "fm6-only: FAILED" : "fm6-only: all ok");
process.exit(failed ? 1 : 0);
