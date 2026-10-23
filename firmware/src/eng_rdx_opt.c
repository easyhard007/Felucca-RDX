/* SPDX-License-Identifier: GPL-3.0-or-later
 * RDX block renderer used by MENU > SYSTEM > RDX TYPE > OPTIMIZED.
 *
 * The envelope control pass emits four 32-sample gain blocks a linear segment
 * at a time.  The twelve algorithm kernels then keep their operator buses in
 * registers: there are no generic input-pointer tests and no audio-rate bus
 * arrays to write and read back.  On FM-1 those kernels live in fast SRAM. */

typedef struct {
    rdx_note_t *note;
    int32_t *out;
    uint32_t (*env)[CTL];
    uint32_t len, norm;
    uint32_t fb_scale[4];
    uint32_t inc[4], inc_delta[4], inc_rem[4], inc_error[4];
    int32_t am[4], am_step[4];
    int32_t amp, damp;
    uint16_t tail_base;
    uint8_t inc_down[4];
    uint8_t enable_mask, fb_type_mask, amd_mask;
    uint8_t tail_start;
} rdx_opt_ctl_t;

/* Fill one operator's gain block in runs that end only at an envelope stage
 * boundary. idle_after is the first sample after whose tick the operator is
 * IDLE; it lets the caller reproduce the original mid-block tail transition. */
static uint32_t rdx_opt_env_segments(rdx_op_t *o, const rdx_op_param_t *p,
                                     int gate, int tail, int32_t rate_macro,
                                     uint32_t *dst, uint32_t len)
{
    uint32_t pos = 0, idle_after = len;
    while (pos < len) {
        uint32_t count, to_target, j;
        if (o->stage == RDX_IDLE) {
            uint32_t gain = tail ? rdx_gain(o->env) : 0u;
            if (idle_after == len)
                idle_after = pos ? pos - 1u : 0u;
            while (pos < len)
                dst[pos++] = gain;
            break;
        }
        if (o->stage == RDX_SUSTAIN) {
            uint32_t gain = rdx_gain(o->env);
            if (!gate) {
                rdx_env_enter(o, p, RDX_RELEASE, o->rate_offset, rate_macro);
                dst[pos++] = gain;             /* change stage before advancing it, like native tick */
                continue;
            }
            while (pos < len)
                dst[pos++] = gain;
            break;
        }
        if (!o->step) {
            uint32_t gain = rdx_gain(o->env);
            while (pos < len)
                dst[pos++] = gain;
            break;
        }
        if (o->rising)
            to_target = (o->target - o->env + o->step - 1u) / o->step;
        else
            to_target = (o->env - o->target + o->step - 1u) / o->step;
        if (!to_target)
            to_target = 1u;
        count = to_target < len - pos ? to_target : len - pos;
        for (j = 0; j < count; j++) {
            uint32_t hit = j + 1u == to_target;
            if (hit) {
                o->env = o->target;
                rdx_env_next(o, p, gate, o->rate_offset, rate_macro);
                if (o->stage == RDX_IDLE && idle_after == len)
                    idle_after = pos;
            } else if (o->rising)
                o->env += o->step;
            else
                o->env -= o->step;
            dst[pos++] = rdx_gain(o->env);
        }
    }
    return idle_after;
}

