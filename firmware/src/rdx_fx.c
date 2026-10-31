/* SPDX-License-Identifier: GPL-3.0-or-later
 * The Reface DX has two serial effect slots.  This file is included by fx.c
 * after Felucca's INSERT implementation, so its inexpensive fixed-point drive,
 * phaser and flanger building blocks can be shared.
 *
 * Delay reuses Felucca's native full-rate feedback-delay core and its complete
 * 65536-sample line.  A Reface DX is monotimbral, so one sounding RDX Part owns
 * that line at a time; this preserves the full 1.486 s range and the serial
 * FX1 -> FX2 position without the dull FS / 8 approximation.  Chorus uses
 * JIANT's three-voice stereo ensemble.  RDX HALL uses the fixed-point port of Clouds'
 * Dattorro/Griesinger topology, and Felucca's older native Hall is excluded
 * from RDX builds.  The chain is stereo: the mono FM mix is duplicated at its
 * input and every slot receives both channels from the preceding slot. */
#if FELUCCA_RDX

#define RFX_NSLOT (NTRK * 2u)
/* RDX's measured Delay table tops out at 52964 samples.  The unused end of
 * Felucca's 65536-sample line hosts the two physical slots' PicoFace stereo
 * modulation lines, avoiding another large SRAM allocation. */
#define RFX_PF_FL_LEN 512u
#define RFX_PF_FL_MASK (RFX_PF_FL_LEN - 1u)
#define RFX_PF_PH_LEN 2048u
#define RFX_PF_PH_MASK (RFX_PF_PH_LEN - 1u)
#define RFX_PF_PH_WORDS (2u * 2u * RFX_PF_PH_LEN)
#define RFX_PF_FL_WORDS (2u * 2u * RFX_PF_FL_LEN)
#define RFX_DLY_LEN (DLY_LEN - RFX_PF_PH_WORDS - RFX_PF_FL_WORDS)
#define RFX_PF_PH_LINE(slot, ch) (&dly_buf[RFX_DLY_LEN + (((slot) & 1u) * 2u + ((ch) & 1u)) * RFX_PF_PH_LEN])
#define RFX_PF_FL_LINE(slot, ch) (&dly_buf[RFX_DLY_LEN + RFX_PF_PH_WORDS + (((slot) & 1u) * 2u + ((ch) & 1u)) * RFX_PF_FL_LEN])
_Static_assert(RFX_DLY_LEN > 52964u, "RDX Delay and PicoFace modulation lines overlap");

/* Tenths of a millisecond.  This is the published community measurement of
 * all 128 Reface DX Delay TIME positions, rather than a guessed curve. */
static const uint16_t RFX_DELAY_T10[128] = {
    116,199,280,362,443,525,606,688,770,852,933,1015,1096,1178,1259,1341,
    1423,1504,1586,1667,1749,1830,1912,1993,2074,2156,2237,2319,2401,2482,2564,2646,
    2728,2809,2890,2972,3053,3135,3216,3297,3379,3460,3542,3623,3704,3786,3867,3949,
    4030,4112,4193,4275,4357,4438,4520,4602,4683,4794,4905,5016,5127,5237,5348,5459,
    5570,5672,5774,5877,5979,6081,6183,6285,6388,6489,6591,6692,6794,6895,6997,7098,
    7200,7303,7406,7509,7613,7716,7819,7922,8025,8127,8229,8331,8433,8534,8636,8738,
    8840,8943,9045,9148,9250,9353,9455,9558,9660,9763,9865,9968,10070,10173,10275,10378,
    10480,10582,10684,10786,10889,10991,11093,11195,11297,11399,11501,11603,11706,11808,11910,12010
};

typedef struct {
    ins_t ins[2];                           /* independent L/R drive / swept states */
    int32_t wah_env[2], wah_ic1[2], wah_ic2[2];
    int32_t ds_acc, ds_out, ds_target, ds_lp;
    uint32_t dist_gain_q16[2];
    uint32_t pf_ph, pf_aux_ph;              /* PicoFace phaser and its short-delay LFO */
    uint32_t pf_alpha[2];                   /* two all-pass coefficients, Q16 */
    int32_t pf_fl_fb_lp[2];                 /* damped RDX Flanger feedback, quarter-scale */
    uint16_t pf_fl_wp, pf_fl_fill, pf_ph_wp, pf_ph_fill;
    uint16_t ds_wp, ds_fill;
    uint16_t rv_comb_i[4], rv_ap_i[2], rv_sp_w;
    int32_t rv_comb_lp[4], rv_sp_lp, rv_sp_hp, rv_sp_he, rv_sp_size;
    uint32_t rv_sp_ph;
    uint8_t ds_phase;
    uint8_t type, send, p1, p2, p3, p4, model, valid;
} rdx_fx_slot_t;

typedef struct {
    rdx_fx_slot_t slot[2];
    uint16_t tail;
} rdx_fx_track_t;

static rdx_fx_track_t rdx_fx[NTRK] __attribute__((section(".pool")));
static uint8_t rfx_delay_owner = 0xFFu;       /* full-rate dly_buf owner: track * 2 + slot */
/* Large delay stores must never be memset from the 2.9 ms audio deadline.
 * The ISR only sets one of these bits and bypasses that wet engine until the
 * main loop has cleared it.  Bits 0..1: native ROOM/SPRING, 2..3: HALL,
 * 4..5: JIANT Chorus, one bit per physical Reface slot. */
static volatile uint8_t rfx_clear_pending;
#define RFX_CLEAR_NATIVE(S) (1u << ((S) & 1u))
#define RFX_CLEAR_HALL(S) (1u << (2u + ((S) & 1u)))
#define RFX_CLEAR_CHORUS(S) (1u << (4u + ((S) & 1u)))
static struct {
    int32_t l[CTL], r[CTL], in[CTL], wet[CTL], side[CTL];
} rfx_work __attribute__((section(".pool")));

/* JIANT's three-voice ensemble gets one line per physical Reface slot, so FX1
 * and FX2 can both use CHORUS without sharing delay history. */
