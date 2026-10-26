/* SPDX-License-Identifier: MIT
 *
 * Fixed-point, half-rate port of the Mutable Instruments Clouds reverb.
 * The signal topology and shared compressed delay-line design are adapted
 * from clouds/dsp/fx/reverb.h and clouds/dsp/fx/fx_engine.h:
 *
 * Copyright 2014 Emilie Gillet
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Clouds' original engine runs at 32 kHz.  FM-1 renders this reverb at
 * 22.05 kHz and linearly reconstructs its stereo return at 44.1 kHz.  Every
 * delay is rescaled by 22050 / 32000, preserving the original times while
 * halving the arithmetic cost.  Each RDX effect slot has its own 32 KiB Q14
 * delay store; FX1 and FX2 therefore never share a tail.
 */
#if FELUCCA_RDX

#define RDX_CLD_MEM_SAMPLES 16384u
#define RDX_CLD_MEM_MASK (RDX_CLD_MEM_SAMPLES - 1u)
#define RDX_CLD_DIFF_Q15 20480                         /* 0.625 */
#define RDX_CLD_INPUT_Q15 13107                        /* 2 * 0.20: stereo sum from a mono send */
/* Gain-two, 15-tap half-band interpolator, reduced to its four non-zero
 * symmetric coefficients.  The adjusted inner tap makes both polyphase paths
 * exactly unity at DC in Q15: 2 * sum(H3_HB) == 32768. */
#define RDX_CLD_H3_HB0 (-109)
#define RDX_CLD_H3_HB1 1128
#define RDX_CLD_H3_HB2 (-4528)
#define RDX_CLD_H3_HB3 19893
#define RDX_CLD_LFO1_STEP (97392u * 32u)               /* 0.5 Hz; oscillator refreshed every 32 wet samples */
#define RDX_CLD_LFO2_STEP (58435u * 32u)               /* 0.3 Hz */

typedef struct { uint16_t base, len; } rdx_cld_region_t;

/* Clouds' 32 kHz regions, compacted after rescaling to 22.05 kHz. */
static const rdx_cld_region_t RDX_CLD_AP1   = {   0,   78};
static const rdx_cld_region_t RDX_CLD_AP2   = {  79,  112};
static const rdx_cld_region_t RDX_CLD_AP3   = { 192,  166};
static const rdx_cld_region_t RDX_CLD_AP4   = { 359,  275};
static const rdx_cld_region_t RDX_CLD_DAP1A = { 635, 1139};
static const rdx_cld_region_t RDX_CLD_DAP1B = {1775, 1404};
static const rdx_cld_region_t RDX_CLD_DEL1  = {3180, 2350};
static const rdx_cld_region_t RDX_CLD_DAP2A = {5531, 1318};
static const rdx_cld_region_t RDX_CLD_DAP2B = {6850, 1146};
static const rdx_cld_region_t RDX_CLD_DEL2  = {7997, 3295};

typedef struct {
    uint16_t wp;
    uint32_t ph1, ph2;
    uint16_t lfo1, lfo2;
    int32_t lp1, lp2;
    int32_t recon_l[8], recon_r[8];
    int32_t input_z, out_l, out_r;
    int32_t time_q15;
    uint8_t recon_w;
} rdx_clouds_t;

typedef struct {
    int16_t *mem;
    uint16_t wp;
    int32_t acc, previous;
} rdx_cld_context_t;

static int16_t rdx_cloud_mem[2][RDX_CLD_MEM_SAMPLES] __attribute__((section(".pool")));
static rdx_clouds_t rdx_cloud[2] __attribute__((section(".pool")));

_Static_assert(7997u + 3295u <= RDX_CLD_MEM_SAMPLES, "Clouds delay regions exceed the compressed store");
_Static_assert((CTL & 1u) == 0u, "Clouds half-rate renderer needs an even audio block");

/* mulq15 without its small-input restriction: Clouds' feedback accumulators
 * deliberately span several ordinary audio full-scales. */
static inline int32_t rdx_cld_mul(int32_t x, int32_t q15)
{
    if (q15 < 0)
        return -mulq16(x, (uint32_t)(-q15) << 1);
    return mulq16(x, (uint32_t)q15 << 1);
}

/* Feedback conversion towards zero.  The generic mulq16 and signed shifts
 * round a negative fractional result down instead; in a feedback loop that
 * asymmetry can preserve a one-LSB offset and turn it into audible hiss. */
