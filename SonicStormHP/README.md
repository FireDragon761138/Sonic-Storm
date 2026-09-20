# SonicStorm HP ⚡🎧 — 7.1 → binaural headphone virtualizer (VST2, 64-bit)

The headphone sibling of SonicStorm. Takes raw Windows **7.1** and renders it
**binaurally for over-ear / on-ear headphones** using a fully analytical
structural head-and-ear model — no measured impulse responses, so no
convolution noise, and every cue scales to *your* head and ears via the Head
and Ear knobs.

## The model (per channel, per ear)

| Stage | Model | Cue |
|---|---|---|
| ITD | Woodworth arc-path delay, exact windowed-sinc fractional delays | Where sounds are, left/right |
| ILD | Brown–Duda spherical-head shadow (pole-zero), ears at ±100° | Natural high-frequency shadowing |
| Pinna | Batteau sliding notch (N1), a second notch (N2 ~11 kHz), frontal concha excess, Blauert rear band, rear flange shelf — all direction-dependent and ear-size-scaled | Front vs back, elevation |
| Torso | "Snowman" shoulder-reflection tap (0.38 ms, lowpassed) — a fixed average (torso size is only weakly correlated with head size, and the cue is a subtle sub-3 kHz body ripple, so it isn't worth a personalization control) | Body realism |
| Room | Image-source model: 4 wall reflections **per source**, each binauralized at its own incidence angle; plus an optional shared late-diffuse tail (Reverb) | Externalization (out-of-head) |

The two pinna notches (N1, N2) plus the first peak are the minimum set that
reconstructs frontal/elevation localization close to a measured HRTF (Iida
2007), and their **frequencies scale as ~1/pinna-size** (Spagnol reflection
model, F = c/2D) — which is what the Ear-size control personalizes.

**Differential voicing:** your own pinna is still in the acoustic path with
over-ear / on-ear headphones, so the model synthesizes only *direction-dependent*
spectral shapes referenced to lateral incidence — never absolute ear
colorations (your ear supplies the concha/canal gain itself). Side channels
pass essentially uncolored; fronts and backs get their differential cues.

**Ear coupling (Over-ear / On-ear):** over-ear (circumaural) leaves the pinna
intact and is the calibration reference. On-ear (supra-aural) presses on and
flattens the *outer* pinna — the helix, flange, and crus ridges — weakening the
direction-dependent cues those structures produce (measured supra-vs-circum
coupling diverges 8–15 dB above 4–5 kHz, but < 5 dB below 2 kHz). So **On-ear**
mode deepens the two outer-pinna cues (the Batteau notch and the rear-flange
shelf) to replace what the compressed pinna no longer supplies; the deep-concha
and low-mid cues, and every head/torso cue (ITD, shadow, shoulder), are
coupling-independent and left untouched. Over-ear is bit-identical to before
this control existed.

Everything is feedforward — unconditionally stable. Double precision
throughout. Center stays *exactly* centered (the virtual room is x-symmetric).
LFE gets proper bass management: a Linkwitz-Riley 4th-order lowpass at 120 Hz
(24 dB/oct) on the LFE path, with a phase-matched 2nd-order allpass on the
mains bus — LR4-LP and AP2 share an identical phase response, so correlated
bass in the two paths sums coherently at every frequency (no crossover notch).
The allpass is identical for both ears, so interaural cues are untouched.
LFE is fed diotically. The fold-down runs **10.5 dB of built-in headroom**
(binaural rendering feeds both channels to each ear, so correlated bass sums
+7 dB on stereo and ~+15 dB on a full 7.1 feed — without headroom that parks
the soft-clipper in constant duty and distorts). A soft-clipper keeps peaks
under 1.0 as a safety net. Latency: 4 samples (sinc-kernel causality).

Channel order is standard Windows 7.1: `FL FR FC LFE BL BR SL SR`
(virtual speakers at ±30°, 0°, ±140°, ±100°).

## Controls

| Knob | What it does | Default |
|---|---|---|
| Space | Room-reflection level; 0 = anechoic (driest, most precise) | 50% |
| Surround | Side + back channel level | 60% |
| Center | Center / dialog level | 60% |
| LFE | Subwoofer-channel level | 40% |
| Output | Master trim (50% = 0 dB reference) | 0 dB |
| Head size | Head radius 6.5–11 cm; rescales every ITD and shadow filter | 8.8 cm |
| Ear coupling | Over-ear (circumaural) or On-ear (supra-aural); On-ear boosts the outer-pinna cues | Over-ear |
| Ear size | Scales the pinna-cue *frequencies* (notches, concha, flange) to your ear; 100% = average, bigger ear → lower cue frequencies. Independent of Head size (ear and head sizes barely correlate) | 100% |
| Reverb | Optional late-diffuse tail for extra room feel. Off = anechoic-precise and free (no CPU cost when unchecked); level follows Space | Off |

> **Gain staging note:** because of the built-in 10.5 dB fold-down headroom,
> Output at 50% plays ~10.5 dB quieter than the raw input. Make the level up
> with your system volume, not the Output knob, to keep the headroom.

**Tuning tips.** These two knobs are set *by ear*, not by measuring yourself:

- **Head size** — play something with a hard-panned rear sound and adjust until
  rear-side sounds sit at a stable point behind your shoulder instead of
  smearing. That's your radius.
- **Ear size** — a *calibration*, not a tone control (its effect is a subtle HF
  notch shift on front/back channels only). To set it, play bright, hard-panned
  **front** content and nudge until front sources anchor solidly *in front* and
  don't lift or flip to the rear. If front/back already images well, leave it at
  100% — it's independent of Head size, so don't change it just because you
  changed Head.

Then set **Space** to taste (higher = more "in a room", lower = more
studio-precise), and tick **Reverb** only if you want a longer room tail.

## Build

Needs WinLibs MinGW-w64 g++ on PATH (`winget install BrechtSanders.WinLibs.POSIX.UCRT`):

```
build_mingw.bat          REM -> SonicStormHP.dll
test_host.exe            REM self-test (built by the same script)
```

**One binary.** `SonicStormHP.dll` carries two copies of the kernel — an x86-64
baseline and an AVX2+FMA build — and picks between them once at load from CPUID.
`SonicStormHP_AVX2.dll` is gone. The pair differed only in `-march`/`-mfma` yet
both claimed uniqueID `'SShp'`, so a host that scanned both recalled whichever it
happened to scan last.

The SSE2 baseline is a supported path, not a formality: the x86-64 ABI mandates
SSE2, so every 64-bit Windows machine from 7 through 11 runs the
per-source-per-ear paired filters natively with no feature check.

The paired primitives (`Biquad2`, `OnePoleLP2`, `HeadShadow2`) still carry the
`__FMA__` branches they always had, and they needed no edits: a `#pragma GCC
target` region sets `__FMA__` for code compiled inside it, so the same source now
yields the plain mul/add form in the baseline kernel and the fused form in the
AVX2 one. Previously that selection came from `-mfma` on the command line, which
is what forced two binaries.

The build greps the linked DLL for `vfmadd` and fails if it is absent — a target
region only governs code compiled *inside* it, so a kernel that stops being
inlined into the AVX2 wrapper silently degrades to a DLL that works, passes every
test, and contains no AVX2 at all.

The self-test verifies the actual psychoacoustics: ITD magnitude and direction,
ILD, front/back spectral cue depth, Head-knob monotonicity, center symmetry,
and the soft-clip ceiling.

## Performance

The render path is heavily optimized but **numerically transparent** — the
reflection stage collapses 112 per-path filters into 4 shared ones via an exact
algebraic identity (the Brown–Duda shadow is affine in its HF gain), so output
is bit-identical (verified to −163 dB, below the 24-bit floor). A per-source
**silence gate** skips any channel whose input is exactly zero once its
reflection tail has drained — free CPU back on stereo/partial content, and
proven transparent through silence transitions (0.0 difference vs an ungated
build). Denormals are handled in hardware (FTZ/DAZ), so silence is exactly
silent. THD sits at the numerical floor (< −147 dB) below the soft-clip knee.
Net: ~1.5× faster on a full 7.1 feed, ~4× on stereo content, with no audible
change.

### Cost and benefit of the single-DLL consolidation

Measured with `bench_cmp.exe`, five **interleaved** pairs each (ns/frame, full 7.1
feed; within-build spread 0.5–0.7):

| Path | Old (two binaries) | New (one DLL) | |
|---|---|---|---|
| baseline / SSE2 | 240.5 | **216.0** | −10.2% |
| AVX2 | 201.9 | 203.7 | +0.9% |

The baseline gained because `-O3` now applies to the whole translation unit — the
old split built only the AVX2 DLL at `-O3` and left the baseline at `-O2` — plus a
`vec4` smoother bank (four of the five knob smoothers are independent one-poles
sharing one coefficient, so they became a single wide update).

The AVX2 path costs 0.9%, and it is worth being precise about why, because it is
*not* the source changes. Compiling this same source whole-file with
`-mavx2 -mfma` benches at 201.5 against the old DLL's 201.3 — indistinguishable.
The 0.9% is the structural cost of runtime dispatch: a target region only makes
the kernel and its inlined callees AVX2, whereas a dedicated build had the entire
translation unit compiled that way. That is the trade for one binary, one
uniqueID, and a 10% faster fallback.

Transparency, across full 7.1, stereo-only and in-place, on all 8 output channels:

| Comparison | Result |
|---|---|
| new baseline kernel vs old `-O2` build | **EXACT 0** — bit-identical |
| new AVX2 kernel vs old `SonicStormHP_AVX2.dll` | 1 sample differs, 4.5e-13 (−247 dBFS) |
| baseline kernel vs AVX2 kernel | 3 samples differ, 4.7e-10 (−187 dBFS) |

The baseline result also confirms `-O2` → `-O3` does not perturb the arithmetic:
GCC will not reassociate floating point without `-ffast-math`.

## Install (Equalizer APO)

1. Set your playback device to **7.1** in Windows Sound settings.
2. Copy `SonicStormHP.dll` to your Equalizer APO `VSTPlugins\` folder
   (if already loaded, comment the `VSTPlugin:` line first — the DLL is
   file-locked while loaded).
3. Add to `config.txt`:
   ```
   VSTPlugin: VSTPlugins\SonicStormHP.dll
   ```
   Equalizer APO stores parameter values on that same line, so an existing chain
   keeps its tuning across a DLL update as long as the parameter list is unchanged.
4. Output appears on front L/R (= headphone L/R); other channels are silenced.

Use **SonicStorm** (speaker version) for your desktop speakers; use **HP** for
headphones. Don't run both at once.

License: BSD-2-Clause.
