/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed-point ports of PicoFaceDX's stereo Flanger and compound Phaser.
 * Included by rdx_fx.c after the original Felucca swept-effect kernel so both
 * models remain available for live A/B comparison. */

#define RFX_FAST __attribute__((noinline))

/* PicoFace Phaser evaluates two first-order all-pass coefficients at the
 * block LFO position.  These 33-point Q16 tables cover the exact 90/500 Hz
 * bases swept over 24 semitones; interpolation error is hidden further by the
 * original 0.15 coefficient smoothing. */
static const uint16_t RFX_PF_AP90_Q16[33] = {
    63877, 63804, 63728, 63650, 63567, 63482, 63392, 63299,
    63201, 63100, 62994, 62884, 62769, 62649, 62524, 62394,
    62259, 62117, 61970, 61817, 61657, 61490, 61317, 61137,
    60949, 60753, 60549, 60337, 60117, 59887, 59648, 59400,
    59141
};
static const uint16_t RFX_PF_AP500_Q16[33] = {
    56820, 56460, 56088, 55701, 55299, 54883, 54451, 54004,
    53540, 53059, 52561, 52046, 51513, 50961, 50390, 49800,
    49190, 48560, 47910, 47238, 46546, 45831, 45095, 44336,
    43555, 42751, 41924, 41074, 40200, 39303, 38382, 37438,
    36469
};

/* PicoFace derives the Phaser's following short-delay excursion from DEPTH:
 * min(30 ms, 1 / (1000 * 2^((.0498*d+.061)/24) - 995)).  Store the result as
 * Q16 samples, so the expensive pow and divide never enter the audio ISR. */
static const uint32_t RFX_PF_PH_DELAY_Q16[128] = {
    86704128, 86704128, 86704128, 86704128, 86704128, 86704128, 86704128, 86704128,
    86704128, 86704128, 86704128, 86704128, 86704128, 86704128, 86704128, 86704128,
    86704128, 86704128, 86704128, 83732553, 80285849, 77107294, 74166770, 71438507,
    68900325, 66533030, 64319928, 62246433, 60299739, 58468563, 56742919, 55113943,
    53573734, 52115230, 50732100, 49418649, 48169742, 46980737, 45847427, 44765988,
    43732941, 42745109, 41799588, 40893718, 40025057, 39191359, 38390555, 37620738,
    36880144, 36167140, 35480214, 34817961, 34179076, 33562344, 32966634, 32390888,
    31834122, 31295411, 30773892, 30268756, 29779243, 29304639, 28844275, 28397520,
    27963778, 27542491, 27133129, 26735195, 26348215, 25971746, 25605363, 25248669,
    24901283, 24562847, 24233019, 23911475, 23597909, 23292025, 22993547, 22702208,
    22417756, 22139950, 21868559, 21603365, 21344157, 21090736, 20842910, 20600496,
    20363319, 20131212, 19904013, 19681569, 19463733, 19250363, 19041322, 18836482,
    18635717, 18438906, 18245934, 18056691, 17871068, 17688964, 17510280, 17334920,
    17162793, 16993810, 16827885, 16664938, 16504888, 16347660, 16193179, 16041373,
    15892176, 15745519, 15601339, 15459573, 15320162, 15183048, 15048174, 14915486,
    14784932, 14656461, 14530022, 14405569, 14283056, 14162436, 14043668, 13926708
};

static inline int32_t rfx_pf_line_read(const int16_t *line, uint32_t wp, uint32_t fill,
                                       uint32_t delay_q16, uint32_t mask)
{
    uint32_t whole = delay_q16 >> 16, frac = (delay_q16 >> 1) & 0x7FFFu;
    uint32_t r0, r1;
    int32_t a;
    if (!whole)
        whole = 1u;
    if (fill < whole + (frac != 0u))
        return 0;
    r0 = (wp - whole) & mask;
    r1 = (r0 - 1u) & mask;
    a = line[r0];
    return a + (((line[r1] - a) * (int32_t)frac) >> 15);
}

/* The retained RDX Flanger is the successful THK-W experiment: PicoFaceDX's
 * 1.5 ms base plus a DEPTH-scaled 0..5 ms excursion, a 5% longer right tap,
 * positive damped feedback and a .30..1.00 wet macro.  DigiChord's DEPTH 64
 * therefore sweeps 1.5..4.02 ms at about 65% wet, keeping its low-mid body. */
