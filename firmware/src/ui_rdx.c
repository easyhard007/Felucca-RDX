/* SPDX-License-Identifier: GPL-3.0-or-later
 * RDX raw-patch editor used by ENV, LFO and EDIT.  The controls edit the
 * published 150-byte voice rather than Felucca's generic macro parameters. */
#if FELUCCA_RDX
static int32_t accel(uint32_t role, int32_t s, int32_t range);

static uint8_t rdx_eg_row, rdx_ksc_row, rdx_mod_row, rdx_lvl_row, rdx_freq_row;

static int rdx_ui_page(uint32_t g)
{
    return g >= GR_RDX_ALG && g <= GR_RDX_LFO;
}

static uint8_t *rdx_ui_row_ptr(uint32_t g)
{
    if (g == GR_RDX_EG) return &rdx_eg_row;
    if (g == GR_RDX_KSC) return &rdx_ksc_row;
    if (g == GR_RDX_MOD) return &rdx_mod_row;
    if (g == GR_RDX_LVL) return &rdx_lvl_row;
    if (g == GR_RDX_FREQ) return &rdx_freq_row;
    return 0;
}

static uint32_t rdx_ui_rows(uint32_t g)
{
    return g == GR_RDX_EG ? 8u : g == GR_RDX_KSC ? 5u :
           g == GR_RDX_MOD ? 3u : g == GR_RDX_LVL ? 4u :
           g == GR_RDX_FREQ ? 4u : 0u;
}

static void rdx_ui_select(int32_t steps)
{
    uint8_t *r = rdx_ui_row_ptr(cur_page()->graph);
    uint32_t n = rdx_ui_rows(cur_page()->graph);
    if (!r || !n || !steps) return;
    *r = (uint8_t)clamp((int32_t)*r + steps, 0, (int32_t)n - 1);
    ui.hot_t = 0;
    ui.force = 1;
}

static uint8_t *rdx_ui_byte(rdx_patch_t *p, uint32_t g, uint32_t row, uint32_t op,
                            int32_t *lo, int32_t *hi)
{
    rdx_op_param_t *o = &p->op[op & 3u];
    *lo = 0; *hi = 127;
    if (g == GR_RDX_ALG) { *hi = 1; return &o->enable; }
    if (g == GR_RDX_PEGR) return &p->common.peg_rate[op & 3u];
    if (g == GR_RDX_PEGL) { *lo = 16; *hi = 112; return &p->common.peg_level[op & 3u]; }
    if (g == GR_RDX_LFO) {
        if (!op) { *hi = 6; return &p->common.lfo_wave; }
        if (op == 1u) return &p->common.lfo_speed;
        if (op == 2u) return &p->common.lfo_delay;
        return &p->common.lfo_pmd;
    }
    if (g == GR_RDX_EG)
        return (row & 1u) ? &o->eg_level[row >> 1] : &o->eg_rate[row >> 1];
    if (g == GR_RDX_KSC) {
        if (!row) return &o->rate_scaling;
        if (row == 1u) return &o->scale_ld;
        if (row == 2u) return &o->scale_rd;
        *hi = 3;
        return row == 3u ? &o->scale_lc : &o->scale_rc;
    }
    if (g == GR_RDX_MOD) {
        if (!row) return &o->lfo_amd;
        *hi = 1;
        return row == 1u ? &o->lfo_pmd_enable : &o->peg_enable;
    }
    if (g == GR_RDX_LVL) {
        if (!row) return &o->vel_sens;
        if (row == 1u) return &o->out_level;
        if (row == 2u) { *hi = 1; return &o->fb_type; }
        return &o->feedback;
    }
    if (g == GR_RDX_FREQ) {
        if (!row) { *hi = 1; return &o->freq_mode; }
        if (row == 1u) { *hi = 31; return &o->freq_coarse; }
        if (row == 2u) { *hi = 99; return &o->freq_fine; }
        return &o->freq_detune;
    }
    return 0;
}

static void rdx_ui_publish(rdx_patch_t *p)
{
    uint32_t tr = song.sel % NTRK;
    rdx_track_set(tr, p, 1);
    TSEL->p[P_E0] = 0;                         /* algorithm is owned by the RDX patch */
    sync_reload = 1;                           /* a connected Web Editor reads the new raw voice */
    ui.force = 1;
}

static void rdx_ui_knob(uint32_t slot, int32_t steps)
{
    rdx_patch_t p;
    uint32_t g = cur_page()->graph, row = 0;
    uint8_t *rp = rdx_ui_row_ptr(g), *b;
    int32_t lo, hi, d, nv;
    if (!rdx_ui_page(g) || !steps) return;
    if (rp) row = *rp;
    rdx_patch_for_track(song.sel, &p);
    b = rdx_ui_byte(&p, g, row, slot, &lo, &hi);
    if (!b) return;
    d = hi <= 6 ? (steps > 0 ? 1 : -1) : accel(EN_K1 + slot, steps, hi - lo);
    nv = clamp((int32_t)*b + d, lo, hi);
    if (nv == *b) return;
    *b = (uint8_t)nv;
    rdx_ui_publish(&p);
}

