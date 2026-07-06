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
// and bass stays solid; a soft clipper keeps the 8->2 sum from clipping the DAC.
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
    inline double process(double x) { z += a * (x - z); return z; }
    void reset() { z = 0.0; }
};

// Power-of-two circular delay line (holds the canceller's fed-back outputs).
struct Delay {
    static const int SZ = 512, MASK = 511;
    double buf[SZ];
    int w = 0;
    void reset() { std::memset(buf, 0, sizeof buf); w = 0; }
    inline double read(int D) const { return buf[(w - D) & MASK]; }
    inline void write(double v) { buf[w] = v; w = (w + 1) & MASK; }
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

// Soft clipper: perfectly linear below 0.8, smooth knee above, ceiling at 1.0.
// Only engages on peaks, so normal-level audio is untouched.
static inline double softclip(double x) {
    const double t = 0.8;
    double a = std::fabs(x);
    if (a <= t) return x;
    double s = (x < 0.0) ? -1.0 : 1.0;
    return s * (t + (1.0 - t) * std::tanh((a - t) / (1.0 - t)));
}

// Knob (0..1) -> internal gains.
static inline double gWidth (float p) { return 0.90 * (double)p; }   // crosstalk cancel gain
static inline double gSurr  (float p) { return 1.40 * (double)p; }
static inline double gCenter(float p) { return 1.40 * (double)p; }
static inline double gLfe   (float p) { return 2.00 * (double)p; }
static inline double gOut   (float p) { return 2.00 * (double)p; }   // 0.5 -> 1.0 (0 dB)

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

    Delay     ringL, ringR;            // fed-back canceller outputs
    OnePoleLP xfBassL, xfBassR;        // bass-protect split in the crossfeed path
    OnePoleLP xfShadowL, xfShadowR;    // head-shadow lowpass in the crossfeed path
    OnePoleLP darkBL, darkBR;          // rear-channel darkening
    OnePoleLP lfeLP;                   // keep LFE to the sub band

    Smooth smG, smSurr, smCen, smLfe, smOut;

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

        xfBassL.setCutoff(fs, kBassProtect);  xfBassR.setCutoff(fs, kBassProtect);
        xfShadowL.setCutoff(fs, kHeadShadow); xfShadowR.setCutoff(fs, kHeadShadow);
        darkBL.setCutoff(fs, kRearDarken);    darkBR.setCutoff(fs, kRearDarken);
        lfeLP.setCutoff(fs, 120.0);

        smG.init  (fs, 30.0, gWidth (params[P_WIDTH]));
        smSurr.init(fs, 30.0, gSurr (params[P_SURR]));
        smCen.init (fs, 30.0, gCenter(params[P_CENTER]));
        smLfe.init (fs, 30.0, gLfe  (params[P_LFE]));
        smOut.init (fs, 30.0, gOut  (params[P_OUT]));
    }

    void resetState() {
        ringL.reset(); ringR.reset();
        xfBassL.reset(); xfBassR.reset();
        xfShadowL.reset(); xfShadowR.reset();
        darkBL.reset(); darkBR.reset();
        lfeLP.reset();
    }

    template <typename T>
    void run(T** in, T** out, VstInt32 n) {
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

        for (VstInt32 i = 0; i < n; ++i) {
            double g    = smG.next  (gWidth (params[P_WIDTH]));
            double surr = smSurr.next(gSurr (params[P_SURR]));
            double cen  = smCen.next (gCenter(params[P_CENTER]));
            double lfe  = smLfe.next (gLfe  (params[P_LFE]));
            double outG = smOut.next (gOut  (params[P_OUT]));

            double fl = rd(CH_FL, i),  fr = rd(CH_FR, i);
            double fc = rd(CH_FC, i),  lf = rd(CH_LFE, i);
            double bl = rd(CH_BL, i),  br = rd(CH_BR, i);
            double sl = rd(CH_SL, i),  sr = rd(CH_SR, i);

            // Rear channels are darkened (duller = "behind you" cue).
            double bld = 0.4 * bl + 0.6 * darkBL.process(bl);
            double brd = 0.4 * br + 0.6 * darkBR.process(br);

            // Spatial bus: place each source in the stereo "ear" field.
            // Fronts keep a little opposite-side bleed (0.30) so the canceller
            // has something to widen; sides are fully lateral; backs mostly
            // lateral with a touch of the far side.
            double busL = fl + 0.30 * fr + surr * (sl + 0.95 * bld + 0.10 * brd);
            double busR = fr + 0.30 * fl + surr * (sr + 0.95 * brd + 0.10 * bld);

            // ---- recursive crosstalk canceller (the out-of-speaker magic) ----
            // Read each channel's own past output, delayed; band-limit it
            // (bass-protect highpass + head-shadow lowpass) so |crossfeed| <= 1,
            // then subtract it from the OPPOSITE channel. Using past outputs
            // (D >= 1) means no algebraic loop; g*|filter| < 1 => stable.
            double dR = ringR.read(D), dL = ringL.read(D);
            double hpR = dR - xfBassL.process(dR);     // bass-protected
            double hpL = dL - xfBassR.process(dL);
            double cfL = xfShadowL.process(hpR);       // head-shadow band-limit
            double cfR = xfShadowR.process(hpL);

            double yL = busL - g * cfL;
            double yR = busR - g * cfR;
            ringL.write(yL); ringR.write(yR);

            double mk = 1.0 / (1.0 + 0.4 * g);         // level makeup for the cancel
            yL *= mk; yR *= mk;

            // ---- direct bus: center + LFE bypass the canceller ----
            double lfeF = lfe * lfeLP.process(lf);
            double cenS = cen * fc * 0.7071;
            double oL = (yL + cenS + 0.7071 * lfeF) * outG;
            double oR = (yR + cenS + 0.7071 * lfeF) * outG;

            if (out[0]) out[0][i] = (T)softclip(oL);
            if (out[1]) out[1][i] = (T)softclip(oR);

            // We fold everything into the front pair; silence the rest so the
            // surround speakers (if the device has them) don't double up.
            for (int c = 2; c < effect.numOutputs; ++c)
                if (out[c]) out[c][i] = (T)0;
        }
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
static void processReplacing(AEffect* e, float** in, float** out, VstInt32 n) {
    ((SonicStorm*)e->object)->run<float>(in, out, n);
}
static void processDoubleReplacing(AEffect* e, double** in, double** out, VstInt32 n) {
    ((SonicStorm*)e->object)->run<double>(in, out, n);
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
