# RDX sound sets

The source Yamaha reface DX bulk SysEx files used to generate the `Selected`
sound set are kept in the maintainer's local library and are not distributed
in this repository. The generated fixed-size patch records are committed in
`firmware/src/felucca_rdx_data.h` and compiled directly into the firmware.

`tools/gen_rdx_data.py` validates every SysEx message header, model, length, address and Yamaha checksum, then extracts the 38-byte common block and four 28-byte operator blocks. Display names come from the ten-byte voice name stored inside each SysEx file.

`gain-db.json` records the maximum ITU-R BS.1770 K-weighted RMS over a continuous
300 ms window during the first second of C3/C4/C5, plus the resulting integer dB
correction for every visible sound. The shorter perceptual window gives transient
electric-piano and plucked sounds their attack weight without letting sustained
pads dominate or incorrectly penalising a slow attack. Regenerate it with
`node tools/measure_rdx_loudness.mjs build/emu/felucca.wasm`; the firmware data
generator validates that the calibration names exactly match the sound sets.

Sound sets are index lists over one shared patch table. The same patch index may therefore be listed in more than one set without duplicating its 150-byte parameter block. The existing sound-type categories remain independent from sound-set membership.

Regenerate the firmware header from the project root by explicitly selecting a
local SysEx directory:

```powershell
python tools\gen_rdx_data.py <RDX_REFERENCE> firmware\src\felucca_rdx_data.h `
  --selected "<LOCAL_REFACE_LIBRARY>\selected"
```
