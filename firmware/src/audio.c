/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* I2S output (ALNK0 -> external codec) and the
 * audio ISR: per half buffer, blocks of CTL samples: mix_block (fx.c: events ->
 * each synth part -> dist -> level / pan -> sends -> buses -> master) -> USB audio (uac_tap) -> the metronome's
 * click (click.c, the DAC only) -> 24-bit stereo. */
/* registers: hal/fm1_audio.h */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define DAC_TICKS 544u            /* TIMER4 ticks per I2S frame (24 MHz / 44,117.6 Hz) */
#define CPU_AVG (4096u / HALF_FRAMES)   /* halves the load meter averages: ~93 ms whatever the half */
#define OUT_SHIFT 7               /* Q15 -> 24-bit, -6 dBFS ceiling (M0f ran clean at -18 dBFS) */

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

/* diagnostics, kept across resets and UBOOT entry: read with `fm1t memr` */
#define DBG_MAGIC 0x44424731u                       /* "DBG1" */
struct felucca_dbg {
    uint32_t magic, halves, max_us, nested, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots;
    uint32_t stage, page, home;           /* where the main loop is (breadcrumbs) */
    uint32_t prev_stage, prev_page, prev_home, prev_rst, prev_frames;   /* as found at boot */
} felucca_dbg __attribute__((section(".noinit")));
static volatile uint32_t audio_halves, audio_max_us;
static volatile uint32_t audio_late_notice;             /* DMA-deadline misses since this boot, for the UI diagnostic */
enum { AUDIO_LATE_C0 = 1u, AUDIO_LATE_C1 = 2u, AUDIO_LATE_SYNC = 4u };
static volatile uint8_t audio_late_source;              /* source of the most recent miss, for the overlay */
static uint32_t audio_last_half_tick;                   /* previous half IRQ, for non-accumulating entry jitter */
static uint32_t audio_last_late_half;
static uint16_t audio_late_burst;
#if FELUCCA_CPU2
static volatile uint32_t audio_c1_max_us;
#endif
static uint32_t audio_cpu_rem;                         /* keep the fractional IIR step: no low-load bias */
#if FELUCCA_CPU2
static uint32_t audio_cpu1_rem;
#endif

static void audio_block(int32_t *out, uint32_t n)       /* mix (fx.c), then Q15 -> 24 bit */
{
    uint32_t i;
    mix_block(out, n);
    AUDIO_TIME_START(t_tail);
#if FELUCCA_UAC
    uac_tap(out, n);                                    /* the USB audio input: the same master output */
#endif
    if (fx_usb_fixed)                                   /* USB LEVEL FIXED: USB took the full level, the DAC */
        usb_fixed_dac(out, n);                          /* (speaker, headphones) gets MASTER's (fx.c) */
    if (!scope_mix)                                     /* the HOME scope (SCOPE OUT): the music, before the click */
        for (i = 1; i < n; i += 2u)                     /* (MIX: fx.c mix_block took it) */
            scope_buf[scope_w++ & (SCOPE_N - 1u)] = (int16_t)out[2u * i];
    click_render(out, n);                               /* the metronome (click.c): the DAC only, not USB */
    for (i = 0; i < n; i++) {
        out[2u * i] *= 1 << OUT_SHIFT;
        out[2u * i + 1u] *= 1 << OUT_SHIFT;
    }
    AUDIO_TIME_ADD(tail, t_tail);
}

/* Overload: two halves in a row above 85 % (the render plus TIMER5 nested in it), fade one voice over the
 * next block, and one more each half while it lasts. Keep each part's
 * lowest POLY note and MONO / LEGATO / UNISON lead, as the shared budget does. */
static volatile uint8_t shed_req;                     /* bit 0: high load, bit 1: an actual DMA deadline miss */
static uint8_t shed_over;                              /* bit k: the half k halves ago was over 85 % */
#define SHED_TICKS ((HALF_FRAMES * 1000000u / FS) * 85u / 100u * FM1_TICKS_PER_US)   /* 85 % of a half */
static uint32_t shed_count;

/* end of a half: all = its ticks, TIMER5 nested in it included -> the render alone in us. The deadline sees
 * both; shed after 2 overloaded halves in a row (out of line: keeps the ISR's render loops as they were) */