typedef struct {
    int16_t buf[CHO_LEN];
    uint32_t w, ph;
    int32_t depth_cur;
    uint8_t primed;
} rfx_chorus2_t;
static rfx_chorus2_t rfx_chorus2[RFX_NSLOT] __attribute__((section(".pool")));

/* Pure serial effect costs over one complete 128-frame I2S half.  Each timed
 * region includes input preparation, the effect kernel and stereo return,
 * but no RDX voice rendering.  HALL3 is the sole retained HALL algorithm. */
enum { RDX_FXPERF_HALL3, RDX_FXPERF_CHORUS2, RDX_FXPERF_N };
static struct {
    volatile uint32_t avg_q8;
    volatile uint16_t peak_us;
    volatile uint8_t valid;
    uint32_t half_ticks;
    uint8_t half_seen;
} rdx_fx_perf[RDX_FXPERF_N];

#if defined(FM1_TICKS_PER_US)
#define RDX_FX_PERF_TIMER 1
#else
#define RDX_FX_PERF_TIMER 0
#endif

static void rdx_fx_perf_half_begin(void)
{
    uint32_t i;
    for (i = 0; i < RDX_FXPERF_N; i++) {
        rdx_fx_perf[i].half_ticks = 0;
        rdx_fx_perf[i].half_seen = 0;
    }
}

#if RDX_FX_PERF_TIMER
static void rdx_fx_perf_add(uint32_t kind, uint32_t ticks)
{
    if (kind >= RDX_FXPERF_N)
        return;
    rdx_fx_perf[kind].half_ticks += ticks;
    rdx_fx_perf[kind].half_seen = 1;
}
#endif

static void rdx_fx_perf_half_end(void)
{
#if RDX_FX_PERF_TIMER
    uint32_t i;
    for (i = 0; i < RDX_FXPERF_N; i++) {
        uint32_t us, q8;
        if (!rdx_fx_perf[i].half_seen)
            continue;
        us = (rdx_fx_perf[i].half_ticks + FM1_TICKS_PER_US / 2u) / FM1_TICKS_PER_US;
        q8 = us << 8;
        if (!rdx_fx_perf[i].valid) {
            rdx_fx_perf[i].avg_q8 = q8;
            rdx_fx_perf[i].valid = 1;
        } else
            rdx_fx_perf[i].avg_q8 = (rdx_fx_perf[i].avg_q8 * 31u + q8 + 16u) >> 5;
        if (us > rdx_fx_perf[i].peak_us)
            rdx_fx_perf[i].peak_us = (uint16_t)(us > 65535u ? 65535u : us);
    }
#endif
}

/* The lines above use quarter-scale int16 storage: one eighth of PicoFace's
 * float scratch footprint while retaining the original delay ranges. */
#if defined(__EMSCRIPTEN__)
static uint8_t rdx_fx_test_bypass;                     /* engine-only browser regression */
#endif

static uint32_t rfx_slot_index(const track_t *t, uint32_t slot)
{
    return ((uint32_t)(t - trk) % NTRK) * 2u + (slot & 1u);
}

static uint32_t rfx_slot_set(rdx_fx_slot_t *s, uint32_t type, uint32_t send,
                             uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4,
                             uint32_t model)
{
    uint32_t changed = !s->valid || s->type != type || s->model != model ||
                       (type == RFX_REVERB && s->p1 != p1) ||
                       (type == RFX_FLANGER && ((s->p1 == 0u) != (p1 == 0u)));
    if (changed) {
        memset(s, 0, sizeof *s);
        s->valid = 1;
    }
    s->type = (uint8_t)type;
    s->send = (uint8_t)send;
    s->p1 = (uint8_t)p1;
    s->p2 = (uint8_t)p2;
    s->p3 = (uint8_t)p3;
    s->p4 = (uint8_t)p4;
    s->model = (uint8_t)model;
    return changed;
}

/* Each physical Reface slot owns one complete Felucca HALL.  Slot 0 reuses
 * the firmware's original buffers; slot 1 uses the second instance above. */
static void rfx_native_reverb_clear(uint32_t slot)
{
    int16_t *comb = slot ? rdx_rev_comb2 : rev_comb;
    int16_t *ap = slot ? rdx_rev_u2.ap : rev_ap;
    memset(comb, 0, REV_COMB_LEN * sizeof *comb);
    memset(ap, 0, REV_AP_LEN * sizeof *ap);
}

static uint32_t rfx_reverb_clear_bit(uint32_t slot, uint32_t model)
{
    return model >= 2u ? RFX_CLEAR_HALL(slot) : RFX_CLEAR_NATIVE(slot);
}

static void rfx_reverb_clear_later(uint32_t slot, uint32_t model)
{
    rfx_clear_pending |= (uint8_t)rfx_reverb_clear_bit(slot, model);
}

/* Main-loop maintenance.  Clear at most one store per call so browsing does
 * not stall the UI for several milliseconds when both slots change together.
 * An audio interrupt may pre-empt this memset; its pending bit remains set,
 * so that engine is bypassed and cannot read a half-cleared delay line. */
static void rdx_fx_maintenance(void)
{
    uint32_t slot, bit;
    for (slot = 0; slot < 2u; slot++) {
        bit = RFX_CLEAR_HALL(slot);
        if (rfx_clear_pending & bit) {
            rdx_clouds_clear(slot);
            rfx_clear_pending &= (uint8_t)~bit;
            return;
        }
        bit = RFX_CLEAR_NATIVE(slot);
        if (rfx_clear_pending & bit) {
            rfx_native_reverb_clear(slot);
            rfx_clear_pending &= (uint8_t)~bit;
            return;
        }
        bit = RFX_CLEAR_CHORUS(slot);
        if (rfx_clear_pending & bit) {
            memset(&rfx_chorus2[slot], 0, sizeof rfx_chorus2[slot]);
            rfx_clear_pending &= (uint8_t)~bit;
            return;
        }
    }
}

/* PicoFaceDX's one-pole TONE coefficient, Q16.  Its cutoff is linear from
 * 300 Hz to 8 kHz: a = 1 - exp(-2 pi fc / 44100). */
