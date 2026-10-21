/* SPDX-License-Identifier: GPL-3.0-or-later
 * Yamaha reface DX bulk and parameter SysEx for the fixed-point RDX engine.
 *
 * Wire format follows the official reface Data List. ota_frame_get() exposes
 * the bytes between F0 and F7, so this handler runs before Felucca's editor and
 * update handlers and leaves every non-Yamaha/non-DX frame untouched. */
#define RDX_SX_YAMAHA 0x43u
#define RDX_SX_GROUP  0x7Fu
#define RDX_SX_FORMAT 0x1Cu
#define RDX_SX_MODEL  0x05u

typedef struct {
    rdx_patch_t patch;
    uint8_t active, track, blocks;
} rdx_sx_rx_t;

static rdx_sx_rx_t rdx_sx_rx;
static uint8_t rdx_sx_wire[64];

static uint32_t rdx_sx_sum(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0;
    while (n--)
        s += *p++;
    return s;
}

static int rdx_sx_send_bulk(uint32_t device, uint32_t ah, uint32_t am, uint32_t al,
                            const uint8_t *data, uint32_t n)
{
    uint32_t i, count = n + 4u, w = 0, sum;
    if (n > 38u)
        return -1;
    rdx_sx_wire[w++] = 0xF0;
    rdx_sx_wire[w++] = RDX_SX_YAMAHA;
    rdx_sx_wire[w++] = (uint8_t)(device & 15u);
    rdx_sx_wire[w++] = RDX_SX_GROUP;
    rdx_sx_wire[w++] = RDX_SX_FORMAT;
    rdx_sx_wire[w++] = (uint8_t)(count >> 7);
    rdx_sx_wire[w++] = (uint8_t)(count & 127u);
    rdx_sx_wire[w++] = RDX_SX_MODEL;
    rdx_sx_wire[w++] = (uint8_t)ah;
    rdx_sx_wire[w++] = (uint8_t)am;
    rdx_sx_wire[w++] = (uint8_t)al;
    for (i = 0; i < n; i++)
        rdx_sx_wire[w++] = data[i] & 127u;
    sum = rdx_sx_sum(rdx_sx_wire + 7, n + 4u);
    rdx_sx_wire[w++] = (uint8_t)((0u - sum) & 127u);
    rdx_sx_wire[w++] = 0xF7;
    return ota_wire_send(rdx_sx_wire, w);
}

static int rdx_sx_send_parameter(uint32_t device, uint32_t ah, uint32_t am, uint32_t al,
                                 const uint8_t *data, uint32_t n)
{
    uint32_t i, w = 0;
    if (n > 38u)
        return -1;
    rdx_sx_wire[w++] = 0xF0;
    rdx_sx_wire[w++] = RDX_SX_YAMAHA;
    rdx_sx_wire[w++] = (uint8_t)(0x10u | (device & 15u));
    rdx_sx_wire[w++] = RDX_SX_GROUP;
    rdx_sx_wire[w++] = RDX_SX_FORMAT;
    rdx_sx_wire[w++] = RDX_SX_MODEL;
    rdx_sx_wire[w++] = (uint8_t)ah;
    rdx_sx_wire[w++] = (uint8_t)am;
    rdx_sx_wire[w++] = (uint8_t)al;
    for (i = 0; i < n; i++)
        rdx_sx_wire[w++] = data[i] & 127u;
    rdx_sx_wire[w++] = 0xF7;
    return ota_wire_send(rdx_sx_wire, w);
}

static void rdx_sx_applied(uint32_t tr, const rdx_patch_t *p)
{
    track_t *t = &trk[tr % NTRK];
    rdx_sysex_patch_set(tr, p);
    t->user = 0;
    panic_req |= (uint8_t)(1u << (tr % NTRK));
    if (t == TSEL)
        sync_reload = 1;
    ui_message("RDX SYSEX LOADED");
}

static int rdx_sx_block(rdx_patch_t *p, uint32_t ah, uint32_t am, uint32_t al,
                        const uint8_t *data, uint32_t n)
{
    if (ah == 0x30u && am == 0u && al < sizeof(rdx_common_t)) {
        uint32_t take = sizeof(rdx_common_t) - al;
        if (take > n) take = n;
        memcpy((uint8_t *)&p->common + al, data, take);
        return 1;
    }
    if (ah == 0x31u && am < 4u && al < sizeof(rdx_op_param_t)) {
        uint32_t take = sizeof(rdx_op_param_t) - al;
        if (take > n) take = n;
        memcpy((uint8_t *)&p->op[am] + al, data, take);
        return 1;
    }
    return 0;
}

