# Felucca RDX for M-VAVE FM-1

[![Build firmware and deploy Pages](https://github.com/easyhard007/Felucca-RDX/actions/workflows/pages.yml/badge.svg)](https://github.com/easyhard007/Felucca-RDX/actions/workflows/pages.yml)

Felucca RDX is dedicated firmware that turns the M-VAVE FM-1 into an
eight-voice, four-operator synthesizer inspired by the Yamaha reface DX. It is
a modified fork of [Felucca](https://github.com/hugelton/Felucca) by Leo
Kuroshita / Hügelton Instruments. The product UI exposes only the RDX
instrument and its sound sets.

The current firmware version is **Felucca RDX 0.6.11**. RDX is monotimbral: the
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
- five MIDI velocity curves with Q8.8 velocity inside the RDX engine;
- HOME controls for Reface `MODE` (`Poly`, `MonoFull`, `MonoLeg.`), `PORTA`
  (0–127), calibrated per-sound `GAIN` (−24…+24 dB), and `Trans.` (±24
  semitones), plus centred nonzero ±3-octave status shared by panel, MIDI,
  arpeggiator and sequencer notes.

The primary build output is `build/felucca.fwsc`. It can be loaded by the
[FM-1 emulator](https://joakimeriksson.github.io/fm1-emu/?fw=local&mhz=100)
or installed on an FM-1 through the generated local Web MIDI installer.
The build also produces `build/felucca-clean.fwsc`; this fresh-install package
erases saved projects, user voices and samples, autosave data and system settings.

The public GitHub Pages tools are built from `main` by GitHub Actions:

- [Felucca-RDX Installer](https://easyhard007.github.io/Felucca-RDX/webapp/installer/)
- [Felucca-RDX Editor](https://easyhard007.github.io/Felucca-RDX/webapp/editor/#library)

The installer downloads the firmware package produced by the same workflow, so
the website and firmware always come from the same commit and version.

See [GUIDE.md](GUIDE.md) for the build, simulator and installation workflow.

## Build

From PowerShell:

```powershell
git clone --recurse-submodules https://github.com/easyhard007/Felucca-RDX.git
cd Felucca-RDX
.\build-rdx.ps1
```

The default build enables both CPU cores. Use `-Cpu2 0` only when producing a
single-core diagnostic build.

The version is defined once in `firmware/src/rdx_version.h`. Increment its final
component for every firmware delivered to hardware (`0.6.1`, `0.6.2`, ...,
`0.6.11`); the splash, WebAssembly page and installer package use that value.

`.github/workflows/pages.yml` builds the dual-core RDX firmware, assembles both
web applications with `web/make_site.py`, and deploys `build/site` to GitHub
Pages on every push to `main`. The workflow can also be run manually from the
repository's Actions page.

## Source and licences

The RDX patch format, envelopes, operator behaviour, frequency rules,
algorithms and parts of the effects implementation derive from
[RDX_new/PicoFaceDX](https://github.com/Michi71/PicoVintageSynthCollection/tree/main/instruments/PicoFaceDX)
by Michi71. They were rewritten and optimized as fixed-point C for the JieLi
AC79 platform. See [CREDITS.md](CREDITS.md) for contributor attribution and
[LICENSING.md](LICENSING.md) for the file-by-file licence details.

This repository retains parts of upstream Felucca that are still required for
file-format compatibility, build generation and regression tests. Legacy FM6
source filenames therefore do not imply that FM6 is exposed as a selectable
instrument in the RDX firmware.

Felucca RDX as a whole is distributed under GPL-3.0-only. Bundled and ported
components keep their compatible original licences.

M-VAVE, FM-1, Yamaha and reface DX are trademarks of their respective owners.
This independent community project is not affiliated with or endorsed by
Yamaha, M-VAVE, Hügelton Instruments or their respective owners.