static const uint16_t RFX_DIST_TONE_Q16[128] = {
    2742,3282,3818,4349,4875,5397,5914,6427,6935,7439,7939,8434,
    8925,9412,9895,10374,10848,11318,11785,12247,12705,13160,13610,14057,
    14500,14939,15374,15805,16233,16657,17077,17494,17908,18317,18723,19126,
    19525,19921,20313,20702,21088,21470,21849,22225,22597,22967,23333,23696,
    24056,24412,24766,25117,25465,25809,26151,26490,26825,27158,27488,27816,
    28140,28462,28781,29097,29410,29721,30029,30334,30637,30937,31235,31530,
    31822,32112,32400,32685,32967,33248,33525,33801,34074,34344,34613,34878,
    35142,35404,35663,35920,36174,36427,36677,36926,37172,37416,37657,37897,
    38135,38371,38604,38836,39066,39293,39519,39743,39965,40185,40403,40619,
    40833,41046,41256,41465,41672,41877,42081,42283,42483,42681,42877,43072,
    43266,43457,43647,43835,44022,44207,44390,44572
};

static uint32_t rfx_dist_drive_q12(uint32_t drive)
{
    uint32_t d = drive & 127u;
    return 4096u + (d * d * 98304u + 8064u) / 16129u;  /* 1 + 24 (d / 127)^2 */
}

/* Keep the perceived level predictable instead of inheriting PicoFaceDX's
 * erroneous fixed 0.4 makeup.  DRIVE 0 is a true bypass; 100 reaches +30%,
 * and the last 27 parameter steps add saturation without another level jump. */
static uint32_t rfx_dist_target_q16(uint32_t drive)
{
    uint32_t d = drive > 100u ? 100u : drive;
    return 65536u + (d * 19661u + 50u) / 100u;
}

static inline int32_t rfx_dist_cubic(int32_t x)
{
    int32_t x2, x3;
    x = clamp(x, -32768, 32768);
    x2 = (x * x + 16384) >> 15;
    x3 = sh_tz(x2 * x, 15);
    return (3 * x - x3) / 2;                           /* 1.5 (x - x^3 / 3) */
}

/* Fixed-point PicoFaceDX Distortion: quadratic pre-gain, cubic saturator,
 * 50/50 wet/dry blend and one-pole tone.  A slow block-level compensation
 * replaces PicoFaceDX's faulty fixed 0.4 multiplier: it preserves the input's
 * mean absolute level, then applies the deliberate 1.00 .. 1.30 level curve. */
static __attribute__((noinline)) void rfx_dist(rdx_fx_slot_t *s, int32_t *b, uint32_t n, uint32_t ch)
{
    uint32_t i;
    uint32_t in_level = 0, out_level = 0;
    ins_t *q = &s->ins[ch & 1u];
    int32_t g = (int32_t)rfx_dist_drive_q12(s->p1);
    uint32_t tone = RFX_DIST_TONE_Q16[s->p2 & 127u];
    uint32_t target = rfx_dist_target_q16(s->p1), gain;
    int32_t xmax = (32768 * 4096) / g;

    if (!s->p1) {                                      /* DRIVE 0: exact dry bypass */
        s->dist_gain_q16[ch & 1u] = 65536u;
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t dry = clamp(b[i], -262143, 262143);
        int32_t driven = (clamp(dry, -xmax, xmax) * g) >> 12;
        int32_t wet = rfx_dist_cubic(driven);
        int32_t mixed = (wet + dry) >> 1;               /* PicoFaceDX dryMix_ = 0.5 */
        in_level += (uint32_t)(dry < 0 ? -dry : dry);
        q->lp1 += mulq16(mixed - q->lp1, tone);
        b[i] = q->lp1;
        out_level += (uint32_t)(b[i] < 0 ? -b[i] : b[i]);
    }
    if (in_level && out_level) {
        uint32_t wanted;
        while (in_level > 32767u) {                    /* preserve ratio, keep product 32-bit */
            in_level = (in_level + 1u) >> 1;
            out_level = (out_level + 1u) >> 1;
        }
        wanted = (in_level * target + out_level / 2u) / out_level;
        wanted = (uint32_t)clamp((int32_t)wanted, 8192, 98304); /* 0.125x .. 1.5x */
        if (!s->dist_gain_q16[ch & 1u])
            s->dist_gain_q16[ch & 1u] = wanted;
        else
            s->dist_gain_q16[ch & 1u] = (uint32_t)((7u * s->dist_gain_q16[ch & 1u] + wanted + 4u) >> 3);
    }
    gain = s->dist_gain_q16[ch & 1u] ? s->dist_gain_q16[ch & 1u] : 65536u;
    for (i = 0; i < n; i++) {
        int32_t y = gain <= 65536u ? mulq16(b[i], gain) :
                    b[i] + mulq16(b[i], gain - 65536u);
        uint32_t level = ((uint32_t)s->p3 * 32768u + 50u) / 100u;
        y = level <= 32768u ? mulq15(y, (int32_t)level) : y + mulq15(y, (int32_t)level - 32768);
        b[i] = clamp(y, -262143, 262143);
    }
}

/* Envelope-following wah.  SENS controls the upward envelope excursion and
 * REZ the SVF resonance.  SELECT supplies the manual centre position. */
static __attribute__((noinline)) void rfx_wah(rdx_fx_slot_t *s, int32_t *b, uint32_t n,
                                               uint32_t pos, uint32_t ch)
{
    uint32_t i;
    uint32_t cch = ch & 1u;
    int32_t env = s->wah_env[cch], sens = s->p1;
    int32_t e = env > 65535 ? 65535 : env;
    int32_t cut = (10 + (int32_t)pos * 55 / 127 + (e * sens * 50 >> 23)) << 8;
    tsvf_t c;
    tsvf_coef(&c, cut, s->p2);
    for (i = 0; i < n; i++) {
        int32_t x = b[i], a = x < 0 ? -x : x, v3, bp, lp, y;
        a = a > 131071 ? 131071 : a;
        env += a > env ? (a - env) >> 3 : (a - env) >> 9;
        v3 = clamp(x, -150000, 150000) - s->wah_ic2[cch];
        bp = (c.a1 * s->wah_ic1[cch] + c.a2 * v3) >> 13;
        lp = s->wah_ic2[cch] + ((c.a2 * s->wah_ic1[cch] + c.a3 * v3) >> 13);
        s->wah_ic1[cch] = clamp(2 * bp - s->wah_ic1[cch], -150000, 150000);
        s->wah_ic2[cch] = clamp(2 * lp - s->wah_ic2[cch], -150000, 150000);
        y = clamp(lp + bp + (bp >> 1), -180000, 180000);
        b[i] = (x >> 2) + (y >> 1) + (y >> 2);          /* 25 % dry, 75 % wah */
    }
    s->wah_env[cch] = env;
}