static void rdx_ui_open_algorithm(void)
{
    uint32_t i;
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].graph == GR_RDX_ALG) {
            ui.page = (uint8_t)i;
            ui.fam_last[FAM_EDIT] = ui.page;
            ui.home = 0;
            page_entered();
            return;
        }
}

static void rdx_ui_algorithm_turn(int32_t steps)
{
    rdx_patch_t p;
    int32_t a;
    if (!steps) return;
    rdx_patch_for_track(song.sel, &p);
    a = clamp((int32_t)p.common.algorithm + steps, 0, 11);
    if (a != p.common.algorithm) {
        p.common.algorithm = (uint8_t)a;
        rdx_ui_publish(&p);
    }
    rdx_ui_open_algorithm();
}

static void rdx_ui_signed(char *s, int32_t v)
{
    if (v > 0) { *s++ = '+'; }
    fmt_int(s, v);
}

static int rdx_ui_card(uint32_t slot, const char **label, char *val,
                       const char **unit, int32_t *ratio, uint32_t *icon)
{
    static const char *const WAVE[7] = {"SINE","TRI","SAW+","SAW-","SQR","S&H8","S&H"};
    static const char *const CURVE[4] = {"-LIN","-EXP","+EXP","+LIN"};
    static const char *const STAGE[4] = {"1","2","3","4"};
    rdx_patch_t p;
    uint32_t g = cur_page()->graph, row = 0, v;
    uint8_t *rp = rdx_ui_row_ptr(g), *b;
    int32_t lo, hi;
    rdx_patch_for_track(song.sel, &p);
    if (rp) row = *rp;
    b = rdx_ui_byte(&p, g, row, slot, &lo, &hi);
    if (!b) return 0;
    v = *b;
    *unit = ""; *icon = ICON_AUTO;
    *ratio = hi > lo ? ((int32_t)v - lo) * 1000 / (hi - lo) : -1;
    if (g == GR_RDX_ALG) {
        static const char *const OP[4] = {"OP1","OP2","OP3","OP4"};
        *label = OP[slot & 3u]; str_cpy(val, v ? "ON" : "OFF", 12); *ratio = -1;
        return v != 0u;
    }
    if (g == GR_RDX_LFO) {
        static const char *const L[4] = {"WAVE","SPEED","DELAY","PMD"};
        *label = L[slot & 3u];
        if (!slot) str_cpy(val, WAVE[v % 7u], 12); else fmt_int(val, (int32_t)v);
        return 1;
    }
    if (g == GR_RDX_PEGR || g == GR_RDX_PEGL) {
        static const char *const R[4] = {"RATE1","RATE2","RATE3","RATE4"};
        static const char *const L[4] = {"LEVEL1","LEVEL2","LEVEL3","LEVEL4"};
        *label = g == GR_RDX_PEGR ? R[slot & 3u] : L[slot & 3u];
        if (g == GR_RDX_PEGL) rdx_ui_signed(val, (int32_t)v - 64); else fmt_int(val, (int32_t)v);
        return 1;
    }
    {
        static const char *const OP[4] = {"OP1","OP2","OP3","OP4"};
        *label = OP[slot & 3u];
    }
    if ((g == GR_RDX_KSC && row >= 3u)) str_cpy(val, CURVE[v & 3u], 12);
    else if ((g == GR_RDX_MOD && row >= 1u)) str_cpy(val, v ? "ON" : "OFF", 12);
    else if (g == GR_RDX_LVL && row == 2u) str_cpy(val, v ? "SQR" : "SAW", 12);
    else if (g == GR_RDX_FREQ && row == 0u) str_cpy(val, v ? "FIXED" : "RATIO", 12);
    else if (g == GR_RDX_FREQ && row == 3u) rdx_ui_signed(val, (int32_t)v - 64);
    else fmt_int(val, (int32_t)v);
    if (g == GR_RDX_EG) *unit = STAGE[row >> 1];
    return p.op[slot & 3u].enable != 0u;
}

static uint32_t rdx_ui_signature(void)
{
    rdx_patch_t p;
    uint32_t h = 2166136261u;
    rdx_patch_for_track(song.sel, &p);
    h = fnv(h, &p, sizeof p);
    h ^= (rdx_eg_row + 1u) * 40503u + (rdx_ksc_row + 1u) * 7919u;
    h ^= (rdx_mod_row + 1u) * 104729u + (rdx_lvl_row + 1u) * 1299709u;
    return h ^ (rdx_freq_row + 1u) * 2246822519u;
}

