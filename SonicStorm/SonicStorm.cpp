// SonicStorm.cpp
// SonicStorm -- a 7.1 -> 2.0 retro-90s transaural virtualizer (VST 2.4, 64-bit)
// for Equalizer APO.  Replaces a headphone/convolver virtualizer (e.g. HeSuVi's
// Dolby Virtual Speaker) with a lightweight, noise-free synthetic spatializer
// aimed at a pair of real stereo SPEAKERS.
//
// Idea: instead of convolving each channel with a big (noisy) HRIR, we place the
// eight surround channels at synthetic positions in a stereo "ear bus", then run
// that bus through a recursive crosstalk canceller (RACE / ambiophonic style).
// Cancelling the speaker crosstalk is the classic trick 90s "3D audio" systems
// used to make sounds appear BESIDE and BEHIND the listener from only two
// front speakers.
// Center (dialog) and LFE (sub) bypass the canceller so speech stays anchored
// and bass stays solid. The LFE is bass-managed: Linkwitz-Riley 4th-order
// lowpass at 120 Hz with a phase-matched 2nd-order allpass on the main bus, so
// correlated bass in the two paths sums coherently (no crossover suckout).
// The fold-down runs -6 dB of fixed headroom: correlated bass across a 7.1
// explosion sums to ~+9 dB at default knobs, which would otherwise park the
// soft clipper in constant duty. The clipper stays as a peak safety net.
//
// Channel order is standard Windows 7.1 (WAVEFORMATEXTENSIBLE / KSAUDIO):
//   0 FL  1 FR  2 FC  3 LFE  4 BL  5 BR  6 SL  7 SR
//
// Params: 0 Width    out-of-speaker intensity (crosstalk cancel gain)  (0..1)
//         1 Surround side/back channel level                           (0..1)
//         2 Center   center/dialog level                              (0..1)
//         3 LFE      subwoofer level                                  (0..1)
//         4 Output   master output trim (0.5 == unity, 0 dB)          (0..1)
//
// Build: see build_mingw.bat.  License: BSD-2-Clause.

#include "vst2_min.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <immintrin.h>

#if defined(_WIN32)
#include <windows.h>
#include <commctrl.h>   // trackbar (msctls_trackbar32)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The per-sample chain must be inlined into whichever ISA wrapper calls it. A
// GCC target region only governs code compiled INSIDE it, so anything left
// out-of-line here gets emitted once at the x86-64 baseline and BOTH wrappers
// merely call that copy: a DLL that works, passes every test, and contains no
// AVX2 at all. build_mingw.bat greps the linked DLL for vfmadd to catch it.
#if defined(__GNUC__)
#define SSTORM_HOT inline __attribute__((always_inline))
#else
#define SSTORM_HOT inline
#endif

static const VstInt32 kNumParams = 5;
enum { P_WIDTH = 0, P_SURR = 1, P_CENTER = 2, P_LFE = 3, P_OUT = 4 };

// 7.1 channel indices (standard Windows order).
enum { CH_FL = 0, CH_FR, CH_FC, CH_LFE, CH_BL, CH_BR, CH_SL, CH_SR };

// ------------------------------------------------------------- primitives ----
// One-pole lowpass. Gain never exceeds 1, which is what keeps the recursive
// crosstalk canceller unconditionally stable (see run()).
struct OnePoleLP {
    double a = 0.0, z = 0.0;
    void setCutoff(double fs, double fc) {
        if (fc > fs * 0.49) fc = fs * 0.49;
        a = 1.0 - std::exp(-2.0 * M_PI * fc / fs);
    }
    SSTORM_HOT double process(double x) { z += a * (x - z); return z; }
    void reset() { z = 0.0; }
};

