// SonicStormHP.cpp
// SonicStorm HP -- a 7.1 -> binaural HEADPHONE virtualizer (VST 2.4, 64-bit)
// for Equalizer APO.  Sibling of SonicStorm (the speaker version).
//
// Fully analytical / structural HRTF synthesis -- no measured impulse responses,
// so no convolution noise and every cue is tunable. Each of the seven satellite
// channels is a virtual loudspeaker at a fixed azimuth, rendered per ear with:
//
//   * ITD      -- Woodworth arc-path interaural delay, realized as an exact
//                 windowed-sinc fractional delay (positions are fixed, so the
//                 kernels are precomputed; sample-accurate and alias-free).
//   * ILD      -- Brown & Duda (1998) spherical-head shadow: a one-pole/one-zero
//                 filter whose HF gain runs from +6 dB (source at the ear) down
//                 to deep shadow (opposite side), pivoting at f0 = c/(2*pi*a).
//                 Ears sit at +/-100 deg -- real ears are behind head center.
//   * Pinna    -- structural ear model voiced DIFFERENTIALLY for over-ear /
//                 on-ear headphones: the listener's own pinna is still in the
//                 acoustic path, so we synthesize only direction-dependent
//                 deviations from lateral incidence (the driver's position),
//                 never absolute ear colorations (no canal gain, no baseline
//                 concha boost -- your ear supplies those):
//                   - frontal concha excess (~4.3 kHz, Shaw's mode-1 region)
//                   - Batteau reflection notch (a pinna-crus echo of 55-90 us
//                     puts a notch at 5.5-9.3 kHz that slides with azimuth;
//                     depth fades to zero at lateral incidence)
//                   - Blauert "rear band" bump (~1.25 kHz) for back sources
//                   - pinna-flange high shelf cut for back sources (the flange
//                     physically shadows rear treble)
//   * Torso    -- "snowman" model shoulder reflection: a lowpassed tap ~0.38 ms
//                 behind the direct sound (adds the low-mid body ripple).
//   * Room     -- image-source model of a small listening room: 4 first-order
//                 wall reflections PER SOURCE, each binauralized with its own
//                 incidence azimuth through the same head model (delay+shadow).
//                 This per-reflection spatialization is what externalizes the
//                 image ("out of the head") without sounding phasey. The room
//                 is x-symmetric so the center channel stays exactly centered.
//
// LFE is lowpassed and fed diotically. Everything is feedforward -- stable by
// construction. A Head knob (6.5..11 cm radius) rescales all ITDs and shadow
// filters to personalize the fit.
//
// Channel order is standard Windows 7.1 (WAVEFORMATEXTENSIBLE / KSAUDIO):
//   0 FL  1 FR  2 FC  3 LFE  4 BL  5 BR  6 SL  7 SR
//
// Params: 0 Space   room-reflection level (0 = anechoic)          (0..1)
//         1 Surr    side/back channel level                       (0..1)
//         2 Center  center/dialog level                           (0..1)
//         3 LFE     subwoofer-channel level                       (0..1)
//         4 Output  master trim (0.5 == unity, 0 dB)              (0..1)
//         5 Head    head radius 6.5..11 cm (rescales ITD/shadow)  (0..1)
//         6 Couple  ear coupling: <0.5 over-ear, >=0.5 on-ear     (0..1)
//
// Ear coupling: over-ear (circumaural) leaves the pinna intact, so the model's
// differential voicing is calibrated for it. On-ear (supra-aural) presses on
// and flattens the OUTER pinna (helix/flange/crus), weakening exactly the
// direction-dependent cues those structures produce -- measured supra-vs-circum
// coupling diverges 8-15 dB above 4-5 kHz, <5 dB below 2 kHz. So on-ear mode
// synthesizes MORE of the two outer-pinna cues (Batteau notch, flange shelf) to
// replace what the compressed pinna no longer supplies; the deep-concha and
// low-mid cues, and all head/torso cues, are coupling-independent and untouched.
//
// Build: see build_mingw.bat.  License: BSD-2-Clause.

#include "vst2_min.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX        // keep windows.h from clobbering std::min/std::max (MSVC)
#endif
#include <windows.h>
#include <commctrl.h>   // trackbar (msctls_trackbar32)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const VstInt32 kNumParams = 7;
enum { P_SPACE = 0, P_SURR = 1, P_CENTER = 2, P_LFE = 3, P_OUT = 4, P_HEAD = 5, P_MODE = 6 };

