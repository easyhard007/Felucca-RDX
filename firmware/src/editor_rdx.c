/* SPDX-License-Identifier: GPL-3.0-or-later
 * Raw RDX patch transport for the Felucca-RDX librarian.
 * 82 GET: target(0 track, 1 user slot), index -> target,index,rc,150 bytes
 * 83 PUT: target(0 track), index,150 bytes -> target,index,rc */
enum { ED_RDX_GET = 82, ED_RDX_PUT };
static int ed_rdx_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    rdx_patch_t p;
    uint32_t i, rc = 0;
    if (cmd != ED_RDX_GET && cmd != ED_RDX_PUT) return 0;
    if (n < 2u || a[0] > 1u || (a[0] ? a[1] >= UP_SLOTS : a[1] >= NTRK) ||
        (cmd == ED_RDX_GET ? n != 2u : n != 2u + RDX_PATCH_SIZE) ||
        (cmd == ED_RDX_PUT && a[0] != 0u)) rc = 1;
    if (!rc && cmd == ED_RDX_PUT) {
        for (i = 0; i < RDX_PATCH_SIZE; i++)
            ((uint8_t *)&p)[i] = a[2u + i] & 127u;
        rdx_sx_applied(a[1], &p);
    }
    if (!rc && cmd == ED_RDX_GET) {
        if (a[0] ? upr_get(a[1], &p) : (rdx_patch_for_track(a[1], &p), 0)) rc = 2;
    }
    ed_b(n ? a[0] : 127u); ed_b(n > 1u ? a[1] : 127u); ed_b(rc);
    if (!rc && cmd == ED_RDX_GET) {
        for (i = 0; i < RDX_PATCH_SIZE; i++) ed_b(((uint8_t *)&p)[i]);
    }
    return 1;
}
