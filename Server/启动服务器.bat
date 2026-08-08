@echo off
chcp 65001 >nul
title CloudUpdate 管理服务器
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Python not found. Please install Python 3 and add it to PATH.
  pause
  exit /b 1
)

echo Checking whether the server is already running...
curl -s -m 2 http://127.0.0.1:8710/api/status >nul 2>nul
if not errorlevel 1 (
  echo Server is already running: http://127.0.0.1:8710/
  start "" http://127.0.0.1:8710/
  pause
  exit /b 0
)

echo Starting CloudUpdate management server...
python run_server.py
echo.
echo Server stopped.
pause