// Ear coupling: over-ear (0) vs on-ear (1). On-ear multiplies the two
// outer-pinna cues so the model replaces the direction-dependent shaping a
// compressed pinna no longer provides. Over-ear values leave the model
// bit-identical to before this control existed.
static inline bool   onEar(float p)        { return p >= 0.5f; }
static const double kOnEarNotchScale  = 1.35;  // Batteau crus-notch depth
static const double kOnEarFlangeScale = 1.30;  // rear-treble flange shelf

// 7.1 channel indices (standard Windows order).
enum { CH_FL = 0, CH_FR, CH_FC, CH_LFE, CH_BL, CH_BR, CH_SL, CH_SR };

static const int kNumSrc  = 7;    // everything except LFE
static const int kNumRefl = 4;    // first-order wall reflections per source

// Virtual-speaker azimuths in degrees (0 = front, + = right), per 7.1 layout
// recommendations (sides 90..110, backs 135..150).
static const double kSrcAz[kNumSrc]  = { -30.0, +30.0, 0.0, -140.0, +140.0, -100.0, +100.0 };
static const int    kSrcChan[kNumSrc] = { CH_FL, CH_FR, CH_FC, CH_BL, CH_BR, CH_SL, CH_SR };

// Physical constants / geometry.
static const double kSpeedSound = 343.0;    // m/s
static const double kEarAzDeg   = 100.0;    // ear axis sits behind head center
static const double kSrcDist    = 2.0;      // virtual speaker distance (m)
// Virtual listening room (m). x-symmetric so FC stays perfectly centered;
// y is asymmetric so front/back reflection delays differ.
static const double kRoomHalfW  = 2.3;      // walls at x = -2.3 / +2.3
static const double kRoomFrontY = 2.9;      // wall ahead of listener
static const double kRoomBackY  = -2.4;     // wall behind listener
static const double kWallGain   = 0.55;     // wall reflectivity
static const double kWallLP     = 5200.0;   // wall absorption lowpass (Hz)
// Shoulder ("snowman") reflection.
static const double kShoulderMs = 0.38, kShoulderGain = 0.18, kShoulderLP = 2800.0;

static const int kSincTaps = 8;     // fractional-delay kernel length
static const int kLatency  = 4;     // constant offset so sinc kernels stay causal

// Knob (0..1) -> internal values.
static inline double gSpace (float p) { return (double)p; }
static inline double gSurr  (float p) { return 1.40 * (double)p; }
static inline double gCenter(float p) { return 1.40 * (double)p; }
static inline double gLfe   (float p) { return 2.00 * (double)p; }
static inline double gOut   (float p) { return 2.00 * (double)p; }   // 0.5 -> unity
static inline double headRadius(float p) { return 0.065 + 0.045 * (double)p; } // 6.5..11 cm

// ------------------------------------------------------------- primitives ----
struct OnePoleLP {
    double a = 0.0, z = 0.0;
    void setCutoff(double fs, double fc) {
        if (fc > fs * 0.49) fc = fs * 0.49;
        a = 1.0 - std::exp(-2.0 * M_PI * fc / fs);
    }
    inline double process(double x) { z += a * (x - z); return z; }
    void reset() { z = 0.0; }
};

// RBJ biquad (peaking / high-shelf), coefficients in double.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    void setUnity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    // Peaking EQ; negative dB gives the Batteau-style notch cut.
    void setPeaking(double fs, double fc, double Q, double dB) {
        if (fc > fs * 0.45) { setUnity(); return; }
        double A = std::pow(10.0, dB / 40.0);
        double w0 = 2.0 * M_PI * fc / fs, cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn / (2.0 * Q);
        double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cs) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cs) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }
    // RBJ lowpass (cascade two with Butterworth Q's for 24 dB/oct).
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
    // RBJ high shelf, shelf slope S = 1.
    void setHighShelf(double fs, double fc, double dB) {
        if (fc > fs * 0.45) { setUnity(); return; }
        double A = std::pow(10.0, dB / 40.0);
        double w0 = 2.0 * M_PI * fc / fs, cs = std::cos(w0), sn = std::sin(w0);
        double alphaS = sn / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 1.0 - 1.0) + 2.0);
        double twoRootAalpha = 2.0 * std::sqrt(A) * alphaS;
        double a0 =        (A + 1.0) - (A - 1.0) * cs + twoRootAalpha;
        b0 = (    A * ((A + 1.0) + (A - 1.0) * cs + twoRootAalpha)) / a0;
        b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cs)) / a0;
        b2 = (    A * ((A + 1.0) + (A - 1.0) * cs - twoRootAalpha)) / a0;
        a1 = ( 2.0 * ((A - 1.0) - (A + 1.0) * cs)) / a0;
        a2 = (       (A + 1.0) - (A - 1.0) * cs - twoRootAalpha) / a0;
    }
    inline double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Brown-Duda spherical-head shadow: H(w) = (1 + jw*alpha/(2w0)) / (1 + jw/(2w0)),
