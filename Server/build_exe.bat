@echo off
setlocal
cd /d "%~dp0"
title Build CloudUpdateServer.exe

echo ============================================================
echo   Build CloudUpdateServer.exe  (single file, web UI bundled)
echo ============================================================
echo.

REM ---------- 1. locate a python interpreter ----------
set "PY="
where python >nul 2>nul
if not errorlevel 1 set "PY=python"
if not defined PY (
  where py >nul 2>nul
  if not errorlevel 1 set "PY=py"
)
if not defined PY (
  echo [ERROR] Python was not found in PATH.
  echo         Install Python 3.9+ and tick "Add python.exe to PATH".
  echo.
  pause
  exit /b 1
)
echo [1/4] Using interpreter: %PY%

REM ---------- 2. isolated build virtual environment ----------
set "VENV=%~dp0build_venv"
set "VPY=%VENV%\Scripts\python.exe"
if not exist "%VPY%" (
  echo [2/4] Creating build virtual environment ...
  "%PY%" -m venv "%VENV%"
  if errorlevel 1 (
    echo [ERROR] Could not create the virtual environment.
    echo         Check that your Python installation is complete.
    echo.
    pause
    exit /b 1
  )
) else (
  echo [2/4] Reusing existing build virtual environment.
)

"%VPY%" -m pip --version >nul 2>nul
if errorlevel 1 (
  echo       pip is missing - bootstrapping with ensurepip ...
  "%VPY%" -m ensurepip --upgrade
)

REM ---------- 3. dependencies ----------
echo [3/4] Installing build dependencies (pyinstaller, cryptography) ...
"%VPY%" -m pip install --disable-pip-version-check -q pyinstaller cryptography
if errorlevel 1 (
  echo [ERROR] Dependency installation failed. Check network / proxy settings.
  echo.
  pause
  exit /b 1
)

REM ---------- 4. build (via staging dir to avoid in-place overwrite lock) ----------
taskkill /F /IM CloudUpdateServer.exe >nul 2>nul

echo [4/4] Building with PyInstaller ...
echo.
if exist build_stage rmdir /s /q build_stage
if exist dist_stage rmdir /s /q dist_stage
"%VPY%" -m PyInstaller --noconfirm --distpath dist_stage --workpath build_stage build_exe.spec
if errorlevel 1 (
  echo.
  echo [ERROR] Build failed. Scroll up to see the PyInstaller output.
  echo.
  pause
  exit /b 1
)

REM ---------- 5. replace dist exe (kill + retry while locked) ----------
taskkill /F /IM CloudUpdateServer.exe >nul 2>nul
set "TRIES=0"
:replace_loop
set /a TRIES+=1
if %TRIES% GTR 20 goto :replace_fail
if exist dist\CloudUpdateServer.exe del /F /Q dist\CloudUpdateServer.exe 2>nul
if not exist dist\CloudUpdateServer.exe (
  if exist dist_stage\CloudUpdateServer.exe (
    move /Y dist_stage\CloudUpdateServer.exe dist\CloudUpdateServer.exe >nul 2>nul
  )
)
if not exist dist_stage\CloudUpdateServer.exe goto :replace_done
timeout /t 1 >nul
goto :replace_loop
:replace_done
rmdir /s /q build_stage 2>nul
rmdir /s /q dist_stage 2>nul
goto :build_ok

:replace_fail
echo.
echo [ERROR] Could not replace dist\CloudUpdateServer.exe - the file is locked
echo         (still running, or held by antivirus real-time scanning).
echo         Close the running server/launcher, or add a Windows Defender exclusion
echo         for this folder, then run build_exe.bat again.
echo.
pause
exit /b 1

:build_ok
echo.
echo ============================================================
echo   Build finished:  dist\CloudUpdateServer.exe
echo.
echo   To run it, double-click Start.bat
echo   (or run: dist\CloudUpdateServer.exe --config config.json serve)
echo ============================================================
echo.
pause
