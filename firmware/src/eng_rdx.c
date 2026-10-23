/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed-point Reface DX engine for Felucca.
 *
 * The patch layout, envelope behaviour, operator frequency rules and twelve
 * algorithms follow the RDX_new/PicoFaceDX-derived implementation in the
 * CoreS3 reference project.  The sample loop is an independent integer C
 * implementation for the AC79: no C++ runtime, libm or heap is required.
 * Phase and modulation use Q0.32, audio Q1.15, envelope levels Q16.16. */
#include "felucca_rdx_data.h"

#define RDX_POLY 8u

enum {
    RFX_THRU, RFX_DIST, RFX_WAH, RFX_CHORUS,
    RFX_FLANGER, RFX_PHASER, RFX_DELAY, RFX_REVERB, RFX_N
};
static const char *const RDX_FX_NAMES[RFX_N] = {
    "THRU", "DIST", "WAH", "CHORUS", "FLANGE", "PHASER", "DELAY", "REVERB"
};
#define RDX_FX_MARK 126

typedef struct __attribute__((packed)) {
    uint8_t voice_name[10], reserved1[2], transpose, mono_poly, porta_time, pb_range;
    uint8_t algorithm, lfo_wave, lfo_speed, lfo_delay, lfo_pmd;
    uint8_t peg_rate[4], peg_level[4], effects[2][3], reserved2[3];
} rdx_common_t;

typedef struct __attribute__((packed)) {
    uint8_t enable, eg_rate[4], eg_level[4], rate_scaling;
    uint8_t scale_ld, scale_rd, scale_lc, scale_rc;
    uint8_t lfo_amd, lfo_pmd_enable, peg_enable, vel_sens, out_level;
    uint8_t feedback, fb_type, freq_mode, freq_coarse, freq_fine, freq_detune;
    uint8_t reserved[3];
} rdx_op_param_t;

typedef struct __attribute__((packed)) {
    rdx_common_t common;
    rdx_op_param_t op[4];
} rdx_patch_t;

_Static_assert(sizeof(rdx_common_t) == 38u, "RDX common patch layout");
_Static_assert(sizeof(rdx_op_param_t) == 28u, "RDX operator patch layout");
_Static_assert(sizeof(rdx_patch_t) == RDX_PATCH_SIZE, "RDX patch layout");

enum { RDX_ATTACK, RDX_DECAY1, RDX_DECAY2, RDX_RELEASE, RDX_SUSTAIN, RDX_IDLE };
enum { RDX_PEG_ATTACK, RDX_PEG_DECAY1, RDX_PEG_DECAY2, RDX_PEG_RELEASE,
       RDX_PEG_SUSTAIN, RDX_PEG_IDLE };

typedef struct {
    uint32_t ph, inc;
    uint32_t env, target, step;                 /* Reface level units, Q16.16 */
    uint32_t out_gain;                          /* Q15, may exceed 1.0 */
    int32_t fb, fb_filter;                      /* oscillator output, Q15 */
    uint8_t stage, rising, rate_offset;
} rdx_op_t;

typedef struct {
    rdx_op_t op[4];
    rdx_patch_t patch;                          /* immutable snapshot for this note */
    int32_t dc, last_out, correction;
    int32_t base_pitch, pitch, pitch_target;    /* absolute semitones, Q16 */
    int32_t peg, peg_target, peg_step;           /* level around centre 64, Q16 */
    uint32_t porta_left;
    uint32_t lfo_phase, lfo_random;
    uint32_t lfo_fade, lfo_fade_total;
    uint32_t pm_depth;                          /* immutable patch values cached off the sample path */
    uint16_t am_depth[4], fb_scale[4];
    int16_t fb_macro;
    uint16_t norm;
    uint16_t tail_left;
    uint8_t correction_left;
    uint8_t live, down, algorithm, transition_pending, ending;
    uint8_t peg_stage, peg_rising, lfo_sh_step;
    uint8_t carriers, enable_mask, fb_type_mask, amd_mask, pmd_mask, peg_mask;
} rdx_note_t;

ENG_MEM_FITS("RDX", sizeof(rdx_note_t) * RDX_POLY);

static const uint8_t RDX_CARRIERS[12] = {
    0x1, 0x1, 0x1, 0x1, 0x1, 0x3, 0x3, 0x5, 0x7, 0x7, 0x7, 0xF
};

/* A track owns one editable Reface patch.  Main-loop SysEx writes the inactive
 * copy and flips the selector only after the full 150 bytes are visible; the
 * audio ISR takes a private per-note snapshot, so a transfer cannot tear a
 * sounding voice. */
static rdx_patch_t rdx_track_patch[NTRK][2];
static volatile uint8_t rdx_track_sel[NTRK];
static uint8_t rdx_track_slot[NTRK], rdx_track_custom, rdx_track_valid;
/* Touch Wah has no touch strip on FM-1.  SELECT is its manual centre control;
 * the audio envelope still supplies the touch-sensitive part of the sweep. */
static volatile uint8_t rdx_touch_wah_pos[NTRK] = {64};
static const rdx_patch_t *rdx_track_get(uint32_t tr);
static uint32_t rdx_fx_type_of(const track_t *t, uint32_t slot);
static uint32_t rdx_fx_send_of(const track_t *t, uint32_t slot);
/* MENU > SYSTEM > RDX TYPE.  Kept outside the settings byte because the
 * engine is compiled before ui.c in Felucca's single translation unit. */
static uint8_t rdx_type = 1u;                 /* OPTIMIZED is the default for a fresh or upgraded unit */
static uint32_t rdx_type_get(void) { return rdx_type != 0u; }
static void rdx_type_set(uint32_t type) { rdx_type = (uint8_t)!!type; }

/* MENU > AUDIO > PHS/FLG MODEL.  The original fixed-point Felucca inserts
 * remain available for controlled A/B tests; RDX selects the fixed-point
 * ports of PicoFaceDX's stereo algorithms.  RDX is the default; the saved
 * preference bits are inverted so older all-zero settings receive it while
 * an explicit Felucca A/B choice can still persist. */
static uint8_t rdx_phaser_model = 1u, rdx_flanger_model = 1u;
static uint32_t rdx_phaser_model_get(void) { return rdx_phaser_model != 0u; }
static uint32_t rdx_flanger_model_get(void) { return rdx_flanger_model != 0u; }
static void rdx_phaser_model_set(uint32_t model) { rdx_phaser_model = (uint8_t)!!model; }
static void rdx_flanger_model_set(uint32_t model) { rdx_flanger_model = (uint8_t)!!model; }

static uint32_t rdx_touch_wah_active(const track_t *t)
{
    if (t < &trk[0] || t >= &trk[NTRK] || t->eng_req != ENGI_RDX)
        return 0;
    return rdx_fx_type_of(t, 0u) == RFX_WAH || rdx_fx_type_of(t, 1u) == RFX_WAH;
}

