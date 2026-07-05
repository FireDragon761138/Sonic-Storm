// test_host.cpp -- minimal VST2 host to sanity-check SonicStorm.dll (7.1 -> 2.0).
// Loads the DLL, replicates Equalizer APO's editor open (the null-deref trap),
// checks param-string buffer safety, then feeds 8-channel test signals and
// verifies:
//   - output is finite and stable over a full second (recursive canceller)
//   - a CENTER-only feed comes out equally in L and R (dialog stays centered)
//   - a SIDE-only feed comes out lateralized (L != R) -> spatialization works
//   - Width=0 collapses to a plain, finite downmix
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

int main() {
    HMODULE dll = LoadLibraryA("SonicStorm.dll");
    if (!dll) { printf("FAIL: cannot load SonicStorm.dll (err %lu)\n", GetLastError()); return 1; }
    EntryProc entry = (EntryProc)GetProcAddress(dll, "VSTPluginMain");
    if (!entry) { printf("FAIL: no VSTPluginMain export\n"); return 1; }

    AEffect* fx = entry(VSTCALLBACK_master);
    if (!fx || fx->magic != kEffectMagic) { printf("FAIL: bad AEffect\n"); return 1; }
    printf("OK: loaded. numParams=%d in=%d out=%d flags=0x%x\n",
           fx->numParams, fx->numInputs, fx->numOutputs, fx->flags);
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
                                      0, 0, 500, 400, nullptr, nullptr,
                                      GetModuleHandle(nullptr), nullptr);
        VstRect* rect = (VstRect*)(intptr_t)0x1;   // garbage, like the host
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

    const int N = 48000;              // 1 second
    const double f = 500.0;
    std::vector<std::vector<float>> chin(8, std::vector<float>(N, 0.0f));
    std::vector<float> outL(N), outR(N), zero(N, 0.0f);
    std::vector<std::vector<float>> outrest(6, std::vector<float>(N, 0.0f));

    // Feed one source channel with a sine; run the whole 8->8 graph in blocks.
    auto run_with = [&](int srcChannel, float width) {
        for (auto& c : chin) std::fill(c.begin(), c.end(), 0.0f);
        if (srcChannel >= 0)
            for (int i = 0; i < N; ++i)
                chin[srcChannel][i] = 0.6f * (float)std::sin(2.0*M_PI*f*i/fs);

        fx->setParameter(fx, 0, width);   // Width
        fx->setParameter(fx, 1, 0.8f);    // Surround
        fx->setParameter(fx, 2, 0.7f);    // Center
        fx->setParameter(fx, 3, 0.5f);    // LFE
        fx->setParameter(fx, 4, 0.5f);    // Output (unity)
        fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0);
        fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0);

        for (int off = 0; off < N; off += 512) {
            int n = (off + 512 <= N) ? 512 : (N - off);
            float* bi[8]; for (int c = 0; c < 8; ++c) bi[c] = chin[c].data() + off;
            float* bo[8];
            bo[0] = outL.data() + off; bo[1] = outR.data() + off;
            for (int c = 2; c < 8; ++c) bo[c] = outrest[c-2].data() + off;
            fx->processReplacing(fx, bi, bo, n);
        }
    };
    auto finite = [&](const std::vector<float>& b) {
        for (float v : b) if (!std::isfinite(v)) return false;
        return true;
    };

    bool ok = true;

    // Case 1: CENTER only -> should be centered (L ~= R) and finite/stable.
    run_with(CH_FC, 0.5f);
    if (!finite(outL) || !finite(outR)) { printf("FAIL: non-finite (center)\n"); ok = false; }
    {
        double l = rms(outL, N/2, N), r = rms(outR, N/2, N);
        double asym = std::fabs(l - r) / (0.5*(l+r) + 1e-9);
        printf("center: L=%.4f R=%.4f  asymmetry=%.3f (expect ~0)\n", l, r, asym);
        if (l < 1e-3) { printf("FAIL: center produced no output\n"); ok = false; }
        if (asym > 0.05) { printf("FAIL: center not centered\n"); ok = false; }
    }

    // Case 2: SIDE-LEFT only -> should be lateralized (L != R), finite/stable.
    run_with(CH_SL, 0.5f);
    if (!finite(outL) || !finite(outR)) { printf("FAIL: non-finite (side)\n"); ok = false; }
    {
        double l = rms(outL, N/2, N), r = rms(outR, N/2, N);
        double asym = std::fabs(l - r) / (0.5*(l+r) + 1e-9);
        printf("sideL : L=%.4f R=%.4f  asymmetry=%.3f (expect large, L>R)\n", l, r, asym);
        if (l < 1e-3) { printf("FAIL: side produced no output\n"); ok = false; }
        if (asym < 0.20 || l < r) { printf("FAIL: side not lateralized left\n"); ok = false; }
    }

    // Case 3: Width=0 -> plain downmix, still finite and stable.
    run_with(CH_SL, 0.0f);
    if (!finite(outL) || !finite(outR)) { printf("FAIL: non-finite (width0)\n"); ok = false; }
    else printf("width0: finite/stable downmix OK\n");

    // Case 4: worst-case loudness -- all 8 channels full -> output must not
    // exceed the soft-clip ceiling (no DAC-clipping).
    {
        for (auto& c : chin) for (int i = 0; i < N; ++i)
            c[i] = 0.9f * (float)std::sin(2.0*M_PI*f*i/fs);
        fx->setParameter(fx, 0, 1.0f); fx->setParameter(fx, 1, 1.0f);
        fx->setParameter(fx, 2, 1.0f); fx->setParameter(fx, 3, 1.0f);
        fx->setParameter(fx, 4, 1.0f); // max output
        fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0);
        fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0);
        for (int off = 0; off < N; off += 512) {
            int n = (off + 512 <= N) ? 512 : (N - off);
            float* bi[8]; for (int c = 0; c < 8; ++c) bi[c] = chin[c].data() + off;
            float* bo[8]; bo[0]=outL.data()+off; bo[1]=outR.data()+off;
            for (int c=2;c<8;++c) bo[c]=outrest[c-2].data()+off;
            fx->processReplacing(fx, bi, bo, n);
        }
        double peak = 0;
        for (int i = N/2; i < N; ++i) { peak = std::max(peak, (double)std::fabs(outL[i])); peak = std::max(peak, (double)std::fabs(outR[i])); }
        printf("loud  : peak=%.4f (expect <= 1.0, soft-clipped)\n", peak);
        if (!finite(outL) || peak > 1.0001) { printf("FAIL: output clipped past ceiling\n"); ok = false; }
    }

    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0);
    FreeLibrary(dll);
    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