static void rdx_opt_envelopes(rdx_opt_ctl_t *c, const rdx_patch_t *p, int32_t rate_macro)
{
    rdx_note_t *n = c->note;
    uint32_t idle_after[4], i, k, end_at = 0;
    int was_ending = n->ending;
    for (k = 0; k < 4u; k++) {
        if ((n->enable_mask >> k) & 1u)
            idle_after[k] = rdx_opt_env_segments(&n->op[k], &p->op[k], n->down,
                                                  was_ending, rate_macro, c->env[k], c->len);
        else {
            /* A disabled native operator does not advance its envelope. Keep
             * its stage in the carrier-idle decision exactly as ORIGIN does. */
            idle_after[k] = n->op[k].stage == RDX_IDLE ? 0u : c->len;
            for (i = 0; i < c->len; i++)
                c->env[k][i] = 0u;
        }
    }
    c->tail_start = (uint8_t)c->len;
    c->tail_base = 0u;
    if (was_ending) {
        c->tail_start = 0u;
        c->tail_base = n->tail_left;
    } else if (!n->down) {
        uint32_t active = 0;
        for (k = 0; k < 4u; k++)
            if ((n->carriers >> k) & 1u) {
                active |= idle_after[k] == c->len;
                if (idle_after[k] > end_at)
                    end_at = idle_after[k];
            }
        if (!active) {
            n->ending = 1u;
            n->tail_left = 256u;
            c->tail_start = (uint8_t)end_at;
            c->tail_base = 256u;
            for (k = 0; k < 4u; k++) {
                uint32_t first = end_at + 1u;
                if (first < idle_after[k] + 1u)
                    first = idle_after[k] + 1u;
                for (i = first; i < c->len; i++)
                    c->env[k][i] = rdx_gain(n->op[k].env);
            }
        }
    }
    if (c->tail_start < c->len) {
        uint32_t used = c->len - c->tail_start;
        n->tail_left = n->tail_left > used ? (uint16_t)(n->tail_left - used) : 0u;
    }
}

static __attribute__((always_inline)) int32_t rdx_opt_hot_op(rdx_opt_ctl_t *c,
                                                              uint32_t k, int32_t input,
                                                              uint32_t i)
{
    rdx_op_t *o = &c->note->op[k];
    int32_t f, wave, s;
    uint32_t step;
    if (!((c->enable_mask >> k) & 1u))
        return 0;
    f = o->fb;
    if (((c->fb_type_mask >> k) & 1u) && f < 0)
        f = -f;
    o->fb_filter += mulq15(f - o->fb_filter, 11665);
    f = (o->fb_filter * (int32_t)c->fb_scale[k]) >> 16;
    wave = osc_sine(o->ph + ((uint32_t)(input + f) << 17));
    o->ph += c->inc[k];
    o->fb = wave;
    s = (int32_t)(((int64_t)wave * (int32_t)o->out_gain) >> 15);
    s = (int32_t)(((int64_t)s * c->env[k][i]) >> 15);
    if ((c->amd_mask >> k) & 1u)
        s = mulq15(s, c->am[k]);
    step = c->inc_delta[k];
    c->inc_error[k] += c->inc_rem[k];
    if (c->len > 1u && c->inc_error[k] >= c->len - 1u) {
        c->inc_error[k] -= c->len - 1u;
        step++;
    }
    c->inc[k] = c->inc_down[k] ? c->inc[k] - step : c->inc[k] + step;
    c->am[k] += c->am_step[k];
    return s;
}

static __attribute__((always_inline)) void rdx_opt_hot_post(rdx_opt_ctl_t *c,
                                                             uint32_t i, int32_t s)
{
    rdx_note_t *n = c->note;
    s = mulq16(s, c->norm << 1);
    { int32_t x = s - n->dc; n->dc += x >> 10; s = x; }
    if (n->transition_pending) {
        n->correction = n->last_out - s;
        n->correction_left = 128u;
        n->transition_pending = 0u;
    }
    if (n->correction_left) {
        s += mulq16(n->correction, (uint32_t)n->correction_left << 9);
        n->correction_left--;
    }
    if (i >= c->tail_start) {
        uint32_t used = i - c->tail_start;
        uint32_t left = c->tail_base > used ? c->tail_base - used : 0u;
        s = left ? mulq16(s, left << 8) : 0;
    }
    n->last_out = s;
    c->out[i] += (int32_t)(((int64_t)s * c->amp * VOICE_FS) >> 30);
    c->amp += c->damp;
}

#if defined(FM1_TICKS_PER_US)
#define RDX_FAST __attribute__((section(".fast_text"), noinline, used))
#else
#define RDX_FAST __attribute__((noinline))
#endif
#define RDX_OP(K, X) rdx_opt_hot_op(c, (K), (X), i)
#define RDX_KERNEL(NAME, ...) \
    static RDX_FAST void NAME(rdx_opt_ctl_t *c) { \
        uint32_t i; \
        for (i = 0; i < c->len; i++) { \
            int32_t a = 0, b = 0, d = 0, e = 0, s; \
            __VA_ARGS__; \
            rdx_opt_hot_post(c, i, s); \
        } \
    }