// bilinear-transformed. DC gain 1; HF gain = alpha (2.0 at the near ear down to
// ~0.1 in the deepest shadow, with the slight antipodal "bright spot" the sphere
// solution predicts). w0 = c / a.
struct HeadShadow {
    double b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    void set(double fs, double headA, double thetaIncDeg) {
        double w0    = kSpeedSound / headA;                       // rad/s
        double alpha = 1.05 + 0.95 * std::cos(thetaIncDeg * 1.2 * M_PI / 180.0);
        double den = w0 + fs;
        b0 = (w0 + alpha * fs) / den;
        b1 = (w0 - alpha * fs) / den;
        a1 = (w0 - fs) / den;
    }
    inline double process(double x) {
        double y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x; y1 = y;
        return y;
    }
    void reset() { x1 = y1 = 0; }
};

// Power-of-two ring buffer, one per source. Sized for the longest room
// reflection at 192 kHz (~20 ms) plus ITD and sinc margin.
struct SrcRing {
    static const int SZ = 8192, MASK = 8191;
    double buf[SZ];
    int w = 0;
    void reset() { std::memset(buf, 0, sizeof buf); w = 0; }
    inline void write(double v) { buf[w] = v; w = (w + 1) & MASK; }
    inline double read(int d) const { return buf[(w - 1 - d) & MASK]; }
};

// Precomputed windowed-sinc fractional-delay tap (exact for fixed positions).
struct FracTap {
    int    base = 0;                 // integer delay of kernel start
    double wgt[kSincTaps] = {0};
    void set(double delaySamples) {
        double D = delaySamples;
        int D0 = (int)std::floor(D);
        double f = D - D0;
        base = D0 - (kSincTaps / 2 - 1);      // taps cover D0-3 .. D0+4
        if (base < 0) base = 0;               // (kLatency keeps this causal)
        double sum = 0;
        for (int j = 0; j < kSincTaps; ++j) {
            double arg = (double)(base + j) - D;      // -3-f .. 4-f
            double t = arg * M_PI;
            double snc = (std::fabs(t) < 1e-9) ? 1.0 : std::sin(t) / t;
            double x = arg / (double)(kSincTaps / 2); // window support +/-4
            double win = (std::fabs(x) <= 1.0)
                       ? 0.42 + 0.5 * std::cos(M_PI * x) + 0.08 * std::cos(2.0 * M_PI * x)
                       : 0.0;
            wgt[j] = snc * win;
            sum += wgt[j];
        }
        if (std::fabs(sum) > 1e-9)            // normalize for exact DC gain
            for (int j = 0; j < kSincTaps; ++j) wgt[j] /= sum;
    }
    inline double read(const SrcRing& r) const {
        double acc = 0;
        for (int j = 0; j < kSincTaps; ++j) acc += wgt[j] * r.read(base + j);
        return acc;
    }
};

// One-pole smoother so knob moves don't zipper.
struct Smooth {
    double v = 0, coeff = 0;
    void init(double fs, double ms, double start) {
        coeff = std::exp(-1.0 / (fs * 0.001 * ms));
        v = start;
    }
    inline double next(double target) { return v = target + coeff * (v - target); }
};

// Soft clipper: linear below 0.8, smooth knee, ceiling 1.0.
static inline double softclip(double x) {
    const double t = 0.8;
    double a = std::fabs(x);
    if (a <= t) return x;
    double s = (x < 0.0) ? -1.0 : 1.0;
    return s * (t + (1.0 - t) * std::tanh((a - t) / (1.0 - t)));
}