static __attribute__((always_inline)) inline int32_t rdx_cld_mul_tz_pos(int32_t x, uint32_t q16)
{
    uint32_t low = ((uint32_t)x & 0xFFFFu) * q16;
    uint32_t rem = low & 0xFFFFu;
    uint32_t nonzero = (rem | (0u - rem)) >> 31;
    int32_t floor = (x >> 16) * (int32_t)q16 + (int32_t)(low >> 16);
    /* mulq16 is a floor for negative fractional products.  Add one exactly
     * when X is negative and the discarded fraction is non-zero.  Keeping the
     * low product makes this the same two multiplies as mulq16, with no branch. */
    return floor + (int32_t)(((uint32_t)x >> 31) & nonzero);
}

static __attribute__((always_inline)) inline int32_t rdx_cld_mul_tz(int32_t x, int32_t q15)
{
    if (q15 < 0)
        return -rdx_cld_mul_tz_pos(x, (uint32_t)(-q15) << 1);
    return rdx_cld_mul_tz_pos(x, (uint32_t)q15 << 1);
}

static __attribute__((always_inline)) inline int32_t rdx_cld_sh_tz(int32_t x, uint32_t bits)
{
    return (x + ((x >> 31) & (int32_t)((1u << bits) - 1u))) >> bits;
}

static __attribute__((always_inline)) inline uint32_t rdx_cld_index(const rdx_cld_context_t *c,
                                                                    const rdx_cld_region_t *r, uint32_t off)
{
    return ((uint32_t)c->wp + r->base + off) & RDX_CLD_MEM_MASK;
}

/* HALL raises the delay-store precision to Q14.  Accumulator and filter states
 * remain 32-bit.  Q14 retains two full-scales of tank
 * headroom while reducing every recursive write's quantum from eight to two. */
static __attribute__((always_inline)) inline int32_t rdx_cld_load_h3(
    const rdx_cld_context_t *c, const rdx_cld_region_t *r, uint32_t off)
{
    return (int32_t)c->mem[rdx_cld_index(c, r, off)] * 2;
}

static __attribute__((always_inline)) inline void rdx_cld_read_h3(rdx_cld_context_t *c,
                                                                  const rdx_cld_region_t *r,
                                                                  uint32_t off, int32_t scale)
{
    int32_t v = rdx_cld_load_h3(c, r, off);
    c->previous = v;
    c->acc += rdx_cld_mul_tz(v, scale);
}

static __attribute__((always_inline)) inline void rdx_cld_tail_h3(rdx_cld_context_t *c,
                                                                  const rdx_cld_region_t *r,
                                                                  int32_t scale)
{
    rdx_cld_read_h3(c, r, (uint32_t)r->len - 1u, scale);
}

static __attribute__((always_inline)) inline void rdx_cld_interp_h3(rdx_cld_context_t *c,
                                                                    const rdx_cld_region_t *r,
                                                                    uint32_t off_q16,
                                                                    int32_t scale)
{
    uint32_t whole = off_q16 >> 16;
    int32_t f = (int32_t)((off_q16 >> 8) & 255u);
    int32_t a = rdx_cld_load_h3(c, r, whole);
    int32_t b = rdx_cld_load_h3(c, r, whole + 1u);
    int32_t v = a + rdx_cld_sh_tz((b - a) * f, 8u);
    c->previous = v;
    c->acc += rdx_cld_mul_tz(v, scale);
}

static __attribute__((always_inline)) inline void rdx_cld_write_h3(rdx_cld_context_t *c,
                                                                   const rdx_cld_region_t *r,
                                                                   uint32_t off, int32_t scale)
{
    int32_t v = clamp(c->acc, -65536, 65534);           /* Q14: -2 .. +1.99994 */
    c->mem[rdx_cld_index(c, r, off)] = (int16_t)rdx_cld_sh_tz(v, 1u);
    c->acc = rdx_cld_mul_tz(c->acc, scale);
}

static __attribute__((always_inline)) inline void rdx_cld_allpass_h3(rdx_cld_context_t *c,
                                                                     const rdx_cld_region_t *r,
                                                                     int32_t scale)
{
    rdx_cld_write_h3(c, r, 0u, scale);
    c->acc += c->previous;
}