#define RFX_FL_MIN_Q16 4335206u                    /* 1.50 ms at 44.1 kHz */
#define RFX_FL_SPAN_Q16 14450688u                   /* 5.00 ms at DEPTH 127 */

static inline uint32_t rfx_flanger_max_delay_q16(uint32_t depth)
{
    depth &= 127u;
    return depth ? RFX_FL_MIN_Q16 +
                   (RFX_FL_SPAN_Q16 * depth + 63u) / 127u : 0u;
}

static inline uint32_t rfx_flanger_wet_q15(uint32_t depth)
{
    uint32_t x;
    depth &= 127u;
    if (!depth)
        return 0u;
    x = depth - 1u;
    return 9830u + (22938u * x + 63u) / 126u;          /* .30 .. 1.00 */
}

static inline uint32_t rfx_flanger_feedback_q15(uint32_t depth)
{
    uint32_t x;
    depth &= 127u;
    if (!depth)
        return 0u;
    x = depth - 1u;
    return 9830u + (14746u * x * x + 7938u) / 15876u; /* .30 .. .75 */
}

static inline uint32_t rfx_flanger_speed_inc(uint32_t speed)
{
    speed &= 127u;
    /* Phase increments per sample: 0.0500 Hz + speed^2 * 0.000400 Hz,
     * reaching 6.51 Hz without a divide or table lookup in the audio path. */
    return 4869u + speed * speed * 39u;
}

static inline uint32_t rfx_flanger_exp_mod(int32_t sine)
{
    uint32_t uni = (uint32_t)(sine + 32768);            /* 0 .. 65535 */
    return (uni * uni) >> 16;                           /* exponential unipolar sweep */
}

static RFX_FAST void rfx_flanger_rdx(rdx_fx_slot_t *s, int32_t *l, int32_t *r,
                                     uint32_t n, uint32_t si)
{
    int16_t *ll = RFX_PF_FL_LINE(si, 0), *lr = RFX_PF_FL_LINE(si, 1);
    uint32_t i, wp = s->pf_fl_wp, fill = s->pf_fl_fill, ph = s->pf_ph;
    uint32_t inc = rfx_flanger_speed_inc(s->p2);
    uint32_t dmin = RFX_FL_MIN_Q16;
    uint32_t dmax = rfx_flanger_max_delay_q16(s->p1);
    uint32_t wet = rfx_flanger_wet_q15(s->p1);
    int32_t feedback = (int32_t)rfx_flanger_feedback_q15(s->p1);
    for (i = 0; i < n; i++) {
        uint32_t wave = rfx_flanger_exp_mod(osc_sine(ph));
        uint32_t dl = dmin + (uint32_t)mulq16((int32_t)(dmax - dmin), wave);
        uint32_t dr = dl + dl / 20u;
        int32_t vl = rfx_pf_line_read(ll, wp, fill, dl, RFX_PF_FL_MASK);
        int32_t vr, fbl, fbr, yl, yr;
        vr = rfx_pf_line_read(lr, wp, fill, dr, RFX_PF_FL_MASK);
        int32_t xl = clamp(l[i], -196607, 196607), xr = clamp(r[i], -196607, 196607);
        yl = clamp(xl + (mulq15(vl, (int32_t)wet) << 2), -262143, 262143);
        yr = clamp(xr + (mulq15(vr, (int32_t)wet) << 2), -262143, 262143);
        /* At high feedback, damp the returned energy before the soft knee so
         * short-delay FM harmonics cannot turn into a metallic digital howl. */
        fbl = mulq15(vl, feedback);
        fbr = mulq15(vr, feedback);
        s->pf_fl_fb_lp[0] += ((fbl - s->pf_fl_fb_lp[0]) * 3) >> 2;
        s->pf_fl_fb_lp[1] += ((fbr - s->pf_fl_fb_lp[1]) * 3) >> 2;
        ll[wp] = (int16_t)clamp((xl >> 2) + softclip(s->pf_fl_fb_lp[0]), -32768, 32767);
        lr[wp] = (int16_t)clamp((xr >> 2) + softclip(s->pf_fl_fb_lp[1]), -32768, 32767);
        l[i] = mulq16(yl, 45875u);                     /* x 0.7 */
        r[i] = mulq16(yr, 45875u);
        wp = (wp + 1u) & RFX_PF_FL_MASK;
        if (fill < RFX_PF_FL_LEN)
            fill++;
        ph += inc;
    }
    s->pf_fl_wp = (uint16_t)wp;
    s->pf_fl_fill = (uint16_t)fill;
    s->pf_ph = ph;
}