// Wrap an angle difference to [0, 180] degrees.
static inline double angDist(double a, double b) {
    double d = std::fabs(a - b);
    while (d > 360.0) d -= 360.0;
    if (d > 180.0) d = 360.0 - d;
    return d;
}

// Woodworth per-ear path delay (seconds) vs incidence angle from the ear axis:
// straight line while the ear is visible, then along the head's arc.
static inline double earDelaySec(double headA, double thetaIncDeg) {
    double th = thetaIncDeg * M_PI / 180.0;
    if (thetaIncDeg < 90.0)
        return (headA / kSpeedSound) * (1.0 - std::cos(th));
    return (headA / kSpeedSound) * (1.0 + (th - M_PI / 2.0));
}

// ---------------------------------------------------------------- Plugin ----
struct SonicStormHP {
    AEffect effect;
    audioMasterCallback master = nullptr;

    float  params[kNumParams];
    double fs = 44100.0;
    double lastHead = -1.0;            // triggers model rebuild when knob moves
    double lastMode = -1.0;            // triggers rebuild when ear coupling toggles

    SrcRing ring[kNumSrc];

    // Per source, per ear (0 = L, 1 = R): direct path.
    FracTap    tapDir[kNumSrc][2];
    FracTap    tapShl[kNumSrc][2];     // shoulder reflection tap
    OnePoleLP  shlLP [kNumSrc][2];
    HeadShadow shadow[kNumSrc][2];
    Biquad     concha[kNumSrc][2];     // concha resonance peak
    Biquad     notch [kNumSrc][2];     // Batteau pinna-reflection notch
    Biquad     rearEQ[kNumSrc][2];     // Blauert rear band bump
    Biquad     flange[kNumSrc][2];     // rear flange high-shelf cut

    // Per source, per wall reflection, per ear (integer-delay taps: reflections
    // are diffuse enough that sub-sample accuracy buys nothing).
    int        reflDelay[kNumSrc][kNumRefl][2];
    double     reflGain [kNumSrc][kNumRefl];
    OnePoleLP  reflLP   [kNumSrc][kNumRefl][2];
    HeadShadow reflShad [kNumSrc][kNumRefl][2];

    // LFE path: proper bass management. Linkwitz-Riley 4th-order lowpass
    // (24 dB/oct, two cascaded Q=0.7071 biquads) on the LFE, and a matching
    // 2nd-order allpass on the full-range mains bus. LR4-LP and AP2 share an
    // identical phase response, so correlated bass in the two paths sums
    // coherently at every frequency -- steep junk rejection with no notch.
    // The allpass is the same for both ears, so interaural cues are untouched.
    Biquad lfeLP[2];
    Biquad mainAP[2];      // [0] = L ear bus, [1] = R ear bus
    Smooth smSpace, smSurr, smCen, smLfe, smOut;

#if defined(_WIN32)
    HWND edContainer = nullptr;
    HWND edSlider[kNumParams] = {0};
    HWND edValue[kNumParams]  = {0};
    void openEditor(HWND parent);
    void closeEditor();
    void refreshValue(int i);
    void resetDefaults();      // Defaults button: restore params + sync UI
#endif

    SonicStormHP() {
        setDefaultParams();
        setSampleRate(44100.0);
    }

    // Factory defaults in one place so the editor's Defaults button and the
    // constructor can't drift apart.
    void setDefaultParams() {
        params[P_SPACE]  = 0.5f;
        params[P_SURR]   = 0.6f;
        params[P_CENTER] = 0.6f;
        params[P_LFE]    = 0.4f;
        params[P_OUT]    = 0.5f;   // unity
        params[P_HEAD]   = 0.5f;   // 8.75 cm -- average adult head
        params[P_MODE]   = 0.0f;   // over-ear (calibration reference)
    }

    void setSampleRate(double sr) {
        fs = sr;
        lastHead = -1.0;
        lastMode = -1.0;
        rebuildModel();
        lfeLP[0].setLowpass(fs, 120.0, 0.7071);   // LR4 = two Q=0.7071 sections
        lfeLP[1].setLowpass(fs, 120.0, 0.7071);
        mainAP[0].setAllpass(fs, 120.0, 0.7071);  // phase-match for the mains
        mainAP[1].setAllpass(fs, 120.0, 0.7071);
        smSpace.init(fs, 30.0, gSpace(params[P_SPACE]));
        smSurr.init (fs, 30.0, gSurr (params[P_SURR]));
        smCen.init  (fs, 30.0, gCenter(params[P_CENTER]));
        smLfe.init  (fs, 30.0, gLfe  (params[P_LFE]));
        smOut.init  (fs, 30.0, gOut  (params[P_OUT]));
    }

