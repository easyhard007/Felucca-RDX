/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Sound categories (1.2, Discussion #90): every sound in the PRESETS list belongs to one, so KNOB 4 (LIST) can show
 * only the basses, the pads, .. (ui.c list_mode). Factory presets: a static table here (a letter per preset, in the
 * engine's preset order); user presets: a byte of their record (upreset.c up_cat, set on the NAME screen when saving
 * or renaming, ui_name.c). Included by ui.c and upreset.c (the host build of the user presets needs the numbers only).
 * The numbers are stored (a user preset's record, the editor's UP_LIST / UP_GET / UP_PUT): append-only. CAT_NONE is a
 * record of before (or the editor's without one): its category follows from its engine (cat_of_engine) */
#ifndef FELUCCA_CATEGORY
#define FELUCCA_CATEGORY 1
enum { CAT_NONE, CAT_BASS, CAT_LEAD, CAT_PAD, CAT_PLUCK, CAT_KEYS, CAT_DRUM, CAT_FX, CAT_OTHER,
       CAT_ORGAN, CAT_BRASS, CAT_PERC, CAT_N };
static const char *const CAT_NAME[CAT_N] = {
    "-", "BASS", "LEAD", "PAD", "PLUCK", "KEYS", "DRUM", "FX", "OTHER",
    "ORGAN", "BRASS", "PERC"
};
/* the category a sound without one is listed under: a kit (DRUM, SLICE: engines 10, 13) DRUM, any other OTHER */
static uint32_t cat_of_engine(uint32_t e) { return e == 10u || e == 13u ? CAT_DRUM : CAT_OTHER; }

#ifndef UP_HOST
/* the factory presets' categories: per engine number (engines.c ENGINES[]), a letter per preset in its order (B BASS,
 * L LEAD, P PAD, U PLUCK, K KEYS, D DRUM, F FX, O OTHER); a preset past its string (DIGITAL's, FELUCCA_FM4 builds
 * only; SAMPLE's retired PERC, past SMP_NPRESETS): its engine's (cat_of_engine). tests/ui_test.c checks every string
 * against its engine's preset count. A preset's category is a hint for browsing: nothing else reads it */
static const char *const PRESET_CAT[16] = {
    "LPBPBKLBULLP",              /* 0 ANALOG: SAW LEAD, SOFT PAD, SQR BASS, PWM STR, ACID, SINE KEY, RAVE, SUB BASS,
                                  * PLUCK, BRASS, WIND, STRINGS */
    "",                          /* 1 DIGITAL (retired) */
    "LKPLUU",                    /* 2 PHASE: BRASS, ORGAN, STRING, RESO, BELL, WIRE */
    "LBLLL",                     /* 3 LOFI: PULSE LD, WAVE BASS, ARP 8BIT, WAVE LEAD, STEP LEAD */
    "KKLL",                      /* 4 SAMPLE: PIANO, (PIANO: the retired TRANH's alias), FLUTE, SAX */
    "PLBF",                      /* 5 VOICE: CHOIR AAH, VOX LEAD, WOW BASS, WHISPER */
    "BLLUP",                     /* 6 TRIO: FAT BASS, ARP LEAD, SYNC LEAD, RING BELL, CHIP CHOIR */
    "KKKKK",                     /* 7 WHEEL: the organs */
    "PFPP",                      /* 8 GRAIN: CLOUD PAD, GLITCH, FROZEN, SHIMMER */
    "UUUPUDDPU",                 /* 9 PHYS: BELL TREE, MARIMBA, PLUCK, BOWED METAL, KALIMBA, HAND DRUM, TOMS, DRONE
                                  * STRING, HARP */
    "DDDD",                      /* 10 DRUM: DRUM KIT; 1.5.6: DEEP KIT, TIGHT KIT, PERC KIT */
    "FFFF",                      /* 11 NOISE: WIND, RAIN, ARCADE, METAL */
    "KUBLPUKU",                  /* 12 FM6: TINE EP, BELL, FM BASS, BRASS, PAD, MARIMBA, ORGAN, PLUCK */
    "DD",                        /* 13 SLICE: CHOP, STUTTER */
};
#if FELUCCA_RDX
/* Yamaha publishes the 32 factory names/order and mono flags, but no browsing
 * categories.  These are conservative musical-use tags for Felucca-RDX.  The
 * three retired development voices (0..2) and the user-reviewed Selected
 * collection are tagged too. */
static const uint8_t RDX_PRESET_CAT[RDX_NPATCHES] = {
    [0] = CAT_KEYS, [1] = CAT_KEYS, [2] = CAT_LEAD,
    [3] = CAT_KEYS,       /* DigiChord */
    [4] = CAT_BASS,       /* WobbleBass */
    [5] = CAT_PAD,        /* MotionPad */
    [6] = CAT_KEYS,       /* LegendEP */
    [7] = CAT_LEAD,       /* DynaLead */
    [8] = CAT_BASS,       /* DarkBass */
    [9] = CAT_PLUCK,      /* TublarBell */
    [10] = CAT_DRUM,      /* D_n_Beats */
    [11] = CAT_LEAD,      /* BeginSweep */
    [12] = CAT_LEAD,      /* MoDemLead */
    [13] = CAT_BASS,      /* BeepBass */
    [14] = CAT_LEAD,      /* BitTune */
    [15] = CAT_PERC,      /* TinPerc */
    [16] = CAT_KEYS,      /* BleepClv */
    [17] = CAT_OTHER,     /* FeelIt */
    [18] = CAT_FX,        /* BuzzSiren */
    [19] = CAT_KEYS,      /* WoodEP */
    [20] = CAT_LEAD,      /* UniLead */
    [21] = CAT_BASS,      /* AttackBass */
    [22] = CAT_PAD,       /* CloudPad */
    [23] = CAT_PLUCK,     /* AmbiPluck */
    [24] = CAT_PERC,      /* Marimba */
    [25] = CAT_ORGAN,     /* CheezOrgan */
    [26] = CAT_BRASS,     /* FM_Brass */
    [27] = CAT_PAD,       /* SolPhase */
    [28] = CAT_FX,        /* FlyingKode */
    [29] = CAT_PAD,       /* AlTiPad */
    [30] = CAT_PAD,       /* StarPad */
    [31] = CAT_PAD,       /* WarmPad */
    [32] = CAT_PLUCK,     /* FutureBell */
    [33] = CAT_PLUCK,     /* GlassHarp */
    [34] = CAT_FX,        /* Chopper */
    [35] = CAT_BRASS,     /* Ph.D Brass */
    [36] = CAT_KEYS,      /* Wurlitzer" */
    [37] = CAT_KEYS,      /* Elec.Piano */
    [38] = CAT_PAD,       /* ApostlePad */
    [39] = CAT_KEYS,      /* WahClav. */
    [40] = CAT_KEYS,      /* NeoRhodes */
    [41] = CAT_BRASS,     /* Warm Horn */
    [42] = CAT_LEAD,      /* StrgMachin */
    [43] = CAT_BRASS,     /* HornSect. */
    [44] = CAT_PAD,       /* Dyna Choir */
    [45] = CAT_KEYS,      /* VinylRhod */
    [46] = CAT_PAD,       /* Darker Pad */
    [47] = CAT_BRASS,     /* Juno Brass */
    [48] = CAT_PAD,       /* 5th.FM */
    [49] = CAT_LEAD,      /* Polific */
    [50] = CAT_ORGAN,     /* SambaOrgan */
    [51] = CAT_PAD,       /* DX Singer */
    [52] = CAT_PAD,       /* Cosmos */
    [53] = CAT_ORGAN,     /* Combo0rgan */
    [54] = CAT_BASS,      /* Low Boy */
    [55] = CAT_LEAD       /* Transistor */
};
#endif
static uint32_t preset_cat(uint32_t e, uint32_t k)    /* factory preset k of engine e */
{
    static const char L[] = "?BLPUKDFOGRC";            /* (a letter's index is its CAT_*) */
    const char *s = e < NELEM(PRESET_CAT) && PRESET_CAT[e] ? PRESET_CAT[e] : "";
    uint32_t i, c;
#if FELUCCA_RDX
    if (e == ENGI_RDX && k < RDX_NPATCHES && RDX_PRESET_CAT[k])
        return RDX_PRESET_CAT[k];
#endif
    for (i = 0; i < k && s[i]; i++)
        ;
    for (c = 1; s[i] && L[c] && L[c] != s[i]; c++)
        ;
    return s[i] && L[c] ? c : cat_of_engine(e);
}
#endif
#endif
