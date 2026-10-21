// SPDX-License-Identifier: GPL-3.0-or-later
// RDX regression: every imported factory voice sounds and releases, the engine
// never allocates more than eight voices, and a forced stop fades for 256 samples.
import fs from "fs";

const bytes = fs.readFileSync(process.argv[2] || "build/emu/felucca.wasm");
let failed = false;
const check = (name, ok) => {
  console.log(`rdx: ${name.padEnd(72)} ${ok ? "ok" : "FAIL"}`);
  failed ||= !ok;
};

async function boot() {
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const ex = instance.exports;
  ex._initialize();
  ex.web_nor_erase();
  ex.web_boot();
  const renderFrames = (frames, capture = false) => {
    let peak = 0, energy = 0, firstL = 0, firstR = 0, lastL = 0, lastR = 0, seen = false;
    const samplesL = capture ? new Float32Array(frames) : null;
    const samplesR = capture ? new Float32Array(frames) : null;
    let at = 0;
    for (let left = frames; left > 0;) {
      const n = Math.min(left, 128);
      ex.web_render(n);
      const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), n);
      const r = new Float32Array(ex.memory.buffer, ex.web_out_r(), n);
      for (let i = 0; i < n; i++) {
        if (!seen) { firstL = l[i]; firstR = r[i]; seen = true; }
        peak = Math.max(peak, Math.abs(l[i]), Math.abs(r[i]));
        energy += Math.abs(l[i]) + Math.abs(r[i]);
        lastL = l[i]; lastR = r[i];
        if (capture) { samplesL[at] = l[i]; samplesR[at] = r[i]; }
        at++;
      }
      left -= n;
    }
    return { peak, energy, firstL, firstR, lastL, lastR, samplesL, samplesR };
  };
  const render = (ms) => renderFrames(Math.round(ms * 44.1));
  render(800);
  return { ex, render, renderFrames };
}

const d = await boot();
const held = await boot();
check("engine reports an eight-voice limit", d.ex.web_test_engine_poly() === 8);
check("all 35 RDX factory voices are visible", d.ex.web_test_preset_total() === 35);
check("an old autosave cannot force DynaLead back to polyphony",
      d.ex.web_test_rdx_restore_stale_voice(7) === 1);

for (let p = 0; p < 35; p++) {
  d.ex.web_test_rdx_preset(p);
  held.ex.web_test_rdx_preset(p);
  d.ex.web_test_rdx_dry();
  held.ex.web_test_rdx_dry();
  d.ex.web_test_audio_flush();
  held.ex.web_test_audio_flush();
  d.ex.web_midi(0x90, 60, 100);
  held.ex.web_midi(0x90, 60, 100);
  const audio = d.renderFrames(128 * 120);
  held.renderFrames(128 * 120);
  d.ex.web_midi(0x80, 60, 0);
  const release = d.renderFrames(128, true);
  const continued = held.renderFrames(128, true);
  const heldForJoin = d.ex.web_test_active_voices() > 0;
  const onsetError = Math.max(Math.abs(release.firstL - continued.firstL),
                              Math.abs(release.firstR - continued.firstR));
  held.ex.web_midi(0x80, 60, 0);
  held.renderFrames(128);
  d.render(1200);
  held.render(1200);
  check(`factory voice ${String(p + 1).padStart(2, "0")} renders audio (peak ${audio.peak.toFixed(3)})`,
        audio.peak > 0.002 && audio.energy > 1);
  check(`factory voice ${String(p + 1).padStart(2, "0")} follows the held waveform at note-off (${onsetError.toFixed(5)})`,
        onsetError < 0.002);
  check(`factory voice ${String(p + 1).padStart(2, "0")} keeps its release de-click tail alive`, heldForJoin);
}

d.ex.web_test_rdx_preset(0);
held.ex.web_test_rdx_preset(0);
d.ex.web_test_rdx_dry();
held.ex.web_test_rdx_dry();
d.ex.web_test_audio_flush();
held.ex.web_test_audio_flush();
for (const note of [48, 52, 55]) {
  d.ex.web_midi(0x90, note, 100);
  held.ex.web_midi(0x90, note, 100);
}
d.renderFrames(128 * 120);
held.renderFrames(128 * 120);
for (const note of [48, 52, 55]) d.ex.web_midi(0x80, note, 0);
const chordRelease = d.renderFrames(128);
const chordHeld = held.renderFrames(128);
const chordOnsetError = Math.max(Math.abs(chordRelease.firstL - chordHeld.firstL),
                                 Math.abs(chordRelease.firstR - chordHeld.firstR));