static const char *rdx_ui_row_name(uint32_t g, uint32_t row)
{
    static const char *const EG[8] = {"OP1 RATE","OP1 LEVEL","OP2 RATE","OP2 LEVEL",
                                      "OP3 RATE","OP3 LEVEL","OP4 RATE","OP4 LEVEL"};
    static const char *const KSC[5] = {"RATE","LEFT DEPTH","RIGHT DEPTH","LEFT CURVE","RIGHT CURVE"};
    static const char *const MOD[3] = {"LFO AMD","LFO PMD","PITCH EG"};
    static const char *const LVL[4] = {"VELOCITY","OUT LEVEL","FB TYPE","FEEDBACK"};
    static const char *const FRQ[4] = {"MODE","COARSE","FINE","DETUNE"};
    return g == GR_RDX_EG ? EG[row % 8u] : g == GR_RDX_KSC ? KSC[row % 5u] :
           g == GR_RDX_MOD ? MOD[row % 3u] : g == GR_RDX_LVL ? LVL[row % 4u] : FRQ[row % 4u];
}

static void rdx_ui_graph_matrix(uint32_t g)
{
    static const char *const CURVE[4] = {"-L","-E","+E","+L"};
    rdx_patch_t p;
    uint8_t *rp = rdx_ui_row_ptr(g);
    uint32_t rows = rdx_ui_rows(g), row, op;
    rdx_patch_for_track(song.sel, &p);
    for (row = 0; row < rows; row++) {
        int32_t y = 5 + (int32_t)row * (88 / (int32_t)rows);
        uint16_t bg = rp && row == *rp ? T_THEME : T_SURF;
        uint16_t fg = rp && row == *rp ? T_INK : T_MID;
        char v[12];
        if (rp && row == *rp) cv_rrect(7, y - 2, 226, 15, 3, bg, T_SURF);
        cv_text_on(12, y, &AF_S, rdx_ui_row_name(g, row), fg, bg);
        for (op = 0; op < 4u; op++) {
            rdx_patch_t q = p;
            int32_t lo, hi;
            uint8_t *b = rdx_ui_byte(&q, g, row, op, &lo, &hi);
            uint32_t n = b ? *b : 0u;
            if (g == GR_RDX_KSC && row >= 3u) str_cpy(v, CURVE[n & 3u], sizeof v);
            else if ((g == GR_RDX_MOD && row >= 1u)) str_cpy(v, n ? "ON" : "OFF", sizeof v);
            else if (g == GR_RDX_LVL && row == 2u) str_cpy(v, n ? "SQR" : "SAW", sizeof v);
            else if (g == GR_RDX_FREQ && row == 0u) str_cpy(v, n ? "FIX" : "RAT", sizeof v);
            else if (g == GR_RDX_FREQ && row == 3u) rdx_ui_signed(v, (int32_t)n - 64);
            else fmt_int(v, (int32_t)n);
            cv_text_in(76 + (int32_t)op * 38, y, 36, &AF_S, v,
                       p.op[op].enable ? fg : T_DIM, bg);
        }
    }
    if (g == GR_RDX_FREQ) {
        char f[12];
        cv_text_on(12, 96, &AF_S, "RESULT", T_DIM, T_SURF);
        for (op = 0; op < 4u; op++) {
            const rdx_op_param_t *o = &p.op[op];
            if (!o->freq_mode) {
                uint32_t r = (o->freq_coarse ? o->freq_coarse * 100u : 50u) * (100u + o->freq_fine) / 100u;
                uint32_t n;
                f[0] = 'x'; fmt_int(f + 1, (int32_t)(r / 100u));
                n = str_len(f); f[n++] = '.';
                f[n++] = (char)('0' + r / 10u % 10u);
                f[n++] = (char)('0' + r % 10u); f[n] = 0;
            } else {
                fmt_int(f, (int32_t)o->freq_coarse); str_cpy(f + str_len(f), "F", 2);
            }
            cv_text_in(76 + (int32_t)op * 38, 96, 36, &AF_S, f, p.op[op].enable ? T_MID : T_DIM, T_SURF);
        }
    }
}

