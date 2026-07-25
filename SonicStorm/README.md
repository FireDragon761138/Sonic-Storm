# SonicStorm ⚡ — 7.1 → 2.0 retro-90s 3D virtualizer (VST2, 64-bit) — EXPERIMENTAL

> **Experimental build.** Identical sound to stock SonicStorm, plus a
> **transparent stereo-detection** optimization: when the surround/center
> channels are exactly silent (pure stereo on a 7.1 endpoint), the surround
> path — the two rear-darkening filters and the surround mix — is skipped.
> It is **bit-for-bit identical** to the reference (verified 0.0 difference,
> including silence transitions), so it changes nothing you can hear. The
> payoff is modest here (~5–18% on stereo content): SonicStorm is already very
> light (a stereo-bus canceller, not per-source rendering), so there isn't much
> per-channel work to skip. The technique is the same one that gives the
> headphone build (SonicStorm HP) a ~4× stereo speedup, where it matters more.

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
g++ -O2 -o test_host.exe test_host.cpp && test_host.exe   REM optional self-test
```

This experimental folder ships two binaries — `SonicStorm.dll` (baseline
x86-64) and `SonicStorm_AVX2.dll` (AVX2/FMA, 2013+ CPUs) — plus
`SonicStorm_ref.dll`, the stock build kept alongside for the transparency
diff. All three sound identical; the AVX2 build is bit-equivalent to baseline.

## Install (Equalizer APO)

1. Set your **playback device to 7.1** in Windows Sound settings (so the 8
   channels actually exist for SonicStorm to receive).
2. Copy `SonicStorm.dll` to `C:\Utilities\EqualizerAPO\VSTPlugins\`
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
