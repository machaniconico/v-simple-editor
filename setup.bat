@echo off
chcp 65001 >nul 2>&1
setlocal enabledelayedexpansion
title V Simple Editor - One-Click Setup and Build
color 0A

echo.
echo  ============================================================
echo   V Simple Editor - One-Click Setup and Build
echo   This script will install everything and build the app.
echo.
echo   *** Please run as Administrator for automatic installs ***
echo  ============================================================
echo.

:: Save project directory
set "PROJECT_DIR=%~dp0"

:: ============================================================
:: Step 0 : Windows Defender exclusions (prevents build blocking)
:: ============================================================
echo [Preparing] Adding Windows Defender exclusions for build directories...
powershell -Command "Add-MpPreference -ExclusionPath 'C:\vcpkg'" >nul 2>&1
powershell -Command "Add-MpPreference -ExclusionPath '%PROJECT_DIR%build'" >nul 2>&1
powershell -Command "Add-MpPreference -ExclusionProcess 'cmake.exe'" >nul 2>&1
powershell -Command "Add-MpPreference -ExclusionProcess 'cl.exe'" >nul 2>&1
powershell -Command "Add-MpPreference -ExclusionProcess 'link.exe'" >nul 2>&1
echo   [OK] Defender exclusions set (requires admin)
echo.

:: ============================================================
:: Step 1/7 : Prerequisites (winget, Git, CMake, Visual Studio)
:: ============================================================
echo [Step 1/7] Checking and installing prerequisites...
echo.

:: ---- winget ----
echo   Checking winget...
where winget >nul 2>&1
if !errorlevel! neq 0 (
    echo   [ERROR] winget not found.
    echo   winget is required for automatic installs.
    echo   Windows 10 1809+ / Windows 11 should have it pre-installed.
    echo   https://learn.microsoft.com/en-us/windows/package-manager/winget/
    echo.
    pause
    exit /b 1
)
echo   [OK] winget available

:: ---- Git ----
echo   Checking Git...
where git >nul 2>&1
if !errorlevel! neq 0 (
    echo   [MISSING] Git not found. Installing via winget...
    winget install Git.Git -e --accept-package-agreements --accept-source-agreements
    REM Add default install path for current session
    set "PATH=!PATH!;C:\Program Files\Git\cmd"
    where git >nul 2>&1
    if !errorlevel! neq 0 (
        echo   [ERROR] Git install failed. Please install manually:
        echo     https://git-scm.com/download/win
        echo   Then re-run this script.
        pause
        exit /b 1
    )
    echo   [OK] Git installed
) else (
    echo   [OK] Git found
)

:: ---- CMake ----
echo   Checking CMake...
where cmake >nul 2>&1
if !errorlevel! neq 0 (
    echo   [MISSING] CMake not found. Installing via winget...
    winget install Kitware.CMake -e --accept-package-agreements --accept-source-agreements
    REM Add default install path for current session
    set "PATH=!PATH!;C:\Program Files\CMake\bin"
    where cmake >nul 2>&1
    if !errorlevel! neq 0 (
        echo   [ERROR] CMake install failed. Please install manually:
        echo     https://cmake.org/download/
        echo   Then re-run this script.
        pause
        exit /b 1
    )
    echo   [OK] CMake installed
) else (
    echo   [OK] CMake found
)

:: ---- Visual Studio ----
echo   Checking Visual Studio...
set "VCVARSALL="
call :find_vs
if defined VCVARSALL goto :vs_found

:: Not found — auto-install
echo   [MISSING] Visual Studio 2022/2026 not found.
echo   Installing Visual Studio 2022 Build Tools with C++ workload via winget...
echo   (This may take 10-30 minutes. A progress window will appear.)
echo.
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait" --accept-package-agreements --accept-source-agreements

:: Re-check regardless of winget exit code (it returns non-zero for "already installed")
call :find_vs
if defined VCVARSALL goto :vs_found

echo   [ERROR] Visual Studio not found after install attempt.
echo   Please install manually: https://visualstudio.microsoft.com/downloads/
echo   Select "Desktop development with C++" workload.
pause
exit /b 1

:vs_found
echo   [OK] Visual Studio: !VCVARSALL!

:: Setup MSVC environment
echo   Setting up MSVC environment...
call "!VCVARSALL!" x64 >nul 2>&1
echo   [OK] MSVC x64 environment ready
echo.

:: Skip the subroutine during normal flow
goto :vs_done