static uint32_t rdx_touch_wah_get(const track_t *t)
{
    uint32_t tr = t >= &trk[0] && t < &trk[NTRK] ? (uint32_t)(t - trk) : 0u;
    return rdx_touch_wah_pos[tr];
}

static uint32_t rdx_touch_wah_turn(track_t *t, int32_t delta)
{
    uint32_t tr = t >= &trk[0] && t < &trk[NTRK] ? (uint32_t)(t - trk) : 0u;
    rdx_touch_wah_pos[tr] = (uint8_t)clamp((int32_t)rdx_touch_wah_pos[tr] + delta, 0, 127);
    return rdx_touch_wah_pos[tr];
}

/* A sound set is an independent, many-to-many view over the patch table.  A
 * patch can occur in any number of set index arrays without copying its 150
 * bytes.  Until set browsing is added, the first containing set is the name
 * shown on HOME. */
static const char *rdx_sound_set_name(uint32_t preset)
{
    uint32_t s, i;
    for (s = 0; s < NELEM(RDX_SOUND_SETS); s++)
        for (i = 0; i < RDX_SOUND_SETS[s].count; i++)
            if (RDX_SOUND_SETS[s].preset[i] == preset)
                return RDX_SOUND_SETS[s].name;
    return "Unsorted";
}

static int rdx_preset_visible(uint32_t preset)
{
    uint32_t s, i;
    for (s = 0; s < NELEM(RDX_SOUND_SETS); s++)
        for (i = 0; i < RDX_SOUND_SETS[s].count; i++)
            if (RDX_SOUND_SETS[s].preset[i] == preset)
                return 1;
    return 0;
}

static uint32_t rdx_visible_preset(uint32_t at)
{
    uint32_t i;
    for (i = 0; i < RDX_NPATCHES; i++)
        if (rdx_preset_visible(i) && !at--)
            return i;
    return RDX_SET_FACTORY[0];
}

static void rdx_note_cache(rdx_note_t *n)
{
    uint32_t k, alg = n->algorithm % 12u;
    n->carriers = RDX_CARRIERS[alg];
    n->norm = (uint16_t)(alg < 5u ? 32768u : alg < 8u ? 23170u : alg < 11u ? 18919u : 16384u);
    n->pm_depth = RDX_PM_DEPTH_Q16[n->patch.common.lfo_pmd & 127u];
    n->enable_mask = n->fb_type_mask = n->amd_mask = n->pmd_mask = n->peg_mask = 0u;
    for (k = 0; k < 4u; k++) {
        const rdx_op_param_t *p = &n->patch.op[k];
        n->am_depth[k] = RDX_AM_DEPTH_Q15[p->lfo_amd & 127u];
        n->enable_mask |= (uint8_t)(!!p->enable << k);
        n->fb_type_mask |= (uint8_t)(!!p->fb_type << k);
        n->amd_mask |= (uint8_t)(!!p->lfo_amd << k);
        n->pmd_mask |= (uint8_t)(!!p->lfo_pmd_enable << k);
        n->peg_mask |= (uint8_t)(!!p->peg_enable << k);
    }
    n->fb_macro = INT16_MIN;                    /* force the four feedback coefficients to be filled */
}

static uint32_t rdx_track_index(const track_t *t)
{
    uint32_t i = (uint32_t)(t - trk);
    return i < NTRK ? i : 0u;
}

static void rdx_patch_sanitize(rdx_patch_t *p)
{
    uint32_t i, k;
    uint8_t *b = (uint8_t *)p;
    for (i = 0; i < sizeof *p; i++)
        b[i] &= 0x7Fu;
    p->common.mono_poly = p->common.mono_poly > 2u ? 0u : p->common.mono_poly;
    p->common.transpose = (uint8_t)clamp(p->common.transpose, 40, 88);
    p->common.pb_range = (uint8_t)clamp(p->common.pb_range, 40, 88);
    p->common.algorithm %= 12u;
    p->common.lfo_wave %= 7u;
    for (k = 0; k < 4u; k++) {
        rdx_op_param_t *o = &p->op[k];
        o->enable = !!o->enable;
        o->scale_lc &= 3u;
        o->scale_rc &= 3u;
        o->lfo_pmd_enable = !!o->lfo_pmd_enable;
        o->peg_enable = !!o->peg_enable;
        o->fb_type = !!o->fb_type;
        o->freq_mode = !!o->freq_mode;
    }
}

static void rdx_mode_apply(track_t *t, const rdx_patch_t *p)
{
    /* Reface stores the real three-way Part Mode in the patch:
     * 0 POLY, 1 MONO-FULL, 2 MONO-LEGATO.  Keep that value intact in the
     * track instead of collapsing both mono modes into Felucca LEGATO. */
    t->p[P_VOICE] = p->common.mono_poly;
    /* The VOICE page exposes the original 0..127 PORTA value.  Voice.c keeps
     * generic glide disabled for RDX; eng_rdx applies this value itself. */
    t->p[P_GLIDE] = p->common.porta_time;
}

static int16_t *rdx_fx_values(track_t *t, uint32_t slot)
{
    return slot ? &t->p[P_RFX2_A] : &t->p[P_ITYPE];
}

static uint32_t rdx_fx_type_of(const track_t *t, uint32_t slot)
{
    return (uint32_t)clamp(t->p[slot ? P_DLY : P_DIST], RFX_THRU, RFX_REVERB);
}

static uint32_t rdx_fx_send_of(const track_t *t, uint32_t slot)
{
    return (uint32_t)clamp(t->p[slot ? P_REV : P_CHOR], 0, 127);
}

static uint32_t rdx_fx_default_send(uint32_t type, uint32_t depth)
{
    depth &= 127u;
    if (type == RFX_THRU)
        return 0u;
    if (type == RFX_CHORUS)
        return 20u;
    if (type == RFX_DELAY)
        return (depth * 3u + 2u) / 5u;
    if (type == RFX_REVERB)
        return depth / 3u;
    return 127u;
}

/* Fill the four live controls from the three bytes in a Reface patch.  The
 * extra controls expose values that were fixed in the first RDX firmware:
 * Delay feedback / colour and Reverb model / damping. */
