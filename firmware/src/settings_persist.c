/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared PER4 layout: each feature updates only its own fields, preserving
 * the other feature's saved preferences when either is built independently.
 * PER1/PER2 are upstream; PER3 added bold; PER4 added favorites.
 * palette: UI_PAL_TAG + index; an old id (below 20, earlier firmware) is migrated on import.
 * bold: no longer used (one font weight); kept as it was saved, unless it holds the HOLD setting (panel.c).
 * zoom: no longer used (the large readout); kept as it was saved, unless it holds the LEDS setting (panel.c).
 * favorites.factory[15][28..31]: CLICK / CLICK LEVEL / COUNT-IN (1.1), STYLE, MENU's flags, the layers seen (bytes no
 * engine uses; 0 in older settings = every one's default); [15][27]: SCREEN OFF (1.1.5; 0 = 30 MIN, the default);
 * [15][26]: MENU > DISPLAY > HOME (1.3; 0 = SCOPE, 1 TRACKS, 2 LEVELS (1.5), ui.c ui_home_view);
 * [15][13]: persistent-data owner/version. 0xF6 is the legacy FM6-only/RDX marker; RDX 1.0.2 and later use
 *   RDX_SETTINGS_TAG so one Hub package can retain an earlier RDX installation while rejecting projects, autosaves
 *   and user voices left by another Felucca-derived firmware.
 * [15][24]: MENU's 1.2 flags SCOPE MIX, STEP PREVIEW ON, CHORD ENTRY ADD (bits 0-2, ui.c ui_prefs2; 0 = the defaults),
 *   1.5 HELP ON (bit 3), 1.5.5 SELECT PAGE (bit 4; 0 = BPM, as before), 1.5.5 SCREEN WAKE PANEL (bit 5; 0 = +MIDI);
 *   1.6.1 2ND CORE ON (bit 7; 0 = OFF, one core, as before);
 * [15][10]: MENU > ANIM IDLE (1.2, Discussion #135, ui.c ui_idle: bit 0; 0 = off, ANIM as its bit in MENU's flags says),
 *   and in an RDX build MIDI velocity curve (bits 3-5; 0 = HARD 1). Bits 1-2 and 6-7 held the retired 1.0.4
 *   renderer/effect A/B choices and remain reserved for storage compatibility;
 * [15][11]: PRESETS > LIST's category (1.2, Discussion #90, ui.c ui_lcat: 0 none = ALL / FAV as favorites.filter says);
 * [15][0..9], [12] (1.6, #244: CUSTOM, a palette of the user's: five RGB565 colours little endian, flags: ui.c ui_cpal); older
 * firmware keeps the bytes as saved and shows the built-in palette in persist_t.palette.
 * [15][14..23]: the FX layer's key map (1.2, ui_layer.c fx_keys: 16 white keys x 5 bits, perform.c perf_map_of; 0 = the
 * key's default, an unknown code too: kept as saved, a later firmware's effect survives a round trip here).
 * MENU's flags (ui.c PREF_*) are append-only bits whose 0 is the default: BPM LOCK (16) is clear in every older setting
 * = unlocked, LARGE (32) = OFF, RESTORE LAST OFF (128, 1.2) = ON, read as saved (nothing to migrate, so importing twice changes nothing). */
typedef struct {
    uint32_t magic, palette, lowcut, zoom;
    panel_t panel;
    uint32_t bold;
    struct { uint8_t factory[16][32]; uint32_t user, filter; } favorites;
} persist_t;
#define PERSIST_MAGIC 0x50455234u

#if FELUCCA_FM6_ONLY
/* The regular firmware keeps the PRESETS list mode and the last session in flash.  The first
 * FM6-only build used those records unchanged, so an FM-1 that had FAV selected could appear
 * to have only a few FM6 sounds, and an autosaved non-FM6 track could drive ENG_FM6 without
 * ever loading an FM6 patch.  Byte 13 is otherwise unused.  On the first boot of this fixed
 * build, select ALL and skip the incompatible autosave once; later FM6-only sessions restore
 * normally. */
#define FM6_ONLY_SETTINGS_TAG 0xF6u
#if FELUCCA_RDX
#define RDX_SETTINGS_TAG 0xD1u                 /* high nibble: RDX owner, low nibble: storage schema 1 */
#define ACTIVE_SETTINGS_TAG RDX_SETTINGS_TAG
#else
#define ACTIVE_SETTINGS_TAG FM6_ONLY_SETTINGS_TAG
#endif
static uint8_t fm6_only_migrate;
static uint8_t fm6_only_settings_seen;
#if FELUCCA_RDX
static uint8_t rdx_store_foreign;              /* no RDX marker: do not expose another firmware's objects */
#endif

static void fm6_only_settings_boot(void)
{
    /* A device with no settings record is already clean.  Give its first future settings
     * record the tag without treating the first ordinary save as an upgrade. */
    if (!fm6_only_settings_seen) {
        favorites.filter = 0;
        favorites.factory[15][11] = 0;
        if (!FELUCCA_RDX || !rdx_store_foreign)
            favorites.factory[15][13] = ACTIVE_SETTINGS_TAG;
    }
}
#endif

/* Normalize in place; 1 = current, 2 = migrated, 0 = invalid. */
static int settings_import(persist_t *p, int n)
{
    int current = n == (int)sizeof *p && p->magic == PERSIST_MAGIC;
    int old3 = n == (int)(sizeof *p - sizeof p->favorites) && p->magic == 0x50455233u;
    int old2 = n == (int)(sizeof *p - sizeof p->favorites - sizeof p->bold) && p->magic == 0x50455232u;
    int old1 = n == (int)(8u + sizeof(panel_t)) && p->magic == 0x50455231u;
    if (!(current || old3 || old2 || old1)) return 0;
#if FELUCCA_FM6_ONLY
    fm6_only_settings_seen = 1;
#endif
    if (old1) {
        panel_t old;
        memcpy(&old, (uint8_t *)p + 8, sizeof old);
        p->panel = old;
        p->lowcut = p->zoom = 0;
    }
    if (old1 || old2) p->bold = 0;
    if (!current) memset(&p->favorites, 0, sizeof p->favorites);
#if FELUCCA_FM6_ONLY
    /* 0.6.35..0.6.38 briefly used the high two bits of the old 0xF6 marker for
     * the retired EG A/B choice.  Those records are still an RDX installation.
     * Any other value in an RDX build belongs to upstream/another fork and its
     * engine-specific persistent objects must not be interpreted as RDX data. */
#if FELUCCA_RDX
    if (p->favorites.factory[15][13] != RDX_SETTINGS_TAG) {
        fm6_only_migrate = 1;
        if ((p->favorites.factory[15][13] & 0x3Fu) != (FM6_ONLY_SETTINGS_TAG & 0x3Fu))
            rdx_store_foreign = 1;
    }
#else
    if ((p->favorites.factory[15][13] & 0x3Fu) != (FM6_ONLY_SETTINGS_TAG & 0x3Fu))
        fm6_only_migrate = 1;
#endif
#endif
    if (p->favorites.factory[15][29] > 1u)        /* STYLE (ui.c ui_style): 2, the retired PIXEL = LINE; unknown = FLAT */
        p->favorites.factory[15][29] = p->favorites.factory[15][29] == 2u ? 1u : 0u;
    {   /* CLICK, CLICK LEVEL, COUNT-IN (ui.c ui_rec_prefs): two bits each, 3 is no value: the default (0) */
        uint32_t b = p->favorites.factory[15][28], f;
        for (f = 0; f < 3u; f++)
            if (((b >> (2u * f)) & 3u) == 3u)
                b &= ~(3u << (2u * f));
        p->favorites.factory[15][28] = (uint8_t)b;
    }
    if (p->favorites.factory[15][27] > 7u)         /* SCREEN OFF (1.1.5, ui.c SCR_CODE): unknown = 0 (NEVER since 1.1.5.1) */
        p->favorites.factory[15][27] = 0;
    if (p->favorites.factory[15][11] >= CAT_N)     /* PRESETS TYPE category (ui.c ui_lcat, CAT_*): unknown = none */
        p->favorites.factory[15][11] = 0;
    /* [15][26] HOME (1.3, ui.c ui_home_view): kept as saved; a value this firmware does not know draws SCOPE */
    {   /* CUSTOM (1.6, #244, favorites.c ui_cpal): [15][0..9] five RGB565 colours, [12] bit 0 stored, bit 1 in use. Anything else
         * in [12], or colours without the stored flag: none (zero). The colours themselves are whatever the user chose */
        uint8_t *f = p->favorites.factory[15];
        f[12] &= 3u;
        if (!(f[12] & 1u)) {
            memset(f, 0, 10);
            f[12] = 0;
        }
    }
    p->magic = PERSIST_MAGIC;
    p->palette = palette_to_stored(palette_from_stored(p->palette));
    settings.magic = SETTINGS_MAGIC;
    settings.palette = palette_from_stored(p->palette);
    settings.lowcut = p->lowcut;
    settings.zoom = p->zoom;
    settings_hold = (uint8_t)hold_from_stored(p->bold);
    settings_leds = (uint8_t)leds_from_stored(p->zoom);
#ifdef FELUCCA_FAVORITES
    memcpy(&favorites, &p->favorites, sizeof favorites);
#if FELUCCA_RDX
    velocity_curve_load();
#endif
    favorites.filter = favorites.filter == 1u;
#if FELUCCA_FM6_ONLY
    if (fm6_only_migrate) {
        /* Change the live copy, not p: persist_saved remains the old record, so settings_save
         * below main.c's boot path detects this migration and really commits the marker. */
        favorites.filter = 0;                 /* PRESETS > LIST: ALL */
        favorites.factory[15][11] = 0;        /* no category filter */
#if !FELUCCA_RDX
        favorites.factory[15][13] = FM6_ONLY_SETTINGS_TAG;
#else
        /* A legacy RDX record is compatible and can be promoted immediately.
         * A foreign record gets its tag only after every incompatible object
         * has been atomically invalidated by rdx_store_migrate_boot(). */
        if (!rdx_store_foreign)
            favorites.factory[15][13] = RDX_SETTINGS_TAG;
#endif
    }
#endif
#if defined(FM4_NPRESETS) && !FELUCCA_FM4
    {   /* DIGITAL's starred presets (engine 1, retired) -> the FM6 presets that cover them (fm4_convert.c) */
        uint32_t k;
        for (k = 0; k < FM4_NPRESETS; k++)
            if ((favorites.factory[ENGI_DIGITAL][0] >> k) & 1u)
                favorites.factory[ENGI_FM6][FM4_TO_FM6[k] / 8u] |= (uint8_t)(1u << (FM4_TO_FM6[k] % 8u));
        memset(favorites.factory[ENGI_DIGITAL], 0, sizeof favorites.factory[ENGI_DIGITAL]);
    }
#endif
#ifdef SMP_SET_PERC
    if ((favorites.factory[ENGI_SAMPLE][0] >> SMP_SET_PERC) & 1u) {   /* SAMPLE's PERC (preset 4, retired): DRUM's */
        favorites.factory[ENGI_DRUM][0] |= 1u;                         /* kit (core.h drum_from_perc) */
        favorites.factory[ENGI_SAMPLE][0] &= (uint8_t)~(1u << SMP_SET_PERC);
    }
#endif
#endif
    if (p->panel.magic == PANEL_MAGIC) panel = p->panel;
    return current ? 1 : 2;
}

#if FELUCCA_RDX
/* One package must work both as a first Hub install and as an RDX update.
 * st_save(..., 0) commits an empty newest copy instead of erasing both A/B
 * copies: a power cut leaves either the old marker (so migration retries) or
 * a complete empty object. The settings marker is written only after all
 * objects succeeded. Panel calibration and the safe top-level settings live
 * outside favorites and are deliberately retained. */
static void rdx_store_migrate_boot(void)
{
    static const uint8_t incompatible[] = {
        OBJ_PROJECT0, OBJ_PROJECT0 + 1u, OBJ_PROJECT0 + 2u, OBJ_PROJECT0 + 3u,
        OBJ_UPRESET0, OBJ_UPRESET0 + 1u, OBJ_UPFM6, OBJ_UPRDX1, OBJ_AUTOSAVE
    };
    uint32_t i;
    int failed = 0;

    if (!fm6_only_settings_seen) {
        fm6_only_migrate = 1;
        rdx_store_foreign = 1;
    }
    if (!rdx_store_foreign)
        return;

    /* Do not let foreign page/filter/effect/favorite bytes affect this boot,
     * even if a flash write fails. The imported panel calibration remains. */
    memset(&favorites, 0, sizeof favorites);
    memset(proj_slot, 0, sizeof proj_slot);
    for (i = 0; i < sizeof incompatible / sizeof incompatible[0]; i++) {
        st_hdr_t h;
        /* A clean package and a factory-fresh device have no committed
         * objects, so they pay no needless sector erase or boot delay. */
        if (st_current(incompatible[i], &h) >= 0 && h.len && st_save(incompatible[i], 0, 0))
            failed = 1;
    }
    if (!failed) {
        favorites.factory[15][13] = RDX_SETTINGS_TAG;
        rdx_store_foreign = 0;
    }
}
#endif

/* Start with the last imported/saved object, including fields owned by a
 * feature absent from this build. No save-on-boot or extra flash writes. */
static void settings_export(persist_t *p)
{
    p->magic = PERSIST_MAGIC;
    p->palette = palette_to_stored(settings.palette);
    p->lowcut = settings.lowcut;
    p->zoom = settings.zoom = leds_to_stored(settings.zoom, settings_leds);
    p->panel = panel;
    p->bold = hold_to_stored(p->bold, settings_hold);
#ifdef FELUCCA_FAVORITES
    memcpy(&p->favorites, &favorites, sizeof favorites);
#endif
}