/* Felucca's four-stage phaser and 256-sample flanger.  Chorus uses Felucca's
 * native parallel bus below instead.  Reface has DEPTH / RATE only, so
 * feedback is a conservative fixed part of each remaining model. */
static __attribute__((noinline)) void rfx_swept_felucca(rdx_fx_slot_t *s, int32_t *b,
                                                        uint32_t n, uint32_t ch)
{
    ins_t *q = &s->ins[ch & 1u];
    uint32_t i, inc = LFO_INC[s->p2 & 127u] / CTL, dep = s->p1;
    if (s->type == RFX_PHASER) {
        uint32_t tr = pf_tri(q->ph + (n >> 1) * inc);
        uint32_t z = 32768u + (uint32_t)(((int32_t)tr - 32768) * (int32_t)dep / 127);
        int32_t a = PH_A[z >> 11] + (((PH_A[(z >> 11) + 1u] - PH_A[z >> 11]) * (int32_t)(z & 2047u)) >> 11);
        int32_t fbk = 32 * 180;
        for (i = 0; i < n; i++) {
            int32_t x = b[i], xs = clamp((x + 4) >> 3, -16383, 16383);
            int32_t u = xs + ((q->fb * fbk + 16384) >> 15), y = u;
            uint32_t st;
            for (st = 0; st < PH_ST; st++) {
                y = clamp(q->px[st] + ((a * (u - q->py[st]) + 8192) >> 14), -65535, 65535);
                q->px[st] = u;
                q->py[st] = y;
                u = y;
            }
            q->fb = clamp(y, -32767, 32767);
            y <<= 2;
            q->dc += ((y << 8) - q->dc) >> 9;
            b[i] = (x >> 1) + y - (q->dc >> 8);
        }
        q->ph += n * inc;
        return;
    }
    {
        int32_t fbk = 46 * 219;
        uint32_t dq = dep * 516u;
        for (i = 0; i < n; i++, q->ph += inc) {
            int32_t x = b[i], r0, r1, v, fr;
            uint32_t d;
            uint32_t tr = pf_tri(q->ph), sw = (tr * tr) >> 16;
            d = (11u << 16) + 209u * (((sw >> 1) * dq) >> 15);
            r0 = (int32_t)(q->wp - (d >> 16)) & (int32_t)(IL_LEN - 1u);
            r1 = (r0 - 1) & (int32_t)(IL_LEN - 1u);
            fr = (int32_t)((d >> 1) & 0x7FFFu);
            v = q->line[r0] + (((q->line[r1] - q->line[r0]) * fr) >> 15);
            q->line[q->wp] = (int16_t)clamp((x >> 2) + softclip(mulq15(v, fbk)), -32767, 32767);
            q->wp = (uint16_t)((q->wp + 1u) & (IL_LEN - 1u));
            v <<= 1;
            q->dc += ((v << 8) - q->dc) >> 9;
            b[i] = (x >> 1) + v - (q->dc >> 8);
        }
    }
}

#include "rdx_fx_picoface.c"

/* PicoFaceDX's Reface Delay macro.  The serial insert always receives its
 * input at unity.  DEPTH chooses 10..25% wet (with 0 as bypass) and 0..50%
 * feedback.  The overview SEND card is temporarily a 0.1x..5.0x multiplier
 * on that final wet mix, encoded directly as tenths (10 = 1.0x). */
static uint32_t rfx_delay_feedback(uint32_t depth)
{
    return (depth & 127u) * 64u / 127u;
}

static uint32_t rfx_delay_mix_q15(uint32_t depth, uint32_t ratio10)
{
    uint32_t mix;
    depth &= 127u;
    if (!depth)
        return 0u;
    ratio10 = (uint32_t)clamp((int32_t)ratio10, 1, 50);
    /* 0.10 + 0.15 * DEPTH / 127, matching FxDelay::setFbParam(). */
    mix = 3277u + (depth * 4915u + 63u) / 127u;
    mix = (mix * ratio10 + 5u) / 10u;
    return mix > 32767u ? 32767u : mix;
}

static uint32_t rfx_delay_samples(uint32_t time)
{
    uint32_t dl = ((uint32_t)RFX_DELAY_T10[time & 127u] * FS + 5000u) / 10000u;
    return dl < 1u ? 1u : dl >= RFX_DLY_LEN ? RFX_DLY_LEN - 1u : dl;
}

#define RFX_DLY_COLOR 70u                    /* Felucca's warm default */

static void rfx_delay_state_clear(rdx_fx_slot_t *s)
{
    s->ds_acc = s->ds_out = s->ds_target = s->ds_lp = 0;
    s->ds_wp = s->ds_fill = 0;
    s->ds_phase = 0;
}

static __attribute__((noinline)) void rfx_delay(rdx_fx_slot_t *s, int32_t *b, uint32_t n, uint32_t si)
{
    int16_t *line = dly_buf;
    uint32_t i;
    uint32_t dl = rfx_delay_samples(s->p2);
    int32_t mix = (int32_t)rfx_delay_mix_q15(s->p1, s->send);
    int32_t fb = (int32_t)s->p3 * 258;
    int32_t col = 2000 + (int32_t)s->p4 * 240;
    if (si != rfx_delay_owner || !mix)
        return;
    for (i = 0; i < n; i++) {
        int32_t x = b[i];
        uint32_t rp = s->ds_wp >= dl ? s->ds_wp - dl : s->ds_wp + RFX_DLY_LEN - dl;
        int32_t o = s->ds_fill >= dl ? line[rp] : 0;
        s->ds_lp += mulq15(o - s->ds_lp, col);
        line[s->ds_wp] =
            (int16_t)clamp((x >> 1) + mulq15(s->ds_lp, fb), -32768, 32767);
        if (++s->ds_wp >= RFX_DLY_LEN)
            s->ds_wp = 0;
        if (s->ds_fill < dl)
            s->ds_fill++;
        b[i] = mulq15(x, 32767 - mix) + mulq15(o << 1, mix);
    }
}

