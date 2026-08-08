@echo off
chcp 65001 >nul
title Stop CloudUpdate Server

powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -like '*run_server.py*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Host ('Stopped PID ' + $_.ProcessId) }"

echo.
echo Done.
pause