// RBJ biquad (lowpass / allpass), coefficients in double. Same voicing as the
// bass-management pair in SonicStorm HP.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    void setUnity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    // RBJ lowpass (cascade two with Q=0.7071 for an LR4, 24 dB/oct).
    void setLowpass(double fs, double fc, double Q) {
        if (fc > fs * 0.45) { setUnity(); return; }
        double w0 = 2.0 * M_PI * fc / fs, cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn / (2.0 * Q);
        double a0 = 1.0 + alpha;
        b0 = ((1.0 - cs) * 0.5) / a0;
        b1 = ( 1.0 - cs) / a0;
        b2 = ((1.0 - cs) * 0.5) / a0;
        a1 = (-2.0 * cs) / a0;
        a2 = ( 1.0 - alpha) / a0;
    }
    // RBJ 2nd-order allpass: flat magnitude, phase response identical to an
    // LR4 lowpass at the same fc/Q -- the standard bass-management phase match.
    void setAllpass(double fs, double fc, double Q) {
        if (fc > fs * 0.45) { setUnity(); return; }
        double w0 = 2.0 * M_PI * fc / fs, cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn / (2.0 * Q);
        double a0 = 1.0 + alpha;
        b0 = (1.0 - alpha) / a0;
        b1 = (-2.0 * cs) / a0;
        b2 = (1.0 + alpha) / a0;
        a1 = (-2.0 * cs) / a0;
        a2 = (1.0 - alpha) / a0;
    }
    SSTORM_HOT double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ---- lane-paired variants -----------------------------------------------
// The L and R sides of the crossfeed, the rear darkening and the bass-management
// allpass are INDEPENDENT chains carrying IDENTICAL coefficients, so each pair
// runs as one SSE2 update instead of two scalar ones. Every operation is
// element-wise -- lane 0 never reads lane 1 -- so there is no cross-lane mixing
// to express and the stereo image is untouched by construction.
//
// SSE2 needs no feature check: the x86-64 ABI mandates it, so this is the
// baseline path on every 64-bit Windows CPU. The FMA branches below are taken
// only inside the AVX2 region (the target pragma sets __FMA__ for code compiled
// there), and they mirror the association GCC picks when it contracts the scalar
// filters, which is what keeps the two kernels agreeing.
static SSTORM_HOT double lane0(__m128d v) { return _mm_cvtsd_f64(v); }
static SSTORM_HOT double lane1(__m128d v) { return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v)); }

struct OnePoleLP2 {
    __m128d a = _mm_setzero_pd(), z = _mm_setzero_pd();
    void setCutoff(double fs, double fc) {
        if (fc > fs * 0.49) fc = fs * 0.49;
        a = _mm_set1_pd(1.0 - std::exp(-2.0 * M_PI * fc / fs));
    }
    SSTORM_HOT __m128d process(__m128d x) {
#if defined(__FMA__)
        z = _mm_fmadd_pd(a, _mm_sub_pd(x, z), z);
#else
        z = _mm_add_pd(z, _mm_mul_pd(a, _mm_sub_pd(x, z)));
#endif
        return z;
    }
    void reset() { z = _mm_setzero_pd(); }
};

// DF2T, both lanes on the same coefficients. Coefficients are still computed by
// the scalar Biquad above (once, at setSampleRate) and broadcast in.
struct Biquad2 {
    __m128d b0 = _mm_set1_pd(1.0), b1 = _mm_setzero_pd(), b2 = _mm_setzero_pd(),
            a1 = _mm_setzero_pd(), a2 = _mm_setzero_pd(),
            z1 = _mm_setzero_pd(), z2 = _mm_setzero_pd();
    void pack(const Biquad& q) {
        b0 = _mm_set1_pd(q.b0); b1 = _mm_set1_pd(q.b1); b2 = _mm_set1_pd(q.b2);
        a1 = _mm_set1_pd(q.a1); a2 = _mm_set1_pd(q.a2);
    }
    SSTORM_HOT __m128d process(__m128d x) {
#if defined(__FMA__)
        __m128d y = _mm_fmadd_pd(b0, x, z1);
        z1 = _mm_add_pd(_mm_fmsub_pd(b1, x, _mm_mul_pd(a1, y)), z2);
        z2 = _mm_fmsub_pd(b2, x, _mm_mul_pd(a2, y));
#else
        __m128d y = _mm_add_pd(_mm_mul_pd(b0, x), z1);
        z1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(b1, x), _mm_mul_pd(a1, y)), z2);
        z2 = _mm_sub_pd(_mm_mul_pd(b2, x), _mm_mul_pd(a2, y));
#endif
        return y;
    }
    void reset() { z1 = z2 = _mm_setzero_pd(); }
};

// Power-of-two circular delay line (holds the canceller's fed-back outputs).
struct Delay {
    static const int SZ = 512, MASK = 511;
    double buf[SZ];
    int w = 0;
    void reset() { std::memset(buf, 0, sizeof buf); w = 0; }
    SSTORM_HOT double read(int D) const { return buf[(w - D) & MASK]; }
    SSTORM_HOT void write(double v) { buf[w] = v; w = (w + 1) & MASK; }
};

