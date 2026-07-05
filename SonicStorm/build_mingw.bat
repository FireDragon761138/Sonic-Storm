@echo off
REM Build SonicStorm.dll (64-bit VST2) with MinGW-w64 g++.
REM Statically links libgcc/libstdc++ so the DLL has no external runtime deps.
setlocal
where g++ >nul 2>&1
if errorlevel 1 (
  echo g++ not on PATH. Add the WinLibs mingw64\bin folder to PATH, or run
  echo   set PATH=%%LOCALAPPDATA%%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin;%%PATH%%
  exit /b 1
)
g++ -O2 -shared -static -static-libgcc -static-libstdc++ ^
    -o SonicStorm.dll SonicStorm.cpp -lcomctl32 -lgdi32 -luser32
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo Built SonicStorm.dll
dumpbin /exports SonicStorm.dll 2>nul | findstr /i "VSTPluginMain" || echo (dumpbin unavailable; skipping export check)
endlocal
