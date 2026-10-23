# Felucca RDX for M-VAVE FM-1

Felucca RDX is dedicated firmware that turns the M-VAVE FM-1 into an
eight-voice, four-operator synthesizer inspired by the Yamaha reface DX. It is
based on the open-source Felucca firmware, but the product UI exposes only the
RDX instrument and its sound sets.

The current firmware version is **Felucca RDX 0.6.1**. RDX is monotimbral: the
runtime contains one track and one eight-voice RDX part. The old four-track
Felucca mixer, extra MIDI-track channels and automatic track selection are not
part of the RDX build.

Current features include:

- 12 four-operator FM algorithms and an optimized fixed-point renderer;
- Yamaha reface DX envelopes, Pitch EG, LFO, keyboard scaling, mono/legato and
  portamento behaviour;
- Factory and Selected sound sets, including imported reface DX SysEx voices;
- dual-core voice rendering with detailed timing and AUDIO LATE diagnostics;
- an `FX1 -> FX2` serial stereo effects chain;
- native Felucca Chorus, Delay and Hall algorithms;
- fixed-point PicoFaceDX Phaser-RDX and Flanger-RDX algorithms, with the
  original Felucca models retained for A/B comparison;
- five MIDI velocity curves with Q8.8 velocity inside the RDX engine.

The primary build output is `build/felucca.fwsc`. It can be loaded by the
[FM-1 emulator](https://joakimeriksson.github.io/fm1-emu/?fw=local&mhz=100)
or installed on an FM-1 through the generated local Web MIDI installer.

See [GUIDE.md](GUIDE.md) for the build, simulator and installation workflow.

## Build

From PowerShell:

```powershell
cd E:\Projects\fm1\RDX
.\build-rdx.ps1
```

The default build enables both CPU cores. Use `-Cpu2 0` only when producing a
single-core diagnostic build.

The version is defined once in `firmware/src/rdx_version.h`. Increment its final
component for every firmware delivered to hardware (`0.6.1`, `0.6.2`, ...,
`0.6.10`); the splash, WebAssembly page and installer package use that value.

## Source and licences

This repository retains parts of upstream Felucca that are still required for
file-format compatibility, build generation and regression tests. Legacy FM6
source filenames therefore do not imply that FM6 is exposed as a selectable
instrument in the RDX firmware.

Felucca RDX is GPL-3.0-only. Bundled and ported components keep their original
licences; see [LICENSING.md](LICENSING.md) and [LICENSES](LICENSES/).

M-VAVE, FM-1, Yamaha and reface DX are trademarks of their respective owners.
This project is not affiliated with or endorsed by them.
