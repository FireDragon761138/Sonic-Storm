@echo off
REM Build SonicStorm.dll (64-bit VST2) with MinGW-w64 g++.
REM Statically links libgcc/libstdc++ so the DLL has no external runtime deps.
REM
REM   build_mingw.bat   -> SonicStorm.dll
REM
REM ONE DLL. It carries both an x86-64 baseline kernel and an AVX2+FMA kernel and
REM picks between them at load from CPUID. The old hand-built SonicStorm_AVX2.dll
REM is gone -- it forced an install-time choice and shared uniqueID 'SStm' with
REM the plain build, so a host that scanned both recalled whichever came last.
REM
REM The SSE2 baseline is a supported path, not a formality: the x86-64 ABI
REM mandates SSE2, so every 64-bit Windows machine from 7 through 11 runs the
REM lane-paired filters natively with no feature check. AVX2 is the upgrade.
REM
REM -m64                : a 32-bit DLL fails to load in a 64-bit host with no
REM                       useful diagnostic, so pin the target explicitly.
REM -ffp-contract=fast  : lets the AVX2 region contract mul+add into fma. The
REM                       paired primitives carry a matching __FMA__ path, which
REM                       the target pragma enables for code inside the region.
REM -fno-math-errno     : lets sqrt/exp inline; no libm errno semantics needed.
setlocal
where g++ >nul 2>&1
if errorlevel 1 (
  echo g++ not on PATH. Add the WinLibs mingw64\bin folder to PATH, or run
  echo   set PATH=%%LOCALAPPDATA%%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin;%%PATH%%
  exit /b 1
)
set OUT=SonicStorm.dll
set CXXFLAGS=-O2 -m64 -ffp-contract=fast -fno-math-errno -Wall -Wextra -Wno-unused-parameter

g++ %CXXFLAGS% -shared -static -static-libgcc -static-libstdc++ ^
    -o %OUT% SonicStorm.cpp -lcomctl32 -lgdi32 -luser32
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo Built %OUT%

REM A GCC target region only governs code compiled INSIDE it. The moment the
REM kernel stops being inlined into the AVX2 wrapper it is emitted once at
REM baseline and both wrappers just call that copy -- a DLL that works, passes
REM every test, and contains no AVX2 whatsoever. That is silent, so fail here.
objdump -d %OUT% | findstr /i "vfmadd" >nul
if errorlevel 1 (
  echo ERROR: no FMA instructions in %OUT% -- the AVX2 kernel was not emitted.
  echo Check that run^(^) and the per-sample chain are still SSTORM_HOT
  echo ^(always_inline^), and that -ffp-contract is not set to off.
  exit /b 1
)
echo Verified: AVX2/FMA kernel present in %OUT%

g++ %CXXFLAGS% -o test_host.exe test_host.cpp
if errorlevel 1 (echo TEST HOST BUILD FAILED & exit /b 1)
echo Built test_host.exe
endlocal