static __attribute__((noinline)) uint32_t shed_check(uint32_t all)
{
    shed_over = (uint8_t)(shed_over << 1 | (all > SHED_TICKS));
    if ((shed_over & 3u) == 3u)
        shed_req |= 1u;
    return (all - t5_nested_ticks) / FM1_TICKS_PER_US;
}

static int shed_fade_active(void)
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            if (trk[p].v[i].active && trk[p].v[i].stage == 4u)
                return 1;
    return 0;
}

/* Attribute a missed wall deadline using each core's independent work in the
 * same half.  If neither core alone consumed a full half, the miss came from
 * the four block rendezvous/IRQ timing rather than one overloaded core. */
static uint8_t audio_late_cause(uint32_t all_ticks)
{
    const uint32_t deadline = HALF_FRAMES * DAC_TICKS;
    uint32_t c0_ticks = all_ticks;
    uint8_t source = 0;
#if FELUCCA_CPU2
    c0_ticks = all_ticks > fm1_c1_wait_ticks_half ? all_ticks - fm1_c1_wait_ticks_half : 0u;
    if (c1_half_ticks >= deadline || fm1_c1_failed_half)
        source |= AUDIO_LATE_C1;
#endif
    if (c0_ticks >= deadline)
        source |= AUDIO_LATE_C0;
    return source ? source : AUDIO_LATE_SYNC;
}

/* Publish one half as microseconds.  C0 is wall time less synchronization
 * waiting, matching the existing CORE 0 meter.  The named C0 phases are
 * exclusive; OTHER is the remainder (part inserts/sends, dispatch plumbing
 * outside RDX, and small loop overhead).  TIMER5 is shown separately as a
 * subset of whichever C0 phase it interrupted, so it must not be added again. */
static void audio_timing_publish(uint32_t wall_ticks, uint32_t nested_ticks, int late, uint8_t source)
{
    audio_timing_sample_t s;
    uint32_t named;
    audio_timing.cur.wall = wall_ticks;
    audio_timing.cur.t5 = nested_ticks;
#if FELUCCA_CPU2
    audio_timing.cur.wait = fm1_c1_wait_ticks_half;
    audio_timing.cur.c1 = c1_half_ticks;
    audio_timing.cur.c1_fm = rdx_voice_ticks_c1;
#else
    audio_timing.cur.wait = 0;
    audio_timing.cur.c1 = 0;
    audio_timing.cur.c1_fm = 0;
#endif
#if FELUCCA_RDX
    audio_timing.cur.hall3 = rdx_fx_perf[RDX_FXPERF_HALL3].half_ticks;
    audio_timing.cur.chorus2 = rdx_fx_perf[RDX_FXPERF_CHORUS2].half_ticks;
#endif
    audio_timing.cur.c0 = wall_ticks > audio_timing.cur.wait ? wall_ticks - audio_timing.cur.wait : 0u;
    named = audio_timing.cur.pre + audio_timing.cur.rdx_ctl + audio_timing.cur.c0_fm + audio_timing.cur.xfer +
            audio_timing.cur.rdx_fx + audio_timing.cur.mix + audio_timing.cur.post_fx + audio_timing.cur.tail;
    audio_timing.cur.other = audio_timing.cur.c0 > named ? audio_timing.cur.c0 - named : 0u;
#define AUDIO_US(field) s.field = (audio_timing.cur.field + FM1_TICKS_PER_US / 2u) / FM1_TICKS_PER_US
    AUDIO_US(wall); AUDIO_US(irq); AUDIO_US(c0); AUDIO_US(wait); AUDIO_US(c1); AUDIO_US(c1_fm); AUDIO_US(t5);
    AUDIO_US(t5_input); AUDIO_US(t5_uac);
    AUDIO_US(pre); AUDIO_US(midi); s.midi_events = audio_timing.cur.midi_events;
    AUDIO_US(rdx_ctl); AUDIO_US(c0_fm); AUDIO_US(xfer);
    AUDIO_US(rdx_fx); AUDIO_US(mix); AUDIO_US(post_fx); AUDIO_US(tail); AUDIO_US(other);
    AUDIO_US(hall3); AUDIO_US(chorus2);
#undef AUDIO_US
#define AUDIO_PEAK(field) do { if (s.field > audio_timing.peak.field) audio_timing.peak.field = s.field; } while (0)
    AUDIO_PEAK(wall); AUDIO_PEAK(irq); AUDIO_PEAK(c0); AUDIO_PEAK(wait); AUDIO_PEAK(c1); AUDIO_PEAK(c1_fm); AUDIO_PEAK(t5);
    AUDIO_PEAK(t5_input); AUDIO_PEAK(t5_uac);
    AUDIO_PEAK(pre); AUDIO_PEAK(midi); AUDIO_PEAK(midi_events);
    AUDIO_PEAK(rdx_ctl); AUDIO_PEAK(c0_fm); AUDIO_PEAK(xfer);
    AUDIO_PEAK(rdx_fx); AUDIO_PEAK(mix); AUDIO_PEAK(post_fx); AUDIO_PEAK(tail); AUDIO_PEAK(other);
    AUDIO_PEAK(hall3); AUDIO_PEAK(chorus2);
#undef AUDIO_PEAK
    if (late) {
        uint32_t seq = audio_timing.late_seq + 1u, slot = (seq - 1u) & (AUDIO_LATE_HISTORY - 1u);
        uint32_t gap = audio_last_late_half ? audio_halves - audio_last_late_half : 0u;
        volatile audio_late_record_t *r = &audio_timing.late[slot];
        audio_late_burst = (audio_last_late_half && gap <= 4u) ? (uint16_t)(audio_late_burst + 1u) : 1u;
        audio_last_late_half = audio_halves;
        r->timing = s;
        r->count = audio_late_notice + 1u;
        r->gap = gap;
        r->burst = audio_late_burst;
        r->source = source;
        RING_PUBLISH();                                /* record contents before making the slot visible */
        audio_timing.late_seq = seq;
    }
}