    // Recompute every geometry-derived constant. Cheap (a few k flops); called
    // at block boundaries when the Head knob moves.
    void rebuildModel() {
        double a = headRadius(params[P_HEAD]);
        double mode = onEar(params[P_MODE]) ? 1.0 : 0.0;
        if (std::fabs(a - lastHead) < 1e-5 && mode == lastMode) return;
        lastHead = a;
        lastMode = mode;

        // On-ear boosts the two OUTER-pinna cues (see header): the earpad
        // flattens the structures that produce them, so the model supplies more.
        double notchScale  = (mode > 0.5) ? kOnEarNotchScale  : 1.0;
        double flangeScale = (mode > 0.5) ? kOnEarFlangeScale : 1.0;

        const double earAz[2] = { -kEarAzDeg, +kEarAzDeg };

        for (int s = 0; s < kNumSrc; ++s) {
            double az = kSrcAz[s];
            double frontness = std::cos(az * M_PI / 180.0);   // +1 front .. -1 back

            for (int e = 0; e < 2; ++e) {
                double thInc = angDist(az, earAz[e]);

                // Direct + shoulder taps (windowed-sinc fractional delays).
                double dDir = kLatency + earDelaySec(a, thInc) * fs;
                tapDir[s][e].set(dDir);
                tapShl[s][e].set(dDir + kShoulderMs * 0.001 * fs);
                shlLP[s][e].setCutoff(fs, kShoulderLP);

                // Spherical-head shadow.
                shadow[s][e].set(fs, a, thInc);

                // --- structural pinna, voiced DIFFERENTIALLY for over-ear /
                // on-ear headphones. The listener's own pinna is still in the
                // acoustic path (the driver fires into the outer ear from the
                // side), so absolute pinna colorations (concha/canal gain)
                // would be applied twice. We synthesize only what the fixed
                // driver position cannot produce: direction-DEPENDENT shapes
                // referenced to lateral incidence. Side channels (~ear axis)
                // therefore pass essentially uncolored.
                // Concha boost: only the frontal-incidence excess over lateral.
                double conchaDb = 4.0 * std::max(frontness, 0.0);
                if (conchaDb > 0.05) concha[s][e].setPeaking(fs, 4300.0, 2.2, conchaDb);
                else                 concha[s][e].setUnity();
                // Batteau crus echo -> notch slides with azimuth; depth scales
                // with distance from lateral so sides get no synthetic notch.
                double fNotch = 6500.0 + 2800.0 * std::max(frontness, 0.0)
                                       -  900.0 * std::max(-frontness, 0.0);
                double notchDb = -7.5 * std::fabs(frontness) * notchScale;
                if (notchDb < -0.05) notch[s][e].setPeaking(fs, fNotch, 4.5, notchDb);
                else                 notch[s][e].setUnity();
                // Blauert rear band (~1.25 kHz) for back sources.
                double rearDb = 3.5 * std::max(-frontness, 0.0);
                if (rearDb > 0.05) rearEQ[s][e].setPeaking(fs, 1250.0, 1.4, rearDb);
                else               rearEQ[s][e].setUnity();
                // Pinna flange shadows rear treble. Voiced strong on purpose:
                // the bare-sphere head model actually BRIGHTENS rear-lateral
                // sources (they sit near the ear axis), so the flange must
                // overpower it -- measured HRTFs show rear sources losing
                // 5-15 dB above ~8 kHz relative to front.
                double shelfDb = -14.0 * std::max(-frontness, 0.0) * flangeScale;
                if (shelfDb < -0.05) flange[s][e].setHighShelf(fs, 7800.0, shelfDb);
                else                 flange[s][e].setUnity();
            }

            // --- image-source room reflections (4 first-order walls) ---
            double sx = kSrcDist * std::sin(az * M_PI / 180.0);
            double sy = kSrcDist * std::cos(az * M_PI / 180.0);
            double ix[kNumRefl] = { 2.0 * (-kRoomHalfW) - sx,   // left wall
                                    2.0 * ( kRoomHalfW) - sx,   // right wall
                                    sx,                          // front wall
                                    sx };                        // back wall
            double iy[kNumRefl] = { sy, sy,
                                    2.0 * kRoomFrontY - sy,
                                    2.0 * kRoomBackY  - sy };
            for (int r = 0; r < kNumRefl; ++r) {
                double path = std::sqrt(ix[r] * ix[r] + iy[r] * iy[r]);
                double reflAz = std::atan2(ix[r], iy[r]) * 180.0 / M_PI;
                double dExtra = (path - kSrcDist) / kSpeedSound;   // s after direct
                reflGain[s][r] = kWallGain * (kSrcDist / path);
                for (int e = 0; e < 2; ++e) {
                    double thInc = angDist(reflAz, earAz[e]);
                    reflDelay[s][r][e] = (int)std::lround(
                        kLatency + (dExtra + earDelaySec(a, thInc)) * fs);
                    if (reflDelay[s][r][e] > SrcRing::SZ - 2)
                        reflDelay[s][r][e] = SrcRing::SZ - 2;
                    reflLP[s][r][e].setCutoff(fs, kWallLP);
                    reflShad[s][r][e].set(fs, a, thInc);
                }
            }
        }
    }

