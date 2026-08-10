@echo off
cd /d "%~dp0"
title Stop CloudUpdate Server

echo Stopping CloudUpdate server processes ...
echo.

taskkill /F /IM CloudUpdateServer.exe >nul 2>nul
if not errorlevel 1 echo Stopped CloudUpdateServer.exe

powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -like '*run_server.py*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Host ('Stopped python PID ' + $_.ProcessId) }"

echo.
echo Done.
echo.
pause
