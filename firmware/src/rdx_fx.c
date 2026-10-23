/* SPDX-License-Identifier: GPL-3.0-or-later
 * The Reface DX has two mono effect slots.  This file is included by fx.c
 * after Felucca's INSERT implementation, so its inexpensive fixed-point drive,
 * phaser and flanger building blocks can be shared.  Chorus and Reverb take
 * sends at their slot positions into Felucca's native parallel buses.
 *
 * Delay reuses Felucca's native full-rate feedback-delay core and its complete
 * 65536-sample line.  A Reface DX is monotimbral, so one sounding RDX Part owns
 * that line at a time; this preserves the full 1.486 s range and the serial
 * FX1 -> FX2 position without the dull FS / 8 approximation.  Chorus and HALL
 * also keep Felucca's native kernels, but their wet returns are mixed back into
 * the mono signal at each slot, so the next slot receives dry + wet.  HALL's
 * stereo difference is retained when Reverb is the final active slot. */
#if FELUCCA_RDX

enum {
    RFX_THRU, RFX_DIST, RFX_WAH, RFX_CHORUS,
    RFX_FLANGER, RFX_PHASER, RFX_DELAY, RFX_REVERB, RFX_N
};

#define RFX_NSLOT (NTRK * 2u)
#define RFX_DLY_MASK (DLY_LEN - 1u)
_Static_assert((DLY_LEN & RFX_DLY_MASK) == 0u, "RDX delay line must be a power of two");

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
    ins_t ins;                              /* Felucca drive / swept-effect state */
    int32_t wah_env, wah_ic1, wah_ic2;
    int32_t ds_acc, ds_out, ds_target, ds_lp;
    uint32_t dist_gain_q16;
    uint16_t ds_wp, ds_fill;
    uint8_t ds_phase;
    uint8_t type, p1, p2, valid;
} rdx_fx_slot_t;

typedef struct {
    rdx_fx_slot_t slot[2];
    uint16_t tail;
} rdx_fx_track_t;

static rdx_fx_track_t rdx_fx[NTRK] __attribute__((section(".pool")));
static uint8_t rfx_delay_owner = 0xFFu;       /* full-rate dly_buf owner: track * 2 + slot */
static struct {
    int32_t in[CTL], wet[CTL], side[CTL];
} rfx_work __attribute__((section(".pool")));
#if defined(__EMSCRIPTEN__)
static uint8_t rdx_fx_test_bypass;                     /* engine-only browser regression */
#endif

static uint32_t rfx_slot_index(const track_t *t, uint32_t slot)
{
    return ((uint32_t)(t - trk) % NTRK) * 2u + (slot & 1u);
}

static uint32_t rfx_slot_set(rdx_fx_slot_t *s, uint32_t type, uint32_t p1, uint32_t p2)
{
    if (s->valid && s->type == type && s->p1 == p1 && s->p2 == p2)
        return 0u;
    memset(s, 0, sizeof *s);
    s->valid = 1;
    s->type = (uint8_t)type;
    s->p1 = (uint8_t)p1;
    s->p2 = (uint8_t)p2;
    return 1u;
}

/* Each physical Reface slot owns one complete Felucca HALL.  Slot 0 reuses
 * the firmware's original buffers; slot 1 uses the second instance above. */