RDX_KERNEL(rdx_opt_alg0,  e=RDX_OP(3,0); d=RDX_OP(2,e); b=RDX_OP(1,d); a=RDX_OP(0,b); s=a)
RDX_KERNEL(rdx_opt_alg1,  e=RDX_OP(3,0); d=RDX_OP(2,0); b=RDX_OP(1,e+d); a=RDX_OP(0,b); s=a)
RDX_KERNEL(rdx_opt_alg2,  d=RDX_OP(2,0); b=RDX_OP(1,d); e=RDX_OP(3,0); a=RDX_OP(0,b+e); s=a)
RDX_KERNEL(rdx_opt_alg3,  e=RDX_OP(3,0); b=RDX_OP(1,e); d=RDX_OP(2,e); a=RDX_OP(0,b+d); s=a)
RDX_KERNEL(rdx_opt_alg4,  b=RDX_OP(1,0); d=RDX_OP(2,0); e=RDX_OP(3,0); a=RDX_OP(0,b+d+e); s=a)
RDX_KERNEL(rdx_opt_alg5,  a=RDX_OP(0,0); e=RDX_OP(3,0); d=RDX_OP(2,e); b=RDX_OP(1,d); s=a+b)
RDX_KERNEL(rdx_opt_alg6,  e=RDX_OP(3,0); d=RDX_OP(2,e); a=RDX_OP(0,d); b=RDX_OP(1,d); s=a+b)
RDX_KERNEL(rdx_opt_alg7,  b=RDX_OP(1,0); a=RDX_OP(0,b); e=RDX_OP(3,0); d=RDX_OP(2,e); s=a+d)
RDX_KERNEL(rdx_opt_alg8,  e=RDX_OP(3,0); a=RDX_OP(0,e); b=RDX_OP(1,e); d=RDX_OP(2,e); s=a+b+d)
RDX_KERNEL(rdx_opt_alg9,  e=RDX_OP(3,0); a=RDX_OP(0,0); b=RDX_OP(1,e); d=RDX_OP(2,e); s=a+b+d)
RDX_KERNEL(rdx_opt_alg10, a=RDX_OP(0,0); b=RDX_OP(1,0); e=RDX_OP(3,0); d=RDX_OP(2,e); s=a+b+d)
RDX_KERNEL(rdx_opt_alg11, a=RDX_OP(0,0); b=RDX_OP(1,0); d=RDX_OP(2,0); e=RDX_OP(3,0); s=a+b+d+e)

#undef RDX_KERNEL
#undef RDX_OP
#undef RDX_FAST

typedef void (*rdx_opt_kernel_t)(rdx_opt_ctl_t *c);
static const rdx_opt_kernel_t RDX_OPT_KERNEL[12] = {
    rdx_opt_alg0, rdx_opt_alg1, rdx_opt_alg2, rdx_opt_alg3,
    rdx_opt_alg4, rdx_opt_alg5, rdx_opt_alg6, rdx_opt_alg7,
    rdx_opt_alg8, rdx_opt_alg9, rdx_opt_alg10, rdx_opt_alg11
};