check(`three-note simultaneous release follows the held chord (${chordOnsetError.toFixed(5)})`,
      chordOnsetError < 0.006);
check("three simultaneous releases keep all de-click tails alive", d.ex.web_test_active_voices() === 3);
for (const note of [48, 52, 55]) held.ex.web_midi(0x80, note, 0);
d.render(1500);
held.render(1500);

d.ex.web_test_rdx_preset(0);
const stressNotes = [48, 50, 52, 53, 55, 57, 59, 60, 64];
for (const note of stressNotes) d.ex.web_midi(0x90, note, 100);
d.render(100);
check("nine held notes are capped at eight active voices", d.ex.web_test_active_voices() === 8);
for (const note of stressNotes) d.ex.web_midi(0x80, note, 0);
d.render(2500);
check("released voices eventually become inactive", d.ex.web_test_active_voices() === 0);

d.ex.web_test_rdx_preset(0);
d.ex.web_test_rdx_voice_mode_set(1);          // native Reface MONO-FULL
d.ex.web_midi(0x90, 60, 100);
d.renderFrames(128);
check("RDX MONO-FULL uses one native Reface mono voice",
      d.ex.web_test_active_voices() === 1 && d.ex.web_test_rdx_voice_mode() === 1);
d.ex.web_midi(0x90, 72, 100);                 // overlap retunes the same voice
d.renderFrames(128);
check("RDX MONO-FULL overlap reuses the live voice without a note ladder",
      d.ex.web_test_active_voices() === 1);
d.ex.web_midi(0x80, 72, 0); d.ex.web_midi(0x80, 60, 0); d.render(1800);

d.ex.web_test_rdx_preset(0);
d.ex.web_test_shed_reset();
for (const note of [48, 52, 55, 60]) d.ex.web_midi(0x90, note, 100);
d.renderFrames(64);
d.ex.web_test_shed(0);                       // two >85% halves, but no DMA miss
check("the 85 percent warning preserves all four held RDX notes",
      d.ex.web_test_active_voices() === 4 && d.ex.web_test_fading_voices() === 0);
d.ex.web_test_shed(1);                       // a real AUDIO LATE may sacrifice one
const hardShedCount = d.ex.web_test_shed(1); // the first fade is still running
check("a hard shed fades only one held note until its 256 samples finish",
      hardShedCount === 1 && d.ex.web_test_fading_voices() === 1);
for (const note of [48, 52, 55, 60]) d.ex.web_midi(0x80, note, 0);
d.render(1800);

d.ex.web_test_rdx_preset(0);
d.ex.web_test_rdx_dry();
d.ex.web_midi(0x90, 60, 100);
d.render(200);
d.ex.web_test_audio_flush();
check("forced stop finds an active RDX voice", d.ex.web_test_rdx_force_stop() === 1);
check("forced stop starts a 256-sample RDX fade", d.ex.web_test_rdx_force_tail() === 256);
d.renderFrames(128);
check("forced-stop fade has 128 samples left after one audio half",
      d.ex.web_test_rdx_force_tail() === 128 && d.ex.web_test_active_voices() === 1);
d.renderFrames(128);
check("forced-stop voice stays alive through all 256 fade samples",
      d.ex.web_test_rdx_force_tail() === 0 && d.ex.web_test_active_voices() === 1);
d.renderFrames(32);
check("forced-stop voice retires on the block after its fade", d.ex.web_test_active_voices() === 0);

const sxSend = (bytes) => {
  for (const b of bytes) d.ex.web_test_rdx_sysex_byte(b);
};
const sxBulk = (ah, am, al, data = [], corrupt = false) => {
  const count = data.length + 4;
  const body = [0x05, ah, am, al, ...data.map((v) => v & 0x7f)];
  let checksum = (-body.reduce((a, b) => a + b, 0)) & 0x7f;
  if (corrupt) checksum = (checksum + 1) & 0x7f;
  sxSend([0xf0, 0x43, 0x00, 0x7f, 0x1c, count >> 7, count & 0x7f,
          ...body, checksum, 0xf7]);
};
const patchBytes = () => Uint8Array.from({length: 150}, (_, i) => d.ex.web_test_rdx_patch_byte(i));
const importBlocks = (common, ops = []) => {
  sxBulk(0x0e, 0x0f, 0x00);
  if (common) sxBulk(0x30, 0x00, 0x00, common);
  for (const [op, data] of ops) sxBulk(0x31, op, 0x00, data);
  sxBulk(0x0f, 0x0f, 0x00);
};

