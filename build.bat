@echo off
REM ============================================================
REM v-simple-editor: ワンクリックビルドスクリプト
REM
REM 使い方:
REM   build.bat                   GPU 自動判定 + 5 秒の選択タイマー
REM   build.bat --modern          AV1 対応で強制ビルド
REM   build.bat --classic         H.264/HEVC のみで強制ビルド
REM   build.bat --modern --yes    確認プロンプトをスキップ
REM
REM 必要なもの (見つからなければ案内):
REM   - Visual Studio 2022 (C++ デスクトップ開発)
REM   - CMake 3.20 以上
REM   - Git
REM ============================================================

setlocal EnableDelayedExpansion
set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

REM ------- 0. 引数解析 -------
set EDITION=auto
set USER_OVERRIDE=0
set SKIP_CONFIRM=0

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--modern" (set EDITION=modern& set USER_OVERRIDE=1& shift& goto parse_args)
if /I "%~1"=="--classic" (set EDITION=classic& set USER_OVERRIDE=1& shift& goto parse_args)
if /I "%~1"=="--auto" (set EDITION=auto& shift& goto parse_args)
if /I "%~1"=="--yes" (set SKIP_CONFIRM=1& shift& goto parse_args)
if /I "%~1"=="-y" (set SKIP_CONFIRM=1& shift& goto parse_args)
echo [ERROR] 不明な引数: %~1
echo 使い方: build.bat [--modern ^| --classic ^| --auto] [--yes]
exit /b 1

:args_done
echo.
echo ============================================================
echo  v-simple-editor build
echo ============================================================
echo.

REM ------- 1. 必須ツールのチェック -------
where cmake >nul 2>&1
if errorlevel 1 goto err_no_cmake
where git >nul 2>&1
if errorlevel 1 goto err_no_git
where cl >nul 2>&1
if errorlevel 1 echo [INFO] cl.exe は PATH 上に未検出 ^(CMake が自動検出を試みます^)

REM ------- 2. GPU 自動判定 -------
if /I not "%EDITION%"=="auto" goto skip_auto_detect
echo [STEP 1/5] GPU を判定しています...
for /f "delims=" %%E in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\detect_gpu.ps1"') do set EDITION=%%E
echo    -^> 検出結果: !EDITION!
:skip_auto_detect

REM ------- 2b. 確認プロンプト (上書き未指定 & 非サイレント時のみ) -------
if "%USER_OVERRIDE%"=="1" goto skip_confirm
if "%SKIP_CONFIRM%"=="1" goto skip_confirm

echo.
echo ============================================================
echo  Edition 選択 ^(現在: !EDITION!^)
echo ============================================================
echo    Enter   このまま !EDITION! でビルド
echo    M       Modern   AV1 対応 / 推奨 GPU: RTX30+, Arc, RX6000+, M3+
echo    C       Classic  H.264/HEVC のみ / 旧 GPU 互換
echo ------------------------------------------------------------
echo    5 秒で自動的に !EDITION! で続行します
choice /T 5 /D Y /N /C YMC >nul
set CHOICE_RC=!errorlevel!
if "!CHOICE_RC!"=="3" (set EDITION=classic& echo    -^> Classic に切替えました& goto skip_confirm)
if "!CHOICE_RC!"=="2" (set EDITION=modern& echo    -^> Modern に切替えました& goto skip_confirm)
echo    -^> !EDITION! で続行します

:skip_confirm
echo.
echo [INFO] Final edition: !EDITION!
echo.

REM ------- 3. vcpkg 自動セットアップ -------
set VCPKG_ROOT=%USERPROFILE%\.veditor-vcpkg
if exist "%VCPKG_ROOT%\vcpkg.exe" goto vcpkg_ready
echo [STEP 2/5] 初回セットアップ: vcpkg をダウンロードしています...
if exist "%VCPKG_ROOT%" goto vcpkg_clone_done
git clone --depth 1 https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
if errorlevel 1 goto err_vcpkg_clone
:vcpkg_clone_done
call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 goto err_vcpkg_bootstrap
echo    -^> vcpkg のセットアップが完了しました
echo.
goto vcpkg_done
:vcpkg_ready
echo [STEP 2/5] vcpkg は既に存在します
echo.
:vcpkg_done

REM ------- 4. FFmpeg + 依存パッケージ -------
echo [STEP 3/5] FFmpeg 等の依存ライブラリを準備しています ^(初回のみ時間がかかります^)...
if /I "!EDITION!"=="modern" goto ffmpeg_modern
set FFMPEG_PKG=ffmpeg[avcodec,avformat,avfilter,swscale,swresample,x264,x265,opus,mp3lame]:x64-windows
goto ffmpeg_install
:ffmpeg_modern
set FFMPEG_PKG=ffmpeg[avcodec,avformat,avfilter,swscale,swresample,x264,x265,opus,mp3lame,svt-av1,dav1d]:x64-windows
:ffmpeg_install
"%VCPKG_ROOT%\vcpkg.exe" install !FFMPEG_PKG! --recurse
if errorlevel 1 goto err_vcpkg_install
echo    -^> 依存ライブラリの準備が完了しました
echo.

REM ------- 5. CMake configure -------
echo [STEP 4/5] CMake で構成中...
set BUILD_DIR=build
cmake -B "%BUILD_DIR%" -S . -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DVEDITOR_EDITION=!EDITION!
if errorlevel 1 goto err_cmake
echo.

REM ------- 6. ビルド -------
echo [STEP 5/5] ビルド中...
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 goto err_build

echo.
echo ============================================================
echo  ビルド成功
echo ============================================================
echo  Edition  : !EDITION!
echo  実行ファイル: %SCRIPT_DIR%%BUILD_DIR%\Release\v-simple-editor.exe
echo ============================================================
echo.

if not exist "%SCRIPT_DIR%%BUILD_DIR%\Release\v-simple-editor.exe" goto done
explorer "%SCRIPT_DIR%%BUILD_DIR%\Release"
goto done

:err_no_cmake
echo [ERROR] CMake が見つかりません。
echo    https://cmake.org/download/ からインストールしてください。
exit /b 1

:err_no_git
echo [ERROR] Git が見つかりません。
echo    https://git-scm.com/download/win からインストールしてください。
exit /b 1

:err_vcpkg_clone
echo [ERROR] vcpkg の clone に失敗しました。
exit /b 1

:err_vcpkg_bootstrap
echo [ERROR] vcpkg bootstrap に失敗しました。
exit /b 1

:err_vcpkg_install
echo [ERROR] vcpkg install に失敗しました。
echo    ネットワーク接続と Visual Studio C++ ツールセットを確認してください。
exit /b 1

:err_cmake
echo [ERROR] CMake configure に失敗しました。
exit /b 1

:err_build
echo [ERROR] ビルドに失敗しました。
exit /b 1

:done
endlocal
exit /b 0
