# SonicStorm HP ⚡🎧 — 7.1 → binaural headphone virtualizer (VST2, 64-bit)

The headphone sibling of SonicStorm. Takes raw Windows **7.1** and renders it
**binaurally for over-ear / on-ear headphones** using a fully analytical
structural head-and-ear model — no measured impulse responses, so no
convolution noise, and every cue scales to *your* head via the Head knob.

## The model (per channel, per ear)

| Stage | Model | Cue |
|---|---|---|
| ITD | Woodworth arc-path delay, exact windowed-sinc fractional delays | Where sounds are, left/right |
| ILD | Brown–Duda spherical-head shadow (pole-zero), ears at ±100° | Natural high-frequency shadowing |
| Pinna | Batteau sliding notch, frontal concha excess, Blauert rear band, rear flange shelf | Front vs back discrimination |
| Torso | "Snowman" shoulder-reflection tap (0.38 ms, lowpassed) | Body realism |
| Room | Image-source model: 4 wall reflections **per source**, each binauralized at its own incidence angle | Externalization (out-of-head) |

**Over-ear voicing:** your own pinna is still in the acoustic path with
circumaural headphones, so the model synthesizes only *direction-dependent*
spectral shapes referenced to lateral incidence — never absolute ear
colorations (your ear supplies the concha/canal gain itself). Side channels
pass essentially uncolored; fronts and backs get their differential cues.

Everything is feedforward — unconditionally stable. Double precision
throughout. Center stays *exactly* centered (the virtual room is x-symmetric).
LFE gets proper bass management: a Linkwitz-Riley 4th-order lowpass at 120 Hz
(24 dB/oct) on the LFE path, with a phase-matched 2nd-order allpass on the
mains bus — LR4-LP and AP2 share an identical phase response, so correlated
bass in the two paths sums coherently at every frequency (no crossover notch).
The allpass is identical for both ears, so interaural cues are untouched.
LFE is fed diotically. A soft-clipper
keeps the 8→2 fold under 1.0. Latency: 4 samples (sinc-kernel causality).

Channel order is standard Windows 7.1: `FL FR FC LFE BL BR SL SR`
(virtual speakers at ±30°, 0°, ±140°, ±100°).

## Controls

| Knob | What it does | Default |
|---|---|---|
| Space | Room-reflection level; 0 = anechoic (driest, most precise) | 50% |
| Surround | Side + back channel level | 60% |
| Center | Center / dialog level | 60% |
| LFE | Subwoofer-channel level | 40% |
| Output | Master trim (50% = unity / 0 dB) | 0 dB |
| Head size | Head radius 6.5–11 cm; rescales every ITD and shadow filter | 8.8 cm |

**Tuning tip:** play something with a hard-panned rear sound. Adjust **Head
size** until rear-side sounds sit at a stable point behind your shoulder
instead of smearing — that's your radius. Then set **Space** to taste: higher
= more "in a room", lower = more studio-precise.

## Build

Needs WinLibs MinGW-w64 g++ on PATH (`winget install BrechtSanders.WinLibs.POSIX.UCRT`):

```
build_mingw.bat          REM -> SonicStormHP.dll
g++ -O2 -o test_host.exe test_host.cpp && test_host.exe   REM optional self-test
```

The self-test verifies the actual psychoacoustics: ITD magnitude and direction,
ILD, front/back spectral cue depth, Head-knob monotonicity, center symmetry,
and the soft-clip ceiling.

## Install (Equalizer APO)

1. Set your playback device to **7.1** in Windows Sound settings.
2. Copy `SonicStormHP.dll` to `C:\Utilities\EqualizerAPO\VSTPlugins\`
   (if already loaded, comment the `VSTPlugin:` line first — the DLL is
   file-locked while loaded).
3. Add to `config.txt`:
   ```
   VSTPlugin: VSTPlugins\SonicStormHP.dll
   ```
4. Output appears on front L/R (= headphone L/R); other channels are silenced.

Use **SonicStorm** (speaker version) for your desktop speakers; use **HP** for
headphones. Don't run both at once.

License: BSD-2-Clause.