static void rdx_fx_slot_defaults(track_t *t, uint32_t slot, uint32_t type,
                                 uint32_t p1, uint32_t p2)
{
    int16_t *v = rdx_fx_values(t, slot);
    uint32_t send_id = slot ? P_REV : P_CHOR;
    uint32_t type_id = slot ? P_DLY : P_DIST;
    uint32_t fb;
    type = type < RFX_N ? type : RFX_THRU;
    p1 &= 127u;
    p2 &= 127u;
    t->p[type_id] = (int16_t)type;
    t->p[send_id] = (int16_t)rdx_fx_default_send(type, p1);
    v[0] = v[1] = v[2] = v[3] = 0;
    switch (type) {
    case RFX_DIST:    v[0] = (int16_t)p1; v[1] = (int16_t)p2; v[2] = 100; break;
    case RFX_WAH:     v[0] = (int16_t)p1; v[1] = (int16_t)p2; break;
    case RFX_CHORUS:
    case RFX_FLANGER:
    case RFX_PHASER:  v[0] = (int16_t)p1; v[1] = (int16_t)p2; break;
    case RFX_DELAY:
        fb = (p1 * 6u + 2u) / 5u;
        v[0] = (int16_t)p1; v[1] = (int16_t)p2;
        v[2] = (int16_t)(fb > 120u ? 120u : fb); v[3] = 70;
        break;
    case RFX_REVERB:
        v[0] = 2; v[1] = 127; v[2] = (int16_t)p2; v[3] = 60;
        break;
    default: break;
    }
    t->p[P_IMIX] = RDX_FX_MARK;
}

static void rdx_fx_from_patch(track_t *t, const rdx_patch_t *p)
{
    uint32_t k;
    for (k = 0; k < 2u; k++)
        rdx_fx_slot_defaults(t, k, p->common.effects[k][0],
                             p->common.effects[k][1], p->common.effects[k][2]);
}

/* A type selected on page 1 gets useful neutral parameters immediately.
 * This also prevents THRU's zero send from making the newly selected effect
 * appear broken. */
static void rdx_fx_type_changed(track_t *t, uint32_t slot, uint32_t type)
{
    uint32_t p1 = 64u, p2 = 64u;
    if (type == RFX_DIST) p1 = 24u;
    if (type == RFX_CHORUS) p2 = 40u;
    if (type == RFX_REVERB) p2 = 60u;
    rdx_fx_slot_defaults(t, slot, type, p1, p2);
}

static const rdx_patch_t *rdx_track_get(uint32_t tr)
{
    tr %= NTRK;
    RING_PUBLISH();
    return &rdx_track_patch[tr][rdx_track_sel[tr] & 1u];
}

static void rdx_track_set(uint32_t tr, const rdx_patch_t *src, int custom)
{
    uint32_t next;
    tr %= NTRK;
    next = (rdx_track_sel[tr] ^ 1u) & 1u;
    memcpy(&rdx_track_patch[tr][next], src, sizeof *src);
    rdx_patch_sanitize(&rdx_track_patch[tr][next]);
    RING_PUBLISH();
    rdx_track_sel[tr] = (uint8_t)next;
    rdx_track_valid |= (uint8_t)(1u << tr);
    if (custom)
        rdx_track_custom |= (uint8_t)(1u << tr);
    else
        rdx_track_custom &= (uint8_t)~(1u << tr);
}

/* Called after a factory preset is installed (UI, project migration and host
 * simulator use the same hook). */
static void rdx_track_loaded(track_t *t)
{
    uint32_t tr, slot;
    if (t->eng_req != ENGI_RDX)
        return;
    tr = rdx_track_index(t);
    slot = (uint32_t)clamp(t->p[P_E7], 0, (int32_t)RDX_NPATCHES - 1);
    if (!rdx_preset_visible(slot)) {
        slot = RDX_FIRST_VISIBLE;
        t->p[P_E7] = (int16_t)slot;
    }
    /* Keep the generic preset id and RDX's PATCH parameter inseparable.  Old
     * autosaves can contain values written by different RDX preset layouts;
     * without this, HOME may name one voice while the engine loads another. */
    t->preset = (uint8_t)slot;
    rdx_track_set(tr, (const rdx_patch_t *)RDX_PATCHES[slot].patch, 0);
    rdx_track_slot[tr] = (uint8_t)slot;
    rdx_mode_apply(t, rdx_track_get(tr));
    rdx_fx_from_patch(t, rdx_track_get(tr));
}

/* Project and user-preset records predate the 150-byte RDX patch and therefore
 * only retain its factory slot in P_E7.  Reload that patch after the generic
 * Felucca parameters have been restored: in particular, an older autosave may
 * contain VOICE=POLY even when the RDX common block says MONO. */
static void rdx_track_restored(track_t *t)
{
    uint32_t slot = t->preset;
    int keep_fx = t->p[P_IMIX] == RDX_FX_MARK;
    int16_t saved[13];
    uint32_t i;
    if (keep_fx) {
        saved[0] = t->p[P_DIST]; saved[1] = t->p[P_CHOR];
        saved[2] = t->p[P_DLY];  saved[3] = t->p[P_REV];
        for (i = 0; i < 5u; i++) saved[4u + i] = t->p[P_ITYPE + i];
        for (i = 0; i < 4u; i++) saved[9u + i] = t->p[P_RFX2_A + i];
    }
    /* A project stores both preset and P_E7.  The preset id is the sound the
     * user selected; force PATCH to it before copying the current firmware's
     * complete 150-byte factory/SysEx data. */
    if (slot < RDX_NPATCHES && rdx_preset_visible(slot))
        t->p[P_E7] = (int16_t)slot;
    if (t->eng_req != ENGI_RDX)
        return;
    rdx_track_loaded(t);
    if (keep_fx) {
        t->p[P_DIST] = saved[0]; t->p[P_CHOR] = saved[1];
        t->p[P_DLY] = saved[2];  t->p[P_REV] = saved[3];
        for (i = 0; i < 5u; i++) t->p[P_ITYPE + i] = saved[4u + i];
        for (i = 0; i < 4u; i++) t->p[P_RFX2_A + i] = saved[9u + i];
    }
}

static void rdx_sysex_patch_set(uint32_t tr, const rdx_patch_t *p)
{
    track_t *t = &trk[tr % NTRK];
    rdx_track_set(tr, p, 1);
    rdx_mode_apply(t, rdx_track_get(tr));
    rdx_fx_from_patch(t, rdx_track_get(tr));
}

static void rdx_patch_for_track(uint32_t tr, rdx_patch_t *out)
{
    uint32_t slot;
    tr %= NTRK;
    slot = (uint32_t)clamp(trk[tr].p[P_E7], 0, (int32_t)RDX_NPATCHES - 1);
    if ((rdx_track_valid & (1u << tr)) &&
        ((rdx_track_custom & (1u << tr)) || rdx_track_slot[tr] == slot))
        memcpy(out, rdx_track_get(tr), sizeof *out);
    else
        memcpy(out, RDX_PATCHES[slot].patch, sizeof *out);
}

static rdx_note_t *rdx_note(track_t *t, voice_t *v)
{
    uint32_t i;
    if (t < &trk[0] || t >= &trk[NPART])
        return 0;
    i = (uint32_t)(v - t->v);
    return i < RDX_POLY ? (rdx_note_t *)ENG_MEM(t - trk) + i : 0;
}