// One-pole smoother so knob moves don't zipper.
struct Smooth {
    double v = 0, coeff = 0;
    void init(double fs, double ms, double start) {
        coeff = std::exp(-1.0 / (fs * 0.001 * ms));
        v = start;
    }
    SSTORM_HOT double next(double target) { return v = target + coeff * (v - target); }
};

// Gain smoother bank. The five knob smoothers are independent one-poles that all
// share a single coefficient, so four of them run as one wide update instead of
// four scalar ones. The fifth (output trim) stays scalar rather than padding a
// lane to no purpose.
//
// Declared with GCC's vector_size extension rather than an intrinsic on purpose:
// the SAME source lowers to two SSE2 updates at the baseline and one AVX2 update
// inside the target region, so there is no second version to keep in sync.
typedef double vec4 __attribute__((vector_size(32)));

// Passed and returned by reference, never by value: a 32-byte vector crossing a
// function boundary without -mavx has a different ABI than with it, which GCC
// warns about (-Wpsabi). Everything here is always_inline so no call survives,
// but keeping wide types out of the signatures avoids the question entirely.
struct Smooth4 {
    vec4   v     = { 0, 0, 0, 0 };
    double coeff = 0;
    void init(double fs, double ms, const vec4& start) {
        coeff = std::exp(-1.0 / (fs * 0.001 * ms));
        v = start;
    }
    // Same arithmetic per lane as Smooth::next, so this stays bit-identical to
    // the four scalar smoothers it replaces.
    SSTORM_HOT void next(const vec4& target) { v = target + coeff * (v - target); }
};

// Soft clipper: perfectly linear below 0.8, smooth knee above, ceiling at 1.0.
// Only engages on peaks, so normal-level audio is untouched.
static SSTORM_HOT double softclip(double x) {
    const double t = 0.8;
    double a = std::fabs(x);
    if (a <= t) return x;
    double s = (x < 0.0) ? -1.0 : 1.0;
    return s * (t + (1.0 - t) * std::tanh((a - t) / (1.0 - t)));
}

// Knob (0..1) -> internal gains.
static SSTORM_HOT double gWidth (float p) { return 0.90 * (double)p; }   // crosstalk cancel gain
static SSTORM_HOT double gSurr  (float p) { return 1.40 * (double)p; }
static SSTORM_HOT double gCenter(float p) { return 1.40 * (double)p; }
static SSTORM_HOT double gLfe   (float p) { return 2.00 * (double)p; }
static SSTORM_HOT double gOut   (float p) { return 2.00 * (double)p; }   // 0.5 -> 1.0 (0 dB)

// Fixed fold-down headroom (-6 dB). Correlated bass across a full 7.1 feed
// sums to ~+9 dB at default knobs (measured); without this the soft clipper
// runs in constant duty on loud multichannel content and the bass distorts.
// The Output knob's dB readout stays knob-relative (0.5 = 0 dB reference).
static const double kFoldHeadroom = 0.5;

// ---------------------------------------------------------------- Plugin ----
struct SonicStorm {
    AEffect effect;
    audioMasterCallback master = nullptr;

    float  params[kNumParams];
    double fs = 44100.0;
    int    D  = 10;                    // crosstalk delay in samples (~0.22 ms)

    // Crosstalk-cancel voicing (fixed -- these define the SonicStorm "sound").
    static constexpr double kDelayMs      = 0.22;   // interaural-ish path delay
    static constexpr double kBassProtect  = 150.0;  // don't cancel below this (keeps bass mono/solid)
    static constexpr double kHeadShadow   = 6000.0; // contralateral high-freq rolloff
    static constexpr double kRearDarken   = 5500.0; // back channels sound duller (front/back cue)

    Delay      ringL, ringR;           // fed-back canceller outputs
    OnePoleLP2 xfBass2;                // bass-protect split, both sides in lanes
    OnePoleLP2 xfShadow2;              // head-shadow lowpass, both sides in lanes
    OnePoleLP2 darkB2;                 // rear-channel darkening, BL/BR in lanes

    // LFE path: proper bass management (same arrangement as SonicStorm HP).
    // Linkwitz-Riley 4th-order lowpass at 120 Hz (two cascaded Q=0.7071
    // biquads) on the LFE, and a matching 2nd-order allpass on the main bus.
    // LR4-LP and AP2 share an identical phase response, so correlated bass in
    // the two paths sums coherently at every frequency -- steep junk rejection
    // with no crossover suckout. The allpass is identical for both channels,
    // so the canceller's stereo image is untouched.
    Biquad  lfeLP[2];      // serial LR4 cascade on the mono LFE -- not pairable
    Biquad2 mainAP2;       // L and R buses in the two lanes (identical coeffs)

