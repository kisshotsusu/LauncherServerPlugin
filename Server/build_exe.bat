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

REM ---------- 4. build ----------
taskkill /F /IM CloudUpdateServer.exe >nul 2>nul

echo [4/4] Building with PyInstaller ...
echo.
"%VPY%" -m PyInstaller --noconfirm --distpath dist --workpath build build_exe.spec
if errorlevel 1 (
  echo.
  echo [ERROR] Build failed. Scroll up to see the PyInstaller output.
  echo.
  pause
  exit /b 1
)

echo.
echo ============================================================
echo   Build finished:  dist\CloudUpdateServer.exe
echo.
echo   To run it, double-click Start.bat
echo   (or run: dist\CloudUpdateServer.exe --config config.json serve)
echo ============================================================
echo.
pause
