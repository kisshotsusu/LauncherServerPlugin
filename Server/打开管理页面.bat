@echo off
cd /d "%~dp0"
title CloudUpdate Web Console

set "PORT=8710"
set "URL="

echo Checking whether the server is running on port %PORT% ...

curl -s -m 3 -o nul "http://127.0.0.1:%PORT%/api/status"
if not errorlevel 1 set "URL=http://127.0.0.1:%PORT%/"

if not defined URL (
  curl -sk -m 3 -o nul "https://127.0.0.1:%PORT%/api/status"
  if not errorlevel 1 set "URL=https://127.0.0.1:%PORT%/"
)

if defined URL goto OPEN

echo.
echo The server is NOT running yet - starting it now...
echo A new window will appear and the browser will open by itself.
echo.
start "CloudUpdate Server" "%~dp0Start.bat"
exit /b 0

:OPEN
echo Server is up. Opening %URL%
start "" "%URL%"
exit /b 0
