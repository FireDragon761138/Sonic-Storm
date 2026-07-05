# SonicStorm ⚡ & SonicStorm HP 🎧

64-bit VST2 plugins that virtualize 7.1 surround for **Equalizer APO**. A retro
throwback to the great "3D sound" technologies of the 1990s.

- **[SonicStorm](SonicStorm/)** renders 7.1 to **two real speakers** using
  recursive crosstalk cancellation — sounds image beside and behind you from
  just the front pair.
- **[SonicStorm HP](SonicStormHP/)** renders 7.1 **binaurally for over-ear
  headphones** using an analytical head-and-ear simulation: interaural
  time/level differences, structural pinna cues, and an image-source room
  model for out-of-head externalization.

I coded these with Anthropic's Claude Fable and tested them myself on a wide
range of content. It's one of the best "3D sound" simulations I've heard, and
more customizable than typical solutions — including a head-size control that
scales the model to your actual head. Because the spatial cues are synthesized
rather than convolved from captured impulse responses, there's none of the
baked-in noise you get with HRIR-based virtualizers, and CPU usage is
negligible on any modern PC.

## Quick start

1. Install [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) and
   set your playback device to **7.1** in Windows Sound settings.
2. Grab a prebuilt DLL — [SonicStorm.dll](SonicStorm/SonicStorm.dll) or
   [SonicStormHP.dll](SonicStormHP/SonicStormHP.dll) — or build it yourself
   (see the per-plugin READMEs).
3. Copy the DLL to `EqualizerAPO\VSTPlugins\` and add to your config:
   ```
   VSTPlugin: VSTPlugins\SonicStormHP.dll
   ```
   Use **SonicStorm** for speakers or **SonicStorm HP** for headphones — one at
   a time, not both.

Each plugin folder has a full README covering the DSP model, controls, tuning
tips, build instructions, and a self-testing host that verifies the
psychoacoustics (ITD, ILD, front/back cues, clipping ceiling).

## Building

No Steinberg SDK required — the plugins use clean-room VST 2.4 interface
definitions (`vst2_min.h`). Build with MinGW-w64 g++
(`winget install BrechtSanders.WinLibs.POSIX.UCRT`):

```
cd SonicStormHP
build_mingw.bat
```

## License

BSD-2-Clause — see [LICENSE](LICENSE).