/* Reface Depth is much larger numerically than Felucca's old send setting.
 * The requested mapping is deliberately simple: rounded Depth / 2.5. */
static uint32_t rfx_reverb_send(uint32_t depth)
{
    depth &= 127u;
    return (depth * 2u + 2u) / 5u;                     /* Reface DEPTH / 2.5, rounded */
}

/* Reface TIME values around 60 sound too short when copied directly to the
 * native HALL.  Stretch them by 1.5 and saturate at Felucca's 0 .. 127 SIZE. */
static uint32_t rfx_reverb_size(uint32_t time)
{
    uint32_t size = (time & 127u) * 3u / 2u;
    return size > 127u ? 127u : size;
}

/* Parameterised copies of Felucca's ROOM and SPRING kernels.  The arithmetic
 * and delay lengths are unchanged; only their state is moved into an RDX slot
 * so FX1 and FX2 can select different models without sharing indices. */
static __attribute__((noinline)) void rfx_rev_room(rdx_fx_slot_t *s, const int32_t *in,
                                                   int32_t *out, uint32_t n,
                                                   int16_t *comb, int16_t *ap,
                                                   int32_t size_v, int32_t damp_v)
{
    uint32_t i, k;
    int32_t size = 25000 + size_v * 50, damp = 32767 - damp_v * 200;
    for (i = 0; i < n; i++) {
        int32_t a = 0;
        int16_t *c = comb;
        int32_t x = mulq15(in[i], 2580);
        for (k = 0; k < 4u; k++) {
            int32_t o = c[s->rv_comb_i[k]];
            s->rv_comb_lp[k] = o + mulq15(s->rv_comb_lp[k] - o, 32767 - damp);
            c[s->rv_comb_i[k]] = (int16_t)clamp(x + mulq15(s->rv_comb_lp[k], size), -32768, 32767);
            if (++s->rv_comb_i[k] >= REV_COMB[k]) s->rv_comb_i[k] = 0;
            a += o;
            c += REV_COMB[k];
        }
        c = ap;
        for (k = 0; k < 2u; k++) {
            int32_t o = c[s->rv_ap_i[k]], v = a + (o >> 1);
            c[s->rv_ap_i[k]] = (int16_t)clamp(v, -32768, 32767);
            a = o - a;
            if (++s->rv_ap_i[k] >= REV_AP[k]) s->rv_ap_i[k] = 0;
            c += REV_AP[k];
        }
        out[i] += a;
    }
}

static __attribute__((noinline)) void rfx_rev_spring(rdx_fx_slot_t *s, const int32_t *in,
                                                     int32_t *out, uint32_t n,
                                                     int16_t *line, int32_t *ap,
                                                     int32_t size_v, int32_t damp_v)
{
    uint32_t i, k, sz = (uint32_t)size_v;
    int32_t g = 19661 + (int32_t)sz * 85;
    int32_t kl = 26000 - damp_v * 160;
    int32_t len = (int32_t)(1323u + ((sz * 1323u) >> 7)) << 8, L, L2, L3, f, w;
    if (!s->rv_sp_size) s->rv_sp_size = len;
    s->rv_sp_size += clamp(len - s->rv_sp_size, -256, 256);
    L = s->rv_sp_size >> 8;
    s->rv_sp_ph += 2u * LFO_INC[24];
    w = (s->rv_sp_size >> 1) + ((osc_sine(s->rv_sp_ph) * 3) >> 8);
    L2 = w >> 8; f = w & 255; L3 = (L * 3) >> 2;
    for (i = 0; i < n; i++) {
        uint32_t wp = s->rv_sp_w, j = (wp & 3u) * (SP_N + 1u);
        int32_t x = mulq15(in[i], 2580), r = line[(wp - (uint32_t)L) & SP_MASK], p, o;
        int32_t t0 = line[(wp - (uint32_t)L2) & SP_MASK];
        int32_t t1 = line[(wp - (uint32_t)L2 - 1u) & SP_MASK];
        s->rv_sp_lp += mulq15(r - s->rv_sp_lp, kl);
        o = s->rv_sp_lp * g;
        x += (o + ((o >> 31) & 32767)) >> 15;
        o = x - s->rv_sp_hp + s->rv_sp_he;
        s->rv_sp_he = o & 63;
        s->rv_sp_hp += o >> 6;
        x -= s->rv_sp_hp;
        p = ap[j]; ap[j] = x;
        for (k = 1; k <= SP_N; k++) {
            int32_t v = (x - ap[j + k]) * SP_A;
            o = ap[j + k];
            x = ((v + ((v >> 31) & 4095)) >> 12) + p;
            p = o; ap[j + k] = x;
        }
        line[wp & SP_MASK] = (int16_t)clamp(x, -32768, 32767);
        s->rv_sp_w = (uint16_t)(wp + 1u);
        out[i] += (t0 + (((t1 - t0) * f) >> 8)) * 4 + line[(wp - (uint32_t)L3) & SP_MASK] * 2;
    }
}