/* A panel/editor change of VOICE is a Reface patch edit for RDX, not a
 * Felucca allocator override.  Publish it through the same double-buffered
 * patch path as SysEx and update live snapshots so the next legato pitch
 * change immediately uses the selected portamento rule. */
static void rdx_voice_settings_set(track_t *t, uint32_t mode, uint32_t porta)
{
    rdx_patch_t p;
    uint32_t tr = rdx_track_index(t), i;
    mode = mode > 2u ? 2u : mode;
    porta = porta > 127u ? 127u : porta;
    rdx_patch_for_track(tr, &p);
    if (p.common.mono_poly == mode && p.common.porta_time == porta)
        return;
    p.common.mono_poly = (uint8_t)mode;
    p.common.porta_time = (uint8_t)porta;
    rdx_track_set(tr, &p, 1);
    for (i = 0; i < RDX_POLY; i++) {
        rdx_note_t *n = rdx_note(t, &t->v[i]);
        if (n && n->live) {
            n->patch.common.mono_poly = (uint8_t)mode;
            n->patch.common.porta_time = (uint8_t)porta;
        }
    }
}

static uint32_t rdx_gain(uint32_t level_q16)
{
    uint32_t i = level_q16 >> 16, f = level_q16 & 0xFFFFu;
    int32_t a, b;
    if (i >= 191u)
        return RDX_GAIN_Q15[191];
    a = (int32_t)RDX_GAIN_Q15[i];
    b = (int32_t)RDX_GAIN_Q15[i + 1u];
    /* Adjacent table entries differ by at most 5377, so this product is
     * bounded well inside int32_t and does not need the AC79 64-bit helper. */
    return (uint32_t)(a + (((b - a) * (int32_t)f) >> 16));
}

/* 2^(semitones/12), Q16.  The 192-entry octave table gives 1/16-semitone
 * knots; interpolation keeps pitch bend, PEG and portamento smooth without a
 * logarithm or floating point in the audio interrupt. */
static uint32_t rdx_pitch_ratio(int32_t semitones)
{
    int32_t index, frac, octave, within;
    uint32_t a, b, r;
    semitones = clamp(semitones, -96 * 65536, 96 * 65536);
    index = semitones / 4096;
    frac = semitones % 4096;
    if (frac < 0) {
        frac += 4096;
        index--;
    }
    octave = index / 192;
    within = index % 192;
    if (within < 0) {
        within += 192;
        octave--;
    }
    a = RDX_PITCH_RATIO_Q16[within];
    b = within == 191 ? RDX_PITCH_RATIO_Q16[0] << 1 : RDX_PITCH_RATIO_Q16[within + 1];
    r = a + (uint32_t)(((uint64_t)(b - a) * (uint32_t)frac) >> 12);
    if (octave > 0)
        r = octave >= 15 || r > (0xFFFFFFFFu >> octave) ? 0xFFFFFFFFu : r << octave;
    else if (octave < 0)
        r = -octave >= 31 ? 0u : r >> -octave;
    return r;
}