#undef RFX_FL_MIN_Q16
#undef RFX_FL_SPAN_Q16

static inline uint32_t rfx_pf_ap_coef(const uint16_t *tab, uint32_t tri)
{
    uint32_t i = tri >> 11, f = tri & 2047u;
    return tab[i] + (uint32_t)(((int32_t)tab[i + 1u] - tab[i]) * (int32_t)f >> 11);
}

static inline int32_t rfx_phaser_ap_sample(rdx_fx_slot_t *s, int32_t x, uint32_t ch)
{
    ins_t *q = &s->ins[ch];
    int32_t dry = clamp(x, -196607, 196607);
    int32_t y = dry - q->hp + mulq16(q->lp1, 65274u);  /* PicoFace dcBlock, k .996 */
    uint32_t st;
    q->hp = dry;
    q->lp1 = y;
    y = clamp(y + mulq16(q->fb, 45875u), -262143, 262143); /* feedback .7 */
    for (st = 0; st < 2u; st++) {
        int32_t t = q->px[st] - mulq16(y, s->pf_alpha[st]);
        q->px[st] = clamp(y + mulq16(t, s->pf_alpha[st]), -262143, 262143);
        y = clamp(t, -262143, 262143);
    }
    q->fb = y;
    return dry + mulq16(y - dry, 39322u);               /* phaserMix .6 */
}

/* PicoFaceDX Phaser is deliberately compound: a two-stage stereo all-pass
 * phaser followed by a no-feedback stereo modulated delay at 45% mix. */
static RFX_FAST void rfx_phaser_rdx(rdx_fx_slot_t *s, int32_t *l, int32_t *r,
                                    uint32_t n, uint32_t si)
{
    int16_t *ll = RFX_PF_PH_LINE(si, 0), *lr = RFX_PF_PH_LINE(si, 1);
    uint32_t i, wp = s->pf_ph_wp, fill = s->pf_ph_fill;
    uint32_t phinc = (uint32_t)mulq16((int32_t)RDX_LFO_INC[s->p2 & 127u], 12321u) + 9118u;
    uint32_t auxinc = RDX_LFO_INC[s->p2 & 127u] >> 1;
    uint32_t tr, target, depth = RFX_PF_PH_DELAY_Q16[s->p1 & 127u];
    uint32_t fl, fr, dl, dr;

    s->pf_ph += n * phinc;                             /* PicoFace updates both LFOs once per block */
    tr = pf_tri(s->pf_ph);
    target = rfx_pf_ap_coef(RFX_PF_AP90_Q16, tr);
    s->pf_alpha[0] += mulq16((int32_t)target - (int32_t)s->pf_alpha[0], 9830u); /* .15 smoothing */
    target = rfx_pf_ap_coef(RFX_PF_AP500_Q16, tr);
    s->pf_alpha[1] += mulq16((int32_t)target - (int32_t)s->pf_alpha[1], 9830u);

    s->pf_aux_ph += n * auxinc;
    tr = pf_tri(s->pf_aux_ph);
    fl = 6554u + (uint32_t)mulq16((int32_t)tr, 39322u); /* .1 + .6 triangle */
    fr = 45875u - (uint32_t)mulq16((int32_t)tr, 39322u);/* .7 - .6 triangle */
    dl = (uint32_t)mulq16((int32_t)depth, fl);
    dr = (uint32_t)mulq16((int32_t)depth, fr);

    for (i = 0; i < n; i++) {
        int32_t xl = rfx_phaser_ap_sample(s, l[i], 0u);
        int32_t xr = rfx_phaser_ap_sample(s, r[i], 1u);
        int32_t vl = rfx_pf_line_read(ll, wp, fill, dl, RFX_PF_PH_MASK) << 2;
        int32_t vr = rfx_pf_line_read(lr, wp, fill, dr, RFX_PF_PH_MASK) << 2;
        ll[wp] = (int16_t)clamp(xl >> 2, -32768, 32767);
        lr[wp] = (int16_t)clamp(xr >> 2, -32768, 32767);
        l[i] = xl + mulq16(vl - xl, 29491u);            /* flangerMix .45 */
        r[i] = xr + mulq16(vr - xr, 29491u);
        wp = (wp + 1u) & RFX_PF_PH_MASK;
        if (fill < RFX_PF_PH_LEN)
            fill++;
    }
    s->pf_ph_wp = (uint16_t)wp;
    s->pf_ph_fill = (uint16_t)fill;
}

#undef RFX_FAST