:: ---- Subroutine: find vcvarsall.bat ----
:find_vs
set "VCVARSALL="
:: Use vswhere if available (most reliable)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARSALL=%%i\VC\Auxiliary\Build\vcvarsall.bat"
    )
    if defined VCVARSALL exit /b 0
)
:: Fallback: check known paths one by one (avoids parentheses-in-for-loop issues)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    exit /b 0
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    exit /b 0
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    exit /b 0
)
set "BT22=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if exist "!BT22!" (
    set "VCVARSALL=!BT22!"
    exit /b 0
)
if exist "C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvarsall.bat"
    exit /b 0
)
if exist "C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    exit /b 0
)
set "BT26=C:\Program Files (x86)\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if exist "!BT26!" (
    set "VCVARSALL=!BT26!"
    exit /b 0
)
exit /b 1

:vs_done

:: ============================================================
:: Step 2/7 : Setup vcpkg
:: ============================================================
echo [Step 2/7] Setting up vcpkg...
set "VCPKG_ROOT=C:\vcpkg"

if exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo   [OK] vcpkg already installed at %VCPKG_ROOT%
) else (
    echo   Installing vcpkg to %VCPKG_ROOT%...
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
    if !errorlevel! neq 0 (
        echo   [ERROR] Failed to clone vcpkg.
        pause
        exit /b 1
    )
    pushd "%VCPKG_ROOT%"
    call bootstrap-vcpkg.bat -disableMetrics
    popd
    echo   [OK] vcpkg installed
)

:: Integrate vcpkg with MSBuild
"%VCPKG_ROOT%\vcpkg.exe" integrate install >nul 2>&1
echo   [OK] vcpkg integrated
echo.

:: ============================================================
:: Step 3/7 : Install libraries via vcpkg
:: ============================================================
echo [Step 3/7] Installing libraries via vcpkg (first time may take 30-60 min)...
echo   Packages: Qt6, FFmpeg, pkgconf
echo.

set "VCPKG=%VCPKG_ROOT%\vcpkg.exe"
set "TRIPLET=x64-windows"

:: Qt6
echo   [Installing] Qt6...
"%VCPKG%" install qtbase:x64-windows qtmultimedia:x64-windows qtnetworkauth:x64-windows
if !errorlevel! neq 0 (
    echo   [ERROR] Failed to install Qt6
    pause
    exit /b 1
)
echo   [OK] Qt6 ready

:: FFmpeg
echo   [Installing] FFmpeg...
"%VCPKG%" install ffmpeg[core,avformat,avcodec,swscale,swresample,avfilter,aom,amf]:x64-windows
if !errorlevel! neq 0 (
    echo   [ERROR] Failed to install FFmpeg
    pause
    exit /b 1
)
echo   [OK] FFmpeg ready

:: pkgconf
echo   [Installing] pkgconf...
"%VCPKG%" install pkgconf:x64-windows
echo   [OK] pkgconf ready

echo.

:: ============================================================
:: Step 4/7 : CMake configure
:: ============================================================
echo [Step 4/7] Configuring with CMake...
cd /d "%PROJECT_DIR%"

:: Clean old CMake cache if generator changed
if exist build\CMakeCache.txt (
    echo   Cleaning old CMake cache...
    del /q build\CMakeCache.txt >nul 2>&1
    rmdir /s /q build\CMakeFiles >nul 2>&1
)
if not exist build mkdir build

:: Detect Visual Studio generator from the actually installed version
set "CMAKE_GEN=Visual Studio 17 2022"
echo !VCVARSALL! | findstr /c:"2026" >nul 2>&1
if !errorlevel! equ 0 (
    set "CMAKE_GEN=Visual Studio 18 2026"
)

echo   Using generator: !CMAKE_GEN!

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DVEDITOR_EDITION=modern -G "!CMAKE_GEN!" -A x64

if !errorlevel! neq 0 (
    echo.
    echo   [ERROR] CMake configure failed!
    echo   Check error messages above.
    pause
    exit /b 1
)
echo   [OK] CMake configured
echo.

:: ============================================================
:: Step 5/7 : Build
:: ============================================================
echo [Step 5/7] Building (Release)...
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

:: ============================================================
:: Step 6/7 : Deploy DLLs and runtime tools
:: ============================================================
echo [Step 6/7] Deploying DLLs and runtime tools...
set "EXE_PATH="
set "EXE_DIR="
for %%d in (Release RelWithDebInfo Debug MinSizeRel) do (
    if exist "build\%%d\v-simple-editor.exe" (
        set "EXE_PATH=%PROJECT_DIR%build\%%d\v-simple-editor.exe"
        set "EXE_DIR=%PROJECT_DIR%build\%%d"
    )
)

if not defined EXE_DIR goto :deploy_done

REM --- windeployqt ---
where windeployqt >nul 2>&1
if !errorlevel! equ 0 (
    echo   Running windeployqt...
    windeployqt --release --no-translations "!EXE_PATH!" >nul 2>&1
    echo   [OK] Qt DLLs deployed
    goto :qt_deployed
)
set "WINDEPLOY=%VCPKG_ROOT%\installed\x64-windows\tools\Qt6\bin\windeployqt.exe"
if exist "!WINDEPLOY!" (
    echo   Running windeployqt from vcpkg...
    "!WINDEPLOY!" --release --no-translations "!EXE_PATH!" >nul 2>&1
    echo   [OK] Qt DLLs deployed
    goto :qt_deployed
)
echo   [WARN] windeployqt not found
:qt_deployed