    Smooth4 smBus;         // {Width, Surround, Center, LFE} in four lanes
    Smooth  smOut;         // output trim, scalar

    // Current knob targets as a vector, so the bank update takes no shuffling.
    SSTORM_HOT void busTargets(vec4& t) const {
        t = vec4{ gWidth (params[P_WIDTH]),  gSurr(params[P_SURR]),
                  gCenter(params[P_CENTER]), gLfe (params[P_LFE]) };
    }

    // Transparent stereo detection. When BL/BR/SL/SR/FC are all
    // exactly zero (pure stereo content on a 7.1 endpoint) the surround/center
    // path -- the two rear-darkening one-poles and the surround mix -- produces
    // nothing, so we skip it. Exact-zero test only; a short drain window lets the
    // darkening filters settle to zero before we freeze them, so the fast path is
    // bit-identical to the full path. The canceller and LFE always run.
    int surrSilent = 0;    // samples since surround/center last carried signal
    int gDrainSurr = 0;    // drain window (rear-darkening one-pole settling)

#if defined(_WIN32)
    HWND edContainer = nullptr;
    HWND edSlider[kNumParams] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    HWND edValue[kNumParams]  = { nullptr, nullptr, nullptr, nullptr, nullptr };
    void openEditor(HWND parent);
    void closeEditor();
    void refreshValue(int i);
    void resetDefaults();      // Defaults button: restore params + sync UI
#endif

    SonicStorm() {
        setDefaultParams();
        setSampleRate(44100.0);
    }

    // Factory defaults in one place so the editor's Defaults button and the
    // constructor can't drift apart.
    void setDefaultParams() {
        params[P_WIDTH]  = 0.5f;   // moderate out-of-speaker spread
        params[P_SURR]   = 0.6f;
        params[P_CENTER] = 0.6f;
        params[P_LFE]    = 0.4f;
        params[P_OUT]    = 0.5f;   // unity
    }

    void setSampleRate(double sr) {
        fs = sr;
        D = (int)std::lround(kDelayMs * 0.001 * fs);
        if (D < 1) D = 1;
        if (D > Delay::SZ - 1) D = Delay::SZ - 1;

        xfBass2.setCutoff(fs, kBassProtect);
        xfShadow2.setCutoff(fs, kHeadShadow);
        darkB2.setCutoff(fs, kRearDarken);
        // Drain window: the rear-darkening one-poles (5.5 kHz) settle in a few
        // dozen samples; 12 ms is a generous, safe margin before we freeze them.
        gDrainSurr = (int)std::lround(fs * 0.012);
        lfeLP[0].setLowpass(fs, 120.0, 0.7071);   // LR4 = two Q=0.7071 sections
        lfeLP[1].setLowpass(fs, 120.0, 0.7071);
        Biquad ap; ap.setAllpass(fs, 120.0, 0.7071);  // phase-match for the mains
        mainAP2.pack(ap);                             // same coeffs in both lanes

        vec4 t0; busTargets(t0);
        smBus.init(fs, 30.0, t0);
        smOut.init(fs, 30.0, gOut(params[P_OUT]));
    }

    void resetState() {
        ringL.reset(); ringR.reset();
        xfBass2.reset(); xfShadow2.reset(); darkB2.reset();
        surrSilent = gDrainSurr + 1;      // start in the fast path until surround arrives
        lfeLP[0].reset(); lfeLP[1].reset();
        mainAP2.reset();
    }

