// SPDX-License-Identifier: GPL-3.0-or-later
// RDX regression: every imported factory voice sounds and releases, the engine
// never allocates more than eight voices, and a forced stop fades for 256 samples.
import fs from "fs";

const bytes = fs.readFileSync(process.argv[2] || "build/emu/felucca.wasm");
const versionHeader = fs.readFileSync("firmware/src/rdx_version.h", "utf8");
const uiDrawSource = fs.readFileSync("firmware/src/ui_draw.c", "utf8");
const versionMatch = versionHeader.match(/^#define RDX_VERSION "([^"]+)"$/m);
if (!versionMatch) throw new Error("Cannot read RDX_VERSION from firmware/src/rdx_version.h");
const expectedFirmwareVersion = `RDX ${versionMatch[1]}`;
let failed = false;
const check = (name, ok) => {
  console.log(`rdx: ${name.padEnd(72)} ${ok ? "ok" : "FAIL"}`);
  failed ||= !ok;
};
const readIndexedString = (ex, name, ...prefix) => {
  let value = "";
  for (let i = 0; i < 32; i++) {
    const c = ex[name](...prefix, i);
    if (!c) break;
    value += String.fromCharCode(c);
  }
  return value;
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
const effected = await boot();
const fxab = await boot();
{
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const migration = instance.exports;
  migration._initialize();
  check("foreign Felucca data is cleared while panel calibration is retained",
        migration.web_test_rdx_storage_migration(0) === 15);
  check("legacy RDX data is promoted to the new owner marker without clearing",
        migration.web_test_rdx_storage_migration(1) === 15);
  check("current RDX data survives an ordinary firmware update unchanged",
        migration.web_test_rdx_storage_migration(2) === 15);
}
check("engine reports an eight-voice limit", d.ex.web_test_engine_poly() === 8);
check("RDX firmware has one runtime track", d.ex.web_test_track_count() === 1);
check("AUDIO LATE remains an INFO diagnostic without a giant popup",
      !uiDrawSource.includes("draw_audio_late_notice"));
check("RDX editor exposes only canonical engine slot 12",
      d.ex.web_test_visible_engine_count() === 1 && d.ex.web_test_first_visible_engine() === 12);
check("32 Factory and 21 Selected RDX voices are visible", d.ex.web_test_preset_total() === 53);
let editorSlotsOk = true;
const editorSlotNames = new Set();
for (let shown = 0; shown < 53; shown++) {
  const internal = d.ex.web_test_rdx_visible_preset(shown) >>> 0;
  editorSlotsOk &&= internal !== 0xFFFFFFFF && (d.ex.web_test_rdx_visible_rank(internal) >>> 0) === shown;
  const name = readIndexedString(d.ex, "web_test_rdx_preset_name_byte", internal);
  editorSlotsOk &&= name.length > 0 && !editorSlotNames.has(name);
  editorSlotNames.add(name);
}
check("Editor device presets map 53 unique visible slots bidirectionally", editorSlotsOk);
check("RDX TYPE defaults to the optimized renderer", d.ex.web_test_rdx_type() === 1);
d.ex.web_test_rdx_type_set(0);
check("System RDX TYPE can switch back to the original renderer", d.ex.web_test_rdx_type() === 0);
d.ex.web_test_rdx_type_set(1);
check("System RDX TYPE can switch to the optimized renderer", d.ex.web_test_rdx_type() === 1);
check("RDX HOME no longer exposes the obsolete CLOCK page",
      d.ex.web_test_rdx_home_has_no_clock() === 1);
check("RDX FX double tap no longer locks the MACROS/EFFECTS layer",
      d.ex.web_test_rdx_fx_double_tap_disabled() === 1);
check("PHS and FLG MODEL default to the fixed-point PicoFace RDX algorithms",
      d.ex.web_test_rdx_phaser_model() === 1 && d.ex.web_test_rdx_flanger_model() === 1);
d.ex.web_test_rdx_phaser_model_set(0);
d.ex.web_test_rdx_flanger_model_set(0);
check("PHS and FLG MODEL retain the original Felucca algorithms for A/B tests",
      d.ex.web_test_rdx_phaser_model() === 0 && d.ex.web_test_rdx_flanger_model() === 0);
d.ex.web_test_rdx_phaser_model_set(1);
d.ex.web_test_rdx_flanger_model_set(1);
check("RDX Flanger uses the retained THK-W linear 30..100 percent wet macro",
      d.ex.web_test_rdx_flanger_wet_q15(0) === 0 &&
      d.ex.web_test_rdx_flanger_wet_q15(1) === 9830 &&
      d.ex.web_test_rdx_flanger_wet_q15(64) === 21299 &&
      d.ex.web_test_rdx_flanger_wet_q15(127) === 32768);
check("RDX Flanger DEPTH raises damped feedback from 0.30 to 0.75",
      d.ex.web_test_rdx_flanger_feedback_q15(0) === 0 &&
      d.ex.web_test_rdx_flanger_feedback_q15(1) === 9830 &&
      d.ex.web_test_rdx_flanger_feedback_q15(127) === 24576);
check("RDX Flanger RATE follows the 0.05..6.5 Hz quadratic phase curve",
      d.ex.web_test_rdx_flanger_speed_inc(0) === 4869 &&
      d.ex.web_test_rdx_flanger_speed_inc(127) === 633900);
check("RDX Flanger squares the unipolar sine before delay modulation",
      d.ex.web_test_rdx_flanger_exp_mod(-32768) === 0 &&
      d.ex.web_test_rdx_flanger_exp_mod(0) === 16384 &&
      d.ex.web_test_rdx_flanger_exp_mod(32767) === 65534);
check("RDX Flanger retains THK-W's 1.5 ms base plus five-ms DEPTH excursion",
      d.ex.web_test_rdx_flanger_max_delay_q16(0) === 0 &&
      d.ex.web_test_rdx_flanger_max_delay_q16(64) === 11617442 &&
      d.ex.web_test_rdx_flanger_max_delay_q16(127) === 18785894);
check("MIDI velocity curve defaults to HARD 1", d.ex.web_test_velocity_curve() === 3);
const velocityAt64 = [96, 80, 64, 48, 32];
let velocityCurvesOk = true;
let velocityFractionsOk = true;
for (let curve = 0; curve < velocityAt64.length; curve++) {
  d.ex.web_test_velocity_curve_set(curve);
  velocityCurvesOk &&= d.ex.web_test_velocity_curve() === curve;
  velocityCurvesOk &&= d.ex.web_test_velocity_map(64) === velocityAt64[curve];
  let previous = 0;
  let previousQ8 = 0;
  let fractional = false;
  for (let velocity = 1; velocity <= 127; velocity++) {
    const mapped = d.ex.web_test_velocity_map(velocity);
    const mappedQ8 = d.ex.web_test_velocity_map_q8(velocity);
    velocityCurvesOk &&= mapped >= previous && mapped >= 1 && mapped <= 127;
    velocityFractionsOk &&= mappedQ8 > previousQ8 && mappedQ8 <= 127 * 256;
    fractional ||= (mappedQ8 & 255) !== 0;
    previous = mapped;
    previousQ8 = mappedQ8;
  }
  velocityCurvesOk &&= previous === 127;
  velocityFractionsOk &&= previousQ8 === 127 * 256;
  if (curve !== 2) velocityFractionsOk &&= fractional;
}
check("all five MIDI velocity curves are monotonic with the expected shapes", velocityCurvesOk);
check("curved MIDI velocity retains distinct Q8.8 fractional values", velocityFractionsOk);
d.ex.web_test_velocity_curve_set(0);
const soft2a = d.ex.web_test_velocity_map_q8(69);
const soft2b = d.ex.web_test_velocity_map_q8(70);
check("RDX interpolates two fractional velocities that round to the same 7-bit value",
      d.ex.web_test_velocity_map(69) === d.ex.web_test_velocity_map(70) &&
      soft2a !== soft2b &&
      d.ex.web_test_rdx_velocity_gain(soft2a) !== d.ex.web_test_rdx_velocity_gain(soft2b));
d.ex.web_test_velocity_curve_set(3);
const cstr = (fn, index) => {
  let s = "";
  for (let i = 0; i < 32; i++) {
    const c = fn(index, i);
    if (!c) break;
    s += String.fromCharCode(c);
  }
  return s;
};
let firmwareVersion = "";
for (let i = 0; i < 24; i++) {
  const c = d.ex.web_test_version_byte(i);
  if (!c) break;
  firmwareVersion += String.fromCharCode(c);
}
check(`RDX firmware identifies itself as Felucca ${expectedFirmwareVersion}`,
      firmwareVersion === expectedFirmwareVersion);
check("HOME exposes MODE, PORTA, GAIN and Trans. in that order",
      d.ex.web_test_rdx_home_layout() === 1 &&
      cstr(d.ex.web_test_rdx_home_label_byte, 0) === "MODE" &&
      cstr(d.ex.web_test_rdx_home_label_byte, 1) === "PORTA" &&
      cstr(d.ex.web_test_rdx_home_label_byte, 2) === "GAIN" &&
      cstr(d.ex.web_test_rdx_home_label_byte, 3) === "Trans.");
check("HOME knob 4 is the Trans. master transpose control",
      d.ex.web_test_rdx_home_knob4_is_transpose() === 1 &&
      cstr(d.ex.web_test_rdx_home_label_byte, 3) === "Trans." &&
      d.ex.web_test_rdx_home_icon_is_transpose() === 1);
d.ex.web_test_rdx_voice_mode_set(1);
check("RDX mode names retain the full Reface wording",
      cstr(d.ex.web_test_rdx_home_value_byte, 0) === "MonoFull");
d.ex.web_test_rdx_voice_mode_set(2);
check("RDX legato mode is shown as MonoLeg.",
      cstr(d.ex.web_test_rdx_home_value_byte, 0) === "MonoLeg.");
d.ex.web_test_rdx_porta_set(127);
check("HOME PORTA covers the complete 0..127 range",
      cstr(d.ex.web_test_rdx_home_value_byte, 1) === "127");
d.ex.web_test_rdx_gain_set(6);
check("HOME GAIN is a signed -24..+24 dB control",
      d.ex.web_test_rdx_gain() === 6 && cstr(d.ex.web_test_rdx_home_value_byte, 2) === "+6");
let gainPrecisionOk = true;
const gainSamples = [-32767, -4097, -257, -17, -1, 1, 17, 257, 4097, 32767];
for (let gain = -24; gain <= 24; gain++) {
  for (const sample of gainSamples) {
    const actual = d.ex.web_test_rdx_gain_sample(sample, gain);
    const expected = Math.round(sample * 10 ** (gain / 20));
    gainPrecisionOk &&= Math.abs(actual - expected) <= 1;
    if (gain === 1) gainPrecisionOk &&= actual !== 0;
  }
}
check("GAIN preserves low-level samples and stays linear over -24..+24 dB", gainPrecisionOk);
d.ex.web_test_rdx_gain_set(0);
check("preset loads install their measured loudness calibration",
      (d.ex.web_test_rdx_preset(0), d.ex.web_test_rdx_gain()) === 4 &&
      (d.ex.web_test_rdx_preset(51), d.ex.web_test_rdx_gain()) === -6);
check("old autosaves migrate from MLVL to the preset GAIN default",
      d.ex.web_test_rdx_restore_gain(0, -17, 0) === 4);
check("0.6.8/0.6.9 autosaves migrate to the new perceptual GAIN default",
      d.ex.web_test_rdx_restore_previous_gain(0, -3) === 4);
check("new autosaves preserve a user-edited GAIN",
      d.ex.web_test_rdx_restore_gain(0, -3, 1) === -3);
check("RDX header is empty at octave zero and shows octave only when shifted",
      d.ex.web_test_rdx_header_center_ink(0) === 0 &&
      d.ex.web_test_rdx_header_center_ink(1) > 0 &&
      d.ex.web_test_rdx_header_center_ink(-1) > 0);
check("master transpose and octave add once and clamp to MIDI note boundaries",
      d.ex.web_test_rdx_master_note(60, 0, 0) === 60 &&
      d.ex.web_test_rdx_master_note(60, 24, 3) === 120 &&
      d.ex.web_test_rdx_master_note(120, 24, 3) === 127 &&
      d.ex.web_test_rdx_master_note(5, -24, -3) === 0);
check("sound-set registry contains Factory and Selected",
      d.ex.web_test_rdx_set_count() === 2 &&
      cstr(d.ex.web_test_rdx_set_name_byte, 0) === "Factory" &&
      cstr(d.ex.web_test_rdx_set_name_byte, 1) === "Selected");
const factoryOrder = [6, 7, 3, 4, 5, ...Array.from({length: 27}, (_, i) => i + 8)];
check("Factory starts with LegendEP and DynaLead, then keeps the original order",
      d.ex.web_test_rdx_set_size(0) === factoryOrder.length &&
      factoryOrder.every((preset, at) => d.ex.web_test_rdx_set_preset(0, at) === preset) &&
      cstr(d.ex.web_test_rdx_preset_name_byte, 6) === "LegendEP" &&
      cstr(d.ex.web_test_rdx_preset_name_byte, 7) === "DynaLead");
check("RDX release output is +6 dB and fresh installs default to VIOLET",
      d.ex.web_test_rdx_master_mix_shift() === 1 &&
      d.ex.web_test_rdx_default_palette() === 4);
check("Selected set has 21 imported SysEx entries",
      d.ex.web_test_rdx_set_size(1) === 21 &&
      d.ex.web_test_rdx_set_preset(1, 0) === 35 &&
      cstr(d.ex.web_test_rdx_preset_name_byte, 35) === "Ph.D Brass" &&
      cstr(d.ex.web_test_rdx_preset_name_byte, 55) === "Transistor");
check("PRESETS LIST separates Factory and Selected while TYPE combines with it",
      d.ex.web_test_rdx_filter_total(1, 0) === 32 &&
      d.ex.web_test_rdx_filter_total(2, 0) === 21 &&
      d.ex.web_test_rdx_filter_total(1, 1) === 4 &&       // Factory BASS
      d.ex.web_test_rdx_filter_total(1, 3) === 6);       // Factory PAD
check("Factory preset types include dedicated keys, lead, organ, brass and percussion tags",
      d.ex.web_test_rdx_preset_category(6) === 5 &&      // LegendEP: KEYS
      d.ex.web_test_rdx_preset_category(7) === 2 &&      // DynaLead: LEAD
      d.ex.web_test_rdx_preset_category(25) === 9 &&     // CheezOrgan: ORGAN
      d.ex.web_test_rdx_preset_category(26) === 10 &&    // FM_Brass: BRASS
      d.ex.web_test_rdx_preset_category(24) === 11);     // Marimba: PERC
check("Selected preset types use the reviewed compact taxonomy without VOCAL",
      d.ex.web_test_rdx_filter_total(2, 2) === 3 &&      // Selected LEAD
      d.ex.web_test_rdx_filter_total(2, 3) === 6 &&      // Selected PAD
      d.ex.web_test_rdx_filter_total(2, 5) === 5 &&      // Selected KEYS
      d.ex.web_test_rdx_filter_total(2, 10) === 4 &&     // Selected BRASS
      d.ex.web_test_rdx_preset_category(42) === 2 &&     // StrgMachin: LEAD
      d.ex.web_test_rdx_preset_category(48) === 3 &&     // 5th.FM: PAD
      d.ex.web_test_rdx_preset_category(49) === 2 &&     // Polific: LEAD
      d.ex.web_test_rdx_preset_category(51) === 3 &&     // DX Singer: PAD
      d.ex.web_test_rdx_preset_category(55) === 2);      // Transistor: LEAD
check("fixed-unity Flanger hides its SEND card instead of presenting a fake control",
      uiDrawSource.includes("fixed at unity: no fake editable SEND card"));
check("RDX EDIT footer exposes ALGORITHM, row selection and next-page controls",
      uiDrawSource.includes('KC_ALGO, "CHANGE"') &&
      uiDrawSource.includes('KC_SELECT, "ROW"') &&
      uiDrawSource.includes('KC_EDIT, "NEXT"'));
d.ex.web_test_rdx_custom_name_load();
check("a librarian audition publishes its live SysEx voice name on HOME",
      cstr(d.ex.web_test_rdx_custom_name_byte, 0) === "Audition");
const fmBrassAt = Array.from({length: d.ex.web_test_rdx_set_size(0)}, (_, i) => i)
  .find(i => d.ex.web_test_rdx_set_preset(0, i) === 26);
d.ex.web_test_rdx_preset(fmBrassAt);
check("Factory FM_Brass restores Yamaha bulk Chorus and OP1 velocity bytes",
      d.ex.web_test_rdx_patch_byte(32) === 3 && d.ex.web_test_rdx_patch_byte(55) === 72);
check("FM_Brass PEG Rate1 reaches its -2-semitone target in a four-frame segment",
      d.ex.web_test_rdx_fmbrass_peg_segments() === 0x204);
d.ex.web_test_rdx_dry();
d.ex.web_test_audio_flush();
d.ex.web_midi(0x90, 60, 100);
d.renderFrames(640);
const fmBrassModAttack = d.ex.web_test_rdx_op_env_gain(1);
check(`FM_Brass modulator attack rises linearly in audible gain (${fmBrassModAttack})`,
      fmBrassModAttack > 14000 && fmBrassModAttack < 18000);
d.ex.web_midi(0x80, 60, 0); d.render(1800);
check("FX hides unfinished GLOBAL FX and opens FX, FX1, FX2 and SLICER in order",
      d.ex.web_test_rdx_fx_page_count() === 4 &&
      ["FX", "FX1", "FX2", "SLICER"].every((name, i) =>
        cstr(d.ex.web_test_rdx_fx_page_name_byte, i) === name));
d.ex.web_test_rdx_preset(4);                          // MotionPad: CHORUS -> DELAY
check("preset FX bytes initialise the live serial type/send controls",
      d.ex.web_test_rdx_fx_live(0, 0) === 3 &&
      d.ex.web_test_rdx_fx_live(0, 1) === 15 &&
      d.ex.web_test_rdx_fx_live(1, 0) === 6);
check("FX1 and FX2 parameter labels follow their selected effect types",
      cstr((slot, pos) => d.ex.web_test_rdx_fx_param_label_byte(0, slot, pos), 0) === "DEPTH" &&
      cstr((slot, pos) => d.ex.web_test_rdx_fx_param_label_byte(0, slot, pos), 1) === "RATE" &&
      cstr((slot, pos) => d.ex.web_test_rdx_fx_param_label_byte(1, slot, pos), 2) === "FDBK" &&
      cstr((slot, pos) => d.ex.web_test_rdx_fx_param_label_byte(1, slot, pos), 3) === "COLOR");
check("an old autosave cannot force DynaLead back to polyphony",
      d.ex.web_test_rdx_restore_stale_voice(1) === 1);
const restoredLegend = d.ex.web_test_rdx_restore_stale_slot();
check(`an old autosave PATCH mismatch reloads the current LegendEP data (0x${restoredLegend.toString(16)})`,
      restoredLegend === ((6 << 16) | (20 << 8) | 62));

d.ex.web_test_rdx_preset(2);                          // DigiChord: FLANGER -> REVERB 59, 20
check("RDX Reverb Depth 59 maps to rounded Depth / 2.5 send 24",
      d.ex.web_test_rdx_reverb_send(1) === 24);
check("RDX Reverb defaults to the Clouds-based HALL with 45% DAMP",
      d.ex.web_test_rdx_reverb_model(1) === 2 &&
      d.ex.web_test_rdx_reverb_time(1) === 20 && d.ex.web_test_rdx_reverb_live_damp(1) === 57);
const reverbSendBeforeModelAB = d.ex.web_test_rdx_fx_live(1, 1);
const reverbTimeBeforeModelAB = d.ex.web_test_rdx_fx_live(1, 4);
d.ex.web_test_rdx_reverb_model_set(1, 0);
d.ex.web_test_rdx_reverb_model_set(1, 2);
check("switching Reverb to HALL writes only its 45% DAMP baseline",
      d.ex.web_test_rdx_reverb_model(1) === 2 &&
      d.ex.web_test_rdx_reverb_live_damp(1) === 57 &&
      d.ex.web_test_rdx_fx_live(1, 1) === reverbSendBeforeModelAB &&
      d.ex.web_test_rdx_fx_live(1, 4) === reverbTimeBeforeModelAB);
check("retired reverb model ids restore as the retained HALL model",
      d.ex.web_test_rdx_restore_legacy_clouds() === ((2 << 8) | 57));
check("HALL Q14 compression keeps two extra delay-line bits symmetrically",
      d.ex.web_test_rdx_clouds_hall_q14(1) === 0 &&
      d.ex.web_test_rdx_clouds_hall_q14(-1) === 0 &&
      d.ex.web_test_rdx_clouds_hall_q14(7) === 3 &&
      d.ex.web_test_rdx_clouds_hall_q14(-7) === -3);
check("HALL feedback multiplication rounds positive and negative values symmetrically",
      d.ex.web_test_rdx_clouds_hall_mul(7, 20480) === 4 &&
      d.ex.web_test_rdx_clouds_hall_mul(-7, 20480) === -4);
let hallMultiplyExact = true;
for (const value of [-262143, -65537, -32769, -32768, -257, -9, -8, -7, -1,
                       0, 1, 7, 8, 9, 257, 32767, 32768, 65537, 262136]) {
  for (const q15 of [-20480, -1, 0, 1, 11469, 20480, 32700, 32767]) {
    const expected = Number((BigInt(value) * BigInt(q15)) / 32768n);
    hallMultiplyExact &&= d.ex.web_test_rdx_clouds_hall_mul(value, q15) === expected;
  }
}
check("HALL branchless multiply remains exact over signed feedback cases", hallMultiplyExact);
check("RDX Reverb retains the 1.5x native-reverb TIME fallback mapping",
      d.ex.web_test_rdx_reverb_mapped_size(1) === 30);
check("RDX Delay follows PicoFace's 0..50 percent feedback macro",
      d.ex.web_test_rdx_delay_feedback(0) === 0 &&
      d.ex.web_test_rdx_delay_feedback(1) === 0 &&
      d.ex.web_test_rdx_delay_feedback(127) === 64);
check("RDX Delay wet mix starts near 10 percent and reaches 25 percent at 1.0x",
      d.ex.web_test_rdx_delay_mix_q15(0, 10) === 0 &&
      d.ex.web_test_rdx_delay_mix_q15(1, 10) === 3316 &&
      d.ex.web_test_rdx_delay_mix_q15(127, 10) === 8192);
check("RDX Delay test ratio covers 0.1x through 5.0x and clips at unity",
      d.ex.web_test_rdx_delay_mix_q15(1, 1) === 332 &&
      d.ex.web_test_rdx_delay_mix_q15(127, 50) === 32767);
check("RDX Delay keeps Felucca's warm COLOR 70 default",
      d.ex.web_test_rdx_delay_color() === 70);
check("RDX Delay TIME uses the measured full-rate 11.6 ms to 1201 ms range",
      d.ex.web_test_rdx_delay_samples(0) === 512 &&
      d.ex.web_test_rdx_delay_samples(127) === 52964);
d.ex.web_test_rdx_preset(1);                          // DynaLead: Delay Depth 1, Time 74
check("DynaLead Delay uses 1.0x ratio, audible base mix and zero feedback",
      d.ex.web_test_rdx_fx_live(0, 0) === 6 &&
      d.ex.web_test_rdx_fx_live(0, 1) === 10 &&
      d.ex.web_test_rdx_fx_live(0, 2) === 1 &&
      d.ex.web_test_rdx_fx_live(0, 3) === 74 &&
      d.ex.web_test_rdx_fx_live(0, 4) === 0 &&
      d.ex.web_test_rdx_fx_live(0, 5) === 70);
check("Delay SEND card exposes the temporary 0.1x..5.0x ratio with 1.0x default",
      d.ex.web_test_rdx_fx_send_desc(0, 0) === 14 &&
      d.ex.web_test_rdx_fx_send_desc(0, 1) === 1 &&
      d.ex.web_test_rdx_fx_send_desc(0, 2) === 50 &&
      d.ex.web_test_rdx_fx_send_desc(0, 3) === 10);
check("RDX Distortion uses PicoFaceDX's linear 300 Hz to 8 kHz one-pole TONE",
      d.ex.web_test_rdx_dist_tone_q16(0) === 2742 &&
      d.ex.web_test_rdx_dist_tone_q16(42) === 21849 &&
      d.ex.web_test_rdx_dist_tone_q16(127) === 44572);
check("RDX Distortion uses PicoFaceDX's quadratic 1x to 25x DRIVE curve",
      d.ex.web_test_rdx_dist_drive_q12(0) === 4096 &&
      d.ex.web_test_rdx_dist_drive_q12(5) === 4248 &&
      d.ex.web_test_rdx_dist_drive_q12(127) === 102400);
check("RDX Distortion level is unity at 0, gently raised at 5, and +30% at 100",
      d.ex.web_test_rdx_dist_target_q16(0) === 65536 &&
      d.ex.web_test_rdx_dist_target_q16(5) === 66519 &&
      d.ex.web_test_rdx_dist_target_q16(100) === 85197 &&
      d.ex.web_test_rdx_dist_target_q16(127) === 85197);
d.ex.web_test_rdx_preset(2);                          // DigiChord: TIME 20 -> native fallback SIZE 30
d.ex.web_midi(0x90, 60, 100);
d.renderFrames(128);
check("a sounding RDX patch publishes the native-reverb fallback SIZE diagnostic",
      d.ex.web_test_rdx_reverb_size() === 30);
d.ex.web_midi(0x80, 60, 0);
d.render(1200);

for (let p = 0; p < 53; p++) {
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
  check(`RDX voice ${String(p + 1).padStart(2, "0")} renders audio (peak ${audio.peak.toFixed(3)})`,
        audio.peak > 0.002 && audio.energy > 1);
  check(`RDX voice ${String(p + 1).padStart(2, "0")} follows the held waveform at note-off (${onsetError.toFixed(5)})`,
        onsetError < 0.002);
  check(`RDX voice ${String(p + 1).padStart(2, "0")} keeps its release de-click tail alive`, heldForJoin);
}

// One serial-chain example for every Reface effect type.  These run with the
// native patch FX enabled; the engine-only loop above deliberately bypasses
// them so a dark effect cannot masquerade as a silent oscillator patch.
for (const [p, label] of [[2, "flanger/reverb"], [3, "distortion/delay"],
                          [4, "chorus/delay"], [8, "phaser/chorus"],
                          [17, "flanger/delay"], [32, "reverb/reverb"],
                          [36, "distortion/touch wah"], [42, "reverb/reverb"],
                          [51, "reverb/distortion"]]) {
  effected.ex.web_test_rdx_preset(p);
  effected.ex.web_test_audio_flush();
  effected.ex.web_midi(0x90, 60, 100);
  const audio = effected.renderFrames(128 * 120);
  if (p === 4) {
    check("RDX Chorus maps MotionPad Depth 30 to SEND 15",
          effected.ex.web_test_rdx_chorus_send() === 15);
    check("RDX Chorus halves MotionPad DEPTH 30 while keeping RATE 74",
          effected.ex.web_test_rdx_chorus_depth() === 15 &&
          effected.ex.web_test_rdx_chorus_rate() === 74);
  }
  effected.ex.web_midi(0x80, 60, 0);
  effected.render(1500);
  check(`native ${label} effect routing renders audio`, audio.peak > 0.002 && audio.energy > 1);
}
effected.ex.web_test_rdx_preset(36);                    // WahClav.: DIST -> TOUCH WAH
check("Touch Wah is detected on the current RDX patch", effected.ex.web_test_rdx_wah_active() === 1);
const wah0 = effected.ex.web_test_rdx_wah_pos();
check("Touch Wah manual centre follows SELECT control",
      effected.ex.web_test_rdx_wah_turn(9) === Math.min(127, wah0 + 9));

const sideEnergy = (audio) => {
  let e = 0;
  for (let i = 0; i < audio.samplesL.length; i++) e += Math.abs(audio.samplesL[i] - audio.samplesR[i]);
  return e;
};
fxab.ex.web_test_rdx_preset(0);
fxab.ex.web_test_rdx_fx_set(0, 4, 96, 64);             // Flanger only
fxab.ex.web_test_rdx_fx_set(1, 0, 0, 0);
check("RDX Flanger fixes the outer serial SEND at 100 percent",
      fxab.ex.web_test_rdx_fx_live(0, 1) === 127 &&
      fxab.ex.web_test_rdx_fx_send_desc(0, 1) === 127 &&
      fxab.ex.web_test_rdx_fx_send_desc(0, 2) === 127 &&
      fxab.ex.web_test_rdx_fx_send_desc(0, 3) === 127);
fxab.ex.web_test_rdx_flanger_model_set(0);
fxab.ex.web_midi(0x90, 60, 100);
const oldFlanger = fxab.renderFrames(128 * 16, true);
fxab.ex.web_midi(0x80, 60, 0); fxab.render(1500);
fxab.ex.web_test_rdx_flanger_model_set(1);
fxab.ex.web_midi(0x90, 60, 100);
const rdxFlanger = fxab.renderFrames(128 * 16, true);
fxab.ex.web_midi(0x80, 60, 0); fxab.render(1500);
check("RDX Flanger creates stereo width while the retained Felucca model remains mono",
      sideEnergy(oldFlanger) < 0.001 && sideEnergy(rdxFlanger) > 0.01);
check("Flanger detail returns to DEPTH and RATE only",
      readIndexedString(fxab.ex, "web_test_rdx_fx_param_label_byte", 0, 0) === "DEPTH" &&
      readIndexedString(fxab.ex, "web_test_rdx_fx_param_label_byte", 0, 1) === "RATE" &&
      readIndexedString(fxab.ex, "web_test_rdx_fx_param_label_byte", 0, 2) === "");

fxab.ex.web_test_rdx_fx_set(0, 5, 96, 64);             // Phaser only
fxab.ex.web_test_rdx_phaser_model_set(1);
fxab.ex.web_midi(0x90, 60, 100);
const rdxPhaser = fxab.renderFrames(128 * 24, true);
fxab.ex.web_midi(0x80, 60, 0); fxab.render(1500);
check("RDX Phaser's PicoFace short-delay stage produces a stereo output",
      sideEnergy(rdxPhaser) > 0.01);

fxab.ex.web_test_rdx_fx_set(0, 3, 96, 64);             // JIANT three-voice Chorus
fxab.ex.web_test_rdx_fx_set(1, 0, 0, 0);
check("JIANT Chorus maps Depth 96 to SEND 48",
      fxab.ex.web_test_rdx_fx_live(0, 1) === 48);
check("JIANT Chorus exposes DEPTH and RATE controls",
      readIndexedString(fxab.ex, "web_test_rdx_fx_param_label_byte", 0, 0) === "DEPTH" &&
      readIndexedString(fxab.ex, "web_test_rdx_fx_param_label_byte", 0, 1) === "RATE");
fxab.ex.web_midi(0x90, 60, 100);
const jiantChorus = fxab.renderFrames(128 * 24, true);
fxab.ex.web_midi(0x80, 60, 0); fxab.render(1500);
check("the sole Chorus type now creates JIANT stereo width",
      sideEnergy(jiantChorus) > 0.01);

fxab.ex.web_test_rdx_fx_set(0, 7, 90, 80);             // Clouds -> clean Distortion
fxab.ex.web_test_rdx_fx_set(1, 1, 0, 64);
fxab.ex.web_midi(0x90, 60, 100);
const hallThroughFx2 = fxab.renderFrames(128 * 48, true);
fxab.ex.web_midi(0x80, 60, 0); fxab.render(1800);
check("HALL Q14 half-band kernel renders a non-silent stereo return",
      hallThroughFx2.peak > 0.002 && sideEnergy(hallThroughFx2) > 0.01);

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
common[12] = 64;                        // neutral transpose for the C4 -> C5 glide assertions
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
