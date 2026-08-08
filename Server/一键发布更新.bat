@echo off
chcp 65001 >nul
title One-Click Publish Update
cd /d "%~dp0"

echo ============ Step 1/2: Import HotPatcher ============
call "%~dp0导入HotPatcher更新包.bat"
if errorlevel 1 (
  echo [ERROR] Import failed, aborting.
  pause
  exit /b 1
)

echo.
echo ============ Step 2/2: Regenerate manifest ============
call "%~dp0重新生成完整性清单.bat"
if errorlevel 1 (
  echo [ERROR] Manifest generation failed.
  pause
  exit /b 1
)

echo.
echo Publish finished. Opening admin page ...
start "" http://127.0.0.1:8710/
pause
