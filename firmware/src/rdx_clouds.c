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
 * halving the arithmetic cost.  Each RDX effect slot has its own 32 KiB,
 * 12-bit-compressed delay store; FX1 and FX2 therefore never share a tail.
 */
#if FELUCCA_RDX

#define RDX_CLD_MEM_SAMPLES 16384u
#define RDX_CLD_MEM_MASK (RDX_CLD_MEM_SAMPLES - 1u)
#define RDX_CLD_DIFF_Q15 20480                         /* 0.625 */
#define RDX_CLD_INPUT_Q15 13107                        /* 2 * 0.20: stereo sum from a mono send */
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
    int32_t input_z, out_l, out_r;
    int32_t time_q15;
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

static inline uint32_t rdx_cld_index(const rdx_cld_context_t *c,
                                     const rdx_cld_region_t *r, uint32_t off)
{
    return ((uint32_t)c->wp + r->base + off) & RDX_CLD_MEM_MASK;
}

static inline int32_t rdx_cld_load(const rdx_cld_context_t *c,
                                   const rdx_cld_region_t *r, uint32_t off)
{
    return (int32_t)c->mem[rdx_cld_index(c, r, off)] * 8;  /* signed Q12 store -> audio Q15 */
}

static inline void rdx_cld_read(rdx_cld_context_t *c, const rdx_cld_region_t *r,
                                uint32_t off, int32_t scale)
{
    int32_t v = rdx_cld_load(c, r, off);
    c->previous = v;
    c->acc += rdx_cld_mul(v, scale);
}

static inline void rdx_cld_tail(rdx_cld_context_t *c, const rdx_cld_region_t *r,
                                int32_t scale)
{
    rdx_cld_read(c, r, (uint32_t)r->len - 1u, scale);
}

static inline void rdx_cld_interp(rdx_cld_context_t *c, const rdx_cld_region_t *r,
                                  uint32_t off_q16, int32_t scale)
{
    uint32_t whole = off_q16 >> 16;
    int32_t f = (int32_t)((off_q16 >> 8) & 255u);
    int32_t a = rdx_cld_load(c, r, whole);
    int32_t b = rdx_cld_load(c, r, whole + 1u);
    int32_t v = a + (((b - a) * f) >> 8);
    c->previous = v;
    c->acc += rdx_cld_mul(v, scale);
}

static inline void rdx_cld_write(rdx_cld_context_t *c, const rdx_cld_region_t *r,
                                 uint32_t off, int32_t scale)
{
    int32_t v = clamp(c->acc, -262144, 262136);         /* Clouds FORMAT_12_BIT: -8 .. +7.99975 */
    c->mem[rdx_cld_index(c, r, off)] = (int16_t)(v >> 3);
    c->acc = rdx_cld_mul(c->acc, scale);
}

static inline void rdx_cld_allpass(rdx_cld_context_t *c, const rdx_cld_region_t *r,
                                   int32_t scale)
{
    rdx_cld_write(c, r, 0u, scale);
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

/* Pure stereo wet return.  SEND has already been applied by rfx_reverb_serial;
 * the detail page's DEPTH is applied when this return is mixed into the dry
 * serial chain, exactly like ROOM / SPRING / HALL. */
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
