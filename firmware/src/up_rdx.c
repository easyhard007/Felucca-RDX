/* SPDX-License-Identifier: GPL-3.0-or-later
 * Complete 150-byte RDX patches carried by the 32 generic user-preset
 * records.  Two 16-slot banks fit comfortably in storage.c's 3840-byte
 * payload.  Each patch's 7-bit bytes are bit-packed and tagged with the
 * matching up_rec_t (excluding its editable name), so a torn two-object save
 * can never attach an old voice to a different preset record. */
#define UPR_MAGIC 0x31525055u                 /* "UPR1" */
#define UPR_PK ((RDX_PATCH_SIZE * 7u + 7u) / 8u)
typedef struct {
    uint32_t tag;
    uint8_t pk[UPR_PK];
} upr_ent_t;
typedef struct {
    uint32_t magic;
    uint16_t ver, nslot;
    uint16_t used, rsv;
    upr_ent_t e[UP_PER_BANK];
} upr_bank_t;
_Static_assert(sizeof(upr_bank_t) <= ST_PAYLOAD_MAX, "RDX user patch bank fits one storage object");
static upr_bank_t upr_bank[UP_SLOTS / UP_PER_BANK] __attribute__((section(".pool")));

static int upr_valid(const upr_bank_t *u)
{
    return u->magic == UPR_MAGIC && u->ver == 1u && u->nslot == UP_PER_BANK;
}
static void upr_empty(upr_bank_t *u)
{
    memset(u, 0, sizeof *u);
    u->magic = UPR_MAGIC; u->ver = 1u; u->nslot = UP_PER_BANK;
}
static uint32_t upr_obj(uint32_t b) { return b ? OBJ_UPRDX1 : OBJ_UPFM6; }

static void upr_pack(const uint8_t *src, uint8_t *dst)
{
    uint32_t i, acc = 0, bits = 0, out = 0;
    memset(dst, 0, UPR_PK);
    for (i = 0; i < RDX_PATCH_SIZE; i++) {
        acc |= (uint32_t)(src[i] & 127u) << bits;
        bits += 7u;
        while (bits >= 8u) { dst[out++] = (uint8_t)acc; acc >>= 8; bits -= 8u; }
    }
    if (bits) dst[out] = (uint8_t)acc;
}
static void upr_unpack(const uint8_t *src, uint8_t *dst)
{
    uint32_t i, acc = 0, bits = 0, in = 0;
    for (i = 0; i < RDX_PATCH_SIZE; i++) {
        while (bits < 7u) { acc |= (uint32_t)src[in++] << bits; bits += 8u; }
        dst[i] = (uint8_t)(acc & 127u); acc >>= 7; bits -= 7u;
    }
}
static int upr_get(uint32_t k, rdx_patch_t *p)
{
    uint32_t b = k / UP_PER_BANK, i = k % UP_PER_BANK;
    upr_bank_t *u;
    if (k >= UP_SLOTS) return 1;
    u = &upr_bank[b];
    if (!upr_valid(u) || !(u->used & (1u << i)) || !up_used(k) ||
        up_rec(k)->engine != ENGI_RDX || u->e[i].tag != upf_tag(up_rec(k))) return 1;
    upr_unpack(u->e[i].pk, (uint8_t *)p);
    return 0;
}
static int upr_save(uint32_t b)
{
#if FELUCCA_FLASH
    if (!flash_ok) return 3;
    return st_save(upr_obj(b), &upr_bank[b], sizeof upr_bank[b]) ? 2 : 0;
#else
    (void)b; return 3;
#endif
}
static void upr_set(uint32_t k, const rdx_patch_t *p)
{
    uint32_t b = k / UP_PER_BANK, i = k % UP_PER_BANK;
    upr_bank_t *u = &upr_bank[b];
    if (!upr_valid(u)) upr_empty(u);
    upr_pack((const uint8_t *)p, u->e[i].pk);
    u->e[i].tag = upf_tag(up_rec(k));
    u->used |= (uint16_t)(1u << i);
}
static void upr_boot(void)
{
    uint32_t b;
    for (b = 0; b < UP_SLOTS / UP_PER_BANK; b++) {
#if FELUCCA_FLASH
        int n = flash_ok ? st_load(upr_obj(b), &upr_bank[b], sizeof upr_bank[b]) : -1;
        if (n != (int)sizeof upr_bank[b] || !upr_valid(&upr_bank[b])) upr_empty(&upr_bank[b]);
#else
        upr_empty(&upr_bank[b]);
#endif
    }
}
static int upr_store(uint32_t k, uint32_t tr)
{
    rdx_patch_t p;
    if (k >= UP_SLOTS) return 1;
    rdx_patch_for_track(tr, &p);
    upr_set(k, &p);
    return upr_save(k / UP_PER_BANK);
}
static void upr_track_load(track_t *t, uint32_t k)
{
    rdx_patch_t p;
    uint32_t tr = (uint32_t)(t - trk);
    if (tr >= NTRK || t->eng_req != ENGI_RDX || upr_get(k, &p)) {
        rdx_track_restored(t);
        return;
    }
    rdx_sysex_patch_set(tr, &p);
}
