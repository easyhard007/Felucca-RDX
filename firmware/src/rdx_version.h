/* SPDX-License-Identifier: GPL-3.0-or-later
 * Single source of truth for the RDX release identity. The eight identity
 * digits are 965 + M + mm + pp, so 1.1.2 is FM-1_96510102. build.py and the
 * Hub verifier reject a version/identity mismatch. The loader keeps its own
 * generic ota-FM-1_900 identity while the running app reports this one. */
#ifndef RDX_VERSION_H
#define RDX_VERSION_H
#define RDX_VERSION "1.1.2"
#define RDX_IDENTITY "FM-1_96510102"
#endif
