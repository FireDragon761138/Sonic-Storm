@echo off
REM Build SonicStormHP.dll (64-bit VST2) with MinGW-w64 g++.
REM Statically links libgcc/libstdc++ so the DLL has no external runtime deps.
setlocal
where g++ >nul 2>&1
if errorlevel 1 (
  echo g++ not on PATH. Add the WinLibs mingw64\bin folder to PATH, or run
  echo   set PATH=%%LOCALAPPDATA%%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin;%%PATH%%
  exit /b 1
)
g++ -O2 -shared -static -static-libgcc -static-libstdc++ ^
    -o SonicStormHP.dll SonicStormHP.cpp -lcomctl32 -lgdi32 -luser32
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo Built SonicStormHP.dll
REM AVX2 build. -O3 -mavx2 -mfma is load-bearing for bit-exactness: this flag
REM set contracts the scalar filters to FMA, and the paired SIMD primitives in
REM the source carry a matching __FMA__ path. Changing these flags requires
REM re-verifying EXACT 0 with the null gate (see repo notes).
g++ -O3 -mavx2 -mfma -shared -static -static-libgcc -static-libstdc++ ^
    -o SonicStormHP_AVX2.dll SonicStormHP.cpp -lcomctl32 -lgdi32 -luser32
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo Built SonicStormHP_AVX2.dll
endlocal
