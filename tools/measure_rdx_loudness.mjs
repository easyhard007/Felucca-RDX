// SPDX-License-Identifier: GPL-3.0-or-later
// Measure the loudest continuous 300 ms perceptual window during the first
// second of every visible RDX preset.
// The two-stage De Man K-weighting filter follows ITU-R BS.1770 at 44.1 kHz.
// Usage: node tools/measure_rdx_loudness.mjs [felucca.wasm] [output.json] [--applied]
import fs from "fs";

const wasmPath = process.argv[2] || "build/emu/felucca.wasm";
const outputPath = process.argv[3] || "build/rdx-loudness.json";
const measureAppliedGain = process.argv.includes("--applied");
const bytes = fs.readFileSync(wasmPath);
const sampleRate = 44100;
const windowSeconds = 0.3;
const scanSeconds = 1.0;
const notes = [48, 60, 72]; // C3, C4, C5 (MIDI convention with C4 = 60)
const velocity = 100;

function deManHighShelf() {
  const gainDb = 3.99984385397;
  const q = 0.7071752369554193;
  const frequency = 1681.9744509555319;
  const k = Math.tan(Math.PI * frequency / sampleRate);
  const vh = 10 ** (gainDb / 20);
  const vb = vh ** 0.499666774155;
  const a0 = 1 + k / q + k * k;
  return {
    b0: (vh + vb * k / q + k * k) / a0,
    b1: 2 * (k * k - vh) / a0,
    b2: (vh - vb * k / q + k * k) / a0,
    a1: 2 * (k * k - 1) / a0,
    a2: (1 - k / q + k * k) / a0,
  };
}

function deManHighPass() {
  const q = 0.5003270373253953;
  const frequency = 38.13547087613982;
  const k = Math.tan(Math.PI * frequency / sampleRate);
  const a0 = 1 + k / q + k * k;
  return {
    b0: 1,
    b1: -2,
    b2: 1,
    a1: 2 * (k * k - 1) / a0,
    a2: (1 - k / q + k * k) / a0,
  };
}

function biquad(coefficients) {
  let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  return (x) => {
    const y = coefficients.b0 * x + coefficients.b1 * x1 + coefficients.b2 * x2
            - coefficients.a1 * y1 - coefficients.a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  };
}

function kWeightedChannel() {
  const shelf = biquad(deManHighShelf());
  const highPass = biquad(deManHighPass());
  return (sample) => highPass(shelf(sample));
}

function kWeightedMeter() {
  const left = kWeightedChannel();
  const right = kWeightedChannel();
  const windowFrames = Math.round(sampleRate * windowSeconds);
  const energy = new Float64Array(windowFrames);
  let at = 0, count = 0, sum = 0, maximum = 0;
  return {
    add(l, r) {
      const wl = left(l);
      const wr = right(r);
      const next = (wl * wl + wr * wr) * 0.5;
      if (count === windowFrames)
        sum -= energy[at];
      else
        count++;
      energy[at] = next;
      sum += next;
      at = (at + 1) % windowFrames;
      if (count === windowFrames && sum > maximum)
        maximum = sum;
    },
    rms() {
      const frames = Math.min(count, windowFrames);
      return frames ? Math.sqrt((maximum || sum) / frames) : 0;
    },
  };
}

function cstr(fn, index) {
  let s = "";
  for (let at = 0; at < 32; at++) {
    const c = fn(index, at);
    if (!c) break;
    s += String.fromCharCode(c);
  }
  return s;
}

async function instanceForPreset(visiblePreset) {
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const ex = instance.exports;
  ex._initialize();
  ex.web_nor_erase();
  ex.web_boot();
  ex.web_master(700);
  render(ex, Math.round(sampleRate * 0.8), false);
  ex.web_test_rdx_preset(visiblePreset);
  // The default mode measures the uncalibrated voice/FX chain so rerunning
  // this tool produces the source correction. --applied retains the preset
  // GAIN and verifies the resulting level match.
  if (!measureAppliedGain && ex.web_test_rdx_gain_set)
    ex.web_test_rdx_gain_set(0);
  render(ex, Math.round(sampleRate * 0.25), false);
  ex.web_test_audio_flush();
  return ex;
}

function render(ex, frames, meter = null) {
  let peak = 0;
  for (let left = frames; left > 0;) {
    const n = Math.min(left, 1024);
    ex.web_render(n);
    if (meter) {
      const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), n);
      const r = new Float32Array(ex.memory.buffer, ex.web_out_r(), n);
      for (let i = 0; i < n; i++) {
        meter.add(l[i], r[i]);
        peak = Math.max(peak, Math.abs(l[i]), Math.abs(r[i]));
      }
    }
    left -= n;
  }
  return { rms: meter ? meter.rms() : 0, peak };
}

async function measureNote(visiblePreset, note) {
  const ex = await instanceForPreset(visiblePreset);
  ex.web_midi(0x90, note, velocity);
  const result = render(ex, Math.round(sampleRate * scanSeconds), kWeightedMeter());
  ex.web_midi(0x80, note, 0);
  return result;
}

const probe = await instanceForPreset(0);
const presetCount = probe.web_test_preset_total();
const absolutePresets = [];
for (let set = 0; set < probe.web_test_rdx_set_count(); set++) {
  for (let at = 0; at < probe.web_test_rdx_set_size(set); at++)
    absolutePresets.push(probe.web_test_rdx_set_preset(set, at));
}
if (absolutePresets.length !== presetCount)
  throw new Error(`visible preset map has ${absolutePresets.length} entries, expected ${presetCount}`);

const rows = [];
for (let visible = 0; visible < presetCount; visible++) {
  const readings = [];
  for (const note of notes)
    readings.push(await measureNote(visible, note));
  const rms = Math.sqrt(readings.reduce((sum, x) => sum + x.rms * x.rms, 0) / readings.length);
  const peak = Math.max(...readings.map(x => x.peak));
  const absolute = absolutePresets[visible];
  const name = cstr(probe.web_test_rdx_preset_name_byte, absolute);
  rows.push({ visible, absolute, name, rms, peak, noteRms: readings.map(x => x.rms) });
  console.log(`${String(visible).padStart(2)} ${name.padEnd(12)} RMS ${rms.toFixed(6)} peak ${peak.toFixed(4)}`);
}

const sorted = rows.map(x => x.rms).filter(x => x > 0).sort((a, b) => a - b);
const targetRms = sorted[Math.floor(sorted.length / 2)];
for (const row of rows) {
  const exact = row.rms > 0 ? 20 * Math.log10(targetRms / row.rms) : 24;
  row.gainDb = Math.max(-24, Math.min(24, Math.round(exact)));
  row.adjustedRms = row.rms * 10 ** (row.gainDb / 20);
}

const result = {
  method: `${measureAppliedGain ? "Calibrated" : "Uncalibrated"} maximum ITU-R BS.1770 K-weighted RMS over a continuous 300 ms window during the first second of separate C3/C4/C5 notes at MIDI velocity 100 and MASTER ADC 700`,
  weighting: "ITU-R BS.1770-5 K-weighting, De Man filters designed for 44100 Hz",
  windowSeconds,
  scanSeconds,
  sampleRate,
  velocity,
  targetRms,
  presets: rows,
};
fs.writeFileSync(outputPath, JSON.stringify(result, null, 2) + "\n");
console.log(`\nTarget RMS ${targetRms.toFixed(6)}; wrote ${outputPath}`);
console.log("Suggested gain dB by absolute patch index:");
console.log(rows.map(x => `${x.absolute}:${x.gainDb}`).join(" "));
