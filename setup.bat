@echo off
setlocal enabledelayedexpansion
title V Editor Simple - Environment Setup
color 0A

echo ============================================================
echo   V Editor Simple - Environment Check ^& Setup
echo   Required: CMake, Qt6, FFmpeg, Visual Studio (MSVC)
echo ============================================================
echo.

set MISSING=0
set INSTALL_LIST=

:: ---- Check Visual Studio / MSVC ----
echo [1/5] Checking Visual Studio / MSVC...
where cl >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK] MSVC compiler found
) else (
    :: Check if VS is installed but not in PATH
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        echo   [OK] Visual Studio 2022 Community found
        echo        Run from "Developer Command Prompt" to use cl.exe
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
        echo   [OK] Visual Studio 2022 Professional found
    ) else (
        echo   [MISSING] Visual Studio / MSVC not found
        set /a MISSING+=1
        set INSTALL_LIST=!INSTALL_LIST! vs_buildtools
    )
)
echo.

:: ---- Check CMake ----
echo [2/5] Checking CMake...
where cmake >nul 2>&1
if %errorlevel% equ 0 (
    for /f "tokens=3" %%v in ('cmake --version 2^>nul ^| findstr /i "version"') do (
        echo   [OK] CMake %%v found
    )
) else (
    echo   [MISSING] CMake not found
    set /a MISSING+=1
    set INSTALL_LIST=!INSTALL_LIST! cmake
)
echo.

:: ---- Check Qt6 ----
echo [3/5] Checking Qt6...
set QT_FOUND=0
:: Check common Qt install locations
for %%d in (
    "C:\Qt\6*"
    "C:\Qt6*"
    "%USERPROFILE%\Qt\6*"
    "D:\Qt\6*"
) do (
    if exist %%d (
        echo   [OK] Qt6 found at %%d
        set QT_FOUND=1
    )
)
if !QT_FOUND! equ 0 (
    :: Check if qmake is in PATH
    where qmake >nul 2>&1
    if !errorlevel! equ 0 (
        echo   [OK] Qt found in PATH
        set QT_FOUND=1
    ) else (
        echo   [MISSING] Qt6 not found
        set /a MISSING+=1
        set INSTALL_LIST=!INSTALL_LIST! qt6
    )
)
echo.

:: ---- Check FFmpeg libraries ----
echo [4/5] Checking FFmpeg development libraries...
set FFMPEG_FOUND=0
:: Check common locations
for %%d in (
    "C:\ffmpeg"
    "C:\ffmpeg\include\libavformat"
    "%USERPROFILE%\ffmpeg"
    "C:\vcpkg\installed\x64-windows\include\libavformat"
    "C:\tools\vcpkg\installed\x64-windows\include\libavformat"
) do (
    if exist %%d (
        echo   [OK] FFmpeg found at %%d
        set FFMPEG_FOUND=1
    )
)
:: Check vcpkg
where vcpkg >nul 2>&1
if !errorlevel! equ 0 (
    vcpkg list ffmpeg 2>nul | findstr /i "ffmpeg" >nul 2>&1
    if !errorlevel! equ 0 (
        echo   [OK] FFmpeg installed via vcpkg
        set FFMPEG_FOUND=1
    )
)
if !FFMPEG_FOUND! equ 0 (
    echo   [MISSING] FFmpeg development libraries not found
    set /a MISSING+=1
    set INSTALL_LIST=!INSTALL_LIST! ffmpeg
)
echo.

:: ---- Check pkg-config (optional but helpful) ----
echo [5/5] Checking pkg-config...
where pkg-config >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK] pkg-config found
) else (
    echo   [INFO] pkg-config not found (optional, vcpkg can replace it)
)
echo.

:: ============================================================
:: Results
:: ============================================================
echo ============================================================
if !MISSING! equ 0 (
    echo   ALL CHECKS PASSED! Ready to build.
    echo.
    echo   To build:
    echo     mkdir build ^&^& cd build
    echo     cmake .. -G "Visual Studio 17 2022"
    echo     cmake --build . --config Release
    echo ============================================================
    goto :end
)