static void rdx_clouds_clear(uint32_t slot)
{
    rdx_clouds_t *s = &rdx_cloud[slot & 1u];
    memset(rdx_cloud_mem[slot & 1u], 0, sizeof rdx_cloud_mem[0]);
    memset(s, 0, sizeof *s);
    s->lfo1 = s->lfo2 = 32768u;
    s->time_q15 = 22938;                               /* original Clouds initial value: 0.70 */
}

/* Q12 HALL and HALL2 are retained below only as source history.  They are not
 * compiled, exposed in the parameter list or reachable by stored projects. */
#if 0
static inline void rdx_clouds_one(rdx_clouds_t *s, int16_t *mem, int32_t input,
                                  int32_t time_q15, int32_t damp_q15,
                                  int32_t *left, int32_t *right)
{
    rdx_cld_context_t c;
    int32_t diffused;
    uint32_t off;

    s->wp = (uint16_t)((s->wp - 1u) & RDX_CLD_MEM_MASK);
    if (!(s->wp & 31u)) {
        s->lfo1 = (uint16_t)(osc_sine(s->ph1) + 32768);
        s->lfo2 = (uint16_t)(osc_sine(s->ph2) + 32768);
        s->ph1 += RDX_CLD_LFO1_STEP;
        s->ph2 += RDX_CLD_LFO2_STEP;
    }
    c.mem = mem;
    c.wp = s->wp;
    c.acc = c.previous = 0;

    /* Input diffusion.  AP1's moving read is inside its own feedback loop. */
    off = (7u << 16) + 41u * (uint32_t)s->lfo1;
    rdx_cld_interp(&c, &RDX_CLD_AP1, off, 32767);
    rdx_cld_write(&c, &RDX_CLD_AP1, 69u, 0);
    c.acc += rdx_cld_mul(input, RDX_CLD_INPUT_Q15);
    rdx_cld_tail(&c, &RDX_CLD_AP1, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_AP1, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail(&c, &RDX_CLD_AP2, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_AP2, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail(&c, &RDX_CLD_AP3, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_AP3, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail(&c, &RDX_CLD_AP4, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_AP4, -RDX_CLD_DIFF_Q15);
    diffused = c.acc;

    /* Left tank. */
    c.acc = diffused;
    off = (3225u << 16) + 69u * (uint32_t)s->lfo2;
    rdx_cld_interp(&c, &RDX_CLD_DEL2, off, time_q15);
    s->lp1 += rdx_cld_mul(c.acc - s->lp1, damp_q15);
    c.acc = s->lp1;
    rdx_cld_tail(&c, &RDX_CLD_DAP1A, -RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_DAP1A, RDX_CLD_DIFF_Q15);
    rdx_cld_tail(&c, &RDX_CLD_DAP1B, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_DAP1B, -RDX_CLD_DIFF_Q15);
    rdx_cld_write(&c, &RDX_CLD_DEL1, 0u, 32767);
    *left = c.acc + c.acc;                              /* original c.Write(del1, 2.0) */

    /* Right tank. */
    c.acc = diffused;
    rdx_cld_tail(&c, &RDX_CLD_DEL1, time_q15);
    s->lp2 += rdx_cld_mul(c.acc - s->lp2, damp_q15);
    c.acc = s->lp2;
    rdx_cld_tail(&c, &RDX_CLD_DAP2A, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_DAP2A, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail(&c, &RDX_CLD_DAP2B, -RDX_CLD_DIFF_Q15);
    rdx_cld_allpass(&c, &RDX_CLD_DAP2B, RDX_CLD_DIFF_Q15);
    rdx_cld_write(&c, &RDX_CLD_DEL2, 0u, 32767);
    *right = c.acc + c.acc;
}

/* Noise-floor-corrected Clouds sample for HALL2.  Its topology, delay lengths
 * and gains are deliberately identical to HALL; only feedback rounding,
 * near-zero convergence and the final anti-image/tone filter differ. */
static __attribute__((always_inline)) inline void rdx_clouds_one_h2(rdx_clouds_t *s,
                                                                    int16_t *mem, int32_t input,
                                                                    int32_t time_q15, int32_t damp_q15,
                                                                    int32_t *left, int32_t *right)
{
    rdx_cld_context_t c;
    int32_t diffused, step;
    uint32_t off;

    s->wp = (uint16_t)((s->wp - 1u) & RDX_CLD_MEM_MASK);
    if (!(s->wp & 31u)) {
        s->lfo1 = (uint16_t)(osc_sine(s->ph1) + 32768);
        s->lfo2 = (uint16_t)(osc_sine(s->ph2) + 32768);
        s->ph1 += RDX_CLD_LFO1_STEP;
        s->ph2 += RDX_CLD_LFO2_STEP;
    }
    c.mem = mem;
    c.wp = s->wp;
    c.acc = c.previous = 0;

    off = (7u << 16) + 41u * (uint32_t)s->lfo1;
    rdx_cld_interp_h2(&c, &RDX_CLD_AP1, off, 32767);
    rdx_cld_write_h2(&c, &RDX_CLD_AP1, 69u, 0);
    c.acc += rdx_cld_mul_tz(input, RDX_CLD_INPUT_Q15);
    rdx_cld_tail_h2(&c, &RDX_CLD_AP1, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_AP1, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h2(&c, &RDX_CLD_AP2, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_AP2, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h2(&c, &RDX_CLD_AP3, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_AP3, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h2(&c, &RDX_CLD_AP4, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_AP4, -RDX_CLD_DIFF_Q15);
    diffused = c.acc;

    c.acc = diffused;
    off = (3225u << 16) + 69u * (uint32_t)s->lfo2;
    rdx_cld_interp_h2(&c, &RDX_CLD_DEL2, off, time_q15);
    step = rdx_cld_mul_tz(c.acc - s->lp1, damp_q15);
    s->lp1 += step;
    if (s->lp1 > -8 && s->lp1 < 8)
        s->lp1 = 0;
    c.acc = s->lp1;
    rdx_cld_tail_h2(&c, &RDX_CLD_DAP1A, -RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_DAP1A, RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h2(&c, &RDX_CLD_DAP1B, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_DAP1B, -RDX_CLD_DIFF_Q15);
    rdx_cld_write_h2(&c, &RDX_CLD_DEL1, 0u, 32767);
    *left = c.acc + c.acc;

    c.acc = diffused;
    rdx_cld_tail_h2(&c, &RDX_CLD_DEL1, time_q15);
    step = rdx_cld_mul_tz(c.acc - s->lp2, damp_q15);
    s->lp2 += step;
    if (s->lp2 > -8 && s->lp2 < 8)
        s->lp2 = 0;
    c.acc = s->lp2;
    rdx_cld_tail_h2(&c, &RDX_CLD_DAP2A, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_DAP2A, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h2(&c, &RDX_CLD_DAP2B, -RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h2(&c, &RDX_CLD_DAP2B, RDX_CLD_DIFF_Q15);
    rdx_cld_write_h2(&c, &RDX_CLD_DEL2, 0u, 32767);
    *right = c.acc + c.acc;

    /* Two cheap poles remove the half-rate image and the last broad Q12 noise
     * band.  The corrected multiply prevents these states becoming another
     * one-LSB feedback loop. */
    s->tone_l1 += rdx_cld_mul_tz(*left - s->tone_l1, RDX_CLD_HALL2_TONE_Q15);
    s->tone_l2 += rdx_cld_mul_tz(s->tone_l1 - s->tone_l2, RDX_CLD_HALL2_TONE_Q15);
    s->tone_r1 += rdx_cld_mul_tz(*right - s->tone_r1, RDX_CLD_HALL2_TONE_Q15);
    s->tone_r2 += rdx_cld_mul_tz(s->tone_r1 - s->tone_r2, RDX_CLD_HALL2_TONE_Q15);
    if (s->tone_l2 > -8 && s->tone_l2 < 8) s->tone_l2 = 0;
    if (s->tone_r2 > -8 && s->tone_r2 < 8) s->tone_r2 = 0;
    *left = s->tone_l2;
    *right = s->tone_r2;
}

#endif

/* The retained HALL uses the Clouds tank at Q14.  Reconstruction is handled
 * at 44.1 kHz by the polyphase half-band filter below. */
static __attribute__((always_inline)) inline void rdx_clouds_one_h3(rdx_clouds_t *s,
                                                                    int16_t *mem, int32_t input,
                                                                    int32_t time_q15, int32_t damp_q15,
                                                                    int32_t *left, int32_t *right)
{
    rdx_cld_context_t c;
    int32_t diffused, step;
    uint32_t off;

    s->wp = (uint16_t)((s->wp - 1u) & RDX_CLD_MEM_MASK);
    if (!(s->wp & 31u)) {
        s->lfo1 = (uint16_t)(osc_sine(s->ph1) + 32768);
        s->lfo2 = (uint16_t)(osc_sine(s->ph2) + 32768);
        s->ph1 += RDX_CLD_LFO1_STEP;
        s->ph2 += RDX_CLD_LFO2_STEP;
    }
    c.mem = mem;
    c.wp = s->wp;
    c.acc = c.previous = 0;

    off = (7u << 16) + 41u * (uint32_t)s->lfo1;
    rdx_cld_interp_h3(&c, &RDX_CLD_AP1, off, 32767);
    rdx_cld_write_h3(&c, &RDX_CLD_AP1, 69u, 0);
    c.acc += rdx_cld_mul_tz(input, RDX_CLD_INPUT_Q15);
    rdx_cld_tail_h3(&c, &RDX_CLD_AP1, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_AP1, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h3(&c, &RDX_CLD_AP2, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_AP2, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h3(&c, &RDX_CLD_AP3, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_AP3, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h3(&c, &RDX_CLD_AP4, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_AP4, -RDX_CLD_DIFF_Q15);
    diffused = c.acc;

    c.acc = diffused;
    off = (3225u << 16) + 69u * (uint32_t)s->lfo2;
    rdx_cld_interp_h3(&c, &RDX_CLD_DEL2, off, time_q15);
    step = rdx_cld_mul_tz(c.acc - s->lp1, damp_q15);
    s->lp1 += step;
    if (s->lp1 > -2 && s->lp1 < 2)
        s->lp1 = 0;
    c.acc = s->lp1;
    rdx_cld_tail_h3(&c, &RDX_CLD_DAP1A, -RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_DAP1A, RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h3(&c, &RDX_CLD_DAP1B, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_DAP1B, -RDX_CLD_DIFF_Q15);
    rdx_cld_write_h3(&c, &RDX_CLD_DEL1, 0u, 32767);
    *left = c.acc + c.acc;

    c.acc = diffused;
    rdx_cld_tail_h3(&c, &RDX_CLD_DEL1, time_q15);
    step = rdx_cld_mul_tz(c.acc - s->lp2, damp_q15);
    s->lp2 += step;
    if (s->lp2 > -2 && s->lp2 < 2)
        s->lp2 = 0;
    c.acc = s->lp2;
    rdx_cld_tail_h3(&c, &RDX_CLD_DAP2A, RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_DAP2A, -RDX_CLD_DIFF_Q15);
    rdx_cld_tail_h3(&c, &RDX_CLD_DAP2B, -RDX_CLD_DIFF_Q15);
    rdx_cld_allpass_h3(&c, &RDX_CLD_DAP2B, RDX_CLD_DIFF_Q15);
    rdx_cld_write_h3(&c, &RDX_CLD_DEL2, 0u, 32767);
    *right = c.acc + c.acc;
}

static __attribute__((always_inline)) inline int32_t rdx_clouds_h3_halfband(
    const int32_t *h, uint32_t w)
{
    int32_t a0 = h[w] + h[(w - 7u) & 7u];
    int32_t a1 = h[(w - 1u) & 7u] + h[(w - 6u) & 7u];
    int32_t a2 = h[(w - 2u) & 7u] + h[(w - 5u) & 7u];
    int32_t a3 = h[(w - 3u) & 7u] + h[(w - 4u) & 7u];
    return rdx_cld_mul_tz(a0, RDX_CLD_H3_HB0) +
           rdx_cld_mul_tz(a1, RDX_CLD_H3_HB1) +
           rdx_cld_mul_tz(a2, RDX_CLD_H3_HB2) +
           rdx_cld_mul_tz(a3, RDX_CLD_H3_HB3);
}

/* Pure stereo wet return.  SEND has already been applied by rfx_reverb_serial;
 * the detail page's DEPTH is applied when this return is mixed into the dry
 * serial chain, exactly like ROOM / SPRING / HALL. */
#if 0
static __attribute__((noinline)) void rdx_clouds_process(uint32_t slot, const int32_t *in,
                                                         int32_t *wet, int32_t *side,
                                                         uint32_t n, uint32_t time,
                                                         uint32_t damp)
{
    rdx_clouds_t *s = &rdx_cloud[slot & 1u];
    int16_t *mem = rdx_cloud_mem[slot & 1u];
    int32_t target_time = 11469 + (int32_t)(time & 127u) * 19661 / 127; /* 0.35 .. 0.95 */
    int32_t time_step = (target_time - s->time_q15) / (int32_t)(n >> 1);
    int32_t damp_q15 = 32700 - (int32_t)(damp & 127u) * 163;           /* DAMP 60 = Clouds' 0.70 */
    int32_t prev_l = s->out_l, prev_r = s->out_r;
    int32_t input_z = s->input_z;
    uint32_t i;

    for (i = 0; i < n; i += 2u) {
        int32_t input = (input_z + in[i] + in[i] + in[i + 1u]) >> 2;
        int32_t wl, wr, ol, or;
        input_z = in[i + 1u];
        s->time_q15 += time_step;
        rdx_clouds_one(s, mem, input, s->time_q15, damp_q15, &wl, &wr);

        ol = (prev_l + wl) >> 1;
        or = (prev_r + wr) >> 1;
        wet[i] = (ol >> 1) + (or >> 1);
        side[i] = (ol >> 1) - (or >> 1);
        wet[i + 1u] = (wl >> 1) + (wr >> 1);
        side[i + 1u] = (wl >> 1) - (wr >> 1);
        prev_l = wl;
        prev_r = wr;
    }
    s->input_z = input_z;
    s->out_l = prev_l;
    s->out_r = prev_r;
    s->time_q15 = target_time;
}
#endif

typedef void (*rdx_cld_process_fn_t)(uint32_t, const int32_t *, int32_t *, int32_t *,
                                     uint32_t, uint32_t, uint32_t);
#if defined(FM1_TICKS_PER_US)
#define RDX_CLD_H3_FAST __attribute__((section(".fast_text"), noinline, used))
#else
#define RDX_CLD_H3_FAST __attribute__((noinline))
#endif
static RDX_CLD_H3_FAST void rdx_clouds_process_h3(uint32_t slot, const int32_t *in,
                                                  int32_t *wet, int32_t *side,
                                                  uint32_t n, uint32_t time,
                                                  uint32_t damp)
{
    rdx_clouds_t *s = &rdx_cloud[slot & 1u];
    int16_t *mem = rdx_cloud_mem[slot & 1u];
    int32_t target_time = 11469 + (int32_t)(time & 127u) * 19661 / 127;
    int32_t time_step = (target_time - s->time_q15) / (int32_t)(n >> 1);
    int32_t damp_q15 = 32700 - (int32_t)(damp & 127u) * 163;
    int32_t input_z = s->input_z;
    uint32_t w = s->recon_w, i;

    for (i = 0; i < n; i += 2u) {
        int32_t input = rdx_cld_sh_tz(input_z + in[i] + in[i] + in[i + 1u], 2u);
        int32_t wl, wr, il, ir, dl, dr;
        input_z = in[i + 1u];
        s->time_q15 += time_step;
        rdx_clouds_one_h3(s, mem, input, s->time_q15, damp_q15, &wl, &wr);

        s->recon_l[w] = wl;
        s->recon_r[w] = wr;
        il = rdx_clouds_h3_halfband(s->recon_l, w);
        ir = rdx_clouds_h3_halfband(s->recon_r, w);
        dl = s->recon_l[(w - 3u) & 7u];
        dr = s->recon_r[(w - 3u) & 7u];
        wet[i] = rdx_cld_sh_tz(il + ir, 1u);
        side[i] = rdx_cld_sh_tz(il - ir, 1u);
        wet[i + 1u] = rdx_cld_sh_tz(dl + dr, 1u);
        side[i + 1u] = rdx_cld_sh_tz(dl - dr, 1u);
        w = (w + 1u) & 7u;
    }
    s->input_z = input_z;
    s->out_l = s->recon_l[(w - 1u) & 7u];
    s->out_r = s->recon_r[(w - 1u) & 7u];
    s->recon_w = (uint8_t)w;
    s->time_q15 = target_time;
}
static rdx_cld_process_fn_t volatile rdx_clouds_process_h3_entry = rdx_clouds_process_h3;
#undef RDX_CLD_H3_FAST

#endif
