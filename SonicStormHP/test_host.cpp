// test_host.cpp -- minimal VST2 host to sanity-check SonicStormHP.dll.
// Loads the DLL, replicates Equalizer APO's editor open (the null-deref trap),
// checks param-string buffer safety, then verifies the binaural model:
//   - CENTER feed comes out exactly symmetric (L == R)
//   - SIDE-LEFT impulse: left ear leads by a plausible ITD (~0.55-0.75 ms)
//     and is louder than the right (ILD)
//   - REAR source has less 10 kHz energy at the near ear than a FRONT source
//     (pinna-flange shadow -> the front/back cue actually exists)
//   - Head knob changes the ITD in the right direction (small head = small ITD)
//   - worst case (all channels hot, all knobs max) stays soft-clipped <= 1.0
//
// Build (MinGW): g++ -O2 -o test_host.exe test_host.cpp
#include "vst2_min.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static VstIntPtr VSTCALLBACK_master(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) { return 0; }
typedef AEffect* (*EntryProc)(audioMasterCallback);

enum { CH_FL=0, CH_FR, CH_FC, CH_LFE, CH_BL, CH_BR, CH_SL, CH_SR };

static double rms(const std::vector<float>& b, int from, int to) {
    double s = 0; int n = 0;
    for (int i = from; i < to; ++i) { s += (double)b[i]*b[i]; ++n; }
    return n ? std::sqrt(s / n) : 0.0;
}
static double goertzel(const std::vector<float>& x, int from, int to, double fs, double f) {
    double w = 2.0 * M_PI * f / fs, cw = std::cos(w), coeff = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = from; i < to; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cw, im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (to - from);
}
// Index where |x| first exceeds frac of its global peak.
static int onset(const std::vector<float>& x, double frac) {
    double pk = 0;
    for (float v : x) pk = std::max(pk, (double)std::fabs(v));
    if (pk <= 0) return -1;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > frac * pk) return (int)i;
    return -1;
}