    template <typename T>
    SSTORM_HOT void run(T** in, T** out, VstInt32 n) {
        // Flush subnormals to zero: the recursive canceller keeps recirculating
        // an exponentially decaying tail after the input goes silent, so its
        // delay lines and one-poles would otherwise sit in denormal range
        // stalling the FPU's fast path indefinitely.
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

        // Safe per-channel accessors (tolerate a host that hands us < 8 buffers).
        auto rd = [&](int c, VstInt32 i) -> double {
            return in[c] ? (double)in[c][i] : 0.0;
        };

        // Stereo detection: does any surround/center channel carry a
        // nonzero sample this block? Exact-zero test only. If not (and the
        // darkening filters have drained), the fast path below skips the whole
        // surround/center mix -- bit-identically, since it would produce zero.
        auto anyNonzero = [&](int c) -> bool {
            if (!in[c]) return false;
            for (VstInt32 i = 0; i < n; ++i) if (in[c][i] != (T)0) return true;
            return false;
        };
        bool surrNZ = anyNonzero(CH_BL) || anyNonzero(CH_BR) ||
                      anyNonzero(CH_SL) || anyNonzero(CH_SR) || anyNonzero(CH_FC);
        if (surrNZ) surrSilent = 0;
        bool fast = !surrNZ && (surrSilent >= gDrainSurr);

        for (VstInt32 i = 0; i < n; ++i) {
            // All smoothers advance every sample in both paths, so the canceller
            // state stays bit-identical to the never-gated version.
            vec4 tgt; busTargets(tgt);
            smBus.next(tgt);
            double g    = smBus.v[0], surr = smBus.v[1],
                   cen  = smBus.v[2], lfe  = smBus.v[3];
            double outG = smOut.next(gOut(params[P_OUT]));

            double fl = rd(CH_FL, i),  fr = rd(CH_FR, i);
            double lf = rd(CH_LFE, i);

            double busL, busR, cenS;
            if (fast) {
                // Pure stereo: surround/center are zero, darkening filters drained.
                busL = fl + 0.30 * fr;
                busR = fr + 0.30 * fl;
                cenS = 0.0;
            } else {
                double fc = rd(CH_FC, i);
                double bl = rd(CH_BL, i),  br = rd(CH_BR, i);
                double sl = rd(CH_SL, i),  sr = rd(CH_SR, i);
                // Rear channels are darkened (duller = "behind you" cue). BL and
                // BR are independent chains on identical coefficients -> one
                // paired update. Lane 0 = BL, lane 1 = BR.
                __m128d b  = _mm_set_pd(br, bl);
                __m128d bd = _mm_add_pd(_mm_mul_pd(_mm_set1_pd(0.4), b),
                                        _mm_mul_pd(_mm_set1_pd(0.6), darkB2.process(b)));
                double bld = lane0(bd), brd = lane1(bd);
                // Spatial bus: place each source in the stereo "ear" field.
                // Fronts keep a little opposite-side bleed (0.30) so the canceller
                // has something to widen; sides are fully lateral; backs mostly
                // lateral with a touch of the far side.
                busL = fl + 0.30 * fr + surr * (sl + 0.95 * bld + 0.10 * brd);
                busR = fr + 0.30 * fl + surr * (sr + 0.95 * brd + 0.10 * bld);
                cenS = cen * fc * 0.7071;
            }

            // ---- recursive crosstalk canceller (the out-of-speaker magic) ----
            // Read each channel's own past output, delayed; band-limit it
            // (bass-protect highpass + head-shadow lowpass) so |crossfeed| <= 1,
            // then subtract it from the OPPOSITE channel. Using past outputs
            // (D >= 1) means no algebraic loop; g*|filter| < 1 => stable.
            //
            // The two sides are independent chains, so the whole band-limiting
            // stage runs paired: lane 0 carries the side fed by the R ring, lane
            // 1 the side fed by the L ring, exactly as the scalar code had it.
            __m128d d  = _mm_set_pd(ringL.read(D), ringR.read(D));  // lane0=dR, lane1=dL
            __m128d hp = _mm_sub_pd(d, xfBass2.process(d));         // bass-protected
            __m128d cf = xfShadow2.process(hp);                     // head-shadow

            __m128d bus = _mm_set_pd(busR, busL);
            __m128d y   = _mm_sub_pd(bus, _mm_mul_pd(_mm_set1_pd(g), cf));
            ringL.write(lane0(y)); ringR.write(lane1(y));

            double mk = 1.0 / (1.0 + 0.4 * g);         // level makeup for the cancel
            y = _mm_mul_pd(y, _mm_set1_pd(mk));

            // ---- direct bus: center + LFE bypass the canceller ----
            // Bass management: allpass the mains (phase match), LR4 the LFE,
            // then sum -- coherent at all frequencies. The LFE cascade is mono
            // and serial, so it stays scalar; the allpass pair does not.
            double lfeF = lfe * lfeLP[1].process(lfeLP[0].process(lf));
            __m128d busOut = mainAP2.process(_mm_add_pd(y, _mm_set1_pd(cenS)));
            __m128d o = _mm_mul_pd(
                _mm_add_pd(busOut, _mm_set1_pd(0.7071 * lfeF)),
                _mm_set1_pd(kFoldHeadroom * outG));

            if (out[0]) out[0][i] = (T)softclip(lane0(o));
            if (out[1]) out[1][i] = (T)softclip(lane1(o));
        }

        // We fold everything into the front pair; silence the rest so the
        // surround speakers (if the device has them) don't double up.
        //
        // This used to run inside the sample loop -- six pointer loads, six null
        // checks and six stores on EVERY sample, against roughly sixty flops of
        // actual DSP. Once per block instead. It has to stay AFTER the loop, not
        // before it: an in-place host aliases out[c] with in[c], and the loop
        // still reads FC, LFE, BL, BR, SL and SR. Writing zero bytes gives +0.0
        // for both float and double, so the result is bit-identical either way.
        for (int c = 2; c < effect.numOutputs; ++c)
            if (out[c]) std::memset(out[c], 0, (size_t)n * sizeof(T));

        // Advance the silence counter for blocks whose surround stayed quiet, so
        // the fast path engages only after the darkening filters have drained.
        if (!surrNZ && surrSilent < gDrainSurr + n) surrSilent += n;
    }
};