/* Main/UI-side stable copy of one recent late half. back=0 is newest. */
static int audio_late_snapshot(uint32_t back, audio_late_record_t *out, uint32_t *available)
{
    uint32_t a, b, n, slot;
    do {
        a = audio_timing.late_seq;
        if (!a) {
            *available = 0;
            return 0;
        }
        n = a < AUDIO_LATE_HISTORY ? a : AUDIO_LATE_HISTORY;
        if (back >= n)
            back = n - 1u;
        slot = (a - 1u - back) & (AUDIO_LATE_HISTORY - 1u);
        *out = audio_timing.late[slot];
        RING_PUBLISH();
        b = audio_timing.late_seq;
    } while (a != b);
    *available = n;
    return 1;
}

static void shed_voice(int hard)
{
    uint32_t p, i;
    voice_t *best = 0;
    track_t *best_t = 0;
    /* A force-stopped RDX voice still costs CPU during its 256-sample fade.
     * Wait for that fade to finish instead of killing another voice every
     * half and collapsing a chord before the first deletion saved any work. */
    if (shed_fade_active())
        return;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &trk[p].v[i];
            if (v->active && !v->gate && v->stage != 4u && (!best || v->env < best->env)) {
                best = v;
                best_t = &trk[p];
            }
        }
    if (best) {
        voice_kill(best_t, best);
        shed_count++;
        return;
    }
    /* Crossing the 85 % warning threshold is enough to shorten an inaudible
     * release tail, but never an excuse to delete a key that is still down.
     * Held notes are sacrificed only after hardware reports a real missed DMA
     * deadline (AUDIO LATE). */
    if (!hard)
        return;
    for (p = 0; p < NPART; p++) {
        const track_t *t = &trk[p];
        uint32_t mode = trk_vmode(t), low = mode == V_POLY ? lowest_held(t) : 0u;
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &trk[p].v[i];
            if (v->active && v->gate && v->stage != 4u &&
                ((mode == V_POLY && i != low) || (mode != V_POLY && i > 0u))) {
                if (!best || v->age < best->age) {
                    best = v;
                    best_t = &trk[p];
                }
            }
        }
    }
    if (best) {
        voice_kill(best_t, best);
        shed_count++;
    }
}

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    uint32_t t0;
    felucca_dbg.in_audio = 1;                   /* first: TIMER5 nests from here on (main.c) */
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, us, total_us, all_ticks, nested_ticks;
        uint8_t late, source = 0;
        int32_t *o = &abuf[half * HALF_WORDS];
        /* Consume the interrupt that brought us here before rendering.  If
         * DMA crosses the next half-buffer boundary while this block is still
         * being produced, the hardware can latch a fresh pending interrupt.
         * Acknowledging only at the end used to clear that new event too,
         * delaying recovery by one complete 128-frame period after a miss. */
        fm1_audio_ack_half();
        memset(&audio_timing.cur, 0, sizeof audio_timing.cur);
        /* Measure only this half's excess start-to-start interval.  The old
         * absolute schedule accumulated a tiny sample-clock mismatch forever
         * (and stayed one whole period behind after a coalesced interrupt), so
         * its IRQ value could grow by tens of milliseconds while audio was
         * otherwise on time. */
        if (audio_last_half_tick) {
            uint32_t gap = t0 - audio_last_half_tick, period = HALF_FRAMES * DAC_TICKS;
            audio_timing.cur.irq = gap > period ? gap - period : 0u;
        }
        audio_last_half_tick = t0;
