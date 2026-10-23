# RDX sound sets

`selected/` contains the user-provided Yamaha reface DX bulk SysEx files used by the `Selected` sound set.

`tools/gen_rdx_data.py` validates every SysEx message header, model, length, address and Yamaha checksum, then extracts the 38-byte common block and four 28-byte operator blocks. Display names come from the ten-byte voice name stored inside each SysEx file.

Sound sets are index lists over one shared patch table. The same patch index may therefore be listed in more than one set without duplicating its 150-byte parameter block. The existing sound-type categories remain independent from sound-set membership.

Regenerate the firmware header from the project root with:

```powershell
python tools\gen_rdx_data.py E:\Projects\CoreS3\reface_cp_assistant\components\rdx_reface firmware\src\felucca_rdx_data.h
```