// ------------------------------------------------------------- editor GUI ----
#if defined(_WIN32)
// Fixed editor size the host reads via effEditGetRect (top,left,bottom,right).
static VstRect g_edRect = { 0, 0, 300, 460 };

// Control id for the Defaults button (sliders use 100+i).
enum { kResetId = 200 };

static HINSTANCE dllInstance() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&g_edRect, &h);
    return (HINSTANCE)h;
}

void SonicStorm::refreshValue(int i) {
    if (!edValue[i]) return;
    char buf[32];
    double v = params[i];
    if (i == P_OUT) {
        double gain = gOut((float)v);
        if (gain <= 1e-5) std::snprintf(buf, sizeof buf, "-inf dB");
        else              std::snprintf(buf, sizeof buf, "%+.1f dB", 20.0 * std::log10(gain));
    } else {
        std::snprintf(buf, sizeof buf, "%.0f %%", v * 100.0);
    }
    SetWindowTextA(edValue[i], buf);
}

static LRESULT CALLBACK EditorWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lp;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    SonicStorm* p = (SonicStorm*)GetWindowLongPtrA(h, GWLP_USERDATA);
    if (msg == WM_HSCROLL && p) {
        HWND tb = (HWND)lp;
        for (int i = 0; i < kNumParams; ++i) {
            if (tb == p->edSlider[i]) {
                int pos = (int)SendMessageA(tb, TBM_GETPOS, 0, 0);
                p->params[i] = (float)(pos / 1000.0);
                p->refreshValue(i);
                break;
            }
        }
        return 0;
    }
    if (msg == WM_COMMAND && p && LOWORD(wp) == kResetId) {
        p->resetDefaults();
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC) return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    return DefWindowProcA(h, msg, wp, lp);
}

void SonicStorm::openEditor(HWND parent) {
    if (edContainer) return;
    HINSTANCE inst = dllInstance();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    static const char* kClass = "SonicStormEditorWnd";
    WNDCLASSEXA wc;
    if (!GetClassInfoExA(inst, kClass, &wc)) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = EditorWndProc;
        wc.hInstance     = inst;
        wc.lpszClassName = kClass;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassExA(&wc);
    }

    edContainer = CreateWindowExA(0, kClass, "", WS_CHILD | WS_VISIBLE,
                                  0, 0, g_edRect.right, g_edRect.bottom,
                                  parent, nullptr, inst, this);
    if (!edContainer) return;

    // Title strip.
    CreateWindowExA(0, "STATIC", "SonicStorm  -  7.1 to 3D stereo",
                    WS_CHILD | WS_VISIBLE,
                    16, 8, 300, 18, edContainer, nullptr, inst, nullptr);

    // Defaults button, tucked into the empty header space (no extra height).
    CreateWindowExA(0, "BUTTON", "Defaults",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    374, 6, 74, 22, edContainer,
                    (HMENU)(intptr_t)kResetId, inst, nullptr);

    const char* names[kNumParams] = { "Width", "Surround", "Center", "LFE", "Output" };
    for (int i = 0; i < kNumParams; ++i) {
        int y = 40 + i * 48;
        CreateWindowExA(0, "STATIC", names[i], WS_CHILD | WS_VISIBLE,
                        16, y, 96, 20, edContainer, nullptr, inst, nullptr);
        HWND tb = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                        116, y - 2, 250, 28, edContainer,
                        (HMENU)(intptr_t)(100 + i), inst, nullptr);
        SendMessageA(tb, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        SendMessageA(tb, TBM_SETPOS, TRUE, (LPARAM)(params[i] * 1000.0f));
        edSlider[i] = tb;
        edValue[i] = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE,
                        374, y, 74, 20, edContainer, nullptr, inst, nullptr);
        refreshValue(i);
    }
}