int main() {
    HMODULE dll = LoadLibraryA("SonicStormHP.dll");
    if (!dll) { printf("FAIL: cannot load SonicStormHP.dll (err %lu)\n", GetLastError()); return 1; }
    EntryProc entry = (EntryProc)GetProcAddress(dll, "VSTPluginMain");
    if (!entry) { printf("FAIL: no VSTPluginMain export\n"); return 1; }

    AEffect* fx = entry(VSTCALLBACK_master);
    if (!fx || fx->magic != kEffectMagic) { printf("FAIL: bad AEffect\n"); return 1; }
    printf("OK: loaded. numParams=%d in=%d out=%d latency=%d flags=0x%x\n",
           fx->numParams, fx->numInputs, fx->numOutputs, fx->initialDelay, fx->flags);
    if (fx->numInputs != 8 || fx->numOutputs != 8) {
        printf("FAIL: expected 8-in/8-out (7.1)\n"); return 1;
    }

    // --- param-string buffer safety (host gives only 8 bytes) ---
    {
        bool safe = true;
        struct { char buf[8]; char canary[8]; } g;
        VstInt32 ops[] = { effGetParamName, effGetParamLabel, effGetParamDisplay };
        for (VstInt32 op : ops)
            for (VstInt32 pi = 0; pi < fx->numParams; ++pi) {
                std::memset(&g, 0xAB, sizeof g);
                fx->dispatcher(fx, op, pi, 0, g.buf, 0);
                for (int k = 0; k < 8; ++k)
                    if ((unsigned char)g.canary[k] != 0xAB) safe = false;
                bool term = false;
                for (int k = 0; k < 8; ++k) if (g.buf[k] == 0) term = true;
                if (!term) safe = false;
            }
        printf("bufsafe: %s\n", safe ? "no overflow, all terminated" : "OVERFLOW/UNTERMINATED");
        if (!safe) return 1;
    }

    // --- editor check: replicate Equalizer APO's startEditing() verbatim ---
    {
        HWND parent = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPEDWINDOW,
                                      0, 0, 500, 420, nullptr, nullptr,
                                      GetModuleHandle(nullptr), nullptr);
        VstRect* rect = (VstRect*)(intptr_t)0x1;
        fx->dispatcher(fx, effEditGetRect, 0, 0, &rect, 0);
        fx->dispatcher(fx, effEditOpen, 0, 0, parent, 0);
        rect = (VstRect*)(intptr_t)0x1;
        fx->dispatcher(fx, effEditGetRect, 0, 0, &rect, 0);
        if (rect == (VstRect*)(intptr_t)0x1 || !rect) {
            printf("FAIL: effEditGetRect did not set the rect pointer\n"); return 1;
        }
        int w = rect->right - rect->left, h = rect->bottom - rect->top;
        printf("editor: rect=%dx%d\n", w, h);
        if (w <= 0 || h <= 0) { printf("FAIL: bad editor rect\n"); return 1; }
        MSG m; int pumped = 0;
        while (pumped++ < 20 && PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m); DispatchMessageA(&m);
        }
        fx->dispatcher(fx, effEditClose, 0, 0, nullptr, 0);
        if (parent) DestroyWindow(parent);
        printf("editor: opened + closed, no crash\n");
    }

    const double fs = 48000.0;
    fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, (float)fs);
    fx->dispatcher(fx, effSetBlockSize, 0, 512, nullptr, 0);
    fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0);

    const int N = 48000;
    std::vector<std::vector<float>> chin(8, std::vector<float>(N, 0.0f));
    std::vector<float> outL(N), outR(N);
    std::vector<std::vector<float>> outrest(6, std::vector<float>(N, 0.0f));

    auto setKnobs = [&](float space, float surr, float cen, float lfe, float outp, float head) {
        fx->setParameter(fx, 0, space); fx->setParameter(fx, 1, surr);
        fx->setParameter(fx, 2, cen);   fx->setParameter(fx, 3, lfe);
        fx->setParameter(fx, 4, outp);  fx->setParameter(fx, 5, head);
    };
    auto process = [&]() {
        fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0);
        fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0);
        for (int off = 0; off < N; off += 512) {
            int n = (off + 512 <= N) ? 512 : (N - off);
            float* bi[8]; for (int c = 0; c < 8; ++c) bi[c] = chin[c].data() + off;
            float* bo[8]; bo[0] = outL.data() + off; bo[1] = outR.data() + off;
            for (int c = 2; c < 8; ++c) bo[c] = outrest[c-2].data() + off;
            fx->processReplacing(fx, bi, bo, n);
        }
    };
    auto clearIn = [&]() { for (auto& c : chin) std::fill(c.begin(), c.end(), 0.0f); };
    auto sine = [&](int ch, double f, float amp) {
        for (int i = 0; i < N; ++i) chin[ch][i] = amp * (float)std::sin(2.0*M_PI*f*i/fs);
    };
    auto finite = [&](const std::vector<float>& b) {
        for (float v : b) if (!std::isfinite(v)) return false;
        return true;
    };

    bool ok = true;

    // Case 1: CENTER sine -> exactly symmetric (room is x-symmetric by design).
    clearIn(); sine(CH_FC, 500.0, 0.6f);
    setKnobs(0.5f, 0.6f, 0.7f, 0.4f, 0.5f, 0.5f);
    process();
    if (!finite(outL) || !finite(outR)) { printf("FAIL: non-finite (center)\n"); ok = false; }
    {
        double l = rms(outL, N/2, N), r = rms(outR, N/2, N);
        double asym = std::fabs(l - r) / (0.5*(l+r) + 1e-9);
        printf("center: L=%.4f R=%.4f asym=%.4f (expect ~0)\n", l, r, asym);
        if (l < 1e-3 || asym > 0.02) { printf("FAIL: center not symmetric\n"); ok = false; }
    }

    // Case 2: SIDE-LEFT impulse, anechoic -> ITD + ILD the right way round.
    int itdDefault = 0;
    {
        clearIn(); chin[CH_SL][2000] = 1.0f;
        setKnobs(0.0f, 0.8f, 0.7f, 0.4f, 0.5f, 0.5f);   // Space=0: direct path only
        process();
        int oL = onset(outL, 0.10), oR = onset(outR, 0.10);
        itdDefault = oR - oL;
        double l = rms(outL, 1900, 2400), r = rms(outR, 1900, 2400);
        printf("sideL : onsetL=%d onsetR=%d ITD=%d smp (%.0f us)  rmsL=%.4f rmsR=%.4f\n",
               oL, oR, itdDefault, itdDefault * 1e6 / fs, l, r);
        // Woodworth at 8.75 cm, source at ear axis: ~600 us => ~29 samples @48k.
        if (oL < 0 || oR < 0 || itdDefault < 18 || itdDefault > 42) {
            printf("FAIL: ITD out of range\n"); ok = false;
        }
        if (l <= r) { printf("FAIL: no ILD (left should be louder)\n"); ok = false; }
    }

    // Case 3: front/back spectral cue -- 10 kHz at the near (left) ear should be
    // weaker from BL (flange shadow + lower Batteau notch) than from FL.
    {
        double eFront, eRear;
        clearIn(); sine(CH_FL, 10000.0, 0.5f);
        setKnobs(0.0f, 1.0f, 0.7f, 0.4f, 0.5f, 0.5f);
        process();
        // FL rides at full level (not Surr), so compare each against its own
        // 1 kHz reference to isolate the spectral tilt from level differences.
        double f10 = goertzel(outL, N/2, N, fs, 10000.0);
        clearIn(); sine(CH_FL, 1000.0, 0.5f); process();
        double f1 = goertzel(outL, N/2, N, fs, 1000.0);
        eFront = f10 / (f1 + 1e-12);

        clearIn(); sine(CH_BL, 10000.0, 0.5f); process();
        double b10 = goertzel(outL, N/2, N, fs, 10000.0);
        clearIn(); sine(CH_BL, 1000.0, 0.5f); process();
        double b1 = goertzel(outL, N/2, N, fs, 1000.0);
        eRear = b10 / (b1 + 1e-12);

        double diffDb = 20.0 * std::log10((eFront + 1e-12) / (eRear + 1e-12));
        printf("f/b   : front 10k/1k=%.3f rear 10k/1k=%.3f  rear duller by %.1f dB (expect >= 3)\n",
               eFront, eRear, diffDb);
        if (!(diffDb >= 3.0)) { printf("FAIL: no front/back spectral cue\n"); ok = false; }
    }

    // Case 4: Head knob scales ITD (small head -> smaller delay).
    {
        clearIn(); chin[CH_SL][2000] = 1.0f;
        setKnobs(0.0f, 0.8f, 0.7f, 0.4f, 0.5f, 0.0f);   // smallest head
        process();
        int small = onset(outR, 0.10) - onset(outL, 0.10);
        setKnobs(0.0f, 0.8f, 0.7f, 0.4f, 0.5f, 1.0f);   // largest head
        process();
        int large = onset(outR, 0.10) - onset(outL, 0.10);
        printf("head  : ITD small=%d default=%d large=%d samples (expect increasing)\n",
               small, itdDefault, large);
        if (!(small < itdDefault && itdDefault < large)) {
            printf("FAIL: Head knob does not scale ITD monotonically\n"); ok = false;
        }
        if (!finite(outL) || !finite(outR)) { printf("FAIL: non-finite (head)\n"); ok = false; }
    }

    // Case 5: room on, everything hot -> finite, soft-clipped <= 1.0.
    {
        clearIn();
        for (int c = 0; c < 8; ++c) sine(c, 500.0 + 37.0 * c, 0.9f);
        setKnobs(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f);
        process();
        double peak = 0;
        for (int i = N/2; i < N; ++i) {
            peak = std::max(peak, (double)std::fabs(outL[i]));
            peak = std::max(peak, (double)std::fabs(outR[i]));
        }
        printf("loud  : peak=%.4f (expect <= 1.0)\n", peak);
        if (!finite(outL) || !finite(outR) || peak > 1.0001) {
            printf("FAIL: clipped past ceiling or non-finite\n"); ok = false;
        }
    }

    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0);
    FreeLibrary(dll);
    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