echo   MISSING %MISSING% component(s): %INSTALL_LIST%
echo ============================================================
echo.

:: ---- Offer to install missing components ----
set /p INSTALL="Install missing components? (y/n): "
if /i not "%INSTALL%"=="y" goto :end

:: Check if winget is available
where winget >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] winget not found. Please install manually:
    goto :manual_instructions
)

:: Check if vcpkg is available for FFmpeg
where vcpkg >nul 2>&1
set VCPKG_AVAILABLE=%errorlevel%

echo.
echo Installing missing components...
echo.

:: Install Visual Studio Build Tools
echo %INSTALL_LIST% | findstr /i "vs_buildtools" >nul 2>&1
if %errorlevel% equ 0 (
    echo [Installing] Visual Studio 2022 Build Tools...
    echo   This will open the Visual Studio Installer.
    echo   Select "Desktop development with C++" workload.
    winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --add Microsoft.VisualStudio.Workload.VCTools"
    echo.
)

:: Install CMake
echo %INSTALL_LIST% | findstr /i "cmake" >nul 2>&1
if %errorlevel% equ 0 (
    echo [Installing] CMake...
    winget install Kitware.CMake
    echo   NOTE: Restart terminal after installation for PATH update.
    echo.
)

:: Install Qt6
echo %INSTALL_LIST% | findstr /i "qt6" >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] Qt6 must be installed manually:
    echo   1. Download Qt Online Installer: https://www.qt.io/download-qt-installer
    echo   2. Select Qt 6.x for MSVC 2022 64-bit
    echo   3. Required modules: Qt Widgets, Qt Multimedia, Qt OpenGL
    echo   4. Set CMAKE_PREFIX_PATH to Qt install dir
    echo.
    echo   Alternative (vcpkg):
    echo     vcpkg install qt6:x64-windows
    echo.
)

:: Install FFmpeg
echo %INSTALL_LIST% | findstr /i "ffmpeg" >nul 2>&1
if %errorlevel% equ 0 (
    if !VCPKG_AVAILABLE! equ 0 (
        echo [Installing] FFmpeg via vcpkg...
        vcpkg install ffmpeg[avformat,avcodec,avutil,swscale,swresample]:x64-windows
    ) else (
        echo [INFO] FFmpeg - recommended install via vcpkg:
        echo   1. Install vcpkg: git clone https://github.com/microsoft/vcpkg
        echo   2. Run: .\vcpkg\bootstrap-vcpkg.bat
        echo   3. Run: vcpkg install ffmpeg[avformat,avcodec,avutil,swscale,swresample]:x64-windows
        echo   4. Run: vcpkg integrate install
        echo.
        echo   Alternative: Download from https://github.com/BtbN/FFmpeg-Builds/releases
        echo   (Get ffmpeg-*-win64-gpl-shared.zip, extract to C:\ffmpeg)
    )
    echo.
)

goto :post_install

:manual_instructions
echo.
echo ---- Manual Installation Guide ----
echo.
echo 1. Visual Studio 2022:
echo    https://visualstudio.microsoft.com/downloads/
echo    Install "Desktop development with C++" workload
echo.
echo 2. CMake:
echo    https://cmake.org/download/
echo    Add to PATH during installation
echo.
echo 3. Qt6:
echo    https://www.qt.io/download-qt-installer
echo    Select Qt 6.x for MSVC 2022 64-bit
echo.
echo 4. FFmpeg (dev libraries):
echo    Option A: vcpkg install ffmpeg:x64-windows
echo    Option B: https://github.com/BtbN/FFmpeg-Builds/releases
echo.
goto :end

:post_install
echo.
echo ============================================================
echo   Installation complete! Next steps:
echo.
echo   1. Restart your terminal (for PATH updates)
echo   2. Open "Developer Command Prompt for VS 2022"
echo   3. Navigate to this project directory
echo   4. Run:
echo        mkdir build
echo        cd build
echo        cmake .. -G "Visual Studio 17 2022"
echo        cmake --build . --config Release
echo.
echo   If using vcpkg, add to cmake command:
echo     -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
echo ============================================================

:end
echo.
pause