#if FELUCCA_CPU2
        c1_half_ticks = 0;
        fm1_c1_wait_ticks_half = 0;
        fm1_c1_failed_half = 0;
#endif
#if FELUCCA_RDX
        rdx_perf_half_begin();
        rdx_fx_perf_half_begin();
#endif
        if (shed_req) {
            uint8_t req = shed_req;
            shed_req = 0;
            shed_voice((req & 2u) != 0u);
        }
#if FELUCCA_UAC
        uac_render_start();
#endif
        for (b = 0; b < HALF_FRAMES; b += CTL) {
#ifdef FM1_INPUT_LAT
            kb_out_tick = t0 + (HALF_FRAMES + b) * DAC_TICKS;   /* when this block plays (seq.c kb_lat) */
#endif
            audio_block(o + 2u * b, CTL);
        }
        all_ticks = fm1_ticks() - t0;                  /* data ready: exclude diagnostic bookkeeping below */
        nested_ticks = t5_nested_ticks;
        late = fm1_audio_free_half() != half;          /* test immediately, while this result still names this half */
#if FELUCCA_RDX
        rdx_perf_half_end();
        rdx_fx_perf_half_end();
#endif
        audio_halves++;
        total_us = shed_check(all_ticks);
#if FELUCCA_CPU2
        /* Independent Core0 work: the half's wall time less synchronization
         * spinning. Core1 has its own actual render counter below. */
        us = (all_ticks > fm1_c1_wait_ticks_half ? all_ticks - fm1_c1_wait_ticks_half : 0u) /
             FM1_TICKS_PER_US;
#else
        us = total_us;
#endif
        if (us > audio_max_us)
            audio_max_us = us;
        {
            uint32_t load = song.cpu_q8 * (CPU_AVG - 1u) + (us * 256u) / (HALF_FRAMES * 1000000u / FS) + audio_cpu_rem;
            song.cpu_q8 = load / CPU_AVG;
            audio_cpu_rem = load % CPU_AVG;
        }
#if FELUCCA_CPU2
        {
            uint32_t c1us = c1_half_ticks / FM1_TICKS_PER_US;
            uint32_t load = song.cpu1_q8 * (CPU_AVG - 1u) +
                            (c1us * 256u) / (HALF_FRAMES * 1000000u / FS) + audio_cpu1_rem;
            song.cpu1_q8 = load / CPU_AVG;
            audio_cpu1_rem = load % CPU_AVG;
            if (c1us > audio_c1_max_us)
                audio_c1_max_us = c1us;
        }
#endif
        if (late) {
            source = audio_late_cause(all_ticks);
            felucca_dbg.late++;                         /* the DMA moved on while we rendered */
            shed_req |= 2u;                             /* next half may fade one held voice */
        }
        audio_timing_publish(all_ticks, nested_ticks, late, source);
        if (late) {
            audio_late_source = source;
            RING_PUBLISH();                            /* source and timing record before the visible count */
            audio_late_notice++;
        }
        felucca_dbg.halves++;
        felucca_dbg.last_us = total_us;                  /* deadline wall time, for console diagnostics */
        if (total_us > felucca_dbg.max_us)
            felucca_dbg.max_us = total_us;
        felucca_dbg.cpu_q8 = song.cpu_q8;
    }
    felucca_dbg.in_audio = 0;
}
extern void isr_alnk0(void);

static void audio_init(void)                   /* codec and ALNK0 bring-up (fm1_audio.h) */
{
    uint32_t i;
    audio_last_half_tick = 0;
    audio_last_late_half = 0;
    audio_late_burst = 0;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