    void resetState() {
        for (int s = 0; s < kNumSrc; ++s) {
            ring[s].reset();
            for (int e = 0; e < 2; ++e) {
                shlLP[s][e].reset(); shadow[s][e].reset();
                concha[s][e].reset(); notch[s][e].reset();
                rearEQ[s][e].reset(); flange[s][e].reset();
                for (int r = 0; r < kNumRefl; ++r) {
                    reflLP[s][r][e].reset(); reflShad[s][r][e].reset();
                }
            }
        }
        lfeLP[0].reset(); lfeLP[1].reset();
        mainAP[0].reset(); mainAP[1].reset();
    }

    template <typename T>
    void run(T** in, T** out, VstInt32 n) {
        rebuildModel();     // no-op unless the Head knob moved

        auto rdch = [&](int c, VstInt32 i) -> double {
            return in[c] ? (double)in[c][i] : 0.0;
        };

        double dn = 1e-20;  // anti-denormal (sign-flipped each sample)

        for (VstInt32 i = 0; i < n; ++i) {
            double space = smSpace.next(gSpace(params[P_SPACE]));
            double surr  = smSurr.next (gSurr (params[P_SURR]));
            double cen   = smCen.next  (gCenter(params[P_CENTER]));
            double lfeG  = smLfe.next  (gLfe  (params[P_LFE]));
            double outG  = smOut.next  (gOut  (params[P_OUT]));
            dn = -dn;

            // Write each source into its ring, knob gain applied at the input
            // so changes ride smoothly through all delayed taps.
            for (int s = 0; s < kNumSrc; ++s) {
                double g = (kSrcChan[s] == CH_FC) ? cen : (s < 2 ? 1.0 : surr);
                // (s < 2 -> FL/FR at full level; FC uses Center; rest use Surr)
                ring[s].write(g * rdch(kSrcChan[s], i) + dn);
            }

            double earL = 0.0, earR = 0.0;

            for (int s = 0; s < kNumSrc; ++s) {
                for (int e = 0; e < 2; ++e) {
                    // Direct + shoulder, then the ear's filter chain.
                    double x = tapDir[s][e].read(ring[s])
                             + kShoulderGain * shlLP[s][e].process(tapShl[s][e].read(ring[s]));
                    x = shadow[s][e].process(x);
                    x = concha[s][e].process(x);
                    x = notch [s][e].process(x);
                    x = rearEQ[s][e].process(x);
                    x = flange[s][e].process(x);

                    // Room reflections, binauralized per incidence angle.
                    double refl = 0.0;
                    for (int r = 0; r < kNumRefl; ++r) {
                        double v = ring[s].read(reflDelay[s][r][e]);
                        v = reflLP[s][r][e].process(v);
                        v = reflShad[s][r][e].process(v);
                        refl += reflGain[s][r] * v;
                    }
                    x += 0.5 * space * refl;

                    if (e == 0) earL += x; else earR += x;
                }
            }

            // Bass management: allpass the mains (phase match), LR4 the LFE,
            // then sum -- coherent at all frequencies. LFE is diotic.
            earL = mainAP[0].process(earL);
            earR = mainAP[1].process(earR);
            double lfe = lfeG * lfeLP[1].process(lfeLP[0].process(rdch(CH_LFE, i)));
            earL += 0.7071 * lfe;
            earR += 0.7071 * lfe;

            if (out[0]) out[0][i] = (T)softclip(earL * outG);
            if (out[1]) out[1][i] = (T)softclip(earR * outG);
            for (int c = 2; c < effect.numOutputs; ++c)
                if (out[c]) out[c][i] = (T)0;
        }
    }
};