void SonicStorm::closeEditor() {
    if (edContainer) { DestroyWindow(edContainer); edContainer = nullptr; }
    for (int i = 0; i < kNumParams; ++i) { edSlider[i] = nullptr; edValue[i] = nullptr; }
}

void SonicStorm::resetDefaults() {
    setDefaultParams();
    for (int i = 0; i < kNumParams; ++i) {
        if (edSlider[i]) SendMessageA(edSlider[i], TBM_SETPOS, TRUE,
                                      (LPARAM)(params[i] * 1000.0f));
        refreshValue(i);
    }
}
#endif // _WIN32

// ---------------------------------------------------- host entry helpers ----
static void setParameter(AEffect* e, VstInt32 index, float value) {
    SonicStorm* p = (SonicStorm*)e->object;
    if (index >= 0 && index < kNumParams) p->params[index] = value;
}
static float getParameter(AEffect* e, VstInt32 index) {
    SonicStorm* p = (SonicStorm*)e->object;
    return (index >= 0 && index < kNumParams) ? p->params[index] : 0.0f;
}
// ------------------------------------------------------------- ISA dispatch ----
// One DLL, one kernel, emitted twice: once at the x86-64 baseline and once for
// AVX2+FMA. Selected once at load from CPUID.
//
// The SSE2 baseline is a real, supported path, not a formality -- the x86-64 ABI
// mandates SSE2, so every 64-bit Windows machine from 7 through 11 runs the
// paired filters natively with no feature check. AVX2 is the opportunistic
// upgrade on top of it.
//
// SSTORM_FORCE_BASE / SSTORM_FORCE_AVX2 exist only so the A/B harness can pin a
// path. Neither is defined in a shipping build.
static void runF_base(SonicStorm* p, float**  i, float**  o, VstInt32 n) { p->run<float> (i, o, n); }
static void runD_base(SonicStorm* p, double** i, double** o, VstInt32 n) { p->run<double>(i, o, n); }

#pragma GCC push_options
#pragma GCC target("avx2,fma")
static void runF_avx2(SonicStorm* p, float**  i, float**  o, VstInt32 n) { p->run<float> (i, o, n); }
static void runD_avx2(SonicStorm* p, double** i, double** o, VstInt32 n) { p->run<double>(i, o, n); }
#pragma GCC pop_options

static void (*g_runF)(SonicStorm*, float**,  float**,  VstInt32) = runF_base;
static void (*g_runD)(SonicStorm*, double**, double**, VstInt32) = runD_base;

// Called from VSTPluginMain: host main thread, before any audio. Never CPUID
// from the audio callback.
static void initDispatch() {
#if defined(SSTORM_FORCE_BASE)
    bool avx2 = false;
#elif defined(SSTORM_FORCE_AVX2)
    bool avx2 = true;
#else
    __builtin_cpu_init();
    // Nonzero bitmask on support, not 1.
    bool avx2 = __builtin_cpu_supports("avx2") != 0 && __builtin_cpu_supports("fma") != 0;
#endif
    g_runF = avx2 ? runF_avx2 : runF_base;
    g_runD = avx2 ? runD_avx2 : runD_base;
}

static void processReplacing(AEffect* e, float** in, float** out, VstInt32 n) {
    g_runF((SonicStorm*)e->object, in, out, n);
}
static void processDoubleReplacing(AEffect* e, double** in, double** out, VstInt32 n) {
    g_runD((SonicStorm*)e->object, in, out, n);
}

// Bounded copy that writes ONLY the needed bytes + terminator (never pads to
// cap). VST2 param strings get only kVstMaxParamStrLen (8) bytes; strncpy's
// zero-fill would overrun them.
static void copyStr(void* dst, const char* s, size_t cap) {
    if (!dst || cap == 0) return;
    char* d = (char*)dst;
    size_t i = 0;
    for (; s[i] && i + 1 < cap; ++i) d[i] = s[i];
    d[i] = 0;
}

enum {
    kMaxParamStr   = 8,    // kVstMaxParamStrLen
    kMaxProgName   = 24,
    kMaxEffectName = 32,
    kMaxVendorStr  = 64,
    kMaxProductStr = 64
};