REM --- FFmpeg DLLs ---
echo   Copying FFmpeg DLLs...
for %%f in (avformat avcodec avutil swscale swresample avfilter avdevice postproc) do (
    if exist "%VCPKG_ROOT%\installed\x64-windows\bin\%%f.dll" (
        copy /y "%VCPKG_ROOT%\installed\x64-windows\bin\%%f.dll" "!EXE_DIR!" >nul 2>&1
    )
)
echo   [OK] FFmpeg DLLs deployed

REM --- FFmpeg CLI tools ---
echo   Copying FFmpeg CLI tools...
set "FFMPEG_TOOLS=%VCPKG_ROOT%\installed\x64-windows\tools\ffmpeg"
if exist "!FFMPEG_TOOLS!\ffmpeg.exe" (
    copy /y "!FFMPEG_TOOLS!\ffmpeg.exe" "!EXE_DIR!" >nul 2>&1
    echo   [OK] ffmpeg.exe deployed
)
if exist "!FFMPEG_TOOLS!\ffprobe.exe" (
    copy /y "!FFMPEG_TOOLS!\ffprobe.exe" "!EXE_DIR!" >nul 2>&1
    echo   [OK] ffprobe.exe deployed
)

:deploy_done
echo.

:: ============================================================
:: Step 7/7 : Optional tools (Python, Whisper)
:: ============================================================
echo [Step 7/7] Optional tools...
echo.

REM --- Python ---
echo   Checking Python...
where python >nul 2>&1
if !errorlevel! equ 0 (
    for /f "tokens=*" %%v in ('python --version 2^>^&1') do echo   [OK] %%v found
    goto :python_done
)
set /p "INSTALL_PYTHON=   Python not found. Install for scripting features? (y/n): "
if /i not "!INSTALL_PYTHON!"=="y" (
    echo   [SKIP] Python skipped
    goto :python_done
)
echo   Installing Python via winget...
winget install Python.Python.3.12 -e --accept-package-agreements --accept-source-agreements
set "PATH=!PATH!;%LOCALAPPDATA%\Programs\Python\Python312;%LOCALAPPDATA%\Programs\Python\Python312\Scripts"
where python >nul 2>&1
if !errorlevel! neq 0 (
    echo   [WARN] Python installed but not on PATH yet. Restart terminal to use.
) else (
    echo   [OK] Python installed
)
:python_done

REM --- Whisper ---
echo   Checking Whisper...
where whisper >nul 2>&1
if !errorlevel! equ 0 (
    echo   [OK] Whisper found
    goto :whisper_done
)
where pip >nul 2>&1
if !errorlevel! neq 0 (
    echo   [SKIP] Whisper requires Python + pip
    goto :whisper_done
)
set /p "INSTALL_WHISPER=   Whisper not found. Install for AI subtitle generation? (y/n): "
if /i not "!INSTALL_WHISPER!"=="y" (
    echo   [SKIP] Whisper skipped
    goto :whisper_done
)
echo   Installing openai-whisper via pip...
pip install openai-whisper
if !errorlevel! neq 0 (
    echo   [WARN] Whisper install failed. Install later: pip install openai-whisper
) else (
    echo   [OK] Whisper installed
)
:whisper_done

echo.

:: ============================================================
:: Done!
:: ============================================================
echo.
echo  ============================================================
echo   BUILD COMPLETE!
echo  ============================================================
echo.

if not defined EXE_PATH goto :no_exe
echo   Executable: !EXE_PATH!
echo.

:: Create desktop shortcut
echo   Creating desktop shortcut...
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'V Simple Editor.lnk')); $lnk.TargetPath = '!EXE_PATH!'; $lnk.WorkingDirectory = (Split-Path '!EXE_PATH!' -Parent); $lnk.IconLocation = '!EXE_PATH!,0'; $lnk.Description = 'V Simple Editor'; $lnk.Save()" >nul 2>&1
if !errorlevel! equ 0 (
    echo   [OK] Desktop shortcut created: V Simple Editor.lnk
) else (
    echo   [WARN] Failed to create desktop shortcut
)
echo.

set /p "LAUNCH=   Launch V Simple Editor now? (y/n): "
if /i "!LAUNCH!"=="y" start "" "!EXE_PATH!"
goto :finish

:no_exe
echo   Executable should be in: build\Release\v-simple-editor.exe

:finish
echo.
echo   To rebuild after code changes:
echo     cmake --build build --config Release
echo.
echo   To update from GitHub:
echo     git pull
echo     cmake --build build --config Release
echo.
pause