static __attribute__((noinline)) void rdx_render_optimized(track_t *t, voice_t *v,
                                                           int32_t *out, uint32_t len,
                                                           const vmod_t *m)
{
    rdx_note_t *n = rdx_note(t, v);
    const rdx_patch_t *p;
    uint32_t env[4][CTL];
    rdx_opt_ctl_t c;
    uint32_t i, k, alg;
    int32_t fb_macro, lfo0, lfo1, peg0, peg1;
    int32_t pitch0, pitch1, target, ext_pitch, mw0, mw1;
    if (!n || !n->live)
        return;
    if (len > CTL) {
        rdx_render_origin(t, v, out, len, m);
        return;
    }
    p = &n->patch;
    alg = n->algorithm % 12u;
    c.note = n;
    c.out = out;
    c.env = env;
    c.len = len;
    c.norm = n->norm;
    c.enable_mask = n->enable_mask;
    c.fb_type_mask = n->fb_type_mask;
    c.amd_mask = n->amd_mask;
    fb_macro = t->p[P_E1];
    if (n->fb_macro != fb_macro) {
        n->fb_macro = (int16_t)fb_macro;
        for (k = 0; k < 4u; k++)
            n->fb_scale[k] = RDX_FEEDBACK_Q16[clamp((int32_t)p->op[k].feedback + fb_macro, 0, 127)];
    }

    target = ((int32_t)rdx_master_note(t, v->note) + (int32_t)p->common.transpose - 64) * 65536;
    if (target != n->pitch_target) {
        n->pitch_target = target;
        if (p->common.mono_poly && p->common.porta_time)
            n->porta_left = RDX_PORTA_SAMPLES[p->common.porta_time & 127u];
        else {
            n->pitch = target;
            n->porta_left = 0u;
        }
    }
    pitch0 = n->pitch;
    if (n->porta_left) {
        uint32_t adv = len < n->porta_left ? len : n->porta_left;
        n->pitch += (n->pitch_target - n->pitch) * (int32_t)adv / (int32_t)n->porta_left;
        n->porta_left -= adv;
        if (!n->porta_left)
            n->pitch = n->pitch_target;
    }
    pitch1 = n->pitch;
    ext_pitch = (m->pitch16 - v->pitch_cur) * 4096 + m->fine * 277;
    peg0 = n->peg - (64 << 16);
    for (i = 0; i < len; i++)
        rdx_peg_tick(n);
    peg1 = n->peg - (64 << 16);
    rdx_lfo_block(n, len, &lfo0, &lfo1);
    mw0 = lfo0 * t->mw / 127;
    mw1 = lfo1 * t->mw / 127;
    for (k = 0; k < 4u; k++) {
        int32_t pm0 = (int32_t)(((int64_t)lfo0 * n->pm_depth) >> 15);
        int32_t pm1 = (int32_t)(((int64_t)lfo1 * n->pm_depth) >> 15);
        int32_t sem0, sem1, a0 = 32768, a1 = 32768;
        uint32_t end, span = len > 1u ? len - 1u : 1u, distance;
        if (!((n->pmd_mask >> k) & 1u))
            pm0 = pm1 = 0;
        sem0 = pitch0 - n->base_pitch + ext_pitch + pm0 + mw0 * 2;
        sem1 = pitch1 - n->base_pitch + ext_pitch + pm1 + mw1 * 2;
        if ((n->peg_mask >> k) & 1u) {
            sem0 += peg0;
            sem1 += peg1;
        }
        c.inc[k] = rdx_inc_mod(n->op[k].inc, sem0);
        end = rdx_inc_mod(n->op[k].inc, sem1);
        c.inc_down[k] = end < c.inc[k];
        distance = c.inc_down[k] ? c.inc[k] - end : end - c.inc[k];
        c.inc_delta[k] = distance / span;
        c.inc_rem[k] = distance % span;
        c.inc_error[k] = 0u;
        if ((n->amd_mask >> k) & 1u) {
            uint32_t depth = n->am_depth[k];
            a0 += (int32_t)(((int64_t)depth * (lfo0 * 2 - 32768)) >> 15) - mw0;
            a1 += (int32_t)(((int64_t)depth * (lfo1 * 2 - 32768)) >> 15) - mw1;
            a0 = clamp(a0, 0, 65536);
            a1 = clamp(a1, 0, 65536);
        }
        c.am[k] = a0;
        c.am_step[k] = len > 1u ? (a1 - a0) / (int32_t)(len - 1u) : 0;
        c.fb_scale[k] = n->fb_scale[k];
    }
    c.amp = m->amp0;
    c.damp = len > 1u ? (m->amp1 - m->amp0) / (int32_t)(len - 1u) : 0;
    rdx_opt_envelopes(&c, p, t->p[P_E4]);
    RDX_OPT_KERNEL[alg](&c);                   /* indirect: XIP and fast SRAM exceed call range */
}