static void rfx_hall_clear(uint32_t slot)
{
    int16_t *comb = slot ? rdx_rev_comb2 : rev_comb;
    int16_t *ap = slot ? rdx_rev_u2.ap : rev_ap;
    hall_t *h = slot ? &rdx_hl2 : &hl;
    memset(comb, 0, REV_COMB_LEN * sizeof *comb);
    memset(ap, 0, REV_AP_LEN * sizeof *ap);
    memset(h, 0, sizeof *h);
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
static __attribute__((noinline)) void rfx_dist(rdx_fx_slot_t *s, int32_t *b, uint32_t n)
{
    uint32_t i;
    uint32_t in_level = 0, out_level = 0;
    int32_t g = (int32_t)rfx_dist_drive_q12(s->p1);
    uint32_t tone = RFX_DIST_TONE_Q16[s->p2 & 127u];
    uint32_t target = rfx_dist_target_q16(s->p1), gain;
    int32_t xmax = (32768 * 4096) / g;

    if (!s->p1) {                                      /* DRIVE 0: exact dry bypass */
        s->dist_gain_q16 = 65536u;
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t dry = clamp(b[i], -262143, 262143);
        int32_t driven = (clamp(dry, -xmax, xmax) * g) >> 12;
        int32_t wet = rfx_dist_cubic(driven);
        int32_t mixed = (wet + dry) >> 1;               /* PicoFaceDX dryMix_ = 0.5 */
        in_level += (uint32_t)(dry < 0 ? -dry : dry);
        s->ins.lp1 += mulq16(mixed - s->ins.lp1, tone);
        b[i] = s->ins.lp1;
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
        if (!s->dist_gain_q16)
            s->dist_gain_q16 = wanted;
        else
            s->dist_gain_q16 = (uint32_t)((7u * s->dist_gain_q16 + wanted + 4u) >> 3);
    }
    gain = s->dist_gain_q16 ? s->dist_gain_q16 : 65536u;
    for (i = 0; i < n; i++) {
        int32_t y = gain <= 65536u ? mulq16(b[i], gain) :
                    b[i] + mulq16(b[i], gain - 65536u);
        b[i] = clamp(y, -262143, 262143);
    }
}

/* Envelope-following wah.  SENS controls the upward envelope excursion and
 * REZ the SVF resonance.  SELECT supplies the manual centre position. */
static __attribute__((noinline)) void rfx_wah(rdx_fx_slot_t *s, int32_t *b, uint32_t n, uint32_t pos)
{
    uint32_t i;
    int32_t env = s->wah_env, sens = s->p1;
    int32_t e = env > 65535 ? 65535 : env;
    int32_t cut = (10 + (int32_t)pos * 55 / 127 + (e * sens * 50 >> 23)) << 8;
    tsvf_t c;
    tsvf_coef(&c, cut, s->p2);
    for (i = 0; i < n; i++) {
        int32_t x = b[i], a = x < 0 ? -x : x, v3, bp, lp, y;
        a = a > 131071 ? 131071 : a;
        env += a > env ? (a - env) >> 3 : (a - env) >> 9;
        v3 = clamp(x, -150000, 150000) - s->wah_ic2;
        bp = (c.a1 * s->wah_ic1 + c.a2 * v3) >> 13;
        lp = s->wah_ic2 + ((c.a2 * s->wah_ic1 + c.a3 * v3) >> 13);
        s->wah_ic1 = clamp(2 * bp - s->wah_ic1, -150000, 150000);
        s->wah_ic2 = clamp(2 * lp - s->wah_ic2, -150000, 150000);
        y = clamp(lp + bp + (bp >> 1), -180000, 180000);
        b[i] = (x >> 2) + (y >> 1) + (y >> 2);          /* 25 % dry, 75 % wah */
    }
    s->wah_env = env;
}

/* Felucca's four-stage phaser and 256-sample flanger.  Chorus uses Felucca's
 * native parallel bus below instead.  Reface has DEPTH / RATE only, so
 * feedback is a conservative fixed part of each remaining model. */
static __attribute__((noinline)) void rfx_swept(rdx_fx_slot_t *s, int32_t *b, uint32_t n)
{
    ins_t *q = &s->ins;
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

/* Reface Delay DEPTH is mapped onto the two controls which shaped Felucca's
 * old parallel delay.  DEPTH 50 therefore has the familiar SEND 30 / FDBK 60
 * balance.  At the top, feedback stops at Felucca's stable documented limit. */
static uint32_t rfx_delay_send(uint32_t depth)
{
    return ((depth & 127u) * 3u + 2u) / 5u;
}

static uint32_t rfx_delay_feedback(uint32_t depth)
{
    uint32_t v = ((depth & 127u) * 6u + 2u) / 5u;
    return v > 120u ? 120u : v;
}

static uint32_t rfx_delay_samples(uint32_t time)
{
    uint32_t dl = ((uint32_t)RFX_DELAY_T10[time & 127u] * FS + 5000u) / 10000u;
    return dl < 1u ? 1u : dl > RFX_DLY_MASK ? RFX_DLY_MASK : dl;
}

#define RFX_DLY_COLOR 70u                    /* Felucca's warm default */
#define RFX_DLY_MIX 90u                      /* Felucca's default wet-bus mix */

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
    int32_t send = (int32_t)rfx_delay_send(s->p1) * 258;
    int32_t fb = (int32_t)rfx_delay_feedback(s->p1) * 230;
    int32_t col = 2000 + (int32_t)RFX_DLY_COLOR * 240;
    int32_t mix = (int32_t)RFX_DLY_MIX * 258;
    int32_t xmax;
    if (si != rfx_delay_owner || !send)
        return;
    xmax = 0x7FFFFFFF / send;
    for (i = 0; i < n; i++) {
        int32_t x = b[i];
        int32_t in = mulq15(clamp(x, -xmax, xmax), send);
        int32_t o = s->ds_fill >= dl ? line[(s->ds_wp - dl) & RFX_DLY_MASK] : 0;
        s->ds_lp += mulq15(o - s->ds_lp, col);
        line[s->ds_wp & RFX_DLY_MASK] =
            (int16_t)clamp((in >> 1) + mulq15(s->ds_lp, fb), -32768, 32767);
        s->ds_wp++;
        if (s->ds_fill < dl)
            s->ds_fill++;
        b[i] = x + mulq15(o << 1, mix);
    }
}

/* Reface Depth is much larger numerically than Felucca's old send setting.
 * The requested mapping is deliberately simple: integer Depth / 3. */
static uint32_t rfx_reverb_send(uint32_t depth) { return (depth & 127u) / 3u; }

/* Reface TIME values around 60 sound too short when copied directly to the
 * native HALL.  Stretch them by 1.5 and saturate at Felucca's 0 .. 127 SIZE. */
static uint32_t rfx_reverb_size(uint32_t time)
{
    uint32_t size = (time & 127u) * 3u / 2u;
    return size > 127u ? 127u : size;
}

static __attribute__((noinline)) void rfx_reverb_serial(rdx_fx_slot_t *s, uint32_t slot,
                                                        int32_t *b, uint32_t n, uint32_t keep_side)
{
    int16_t *comb = slot ? rdx_rev_comb2 : rev_comb;
    int16_t *ap = slot ? rdx_rev_u2.ap : rev_ap;
    hall_t *h = slot ? &rdx_hl2 : &hl;
    uint32_t i, value = rfx_reverb_send(s->p1);
    int32_t send, xmax, size = (int32_t)rfx_reverb_size(s->p2);
    if (!value)
        return;
    send = (int32_t)value * 258;                       /* Felucca send 0 .. 42, Q15 */
    xmax = 0x7FFFFFFF / send;
    for (i = 0; i < n; i++) {
        rfx_work.in[i] = mulq15(clamp(b[i], -xmax, xmax), send);
        rfx_work.wet[i] = 0;
    }
    rev_hall_core(rfx_work.in, rfx_work.wet, n, comb, ap, h, size, HALL_DAMP, h->sd);
    rdx_rev_size = (uint8_t)size;                       /* INFO/browser diagnostic: last HALL run */
    for (i = 0; i < n; i++) {
        b[i] += rfx_work.wet[i];                        /* serial dry + wet into the following slot */
        if (keep_side)
            rfx_work.side[i] += h->sd[i];              /* final slot: preserve Felucca's stereo spread */
    }
}

#define RFX_CHORUS_SEND 20u
static uint8_t rdx_cho_depth = 60u, rdx_cho_rate = 40u;

/* Feed the slot signal into Felucca's native Chorus at SEND 20, then return its
 * wet signal immediately.  The next slot therefore receives dry + Chorus. */
static __attribute__((noinline)) void rfx_chorus_serial(rdx_fx_slot_t *s, int32_t *b, uint32_t n)
{
    uint32_t i;
    uint32_t depth = s->p1 >> 1;                       /* Reface DEPTH -> half Felucca pitch excursion */
    int32_t send = RFX_CHORUS_SEND * 258;
    int32_t xmax = 0x7FFFFFFF / send;
    for (i = 0; i < n; i++)
        rfx_work.in[i] = mulq15(clamp(b[i], -xmax, xmax), send);
    fx_chorus_bus(rfx_work.in, rfx_work.wet, n, s->p2, depth);
    rdx_cho_depth = (uint8_t)depth;
    rdx_cho_rate = s->p2;
    for (i = 0; i < n; i++)
        b[i] += rfx_work.wet[i];
}

static uint32_t rdx_fx_has_effect(const track_t *t)
{
    const rdx_patch_t *p = rdx_track_get((uint32_t)(t - trk));
    return (p->common.effects[0][0] & 7u) || (p->common.effects[1][0] & 7u);
}

/* Select the one full-length delay-line owner before either core renders a
 * Part.  Keep a live owner's tail; otherwise prefer the selected sounding
 * Part, then the first sounding Part.  Other Parts still render their dry
 * serial chain instead of corrupting the shared line. */
static uint32_t rfx_delay_slot_for_track(uint32_t tr)
{
    const rdx_patch_t *p = rdx_track_get(tr);
    uint32_t k;
    if (trk[tr].engine != ENGI_RDX)
        return 2u;
    for (k = 0; k < 2u; k++)
        if ((p->common.effects[k][0] & 7u) == RFX_DELAY &&
            (p->common.effects[k][1] & 127u))
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
    const rdx_patch_t *p = rdx_track_get((uint32_t)(t - trk));
    uint32_t k, tail = 0;
    if (!rdx_fx_has_effect(t)) {
        f->tail = 0;
        return 0;
    }
    if (voices) {
        for (k = 0; k < 2u; k++) {
            uint32_t ty = p->common.effects[k][0] & 7u;
            uint32_t v = ty == RFX_DELAY ? 2500u + p->common.effects[k][1] * 55u :
                         ty == RFX_REVERB ? 2500u + rfx_reverb_size(p->common.effects[k][2]) * 70u : 64u;
            if (v > tail) tail = v;
        }
        f->tail = (uint16_t)(tail > 12000u ? 12000u : tail);   /* at most 8.7 seconds */
    }
    return f->tail != 0u;
}

static __attribute__((noinline)) void rdx_fx_chain(track_t *t, int32_t *b,
                                                   uint32_t n, uint32_t voices)
{
    uint32_t k, tr = (uint32_t)(t - trk) % NTRK;
    const rdx_patch_t *p = rdx_track_get(tr);
    rdx_fx_track_t *f = &rdx_fx[tr];
    for (k = 0; k < n; k++)
        rfx_work.side[k] = 0;
#if defined(__EMSCRIPTEN__)
    if (rdx_fx_test_bypass)
        return;
#endif
    for (k = 0; k < 2u; k++) {
        uint32_t ty = p->common.effects[k][0] & 7u;
        uint32_t p1 = p->common.effects[k][1] & 127u;
        uint32_t p2 = p->common.effects[k][2] & 127u;
        uint32_t si = rfx_slot_index(t, k);
        rdx_fx_slot_t *s = &f->slot[k];
        uint32_t changed = rfx_slot_set(s, ty, p1, p2);
        if (changed)
            rfx_hall_clear(k);                          /* no old-patch HALL tail in this physical slot */
        switch (ty) {
        case RFX_DIST:    rfx_dist(s, b, n); break;
        case RFX_WAH:     rfx_wah(s, b, n, rdx_touch_wah_get(t)); break;
        case RFX_CHORUS:  rfx_chorus_serial(s, b, n); break;
        case RFX_FLANGER:
        case RFX_PHASER:  rfx_swept(s, b, n); break;
        case RFX_DELAY:   if (p1) rfx_delay(s, b, n, si); break;
        case RFX_REVERB:
            /* A downstream Felucca slot is mono.  Feed it HALL's mid return
             * and discard the early stereo difference; keeping that side at
             * the output would create a hidden parallel bypass around FX2. */
            rfx_reverb_serial(s, k, b, n,
                              k == 1u || !(p->common.effects[1][0] & 7u));
            break;
        default: break;
        }
    }
    if (!voices && f->tail)
        f->tail--;
}

/* Add HALL's native stereo difference after Part LEVEL.  Its mono return has
 * already passed through the rest of the serial chain in b. */
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