// ------------------------------------------------------------- editor GUI ----
#if defined(_WIN32)
static VstRect g_edRect = { 0, 0, 404, 460 };

// Control ids: sliders use 100+i; the Defaults button and the two ear-coupling
// radio buttons get their own ids.
enum { kResetId = 200, kModeOverId = 201, kModeOnId = 202 };

static HINSTANCE dllInstance() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&g_edRect, &h);
    return (HINSTANCE)h;
}

void SonicStormHP::refreshValue(int i) {
    if (i == P_MODE) {                 // two radio buttons, not a slider+value pair
        bool on = onEar(params[P_MODE]);
        if (edSlider[P_MODE]) SendMessageA(edSlider[P_MODE], BM_SETCHECK,  // Over-ear
                                           on ? BST_UNCHECKED : BST_CHECKED, 0);
        if (edValue[P_MODE])  SendMessageA(edValue[P_MODE],  BM_SETCHECK,  // On-ear
                                           on ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    if (!edValue[i]) return;
    char buf[32];
    double v = params[i];
    if (i == P_OUT) {
        double gain = gOut((float)v);
        if (gain <= 1e-5) std::snprintf(buf, sizeof buf, "-inf dB");
        else              std::snprintf(buf, sizeof buf, "%+.1f dB", 20.0 * std::log10(gain));
    } else if (i == P_HEAD) {
        std::snprintf(buf, sizeof buf, "%.1f cm", headRadius((float)v) * 100.0);
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
    SonicStormHP* p = (SonicStormHP*)GetWindowLongPtrA(h, GWLP_USERDATA);
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
    if (msg == WM_COMMAND && p) {
        int id = LOWORD(wp);
        if (id == kResetId)   { p->resetDefaults(); return 0; }
        if (id == kModeOverId) { p->params[P_MODE] = 0.0f; p->refreshValue(P_MODE); return 0; }
        if (id == kModeOnId)   { p->params[P_MODE] = 1.0f; p->refreshValue(P_MODE); return 0; }
    }
    if (msg == WM_CTLCOLORSTATIC) return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    return DefWindowProcA(h, msg, wp, lp);
}

void SonicStormHP::openEditor(HWND parent) {
    if (edContainer) return;
    HINSTANCE inst = dllInstance();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    static const char* kClass = "SonicStormHPEditorWnd";
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

    CreateWindowExA(0, "STATIC", "SonicStorm HP  -  7.1 to binaural",
                    WS_CHILD | WS_VISIBLE,
                    16, 8, 300, 18, edContainer, nullptr, inst, nullptr);

    // Defaults button, tucked into the empty header space (no extra height).
    CreateWindowExA(0, "BUTTON", "Defaults",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    374, 6, 74, 22, edContainer,
                    (HMENU)(intptr_t)kResetId, inst, nullptr);

    const char* names[kNumParams] = { "Space", "Surround", "Center", "LFE",
                                      "Output", "Head size", "Ear coupling" };
    for (int i = 0; i < kNumParams; ++i) {
        int y = 40 + i * 48;
        CreateWindowExA(0, "STATIC", names[i], WS_CHILD | WS_VISIBLE,
                        16, y, 96, 20, edContainer, nullptr, inst, nullptr);
        if (i == P_MODE) {             // mutually-exclusive pair, not a slider
            // WS_GROUP on the first radio starts a fresh group so the two
            // auto-radios exclude each other (and nothing above them).
            edSlider[i] = CreateWindowExA(0, "BUTTON", "Over-ear",
                        WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
                        116, y - 2, 108, 24, edContainer,
                        (HMENU)(intptr_t)kModeOverId, inst, nullptr);
            edValue[i] = CreateWindowExA(0, "BUTTON", "On-ear",
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                        232, y - 2, 100, 24, edContainer,
                        (HMENU)(intptr_t)kModeOnId, inst, nullptr);
            refreshValue(i);
            continue;
        }
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

void SonicStormHP::closeEditor() {
    if (edContainer) { DestroyWindow(edContainer); edContainer = nullptr; }
    for (int i = 0; i < kNumParams; ++i) { edSlider[i] = nullptr; edValue[i] = nullptr; }
}

void SonicStormHP::resetDefaults() {
    setDefaultParams();
    for (int i = 0; i < kNumParams; ++i) {
        if (i != P_MODE && edSlider[i])
            SendMessageA(edSlider[i], TBM_SETPOS, TRUE, (LPARAM)(params[i] * 1000.0f));
        refreshValue(i);          // P_MODE updates its radio pair here
    }
}
#endif // _WIN32

// ---------------------------------------------------- host entry helpers ----
static void setParameter(AEffect* e, VstInt32 index, float value) {
    SonicStormHP* p = (SonicStormHP*)e->object;
    if (index >= 0 && index < kNumParams) p->params[index] = value;
}
static float getParameter(AEffect* e, VstInt32 index) {
    SonicStormHP* p = (SonicStormHP*)e->object;
    return (index >= 0 && index < kNumParams) ? p->params[index] : 0.0f;
}
static void processReplacing(AEffect* e, float** in, float** out, VstInt32 n) {
    ((SonicStormHP*)e->object)->run<float>(in, out, n);
}
static void processDoubleReplacing(AEffect* e, double** in, double** out, VstInt32 n) {
    ((SonicStormHP*)e->object)->run<double>(in, out, n);
}

// Bounded copy: writes only the needed bytes + terminator (VST2 param strings
// get an 8-byte buffer; zero-filling to a larger cap would overrun the host).
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
    SonicStormHP* p = (SonicStormHP*)e->object;
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
    // Equalizer APO dereferences the rect pointer without a null check --
    // always hand back a valid rect.
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
            const char* names[] = { "Space", "Surr", "Center", "LFE", "Output", "Head", "Couple" };
            copyStr(ptr, names[index], kMaxParamStr);
        } else if (opcode == effGetParamLabel) {
            const char* labels[] = { "%", "%", "%", "%", "dB", "cm", "" };
            copyStr(ptr, labels[index], kMaxParamStr);
        } else { // display
            if (index == P_OUT) {
                double gain = gOut((float)v);
                if (gain <= 1e-5) std::snprintf(buf, sizeof buf, "-inf");
                else              std::snprintf(buf, sizeof buf, "%+.1f", 20.0 * std::log10(gain));
            } else if (index == P_HEAD) {
                std::snprintf(buf, sizeof buf, "%.1f", headRadius((float)v) * 100.0);
            } else if (index == P_MODE) {
                std::snprintf(buf, sizeof buf, onEar((float)v) ? "On" : "Over");
            } else {
                std::snprintf(buf, sizeof buf, "%.0f", v * 100.0);
            }
            copyStr(ptr, buf, kMaxParamStr);
        }
        return 0;
    }

    case effCanBeAutomated: return 1;

    case effGetEffectName:    copyStr(ptr, "SonicStorm HP", kMaxEffectName); return 1;
    case effGetProductString: copyStr(ptr, "SonicStorm HP 7.1->binaural", kMaxProductStr); return 1;
    case effGetVendorString:  copyStr(ptr, "daede", kMaxVendorStr);   return 1;
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
    SonicStormHP* p = new SonicStormHP();
    AEffect* e = &p->effect;
    std::memset(e, 0, sizeof(AEffect));

    e->magic      = kEffectMagic;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing       = processReplacing;
    e->processDoubleReplacing = processDoubleReplacing;

    e->numPrograms  = 1;
    e->numParams    = kNumParams;
    e->numInputs    = 8;             // 7.1 in
    e->numOutputs   = 8;             // binaural on ch0/ch1, rest silenced
    e->flags        = effFlagsCanReplacing | effFlagsCanDoubleReplacing
                    | effFlagsHasEditor;
    e->initialDelay = kLatency;      // sinc-kernel causality offset
    e->uniqueID     = CCONST('S', 'S', 'h', 'p');  // 'SShp'
    e->version      = 1000;
    e->object       = p;

    p->master = audioMaster;
    return e;
}

#if defined(_WIN32)
VST_EXPORT AEffect* MAIN(audioMasterCallback audioMaster) {
    return VSTPluginMain(audioMaster);
}
#endif