static void rdx_sx_dump(uint32_t tr, uint32_t device, uint32_t ah, uint32_t am, uint32_t al)
{
    rdx_patch_t p;
    uint32_t k;
    rdx_patch_for_track(tr, &p);
    if (ah == 0x0Eu && am == 0x0Fu && al == 0u) {
        rdx_sx_send_bulk(device, 0x0E, 0x0F, 0, 0, 0);
        rdx_sx_send_bulk(device, 0x30, 0, 0, (const uint8_t *)&p.common, sizeof p.common);
        for (k = 0; k < 4u; k++)
            rdx_sx_send_bulk(device, 0x31, k, 0, (const uint8_t *)&p.op[k], sizeof p.op[k]);
        rdx_sx_send_bulk(device, 0x0F, 0x0F, 0, 0, 0);
    } else if (ah == 0x30u && am == 0u)
        rdx_sx_send_bulk(device, ah, am, 0, (const uint8_t *)&p.common, sizeof p.common);
    else if (ah == 0x31u && am < 4u)
        rdx_sx_send_bulk(device, ah, am, 0, (const uint8_t *)&p.op[am], sizeof p.op[am]);
    else if ((ah == 0x0Eu || ah == 0x0Fu) && am == 0x0Fu)
        rdx_sx_send_bulk(device, ah, am, 0, 0, 0);
}

static void rdx_sx_parameter_reply(uint32_t tr, uint32_t device, uint32_t ah, uint32_t am, uint32_t al)
{
    rdx_patch_t p;
    const uint8_t *v = 0;
    rdx_patch_for_track(tr, &p);
    if (ah == 0x30u && am == 0u && al < sizeof p.common)
        v = (const uint8_t *)&p.common + al;
    else if (ah == 0x31u && am < 4u && al < sizeof p.op[0])
        v = (const uint8_t *)&p.op[am] + al;
    if (v)
        rdx_sx_send_parameter(device, ah, am, al, v, 1);
}

static void rdx_sysex_service(void)
{
    const uint8_t *p;
    uint32_t n, kind, device, tr;
    if (!ota_frame_get(&p, &n) || n < 5u || p[0] != RDX_SX_YAMAHA ||
        p[2] != RDX_SX_GROUP || p[3] != RDX_SX_FORMAT)
        return;
    kind = p[1] & 0xF0u;
    device = p[1] & 15u;
    tr = song.sel % NTRK;

    if (kind == 0u && n >= 11u) {                       /* bulk dump */
        uint32_t count = (uint32_t)p[4] << 7 | p[5];
        uint32_t data_n, ah, am, al, valid;
        if (count < 4u || n != count + 7u || p[6] != RDX_SX_MODEL) {
            ota_frame_done();
            return;
        }
        valid = ((rdx_sx_sum(p + 6, count) + p[6 + count]) & 127u) == 0u;
        ah = p[7]; am = p[8]; al = p[9]; data_n = count - 4u;
        if (!valid) {
            rdx_sx_rx.active = 0;                       /* never commit a damaged multi-block transfer */
        } else if (ah == 0x0Eu && am == 0x0Fu && al == 0u && !data_n) {
            rdx_sx_rx.active = 1;
            rdx_sx_rx.track = (uint8_t)tr;
            rdx_sx_rx.blocks = 0;
            rdx_patch_for_track(tr, &rdx_sx_rx.patch);
        } else if (valid && ah == 0x0Fu && am == 0x0Fu && al == 0u && !data_n) {
            if (rdx_sx_rx.active) {
                rdx_sx_applied(rdx_sx_rx.track, &rdx_sx_rx.patch);
                rdx_sx_rx.active = 0;
            }
        } else if (valid) {
            rdx_patch_t one;
            rdx_patch_t *dst;
            if (rdx_sx_rx.active)
                dst = &rdx_sx_rx.patch;
            else {
                rdx_patch_for_track(tr, &one);
                dst = &one;
            }
            if (rdx_sx_block(dst, ah, am, al, p + 10, data_n)) {
                rdx_sx_rx.blocks++;
                if (!rdx_sx_rx.active)
                    rdx_sx_applied(tr, dst);             /* official partial-block update */
            }
        }
        ota_frame_done();
        return;
    }
    if (kind == 0x10u && n >= 9u && p[4] == RDX_SX_MODEL) { /* parameter change */
        rdx_patch_t patch;
        rdx_patch_for_track(tr, &patch);
        if (rdx_sx_block(&patch, p[5], p[6], p[7], p + 8, n - 8u))
            rdx_sx_applied(tr, &patch);
        ota_frame_done();
        return;
    }
    if (kind == 0x20u && n == 8u && p[4] == RDX_SX_MODEL) { /* bulk request */
        uint8_t ah = p[5], am = p[6], al = p[7];
        ota_frame_done();
        rdx_sx_dump(tr, device, ah, am, al);
        return;
    }
    if (kind == 0x30u && n == 8u && p[4] == RDX_SX_MODEL) { /* parameter request */
        uint8_t ah = p[5], am = p[6], al = p[7];
        ota_frame_done();
        rdx_sx_parameter_reply(tr, device, ah, am, al);
        return;
    }
    ota_frame_done();                                  /* malformed RDX frame: ours, but ignored */
}