static void rdx_ui_graph_algorithm(void)
{
    rdx_patch_t p;
    uint32_t alg, src, dst, pass;
    int32_t x[4] = {36, 92, 148, 204}, y[4] = {72,72,72,72};
    uint8_t depth[4] = {0,0,0,0};
    char n[2] = {'1',0}, a[16] = "ALGORITHM ";
    rdx_patch_for_track(song.sel, &p); alg = p.common.algorithm % 12u;
    for (pass = 0; pass < 4u; pass++)
        for (src = 0; src < 4u; src++)
            for (dst = 0; dst < 4u; dst++)
                if ((RDX_ROUTES[alg][src] >> dst) & 1u)
                    if (depth[src] <= depth[dst]) depth[src] = (uint8_t)(depth[dst] + 1u);
    for (src = 0; src < 4u; src++) y[src] -= depth[src] * 22;
    for (src = 0; src < 4u; src++)
        for (dst = 0; dst < 4u; dst++)
            if ((RDX_ROUTES[alg][src] >> dst) & 1u)
                cv_line_t(x[src], y[src] + 12, x[dst], y[dst],
                          p.op[src].enable && p.op[dst].enable ? T_THEME : T_DIM, 2);
    for (src = 0; src < 4u; src++) {
        uint16_t c = p.op[src].enable ? ((RDX_CARRIERS[alg] >> src) & 1u ? T_ACCENT : T_THEME) : T_DIM;
        if ((RDX_CARRIERS[alg] >> src) & 1u) cv_line_t(x[src], y[src] + 18, x[src], 91, c, 2);
        cv_rrect(x[src] - 11, y[src], 22, 18, 4, c, T_SURF);
        n[0] = (char)('1' + src);
        cv_text_in(x[src] - 11, y[src] + 3, 22, &AF_S, n, p.op[src].enable ? T_INK : T_SURF, c);
    }
    fmt_int(a + 10, (int32_t)alg + 1);
    cv_text_on(12, 96, &AF_S, a, T_MID, T_SURF);
}

static void rdx_ui_graph_env(uint32_t pitch)
{
    uint16_t col[4] = {T_THEME,T_ACCENT,T_TEXT,T_MID};
    rdx_patch_t p;
    uint32_t op, st;
    rdx_patch_for_track(song.sel, &p);
    cv_line(12, 88, 216, 88, T_RAISE);
    for (op = 0; op < (pitch ? 1u : 4u); op++) {
        int32_t px = 12, py = pitch ? 50 : 88;
        uint16_t c = pitch ? T_THEME : col[op];
        for (st = 0; st < 4u; st++) {
            uint32_t rate = pitch ? p.common.peg_rate[st] : p.op[op].eg_rate[st];
            int32_t level = pitch ? (int32_t)p.common.peg_level[st] - 16 : p.op[op].eg_level[st];
            int32_t nx = px + 18 + (127 - (int32_t)rate) * 27 / 127;
            int32_t ny = pitch ? 84 - level * 68 / 96 : 88 - level * 70 / 127;
            if (nx > 222) nx = 222;
            cv_line_t(px, py, nx, ny, c, op == (rdx_eg_row >> 1) || pitch ? 2 : 1);
            px = nx; py = ny;
        }
    }
    cv_text_on(12, 96, &AF_S, pitch ? "PITCH ENVELOPE" : rdx_ui_row_name(GR_RDX_EG, rdx_eg_row), T_MID, T_SURF);
}

static void rdx_ui_graph_lfo(void)
{
    rdx_patch_t p;
    uint32_t i, w;
    int32_t px = 12, py = 50;
    rdx_patch_for_track(song.sel, &p); w = p.common.lfo_wave % 7u;
    cv_line(12, 50, 216, 50, T_RAISE);
    for (i = 1; i <= 102u; i++) {
        uint32_t ph = (i * 2u) & 63u;
        int32_t v;
        if (!w) v = osc_sine(ph << 26) * 30 / 32768;
        else if (w == 1u) v = ph < 32u ? -30 + (int32_t)ph * 60 / 32 : 30 - (int32_t)(ph - 32u) * 60 / 32;
        else if (w == 2u) v = -30 + (int32_t)ph * 60 / 63;
        else if (w == 3u) v = 30 - (int32_t)ph * 60 / 63;
        else if (w == 4u) v = ph < 32u ? 30 : -30;
        else v = ((int32_t)((i / (w == 5u ? 7u : 51u)) * 37u % 61u) - 30);
        cv_line_t(px, py, 12 + (int32_t)i * 2, 50 - v, T_THEME, 2);
        px = 12 + (int32_t)i * 2; py = 50 - v;
    }
    cv_text_on(12, 96, &AF_S, "COMMON LFO", T_MID, T_SURF);
}

static void rdx_ui_graph(uint32_t g)
{
    if (g == GR_RDX_ALG) rdx_ui_graph_algorithm();
    else if (g == GR_RDX_EG) rdx_ui_graph_env(0u);
    else if (g == GR_RDX_PEGR || g == GR_RDX_PEGL) rdx_ui_graph_env(1u);
    else if (g == GR_RDX_LFO) rdx_ui_graph_lfo();
    else rdx_ui_graph_matrix(g);
}
#endif