static uint32_t rdx_inc_mod(uint32_t inc, int32_t semitones)
{
    uint64_t v = (uint64_t)inc * rdx_pitch_ratio(semitones) >> 16;
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

/* Exact floor(a*b/d) for the bounded control-rate values below, with both
 * intermediate products kept in 32 bits. */
static uint32_t rdx_muldiv_u32(uint32_t a, uint32_t b, uint32_t d)
{
    return (a / d) * b + ((a % d) * b) / d;
}

/* RDX_new's keyboard level scaling, evaluated only at note start. Curves 0
 * and 1 attenuate; 2 and 3 boost.  Q15 result is limited to 0..2 exactly as
 * the reference implementation. */
static uint32_t rdx_level_scaling(const rdx_op_param_t *p, uint32_t note)
{
    uint32_t depth, curve, distance, range, amount, denom;
    if (note > 60u) {
        depth = p->scale_rd & 127u;
        curve = p->scale_rc & 3u;
        distance = note - 60u;
        range = 60u;                                      /* matches RDX_new's established curve */
    } else if (note < 60u) {
        depth = p->scale_ld & 127u;
        curve = p->scale_lc & 3u;
        distance = 60u - note;
        range = 67u;
    } else
        return 32768u;
    if (curve == 1u || curve == 2u)
        amount = rdx_muldiv_u32(RDX_ENV_TARGET_Q16[depth], distance * 8u,
                                127u * range);            /* Q16 */
    else
        amount = rdx_muldiv_u32(depth * 65536u, distance * 8u,
                                127u * range);            /* Q16 */
    if (curve < 2u) {
        denom = 65536u + amount;
        return denom ? (32768u * 65536u) / denom : 32768u;
    }
    amount = 32768u + (amount >> 1);                     /* Q15 */
    return amount > 65536u ? 65536u : amount;
}

static void rdx_peg_enter(rdx_note_t *n, uint32_t stage)
{
    const rdx_common_t *p = &n->patch.common;
    uint32_t rate;
    n->peg_stage = (uint8_t)stage;
    if (stage == RDX_PEG_IDLE || stage == RDX_PEG_SUSTAIN) {
        n->peg_step = 0;
        return;
    }
    n->peg_target = (int32_t)(p->peg_level[stage] & 127u) << 16;
    n->peg_rising = n->peg_target > n->peg;
    rate = p->peg_rate[stage] & 127u;
    n->peg_step = n->peg_rising ? (int32_t)RDX_PEG_UP_Q16[rate] : -(int32_t)RDX_PEG_DOWN_Q16[rate];
}

static void rdx_peg_next(rdx_note_t *n)
{
    uint32_t s;
    switch (n->peg_stage) {
    case RDX_PEG_ATTACK: s = n->down ? RDX_PEG_DECAY1 : RDX_PEG_RELEASE; break;
    case RDX_PEG_DECAY1: s = n->down ? RDX_PEG_DECAY2 : RDX_PEG_RELEASE; break;
    case RDX_PEG_DECAY2: s = n->down ? RDX_PEG_SUSTAIN : RDX_PEG_RELEASE; break;
    case RDX_PEG_SUSTAIN: s = n->down ? RDX_PEG_SUSTAIN : RDX_PEG_RELEASE; break;
    default: s = RDX_PEG_IDLE; break;
    }
    rdx_peg_enter(n, s);
}

static void rdx_peg_tick(rdx_note_t *n)
{
    int32_t next;
    if (n->peg_stage == RDX_PEG_IDLE)
        return;
    if (n->peg_stage == RDX_PEG_SUSTAIN) {
        if (!n->down)
            rdx_peg_enter(n, RDX_PEG_RELEASE);
        return;
    }
    next = n->peg + n->peg_step;
    if ((n->peg_rising && next >= n->peg_target) || (!n->peg_rising && next <= n->peg_target)) {
        n->peg = n->peg_target;
        rdx_peg_next(n);
    } else
        n->peg = next;
}

static uint32_t rdx_random(uint32_t *state)
{
    uint32_t s = *state ? *state : 0xA5A5A5A5u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    *state = s;
    return s;
}

static int32_t rdx_lfo_wave(rdx_note_t *n, uint32_t phase)
{
    uint32_t u = phase >> 16;
    switch (n->patch.common.lfo_wave % 7u) {
    case 0: return osc_sine(phase);
    case 1: return 32767 - (int32_t)((u > 32768u ? u - 32768u : 32768u - u) << 1);
    case 2: return (int32_t)u - 32768;
    case 3: return 32767 - (int32_t)u;
    case 4: return phase < 0x80000000u ? 32767 : -32768;
    default: return (int32_t)(n->lfo_random >> 16) - 32768;
    }
}

static void rdx_lfo_block(rdx_note_t *n, uint32_t len, int32_t *first, int32_t *last)
{
    const rdx_common_t *p = &n->patch.common;
    uint32_t ph0 = n->lfo_phase, inc = RDX_LFO_INC[p->lfo_speed & 127u];
    uint32_t ph1 = ph0 + inc * len, f0 = n->lfo_fade, f1 = f0;
    uint32_t wave = p->lfo_wave % 7u, step;
    if (n->lfo_fade < 32768u) {
        uint32_t add = n->lfo_fade_total ? len * 32768u / n->lfo_fade_total : 32768u;
        f1 = n->lfo_fade + add;
        if (f1 > 32768u)
            f1 = 32768u;
        n->lfo_fade = f1;
    }
    if (wave == 5u) {                                   /* S&H8: a new value at each eighth-cycle boundary */
        step = ph1 >> 29;
        if (step != n->lfo_sh_step) {
            rdx_random(&n->lfo_random);
            n->lfo_sh_step = (uint8_t)step;
        }
    } else if (wave == 6u && ph1 < ph0)                  /* S&H: one value per cycle */
        rdx_random(&n->lfo_random);
    *first = mulq15(rdx_lfo_wave(n, ph0), (int32_t)f0);
    *last = mulq15(rdx_lfo_wave(n, ph1), (int32_t)f1);
    n->lfo_phase = ph1;
}

static void rdx_env_enter(rdx_op_t *o, const rdx_op_param_t *p, uint32_t stage,
                          uint32_t rate_offset, int32_t rate_macro)
{
    uint32_t rate;
    o->stage = (uint8_t)stage;
    if (stage == RDX_IDLE)
        return;
    if (stage == RDX_SUSTAIN) {
        o->target = RDX_ENV_TARGET_Q16[p->eg_level[2] & 127u];
        o->step = 0;
        return;
    }
    o->target = RDX_ENV_TARGET_Q16[p->eg_level[stage] & 127u];
    o->rising = o->target > o->env;
    rate = (uint32_t)clamp((int32_t)(p->eg_rate[stage] & 127u) + (int32_t)rate_offset - rate_macro,
                           0, 127);
    o->step = o->rising ? RDX_ATTACK_STEP_Q16[rate] : RDX_DECAY_STEP_Q16[rate];
}

static void rdx_env_next(rdx_op_t *o, const rdx_op_param_t *p, int gate,
                         uint32_t rate_offset, int32_t rate_macro)
{
    uint32_t s;
    switch (o->stage) {
    case RDX_ATTACK: s = gate ? RDX_DECAY1 : RDX_RELEASE; break;
    case RDX_DECAY1: s = gate ? RDX_DECAY2 : RDX_RELEASE; break;
    case RDX_DECAY2: s = gate ? RDX_SUSTAIN : RDX_RELEASE; break;
    case RDX_SUSTAIN: s = gate ? RDX_SUSTAIN : RDX_RELEASE; break;
    default: s = RDX_IDLE; break;
    }
    rdx_env_enter(o, p, s, rate_offset, rate_macro);
}

static uint32_t rdx_env_tick(rdx_op_t *o, const rdx_op_param_t *p, int gate, int tail,
                             uint32_t rate_offset, int32_t rate_macro)
{
    if (o->stage == RDX_IDLE)
        return tail ? rdx_gain(o->env) : 0;
    if (o->stage == RDX_SUSTAIN) {
        if (!gate)
            rdx_env_enter(o, p, RDX_RELEASE, rate_offset, rate_macro);
        return rdx_gain(o->env);
    }
    if (o->rising) {
        uint32_t next = o->env + o->step;
        if (next < o->env || next >= o->target) {
            o->env = o->target;
            rdx_env_next(o, p, gate, rate_offset, rate_macro);
        } else
            o->env = next;
    } else if (o->env <= o->target + o->step) {
        o->env = o->target;
        rdx_env_next(o, p, gate, rate_offset, rate_macro);
    } else
        o->env -= o->step;
    return rdx_gain(o->env);
}

static uint32_t rdx_rate_offset(uint32_t note, uint32_t sensitivity)
{
    if (!sensitivity || note <= 21u)
        return 0;
    return (sensitivity * (note - 21u) * 63u + 6731u) / 13462u;
}

static uint32_t rdx_ratio_q16(const rdx_op_param_t *p, int32_t macro)
{
    int32_t coarse = p->freq_coarse;
    uint32_t r;
    if (coarse > 0)
        r = (uint32_t)(clamp(coarse + macro, 1, 63) * 100 + p->freq_fine) * 65536u / 100u;
    else
        r = (uint32_t)(50 + p->freq_fine / 2u) * 65536u / 100u;
    return r;
}

static uint32_t rdx_op_inc(const rdx_op_param_t *p, uint32_t base_inc, int32_t ratio_macro,
                           int32_t carrier_cents)
{
    uint32_t inc, det;
    if (p->freq_mode) {
        uint32_t group = (p->freq_coarse >> 3) > 3u ? 3u : p->freq_coarse >> 3;
        inc = RDX_FIXED_INC[group * 128u + (p->freq_fine & 127u)];
    } else {
        inc = (uint32_t)(((uint64_t)base_inc * rdx_ratio_q16(p, ratio_macro)) >> 16);
    }
    det = RDX_DETUNE_Q16[p->freq_detune & 127u];
    inc = (uint32_t)(((uint64_t)inc * det) >> 16);
    if (carrier_cents) {
        int32_t q = 65536 + carrier_cents * 38;
        inc = (uint32_t)(((uint64_t)inc * (uint32_t)clamp(q, 32768, 98304)) >> 16);
    }
    return inc;
}

static uint32_t rdx_velocity_gain_q15(uint32_t vel_q8, uint32_t sens)
{
    uint32_t vi = vel_q8 >> 8, vf = vel_q8 & 255u;
    uint32_t vr = RDX_VELOCITY_Q15[vi > 127u ? 127u : vi];
    if (vi < 127u)
        vr += ((uint32_t)(RDX_VELOCITY_Q15[vi + 1u] - vr) * vf + 128u) >> 8;
    return (uint32_t)(((127u - sens) * 32768u + sens * vr) / 127u);
}

static uint32_t rdx_output_gain(const rdx_op_param_t *p, uint32_t vel_q8, int32_t level_macro,
                                int32_t velocity_macro, uint32_t note)
{
    uint32_t sens = (uint32_t)clamp((int32_t)p->vel_sens + velocity_macro, 0, 127);
    uint32_t vg = rdx_velocity_gain_q15(vel_q8, sens);
    uint32_t level;
    vg = (vg * 35389u) >> 15;                       /* reference engine's 1.08 velocity headroom */
    level = (uint32_t)clamp((int32_t)p->out_level + level_macro, 0, 127);
    level = (level * vg) >> 15;
    return (uint32_t)(((uint64_t)RDX_GAIN_Q15[level > 191u ? 191u : level] *
                       rdx_level_scaling(p, note)) >> 15);
}

static uint32_t rdx_algorithm(const track_t *t, const rdx_patch_t *p)
{
    int32_t a = t->p[P_E0];
    return a ? (uint32_t)clamp(a - 1, 0, 11) : p->common.algorithm % 12u;
}

/* The RDX build is one instrument, so its track transpose and the panel octave
 * form one master pitch control.  Apply it inside the engine: every source
 * (panel keys, MIDI, arp and sequencer) follows it, while voice ownership and
 * note-off matching keep the original note and cannot hang if the control is
 * changed while a key is held. */
static uint32_t rdx_master_note(const track_t *t, uint32_t note)
{
    int32_t trans = clamp(t->p[P_TRANS], -24, 24);
    int32_t octave = clamp(song.octave, -3, 3);
    return (uint32_t)clamp((int32_t)note + trans + octave * 12, 0, 127);
}

static void rdx_note_on(track_t *t, voice_t *v)
{
    rdx_note_t *n = rdx_note(t, v);
    const rdx_patch_t *p;
    uint32_t slot = (uint32_t)clamp(t->p[P_E7], 0, (int32_t)RDX_NPATCHES - 1);
    uint32_t k, carrier_index = 0, carriers, base_inc, vel_q8, note;
    int32_t old_out, old_pitch;
    int was_live;
    if (!n)
        return;
    old_out = n->last_out;
    old_pitch = n->pitch;
    was_live = n->live;
    memset(n, 0, sizeof *n);
    if ((rdx_track_valid & (1u << rdx_track_index(t))) &&
        ((rdx_track_custom & (1u << rdx_track_index(t))) ||
         rdx_track_slot[rdx_track_index(t)] == slot))
        memcpy(&n->patch, rdx_track_get(rdx_track_index(t)), sizeof n->patch);
    else
        memcpy(&n->patch, RDX_PATCHES[slot].patch, sizeof n->patch);
    p = &n->patch;
    n->live = n->down = 1;
    n->last_out = old_out;
    n->transition_pending = (uint8_t)was_live;  /* retrigger/steal: join the old waveform */
    n->algorithm = (uint8_t)rdx_algorithm(t, p);
    rdx_note_cache(n);
    note = rdx_master_note(t, v->note);
    n->base_pitch = ((int32_t)note + (int32_t)p->common.transpose - 64) * 65536;
    n->pitch_target = n->base_pitch;
    if ((was_live || old_pitch) && p->common.mono_poly == 1u && p->common.porta_time) {
        n->pitch = old_pitch;
        n->porta_left = RDX_PORTA_SAMPLES[p->common.porta_time & 127u];
    } else
        n->pitch = n->pitch_target;
    n->peg = 64 << 16;
    rdx_peg_enter(n, RDX_PEG_ATTACK);
    n->lfo_random = 0xA5A5A5A5u ^ v->age ^ (note << 24);
    n->lfo_sh_step = 0xFFu;
    n->lfo_fade_total = RDX_LFO_DELAY_SAMPLES[p->common.lfo_delay & 127u] / 3u;
    n->lfo_fade = p->common.lfo_delay ? 0u : 32768u;
    carriers = RDX_CARRIERS[n->algorithm];
    base_inc = pitch_inc(clamp((int32_t)note * 16 + ((int32_t)p->common.transpose - 64) * 16, 0, 2047));
    vel_q8 = rdx_note_velocity_q8;
    if (!vel_q8)
        vel_q8 = (uint32_t)(v->mvel ? v->mvel : v->vel) << 8;
    for (k = 0; k < 4u; k++) {
        const rdx_op_param_t *q = &p->op[k];
        rdx_op_t *o = &n->op[k];
        int carrier = (carriers >> k) & 1u;
        int32_t spread = 0, ratio_macro = carrier ? 0 : t->p[P_E3];
        int32_t level_macro = carrier ? 0 : t->p[P_E2];
        if (carrier) {
            static const int8_t SPREAD[4] = {0, 1, -1, 2};
            spread = SPREAD[carrier_index++] * (int32_t)t->p[P_E6] * 12 / 127;
        }
        o->inc = rdx_op_inc(q, base_inc, ratio_macro, spread);
        o->out_gain = rdx_output_gain(q, vel_q8, level_macro, t->p[P_E5], note);
        o->rate_offset = (uint8_t)rdx_rate_offset(note, q->rate_scaling);
        o->stage = RDX_ATTACK;
        o->target = RDX_ENV_TARGET_Q16[q->eg_level[0] & 127u];
        o->rising = 1;
        o->step = RDX_ATTACK_STEP_Q16[clamp((int32_t)q->eg_rate[0] +
                    (int32_t)o->rate_offset - t->p[P_E4], 0, 127)];
    }
}

static int32_t rdx_op_sample(rdx_op_t *o, const rdx_op_param_t *p, int32_t input,
                             int gate, int tail, uint32_t rate_offset, int32_t rate_macro,
                             uint32_t fb_scale, uint32_t inc, int32_t am)
{
    int32_t f, wave, s;
    uint32_t env;
    if (!p->enable)
        return 0;
    f = o->fb;
    if (p->fb_type && f < 0)
        f = -f;
    /* Both products have proven Q15 bounds, so the shared 32-bit DSP helpers
     * avoid two software 64-bit multiplies per operator and sample. */
    o->fb_filter += mulq15(f - o->fb_filter, 11665);
    f = (o->fb_filter * (int32_t)fb_scale) >> 16;
    wave = osc_sine(o->ph + ((uint32_t)(input + f) << 17));
    o->ph += inc;
    o->fb = wave;
    env = rdx_env_tick(o, p, gate, tail, rate_offset, rate_macro);
    s = (int32_t)(((int64_t)wave * (int32_t)o->out_gain) >> 15);
    s = (int32_t)(((int64_t)s * env) >> 15);
    return p->lfo_amd ? mulq15(s, am) : s;
}

static int rdx_done(track_t *t, voice_t *v)
{
    rdx_note_t *n = rdx_note(t, v);
    const rdx_patch_t *p;
    uint32_t k, carriers;
    if (!n || !n->live)
        return 1;
    p = &n->patch;
    if (!v->gate && n->down) {
        n->down = 0;
        if (n->peg_stage != RDX_PEG_IDLE && n->peg_stage != RDX_PEG_RELEASE)
            rdx_peg_enter(n, RDX_PEG_RELEASE);
        for (k = 0; k < 4u; k++)
            if (n->op[k].stage != RDX_IDLE && n->op[k].stage != RDX_RELEASE)
                rdx_env_enter(&n->op[k], &p->op[k], RDX_RELEASE,
                              n->op[k].rate_offset, t->p[P_E4]);
    }
    /* A forced stop has already entered the fixed 256-sample output fade.  Do not wait for the
     * patch's operator release rates: overload shedding has to reclaim the voice predictably. */
    if (n->ending) {
        if (n->tail_left)
            return 0;
        n->live = 0;
        return 1;
    }
    carriers = RDX_CARRIERS[n->algorithm % 12u];
    for (k = 0; k < 4u; k++)
        if (((carriers >> k) & 1u) && n->op[k].stage != RDX_IDLE)
            return 0;
    n->ending = 1;                                    /* continue the final waveform, then fade it to silence */
    n->tail_left = 256u;                              /* 5.8 ms: long enough to remove an EG level-4 edge */
    return 0;
}

static void rdx_force_stop(track_t *t, voice_t *v)
{
    rdx_note_t *n = rdx_note(t, v);
    if (!n || !n->live)
        return;
    n->down = 0;
    n->ending = 1;
    n->tail_left = 256u;                              /* exactly eight 32-sample render blocks */
}

static __attribute__((noinline)) void rdx_render_origin(track_t *t, voice_t *v, int32_t *out,
                                                        uint32_t len, const vmod_t *m)
{
    rdx_note_t *n = rdx_note(t, v);
    const rdx_patch_t *p;
    uint32_t i, k, alg, norm, fb_scale[4], inc_cur[4];
    uint32_t inc_delta[4], inc_rem[4], inc_error[4];
    int32_t am_cur[4], am_step[4];
    uint8_t inc_down[4];
    int32_t amp, damp, rate_macro, fb_macro, lfo0, lfo1, peg0, peg1;
    int32_t pitch0, pitch1, target, ext_pitch, mw0, mw1;
    if (!n || !n->live)
        return;
    p = &n->patch;
    alg = n->algorithm % 12u;
    norm = alg < 5u ? 32768u : alg < 8u ? 23170u : alg < 11u ? 18919u : 16384u;
    rate_macro = t->p[P_E4];
    fb_macro = t->p[P_E1];
    target = ((int32_t)rdx_master_note(t, v->note) + (int32_t)p->common.transpose - 64) * 65536;
    if (target != n->pitch_target) {                    /* LEGATO note-on or held-note fallback */
        n->pitch_target = target;
        if (p->common.mono_poly && p->common.porta_time)
            n->porta_left = RDX_PORTA_SAMPLES[p->common.porta_time & 127u];
        else {
            n->pitch = target;
            n->porta_left = 0;
        }
    }
    pitch0 = n->pitch;
    if (n->porta_left) {
        uint32_t adv = len < n->porta_left ? len : n->porta_left;
        /* The largest MIDI+transpose interval times a 32-sample block stays
         * well inside int32_t. Keeping this division 32-bit avoids AC79's
         * unavailable __divdi3 runtime helper. */
        n->pitch += (n->pitch_target - n->pitch) * (int32_t)adv / (int32_t)n->porta_left;
        n->porta_left -= adv;
        if (!n->porta_left)
            n->pitch = n->pitch_target;
    }
    pitch1 = n->pitch;
    /* Felucca still supplies global tune, channel bend and modulation-matrix
     * pitch. P_GLIDE is held at zero for RDX, so subtracting pitch_cur leaves
     * only those external controls. */
    ext_pitch = (m->pitch16 - v->pitch_cur) * 4096;
    ext_pitch += m->fine * 277;                         /* residual ratio -> approximately Q16 semitones */
    peg0 = n->peg - (64 << 16);
    for (i = 0; i < len; i++)
        rdx_peg_tick(n);
    peg1 = n->peg - (64 << 16);
    rdx_lfo_block(n, len, &lfo0, &lfo1);
    mw0 = lfo0 * t->mw / 127;                           /* Q15, full wheel = +/-1 semitone */
    mw1 = lfo1 * t->mw / 127;
    for (k = 0; k < 4u; k++) {
        const rdx_op_param_t *q = &p->op[k];
        int32_t pm0 = (int32_t)(((int64_t)lfo0 * RDX_PM_DEPTH_Q16[p->common.lfo_pmd & 127u]) >> 15);
        int32_t pm1 = (int32_t)(((int64_t)lfo1 * RDX_PM_DEPTH_Q16[p->common.lfo_pmd & 127u]) >> 15);
        int32_t sem0, sem1, a0 = 32768, a1 = 32768;
        if (!q->lfo_pmd_enable)
            pm0 = pm1 = 0;
        sem0 = pitch0 - n->base_pitch + ext_pitch + pm0 + mw0 * 2;
        sem1 = pitch1 - n->base_pitch + ext_pitch + pm1 + mw1 * 2;
        if (q->peg_enable) {
            sem0 += peg0;
            sem1 += peg1;
        }
        inc_cur[k] = rdx_inc_mod(n->op[k].inc, sem0);
        {
            uint32_t end = rdx_inc_mod(n->op[k].inc, sem1);
            uint32_t span = len > 1u ? len - 1u : 1u;
            uint32_t distance;
            inc_down[k] = end < inc_cur[k];
            distance = inc_down[k] ? inc_cur[k] - end : end - inc_cur[k];
            inc_delta[k] = distance / span;
            inc_rem[k] = distance % span;
            inc_error[k] = 0;
        }
        if (q->lfo_amd) {
            uint32_t depth = RDX_AM_DEPTH_Q15[q->lfo_amd & 127u];
            a0 += (int32_t)(((int64_t)depth * (lfo0 * 2 - 32768)) >> 15) - mw0;
            a1 += (int32_t)(((int64_t)depth * (lfo1 * 2 - 32768)) >> 15) - mw1;
            a0 = clamp(a0, 0, 65536);
            a1 = clamp(a1, 0, 65536);
        }
        am_cur[k] = a0;
        am_step[k] = len > 1u ? (a1 - a0) / (int32_t)(len - 1u) : 0;
        fb_scale[k] = RDX_FEEDBACK_Q16[clamp((int32_t)p->op[k].feedback + fb_macro, 0, 127)];
    }
    amp = m->amp0;
    damp = len > 1u ? (m->amp1 - m->amp0) / (int32_t)(len - 1u) : 0;
    for (i = 0; i < len; i++) {
        int32_t a = 0, b = 0, c = 0, d = 0, s;
#define OP(K, IN) rdx_op_sample(&n->op[(K)], &p->op[(K)], (IN), n->down, n->ending, \
                    n->op[(K)].rate_offset, rate_macro, fb_scale[(K)], inc_cur[(K)], am_cur[(K)])
        switch (alg) {
        case 0: d = OP(3, 0); c = OP(2, d); b = OP(1, c); a = OP(0, b); s = a; break;
        case 1: d = OP(3, 0); c = OP(2, 0); b = OP(1, d + c); a = OP(0, b); s = a; break;
        case 2: c = OP(2, 0); b = OP(1, c); d = OP(3, 0); a = OP(0, b + d); s = a; break;
        case 3: d = OP(3, 0); b = OP(1, d); c = OP(2, d); a = OP(0, b + c); s = a; break;
        case 4: b = OP(1, 0); c = OP(2, 0); d = OP(3, 0); a = OP(0, b + c + d); s = a; break;
        case 5: a = OP(0, 0); d = OP(3, 0); c = OP(2, d); b = OP(1, c); s = a + b; break;
        case 6: d = OP(3, 0); c = OP(2, d); a = OP(0, c); b = OP(1, c); s = a + b; break;
        case 7: b = OP(1, 0); a = OP(0, b); d = OP(3, 0); c = OP(2, d); s = a + c; break;
        case 8: d = OP(3, 0); a = OP(0, d); b = OP(1, d); c = OP(2, d); s = a + b + c; break;
        case 9: d = OP(3, 0); a = OP(0, 0); b = OP(1, d); c = OP(2, d); s = a + b + c; break;
        case 10: a = OP(0, 0); b = OP(1, 0); d = OP(3, 0); c = OP(2, d); s = a + b + c; break;
        default: a = OP(0, 0); b = OP(1, 0); c = OP(2, 0); d = OP(3, 0); s = a + b + c + d; break;
        }
#undef OP
        s = mulq16(s, norm << 1);
        { int32_t x = s - n->dc; n->dc += x >> 10; s = x; }
        if (!n->down && !n->ending) {                  /* a release can reach IDLE in the middle of this block */
            uint32_t k, active = 0, carriers = RDX_CARRIERS[alg];
            for (k = 0; k < 4u; k++)
                active |= ((carriers >> k) & 1u) && n->op[k].stage != RDX_IDLE;
            if (!active) {
                n->ending = 1;
                n->tail_left = 256u;
            }
        }
        if (n->transition_pending) {
            n->correction = n->last_out - s;
            n->correction_left = 128u;          /* one audio half, about 2.9 ms */
            n->transition_pending = 0;
        }
        if (n->correction_left) {
            /* Keep the original correction immutable and apply an exact
             * 128-sample linear ramp.  This removes a signed divide from the
             * hot path and avoids the rounding residue of the recurrence. */
            s += mulq16(n->correction, (uint32_t)n->correction_left << 9);
            n->correction_left--;
        }
        if (n->ending) {
            if (n->tail_left) {
                s = mulq16(s, (uint32_t)n->tail_left << 8);
                n->tail_left--;
            } else
                s = 0;                                  /* do not restart the final-level waveform before done() */
        }
        n->last_out = s;
        out[i] += (int32_t)(((int64_t)s * amp * VOICE_FS) >> 30);
        amp += damp;
        for (k = 0; k < 4u; k++) {
            uint32_t step = inc_delta[k];
            inc_error[k] += inc_rem[k];
            if (len > 1u && inc_error[k] >= len - 1u) {
                inc_error[k] -= len - 1u;
                step++;
            }
            inc_cur[k] = inc_down[k] ? inc_cur[k] - step : inc_cur[k] + step;
            am_cur[k] += am_step[k];
        }
    }
}

