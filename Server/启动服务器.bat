@echo off
chcp 65001 >nul
title CloudUpdate 管理服务器
cd /d "%~dp0"

set "RUN=python run_server.py"
if exist "dist\CloudUpdateServer.exe" set "RUN=dist\CloudUpdateServer.exe"

echo Checking whether the server is already running...
curl -sk -m 2 https://127.0.0.1:8710/api/status >nul 2>nul
if errorlevel 1 curl -s -m 2 http://127.0.0.1:8710/api/status >nul 2>nul
if not errorlevel 1 (
  echo Server is already running on port 8710.
  pause
  exit /b 0
)

echo Starting CloudUpdate management server (standalone) ...
%RUN% serve
echo.
echo Server stopped.
pause
