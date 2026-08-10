@echo off
cd /d "%~dp0"
title CloudUpdate Server

set "RUN=dist\CloudUpdateServer.exe"
if not exist "%RUN%" set "RUN=python run_server.py"

echo.
echo ============================================================
echo   CloudUpdate Server
echo ============================================================
echo   Starting the server...
echo   The web console will open in your browser automatically.
echo   Keep this window open. Press Ctrl+C to stop the server.
echo ============================================================
echo.

%RUN% --config config.json serve --open

echo.
echo Server stopped.
pause