/* The A/B implementation keeps the original sample renderer above intact.
 * It shares the same patch and per-note state but schedules each operator as
 * one 32-sample block instead of making four function calls per sample. */
#include "eng_rdx_opt.c"

static void rdx_render(track_t *t, voice_t *v, int32_t *out, uint32_t len, const vmod_t *m)
{
    if (rdx_type_get())
        rdx_render_optimized(t, v, out, len, m);
    else
        rdx_render_origin(t, v, out, len, m);
}

static const char *const N_RDX_ALG[] = {
    "PAT", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", 0
};
#define RDX_NAME(I) RDX_PATCHES[(I)].name,
static const char *const N_RDX_PATCH[] = { RDX_FOR_EACH_PATCH(RDX_NAME) 0 };
#undef RDX_NAME

#define RDX_PR(I) {RDX_PATCHES[(I)].name, {0, 0, 0, 0, 0, 0, 0, (I)}, {0, 0, 127, 0}, 0, 0, FX(0, 0, 0, 0)},
static const preset_t RDX_PRESETS[] = {
    RDX_FOR_EACH_PATCH(RDX_PR)
};
#undef RDX_PR

static const engine_t ENG_RDX = {
    .name = "RDX",
    .page_title = {"VOICE", "MACRO"},
    .edit = {
        {"ALG", F_INT, 0, 12, 0, N_RDX_ALG, 0},
        {"FB", F_BIPCT, -64, 63, 0, 0, 0},
        {"MLVL", F_BIPCT, -64, 63, 0, 0, 0},
        {"MRAT", F_INT, -16, 16, 0, 0, 0},
        {"MEG", F_BIPCT, -64, 63, 0, 0, 0},
        {"VELO", F_BIPCT, -64, 63, 0, 0, 0},
        {"DTUN", F_PCT, 0, 127, 0, 0, 0},
        {"PATCH", F_ENUM, RDX_FIRST_VISIBLE, RDX_NPATCHES - 1, RDX_FIRST_VISIBLE, N_RDX_PATCH, 0},
    },
    .presets = RDX_PRESETS,
    .npresets = NELEM(RDX_PRESETS),
    .note_on = rdx_note_on,
    .render = rdx_render,
    .knob = {P_E2, P_E3, P_E1, P_TRANS},
    .poly = RDX_POLY,
    .ownenv = 1,
    .done = rdx_done,
    .force_stop = rdx_force_stop,
    .mem = sizeof(rdx_note_t) * RDX_POLY,
};