#define RDX_REVERB_FADE_SAMPLES 512u
static __attribute__((noinline)) void rfx_reverb_serial(rdx_fx_slot_t *s, uint32_t slot,
                                                         int32_t *l, int32_t *r, uint32_t n,
                                                         uint32_t tail_blocks)
{
    int16_t *comb = slot ? rdx_rev_comb2 : rev_comb;
    int16_t *ap = slot ? rdx_rev_u2.ap : rev_ap;
    uint32_t i, value = s->send;
    int32_t send, xmax, size = (int32_t)rfx_reverb_size(s->p3), damp = s->p4;
    int32_t amount;
    if (!value || (rfx_clear_pending & rfx_reverb_clear_bit(slot, s->p1)))
        return;
    send = (int32_t)value * 258;                       /* Felucca send 0 .. 42, Q15 */
    xmax = 0x7FFFFFFF / send;
    for (i = 0; i < n; i++) {
        int32_t mid = (l[i] >> 1) + (r[i] >> 1);
        rfx_work.in[i] = mulq15(clamp(mid, -xmax, xmax), send);
        rfx_work.wet[i] = 0;
        rfx_work.side[i] = 0;
    }
    if (s->p1 == 0u)
        rfx_rev_room(s, rfx_work.in, rfx_work.wet, n, comb, ap, size, damp);
    else if (s->p1 == 1u)
        rfx_rev_spring(s, rfx_work.in, rfx_work.wet, n, comb,
                       slot ? rdx_rev_u2.sp : rev_u.sp, size, damp);
    else
        rdx_clouds_process_h3_entry(slot, rfx_work.in, rfx_work.wet, rfx_work.side,
                                    n, s->p3, s->p4);
    rdx_rev_size = (uint8_t)size;
    amount = (int32_t)s->p2 * 258;
    for (i = 0; i < n; i++) {
        /* DEPTH is outside the reverb feedback loop, so its one-LSB rounding
         * cannot sustain a limit cycle.  Use the cheaper original multiply;
         * HALL keeps corrected rounding inside its Q14 tank. */
        int32_t wet = s->p1 >= 2u ? rdx_cld_mul(rfx_work.wet[i], amount) :
                                    mulq15(rfx_work.wet[i], amount);
        int32_t side = s->p1 >= 2u ? rdx_cld_mul(rfx_work.side[i], amount) : 0;
        /* The common tail counter used to stop on a non-zero noise floor.  A
         * HALL return always approaches that boundary over 512 samples, so
         * the final block reaches effectively zero before processing stops. */
        if (s->p1 >= 2u && tail_blocks && tail_blocks * CTL <= RDX_REVERB_FADE_SAMPLES) {
            uint32_t left = tail_blocks * CTL;
            int32_t fade_q15 = (int32_t)((left > i ? left - i : 0u) *
                                         (32768u / RDX_REVERB_FADE_SAMPLES));
            wet = rdx_cld_mul_tz(wet, fade_q15);
            side = rdx_cld_mul_tz(side, fade_q15);
        }
        l[i] += wet + side;
        r[i] += wet - side;
    }
}

static uint8_t rdx_cho_depth = 60u, rdx_cho_rate = 40u;

/* JIANT's chorus ensemble: three linearly interpolated taps, separated by
 * 120 degrees of LFO phase.  The 300 / 420 / 540-sample bases are roughly
 * 6.8 / 9.5 / 12.2 ms at 44.1 kHz.  The first two taps form the stereo
 * difference while all three form a unity-gain wet centre. */
static inline int32_t rfx_chorus2_tap(const rfx_chorus2_t *q, int32_t delay_q8)
{
    uint32_t ri = (uint32_t)delay_q8 >> 8;
    int32_t f = delay_q8 & 255;
    int32_t a = q->buf[(q->w - ri) & (CHO_LEN - 1u)];
    int32_t b = q->buf[(q->w - ri - 1u) & (CHO_LEN - 1u)];
    return a + (((b - a) * f) >> 8);
}

static inline int32_t rfx_chorus2_ensemble(const rfx_chorus2_t *q, int32_t depth,
                                            int32_t *side)
{
    int32_t a = rfx_chorus2_tap(q, (300 << 8) +
                                (((osc_sine(q->ph) + 32768) * depth) >> 8));
    int32_t b = rfx_chorus2_tap(q, (420 << 8) +
                                (((osc_sine(q->ph + 0x55555555u) + 32768) * depth) >> 8));
    int32_t c = rfx_chorus2_tap(q, (540 << 8) +
                                (((osc_sine(q->ph + 0xAAAAAAAAu) + 32768) * depth) >> 8));
    *side = a - b;
    return ((a + b + c) * 21845) >> 15;                /* x2 / three taps; line stores input / 2 */
}

static __attribute__((noinline)) void rfx_chorus2_serial(rdx_fx_slot_t *s, uint32_t si,
                                                         int32_t *l, int32_t *r, uint32_t n)
{
    rfx_chorus2_t *q = &rfx_chorus2[si % RFX_NSLOT];
    uint32_t i, inc = LFO_INC[s->p2 & 127u] / CTL;
    int32_t target = (int32_t)(s->p1 >> 1) * 6;         /* same half-depth mapping as retained CHORUS */
    int32_t send = (int32_t)s->send * 258;
    int32_t dstep;
    if (!send)
        return;
    if (!q->primed) {
        q->depth_cur = target;
        q->primed = 1;
    }
    /* JIANT glides a quarter of a parameter change during each block. */
    dstep = ((target - q->depth_cur) >> 2) / (int32_t)n;
    if (!dstep && target != q->depth_cur)
        dstep = target > q->depth_cur ? 1 : -1;
    for (i = 0; i < n; i++) {
        int32_t mid = (l[i] >> 1) + (r[i] >> 1), wet, side;
        int32_t in = mulq15(mid, send);
        q->buf[q->w & (CHO_LEN - 1u)] = (int16_t)clamp(in >> 1, -32768, 32767);
        q->ph += inc;
        if ((dstep > 0 && q->depth_cur < target) || (dstep < 0 && q->depth_cur > target))
            q->depth_cur += dstep;
        wet = rfx_chorus2_ensemble(q, q->depth_cur, &side);
        side = (side * 80) >> 7;                        /* JIANT's default stereo width */
        l[i] += wet + side;
        r[i] += wet - side;
        q->w++;
    }
    rdx_cho_depth = (uint8_t)(target / 6);
    rdx_cho_rate = s->p2;
}

/* Delay keeps Felucca's one full-length mono feedback line.  In a stereo
 * chain it listens to the mid signal and adds the same echo to both channels;
 * any width created by an earlier slot remains in the dry component. */