static VstIntPtr dispatcher(AEffect* e, VstInt32 opcode, VstInt32 index,
                            VstIntPtr value, void* ptr, float opt) {
    SonicStorm* p = (SonicStorm*)e->object;
    switch (opcode) {
    case effOpen:  return 0;
    case effClose:
        if (p) {
#if defined(_WIN32)
            p->closeEditor();
#endif
            delete p; e->object = nullptr;
        }
        return 0;

    case effSetSampleRate: p->setSampleRate((double)opt); return 0;
    case effSetBlockSize:  return 0;
    case effMainsChanged:  if (value) p->resetState(); return 0;

#if defined(_WIN32)
    // Equalizer APO calls effEditGetRect and dereferences the returned pointer
    // WITHOUT null-checking, so always hand back a valid rect.
    case effEditGetRect:
        if (ptr) *(VstRect**)ptr = &g_edRect;
        return 1;
    case effEditOpen:
        if (p) p->openEditor((HWND)ptr);
        return 1;
    case effEditClose:
        if (p) p->closeEditor();
        return 1;
    case effEditIdle:
        return 0;
#endif

    case effGetParamName:
    case effGetParamLabel:
    case effGetParamDisplay: {
        char buf[32] = {0};
        if (index < 0 || index >= kNumParams) { copyStr(ptr, "", kMaxParamStr); return 0; }
        double v = p->params[index];
        if (opcode == effGetParamName) {
            // Must fit kVstMaxParamStrLen (8 bytes -> 7 chars).
            const char* names[] = { "Width", "Surr", "Center", "LFE", "Output" };
            copyStr(ptr, names[index], kMaxParamStr);
        } else if (opcode == effGetParamLabel) {
            const char* labels[] = { "%", "%", "%", "%", "dB" };
            copyStr(ptr, labels[index], kMaxParamStr);
        } else { // display
            if (index == P_OUT) {
                double gain = gOut((float)v);
                if (gain <= 1e-5) std::snprintf(buf, sizeof buf, "-inf");
                else              std::snprintf(buf, sizeof buf, "%+.1f", 20.0 * std::log10(gain));
            } else {
                std::snprintf(buf, sizeof buf, "%.0f", v * 100.0);
            }
            copyStr(ptr, buf, kMaxParamStr);
        }
        return 0;
    }

    case effCanBeAutomated: return 1;

    case effGetEffectName:    copyStr(ptr, "SonicStorm", kMaxEffectName); return 1;
    case effGetProductString: copyStr(ptr, "SonicStorm 7.1->2", kMaxProductStr); return 1;
    case effGetVendorString:  copyStr(ptr, "FireDragon761138", kMaxVendorStr);   return 1;
    case effGetVendorVersion: return 1000;
    case effGetPlugCategory:  return kPlugCategEffect;
    case effGetVstVersion:    return 2400;

    case effCanDo: return 0;

    case effGetProgramName: copyStr(ptr, "Default", kMaxProgName); return 0;
    case effSetProgramName: return 0;
    case effGetProgram:     return 0;
    case effSetProgram:     return 0;

    default: return 0;
    }
}

#if defined(_WIN32)
#define VST_EXPORT extern "C" __declspec(dllexport)
#else
#define VST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VST_EXPORT AEffect* VSTPluginMain(audioMasterCallback audioMaster) {
    initDispatch();          // main thread, before any audio; idempotent
    SonicStorm* p = new SonicStorm();
    AEffect* e = &p->effect;
    std::memset(e, 0, sizeof(AEffect));

    e->magic      = kEffectMagic;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing       = processReplacing;
    e->processDoubleReplacing = processDoubleReplacing;

    e->numPrograms = 1;
    e->numParams   = kNumParams;
    e->numInputs   = 8;              // 7.1 in
    e->numOutputs  = 8;              // fold into ch0/ch1, silence the rest
    e->flags       = effFlagsCanReplacing | effFlagsCanDoubleReplacing
                   | effFlagsHasEditor;
    e->uniqueID    = CCONST('S', 'S', 't', 'm');  // 'SStm'
    e->version     = 1000;
    e->object      = p;

    p->master = audioMaster;
    return e;
}

#if defined(_WIN32)
VST_EXPORT AEffect* MAIN(audioMasterCallback audioMaster) {
    return VSTPluginMain(audioMaster);
}
#endif