d.ex.web_test_rdx_preset(0);
let imported = patchBytes();
const common = Array.from(imported.slice(0, 38));
const op0 = Array.from(imported.slice(38, 66));
const oldMode = common[13];
common[13] = 2;                         // MONO LEGATO
common[14] = 96;                        // RDX portamento time
common[17] = 1;                         // triangle LFO
common[18] = 100;
common[20] = 48;                        // pitch modulation depth
common[21] = common[22] = common[23] = common[24] = 127;
common[25] = 100; common[26] = 80; common[27] = 64; common[28] = 64;
op0[16] = 1;                            // PEG enabled
op0[15] = 1;                            // LFO PM enabled
importBlocks(common, [[0, op0]]);
check("valid Yamaha RDX bulk blocks replace common/operator data",
      d.ex.web_test_rdx_patch_byte(13) === 2 && d.ex.web_test_rdx_patch_byte(54) === 1);
check("SysEx MONO LEGATO selects the native Reface mono-legato mode",
      d.ex.web_test_rdx_voice_mode() === 2);

const beforeBad = d.ex.web_test_rdx_patch_byte(13);
const badCommon = [...common]; badCommon[13] = oldMode;
sxBulk(0x30, 0x00, 0x00, badCommon, true);
check("bad Yamaha bulk checksum is rejected", d.ex.web_test_rdx_patch_byte(13) === beforeBad);

sxSend([0xf0, 0x43, 0x20, 0x7f, 0x1c, 0x05, 0x0e, 0x0f, 0x00, 0xf7]);
const tx = Array.from({length: d.ex.web_test_rdx_tx_size()}, (_, i) => d.ex.web_test_rdx_tx_byte(i));
check("Reface DX bulk request returns header, common, four operators and footer",
      tx.length === 241 && tx.filter((b) => b === 0xf0).length === 7 &&
      tx.filter((b) => b === 0xf7).length === 7 && tx.includes(0x30) && tx.includes(0x31));

d.ex.web_test_rdx_dry();
d.ex.web_test_audio_flush();
d.ex.web_midi(0x90, 60, 100);
d.renderFrames(64);
const pegStart = 64 << 16;
check("RDX pitch EG advances from its 64-semitone centre",
      Math.abs(d.ex.web_test_rdx_peg() - pegStart) > 100);
check("RDX LFO phase advances at the patch rate", d.ex.web_test_rdx_lfo_phase() !== 0);
d.ex.web_test_audio_flush();
d.ex.web_midi(0x90, 72, 100);            // overlap: same voice, glide to C5
d.renderFrames(64);
const glidePitch = d.ex.web_test_rdx_pitch();
const glideTarget = d.ex.web_test_rdx_pitch_target();
check("overlapping mono note uses one voice and starts RDX portamento",
      d.ex.web_test_active_voices() === 1 && d.ex.web_test_rdx_porta_left() > 0 &&
      glidePitch > (60 << 16) && glidePitch < glideTarget && glideTarget === (72 << 16));
d.ex.web_midi(0x80, 72, 0); d.ex.web_midi(0x80, 60, 0); d.render(1800);

// Positive-linear right keyboard scaling: C8 must have a larger operator
// gain than the breakpoint C4, while every other patch byte stays identical.
const scaledCommon = [...common]; scaledCommon[13] = 0; scaledCommon[14] = 0;
const scaledOp0 = [...op0]; scaledOp0[11] = 127; scaledOp0[13] = 3;
importBlocks(scaledCommon, [[0, scaledOp0]]);
d.renderFrames(64);                      // consume the SysEx-triggered panic
d.ex.web_test_audio_flush();
d.ex.web_midi(0x90, 60, 100); d.renderFrames(32);
const gain60 = d.ex.web_test_rdx_op_gain(0);
d.ex.web_midi(0x80, 60, 0); d.render(1800);
d.ex.web_test_audio_flush();
d.ex.web_midi(0x90, 108, 100); d.renderFrames(32);
const gain108 = d.ex.web_test_rdx_op_gain(0);
check("positive right keyboard level scaling raises high-note operator gain", gain108 > gain60);
d.ex.web_midi(0x80, 108, 0); d.render(1800);

console.log(failed ? "rdx: FAILED" : "rdx: all ok");
process.exit(failed ? 1 : 0);
