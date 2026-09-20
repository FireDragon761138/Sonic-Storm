# SonicStorm ⚡ — 7.1 → 2.0 retro-90s 3D virtualizer (VST2, 64-bit)

A lightweight surround-to-stereo virtualizer for **Equalizer APO**. It takes raw
Windows **7.1** and folds it into a single stereo pair for **two real speakers**,
recreating the classic 90s "3D audio" trick of throwing sounds *beside and
behind* you from just the front pair — without convolving big, noisy HRIR
impulse responses (the thing that makes HeSuVi's Dolby Virtual Speaker hiss).

## How it works

```
FL FR ─┐                                 ┌─ recursive crosstalk canceller ─┐
SL SR ─┤─► spatial bus (position weights)┤   (RACE / ambiophonic style,    ├─► L
BL BR ─┘   sides lateral, backs darkened └   bass-protected, head-shadowed)┘   R
FC ───────► direct bus (centered, bypasses canceller) ──────────────────────►
LFE ──────► sub lowpass, centered ──────────────────────────────────────────►
                                                          └► soft-clip ►──────
```

- **Crosstalk cancellation** is what makes stereo speakers image *outside*
  themselves — the signature of 90s 3D-audio hardware. Each channel's own
  delayed, band-limited output is subtracted from the opposite channel.
- **Center + LFE bypass the canceller** so dialog stays anchored and bass stays
  solid and mono-safe. The LFE is properly bass-managed: a Linkwitz-Riley
  4th-order lowpass at 120 Hz (24 dB/oct), with a phase-matched 2nd-order
  allpass on the main bus so correlated bass sums coherently (no crossover
  suckout).
- **Bass-protect + head-shadow filters** keep the canceller's crossfeed gain
  below unity, so it is unconditionally stable.
- **Rear channels are darkened** (duller = "behind you"), a cheap front/back cue.
- Fed **pure stereo** (only FL/FR), SonicStorm becomes a plain retro stereo
  widener — and this build **detects that automatically** (exact-zero test on
  BL/BR/SL/SR/FC) to skip the idle surround path, after a short drain window so
  the darkening filters settle first. The crosstalk canceller and LFE bass
  management always run.

Channel order is standard Windows 7.1: `FL FR FC LFE BL BR SL SR`.

## Controls

| Knob     | What it does                                             | Default |
|----------|----------------------------------------------------------|---------|
| Width    | Out-of-speaker intensity (crosstalk-cancel strength). 0 = plain downmix | 50% |
| Surround | Level of the side + back channels                        | 60% |
| Center   | Center / dialog level                                    | 60% |
| LFE      | Subwoofer level                                          | 40% |
| Output   | Master trim (50% = 0 dB reference)                       | 0 dB |

> **Gain staging note:** the fold-down runs **6 dB of built-in headroom** (a
> full 7.1 feed of correlated bass sums to ~+9 dB, which would otherwise drive
> the soft clipper constantly). Output at 50% therefore plays ~6 dB quieter
> than the raw input — make it up with your system volume, not the Output knob,
> to keep the headroom.

## Build

Needs WinLibs MinGW-w64 g++ on PATH (`winget install BrechtSanders.WinLibs.POSIX.UCRT`):

```
build_mingw.bat          REM -> SonicStorm.dll
test_host.exe            REM self-test (built by the same script)
```

**One binary.** `SonicStorm.dll` carries two copies of the kernel — an x86-64
baseline and an AVX2+FMA build — and picks between them once at load from CPUID.
`SonicStorm_AVX2.dll` is gone; it forced an install-time choice and shared
uniqueID `'SStm'` with the plain build, so a host that scanned both recalled
whichever came last.

The SSE2 baseline is a supported path, not a formality. The x86-64 ABI mandates
SSE2, so every 64-bit Windows machine from 7 through 11 runs the lane-paired
filters natively with no feature check and no scalar fallback to maintain. AVX2
is the opportunistic upgrade on top.

The build greps the linked DLL for `vfmadd` and fails if it is absent — a
`#pragma GCC target` region only governs code compiled *inside* it, so a kernel
that stops being inlined into the AVX2 wrapper silently degrades to a DLL that
works, passes every test, and contains no AVX2 at all.

## Performance

~10.4 ns/frame, down from ~16.9. Measured with `bench_cmp.exe`, four reps each;
no overlap between any adjacent pair of builds:

| Build | ns/frame |
|---|---|
| old `SonicStorm.dll` (scalar x86-64) | 16.92 |
| old `SonicStorm_AVX2.dll` (scalar + AVX2) | 16.28 |
| new baseline kernel (SSE2, lane-paired) | 15.90 |
| new AVX2 kernel (what this CPU selects) | 10.39 |

**−38% against the binary this replaces.** Three changes got it there:

1. **The unused-output silencing left the sample loop.** Channels 2–7 are zeroed
   after the fold-down, and that ran *inside* the per-sample loop: six pointer
   loads, six null checks and six stores on every sample, against roughly sixty
   flops of actual DSP. It is now one `memset` per block. It has to sit *after*
   the loop, not before — an in-place host aliases `out[c]` with `in[c]`, and the
   loop still reads FC, LFE, BL, BR, SL and SR.
2. **The L/R filter pairs run in SSE2 lanes.** The crossfeed bass-protect and
   head-shadow one-poles, the rear-darkening one-poles, and the bass-management
   allpass are all independent chains on identical coefficients — four paired
   updates in place of eight scalar ones.
3. **The knob smoothers run as one wide update.** Four of the five share a
   coefficient and are mutually independent, so they became a single `vec4` bank
   (GCC vector extension, so the same source is two SSE2 updates at baseline and
   one AVX2 update inside the target region). Worth 7% on its own.

Note what the AVX2 column did across that work: the *old* AVX2 build was only 4%
faster than scalar, which is why an earlier pass recorded this loop as
latency-bound and left it alone. That reading was measuring **scalar** code,
where AVX2 can only supply VEX encodings — there was nothing to vectorise. Once
the filters are lane-paired the `__FMA__` branches turn each one-pole into a
single `vfmadd`, and the same CPU is now 35% faster than it was. "AVX2 doesn't
help here" was never evidence that it would not help once the code was
vectorised.

Transparency, all three across full 7.1, stereo-only (the fast path) and
in-place, comparing every one of the 8 output channels:

| Comparison | Result |
|---|---|
| new baseline kernel vs old scalar build | **EXACT 0** — bit-identical |
| baseline kernel vs AVX2 kernel | 3 samples differ, peak 5.8e-11 (−205 dBFS) |

The lane-pairing is bit-exact, as it must be: every operation is element-wise, so
lane 0 never reads lane 1 and there is no cross-lane mixing to disturb the image.
The two ISA kernels differ only where FMA drops a rounding step, 85 dB below the
audibility bar. Channels 2–7 verify as exactly silent in both out-of-place and
in-place modes.

## Install (Equalizer APO)

1. Set your **playback device to 7.1** in Windows Sound settings (so the 8
   channels actually exist for SonicStorm to receive).
2. Copy `SonicStorm.dll` to your Equalizer APO `VSTPlugins\` folder
   (unload it first if it is already referenced — the DLL is file-locked while
   loaded; comment the `VSTPlugin:` line, copy, then re-add it).
3. In `config.txt` (or via the Editor), add:
   ```
   VSTPlugin: VSTPlugins\SonicStorm.dll
   ```
4. SonicStorm outputs the stereo mix on the **front L/R** channels and silences the
   rest, so your actual speakers only need to be the front pair.

> Tip: on headphones, set **Width = 0** — crosstalk cancellation is for speakers.

License: BSD-2-Clause.
