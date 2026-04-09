@echo off
setlocal enabledelayedexpansion
title V Editor Simple - One-Click Setup & Build
color 0A

echo.
echo  ============================================================
echo   V Editor Simple - One-Click Setup ^& Build
echo   This script will install everything and build the app.
echo  ============================================================
echo.

:: ---- Step 0: Check for Visual Studio ----
echo [Step 0/5] Checking Visual Studio...
set VS_FOUND=0
set VCVARSALL=

for %%p in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%p (
        set VCVARSALL=%%~p
        set VS_FOUND=1
        echo   [OK] Visual Studio found: %%~p
    )
)

if !VS_FOUND! equ 0 (
    echo   [MISSING] Visual Studio 2022/2026 not found!
    echo.
    echo   Please install Visual Studio 2022 first:
    echo     https://visualstudio.microsoft.com/downloads/
    echo     Select "Desktop development with C++" workload
    echo.
    echo   Or install Build Tools via winget:
    echo     winget install Microsoft.VisualStudio.2022.BuildTools
    echo.
    pause
    exit /b 1
)

:: Setup MSVC environment
echo   Setting up MSVC environment...
call "!VCVARSALL!" x64 >nul 2>&1
echo   [OK] MSVC x64 environment ready
echo.

:: ---- Step 1: Setup vcpkg ----
echo [Step 1/5] Setting up vcpkg...
set VCPKG_ROOT=C:\vcpkg

if exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo   [OK] vcpkg already installed at %VCPKG_ROOT%
) else (
    echo   Installing vcpkg to %VCPKG_ROOT%...
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
    if !errorlevel! neq 0 (
        echo   [ERROR] Failed to clone vcpkg. Is git installed?
        echo   Install git: https://git-scm.com/download/win
        pause
        exit /b 1
    )
    cd /d "%VCPKG_ROOT%"
    call bootstrap-vcpkg.bat -disableMetrics
    cd /d "%~dp0"
    echo   [OK] vcpkg installed
)

:: Integrate vcpkg with MSBuild
"%VCPKG_ROOT%\vcpkg.exe" integrate install >nul 2>&1
echo   [OK] vcpkg integrated
echo.

:: ---- Step 2: Install dependencies ----
echo [Step 2/5] Installing dependencies via vcpkg (this may take 30-60 min first time)...
echo   Packages: qt6, ffmpeg, pkgconf
echo.

set VCPKG=%VCPKG_ROOT%\vcpkg.exe
set TRIPLET=x64-windows

:: Check if Qt6 is already installed
"%VCPKG%" list qtbase:%TRIPLET% 2>nul | findstr /i "qtbase" >nul 2>&1
if !errorlevel! equ 0 (
    echo   [OK] Qt6 already installed
) else (
    echo   [Installing] Qt6 (this is the big one, grab a coffee)...
    "%VCPKG%" install "qtbase[core,gui,widgets,opengl]:%TRIPLET%" qtmultimedia:%TRIPLET% qtnetworkauth:%TRIPLET%
    if !errorlevel! neq 0 (
        echo   [ERROR] Failed to install Qt6
        pause
        exit /b 1
    )
    echo   [OK] Qt6 installed
)

:: FFmpeg
"%VCPKG%" list ffmpeg:%TRIPLET% 2>nul | findstr /i "ffmpeg" >nul 2>&1
if !errorlevel! equ 0 (
    echo   [OK] FFmpeg already installed
) else (
    echo   [Installing] FFmpeg...
    "%VCPKG%" install ffmpeg:%TRIPLET%
    if !errorlevel! neq 0 (
        echo   [ERROR] Failed to install FFmpeg
        pause
        exit /b 1
    )
    echo   [OK] FFmpeg installed
)

:: pkgconf
"%VCPKG%" list pkgconf:%TRIPLET% 2>nul | findstr /i "pkgconf" >nul 2>&1
if !errorlevel! equ 0 (
    echo   [OK] pkgconf already installed
) else (
    echo   [Installing] pkgconf...
    "%VCPKG%" install pkgconf:%TRIPLET%
    echo   [OK] pkgconf installed
)

echo.

:: ---- Step 3: CMake configure ----
echo [Step 3/5] Configuring with CMake...
cd /d "%~dp0"

if not exist build mkdir build

cmake -B build -S . ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
    -G "Visual Studio 17 2022" -A x64

if !errorlevel! neq 0 (
    echo.
    echo   [ERROR] CMake configure failed!
    echo   Check error messages above.
    pause
    exit /b 1
)
echo   [OK] CMake configured
echo.

:: ---- Step 4: Build ----
echo [Step 4/5] Building (Release)...
cmake --build build --config Release --parallel

if !errorlevel! neq 0 (
    echo.
    echo   [ERROR] Build failed!
    echo   Check error messages above.
    pause
    exit /b 1
)
echo   [OK] Build successful!
echo.

:: ---- Step 5: Done! ----
echo [Step 5/5] Locating executable...
set EXE_PATH=
for /r build %%f in (v-editor-simple.exe) do (
    set EXE_PATH=%%f
)

echo.
echo  ============================================================
echo   BUILD COMPLETE!
echo  ============================================================
echo.
if defined EXE_PATH (
    echo   Executable: !EXE_PATH!
    echo.
    set /p LAUNCH="   Launch V Editor Simple now? (y/n): "
    if /i "!LAUNCH!"=="y" (
        start "" "!EXE_PATH!"
    )
) else (
    echo   Executable should be in: build\Release\v-editor-simple.exe
)
echo.
echo   To rebuild after code changes:
echo     cmake --build build --config Release
echo.
echo   To update from GitHub:
echo     git pull
echo     cmake --build build --config Release
echo.
pause