static __attribute__((noinline)) void rfx_delay_stereo(rdx_fx_slot_t *s, int32_t *l,
                                                       int32_t *r, uint32_t n, uint32_t si)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        rfx_work.in[i] = (l[i] >> 1) + (r[i] >> 1);
    rfx_delay(s, rfx_work.in, n, si);
    for (i = 0; i < n; i++) {
        int32_t mid = (l[i] >> 1) + (r[i] >> 1);
        int32_t echo = rfx_work.in[i] - mid;
        l[i] += echo;
        r[i] += echo;
    }
}

static uint32_t rdx_fx_has_effect(const track_t *t)
{
    return (rdx_fx_type_of(t, 0u) && rdx_fx_send_of(t, 0u)) ||
           (rdx_fx_type_of(t, 1u) && rdx_fx_send_of(t, 1u));
}

/* Select the one full-length delay-line owner before either core renders a
 * Part.  Keep a live owner's tail; otherwise prefer the selected sounding
 * Part, then the first sounding Part.  Other Parts still render their dry
 * serial chain instead of corrupting the shared line. */
static uint32_t rfx_delay_slot_for_track(uint32_t tr)
{
    uint32_t k;
    if (trk[tr].engine != ENGI_RDX)
        return 2u;
    for (k = 0; k < 2u; k++)
        if (rdx_fx_type_of(&trk[tr], k) == RFX_DELAY && rdx_fx_send_of(&trk[tr], k))
            return k;
    return 2u;
}

static uint32_t rfx_track_running(uint32_t tr)
{
    uint32_t v;
    if (rdx_fx[tr].tail)
        return 1u;
    for (v = 0; v < NVOICE; v++)
        if (trk[tr].v[v].active)
            return 1u;
    return 0u;
}

static void rdx_fx_delay_prepare(void)
{
    uint32_t tr, slot, owner = 0xFFu;
    if (rfx_delay_owner < RFX_NSLOT) {
        tr = rfx_delay_owner >> 1;
        slot = rfx_delay_slot_for_track(tr);
        if (rfx_track_running(tr) && slot < 2u && (tr * 2u + slot) == rfx_delay_owner)
            owner = rfx_delay_owner;
    }
    tr = song.sel < NTRK ? song.sel : 0u;
    slot = rfx_delay_slot_for_track(tr);
    if (owner == 0xFFu && rfx_track_running(tr) && slot < 2u)
        owner = tr * 2u + slot;
    if (owner == 0xFFu)
        for (tr = 0; tr < NTRK; tr++) {
            slot = rfx_delay_slot_for_track(tr);
            if (rfx_track_running(tr) && slot < 2u) {
                owner = tr * 2u + slot;
                break;
            }
        }
    if (owner != rfx_delay_owner) {
        rfx_delay_owner = (uint8_t)owner;
        if (owner < RFX_NSLOT)
            rfx_delay_state_clear(&rdx_fx[owner >> 1].slot[owner & 1u]);
    }
}

/* Called immediately after track_render.  It also keeps a bounded tail alive
 * when the last voice has ended; the long delay gaps must not be mistaken for
 * silence before the next repeat arrives. */
static uint32_t rdx_fx_continue(track_t *t, uint32_t voices)
{
    rdx_fx_track_t *f = &rdx_fx[(uint32_t)(t - trk) % NTRK];
    uint32_t k, tail = 0;
    if (!rdx_fx_has_effect(t)) {
        f->tail = 0;
        return 0;
    }
    if (voices) {
        for (k = 0; k < 2u; k++) {
            int16_t *p = rdx_fx_values(t, k);
            uint32_t ty = rdx_fx_type_of(t, k);
            uint32_t v = ty == RFX_DELAY ? 2500u + (uint32_t)p[1] * 55u :
                         ty == RFX_REVERB ? 2500u + rfx_reverb_size((uint32_t)p[2]) * 70u : 64u;
            if (v > tail) tail = v;
        }
        f->tail = (uint16_t)(tail > 12000u ? 12000u : tail);   /* at most 8.7 seconds */
    }
    return f->tail != 0u;
}

static void rfx_blend_insert(rdx_fx_slot_t *s, uint32_t n)
{
    uint32_t i;
    int32_t mix = (int32_t)s->send * 258;
    for (i = 0; i < n; i++) {
        rfx_work.l[i] = rfx_work.in[i] + mulq15(rfx_work.l[i] - rfx_work.in[i], mix);
        rfx_work.r[i] = rfx_work.wet[i] + mulq15(rfx_work.r[i] - rfx_work.wet[i], mix);
    }
}

static __attribute__((noinline)) void rdx_fx_chain(track_t *t, int32_t *b,
                                                   uint32_t n, uint32_t voices)
{
    uint32_t k, tr = (uint32_t)(t - trk) % NTRK;
    rdx_fx_track_t *f = &rdx_fx[tr];
    for (k = 0; k < n; k++) {
        rfx_work.l[k] = b[k];
        rfx_work.r[k] = b[k];
        rfx_work.side[k] = 0;
    }
#if defined(__EMSCRIPTEN__)
    if (rdx_fx_test_bypass)
        return;
#endif
    for (k = 0; k < 2u; k++) {
        int16_t *pv = rdx_fx_values(t, k);
        uint32_t ty = rdx_fx_type_of(t, k);
        uint32_t send = rdx_fx_send_of(t, k);
        uint32_t p1 = (uint32_t)clamp(pv[0], 0, 127);
        uint32_t p2 = (uint32_t)clamp(pv[1], 0, 127);
        uint32_t p3 = (uint32_t)clamp(pv[2], 0, 127);
        uint32_t p4 = (uint32_t)clamp(pv[3], 0, 127);
        uint32_t si = rfx_slot_index(t, k);
        rdx_fx_slot_t *s = &f->slot[k];
        uint32_t old_reverb = s->valid && s->type == RFX_REVERB;
        uint32_t old_chorus = s->valid && s->type == RFX_CHORUS;
        uint32_t old_reverb_model = s->p1;
        uint32_t model = ty == RFX_PHASER ? rdx_phaser_model_get() :
                         ty == RFX_FLANGER ? rdx_flanger_model_get() : 0u;
        uint32_t changed;
        if (ty == RFX_REVERB && p1 > 2u)
            p1 = 2u;                                   /* invalid future/corrupt model -> HALL */
        changed = rfx_slot_set(s, ty, send, p1, p2, p3, p4, model);
        /* Same-model Reverb parameter changes deliberately preserve and morph
         * the existing tail.  Leaving a model schedules only that model's
         * store; the new model was cleared when it was last left. */
        if (changed && old_reverb && (ty != RFX_REVERB || old_reverb_model != p1))
            rfx_reverb_clear_later(k, old_reverb_model);
        if (changed && old_chorus && ty != RFX_CHORUS)
            rfx_clear_pending |= (uint8_t)RFX_CLEAR_CHORUS(si);
        if (!send || ty == RFX_THRU)
            continue;
        if (ty >= RFX_DIST && ty <= RFX_PHASER && ty != RFX_CHORUS) {
            uint32_t i;
            for (i = 0; i < n; i++) {
                rfx_work.in[i] = rfx_work.l[i];
                rfx_work.wet[i] = rfx_work.r[i];
            }
        }
        switch (ty) {
        case RFX_DIST:
            rfx_dist(s, rfx_work.l, n, 0u);
            rfx_dist(s, rfx_work.r, n, 1u);
            break;
        case RFX_WAH:
            rfx_wah(s, rfx_work.l, n, rdx_touch_wah_get(t), 0u);
            rfx_wah(s, rfx_work.r, n, rdx_touch_wah_get(t), 1u);
            break;
        case RFX_CHORUS:
            if (rfx_clear_pending & RFX_CLEAR_CHORUS(si))
                break;
#if RDX_FX_PERF_TIMER
            {
                uint32_t fx_t0 = fm1_ticks();
                rfx_chorus2_serial(s, si, rfx_work.l, rfx_work.r, n);
                rdx_fx_perf_add(RDX_FXPERF_CHORUS2, fm1_ticks() - fx_t0);
            }
#else
            rfx_chorus2_serial(s, si, rfx_work.l, rfx_work.r, n);
#endif
            break;
        case RFX_FLANGER:
            if (!p1)
                break;                                  /* Reface DEPTH zero is a true bypass */
            if (model)
                rfx_flanger_rdx(s, rfx_work.l, rfx_work.r, n, si);
            else {
                rfx_swept_felucca(s, rfx_work.l, n, 0u);
                rfx_swept_felucca(s, rfx_work.r, n, 1u);
            }
            break;
        case RFX_PHASER:
            if (model)
                rfx_phaser_rdx(s, rfx_work.l, rfx_work.r, n, si);
            else {
                rfx_swept_felucca(s, rfx_work.l, n, 0u);
                rfx_swept_felucca(s, rfx_work.r, n, 1u);
            }
            break;
        case RFX_DELAY:
            if (p1)
                rfx_delay_stereo(s, rfx_work.l, rfx_work.r, n, si);
            break;
        case RFX_REVERB:
#if RDX_FX_PERF_TIMER
            {
                uint32_t rv_t0 = fm1_ticks();
                rfx_reverb_serial(s, k, rfx_work.l, rfx_work.r, n, f->tail);
                if (p1 == 2u)
                    rdx_fx_perf_add(RDX_FXPERF_HALL3, fm1_ticks() - rv_t0);
            }
#else
            rfx_reverb_serial(s, k, rfx_work.l, rfx_work.r, n, f->tail);
#endif
            break;
        default: break;
        }
        if (ty >= RFX_DIST && ty <= RFX_PHASER && ty != RFX_CHORUS)
            rfx_blend_insert(s, n);
    }
    for (k = 0; k < n; k++) {
        b[k] = (rfx_work.l[k] >> 1) + (rfx_work.r[k] >> 1);
        rfx_work.side[k] = (rfx_work.l[k] >> 1) - (rfx_work.r[k] >> 1);
    }
    if (!voices && f->tail && !--f->tail) {
        /* Do not leave an old tank parked until the next note, but keep the
         * tens-of-kilobytes memset out of this audio half. */
        for (k = 0; k < 2u; k++)
            if (f->slot[k].valid && f->slot[k].type == RFX_REVERB)
                rfx_reverb_clear_later(k, f->slot[k].p1);
    }
}

/* Per-preset loudness calibration, measured after the complete stereo serial
 * chain.  Keeping it here leaves the amount driving DIST/WAH unchanged.  A
 * zero setting is bit-exact and free.  Split gains above unity into an integer
 * and a Q16 fraction, keeping mulq16's coefficient below 1.0 as required by
 * that helper.  Unlike the old pre-shifted Q12 multiply this preserves quiet
 * samples and cannot turn a non-zero GAIN into a bit crusher. */
static inline int32_t rdx_gain_sample(int32_t sample, uint32_t gain_q16)
{
    uint32_t whole = gain_q16 >> 16;
    uint32_t fraction = gain_q16 & 0xFFFFu;
    return sample * (int32_t)whole + mulq16(sample, fraction);
}

static void rdx_gain_apply(const track_t *t, int32_t *b, uint32_t n)
{
    int32_t db = clamp(t->p[P_E2], -24, 24);
    uint32_t i, gain;
    if (!db)
        return;
    gain = RDX_MASTER_GAIN_Q16[db + 24];
    for (i = 0; i < n; i++) {
        b[i] = rdx_gain_sample(b[i], gain);
        rfx_work.side[i] = rdx_gain_sample(rfx_work.side[i], gain);
    }
}

/* Reconstruct the serial chain's side signal after Part LEVEL.  Its mid has
 * already passed through the remaining mono track path in b. */
static void rdx_fx_side_mix(const track_t *t, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    int32_t lvl;
    if ((pf.mute >> (t - trk)) & 1u)
        return;
    lvl = LEVEL_Q12[t->p[P_LEVEL] & 127u];
    for (i = 0; i < n; i++) {
        int32_t x = ((rfx_work.side[i] >> 2) * lvl) >> 10;
        l[i] += x;
        r[i] -= x;
    }
}

#endif /* FELUCCA_RDX */
