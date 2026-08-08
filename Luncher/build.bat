@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo ============================================
echo  CloudUpdate Launcher Build
echo ============================================

set "VCVARS="
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    for /f "delims=" %%i in ('where vcvars64.bat 2^>nul') do set "VCVARS=%%i"
)

if not defined VCVARS (
    echo [ERROR] vcvars64.bat not found. Install Visual Studio C++ workload.
    pause
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to load Visual Studio environment.
    pause
    exit /b 1
)

echo Building Launcher.exe ...
cl /nologo /std:c++17 /O2 /utf-8 /EHsc /W3 /DUNICODE /D_UNICODE ^
    /I third_party\webview2\include ^
    src\main.cpp src\App.cpp src\Config.cpp src\Json.cpp src\Network.cpp src\UpdateManager.cpp ^
    /Fe:Launcher.exe ^
    /link user32.lib gdi32.lib gdiplus.lib winhttp.lib advapi32.lib shell32.lib ole32.lib ^
    shlwapi.lib comctl32.lib dwmapi.lib third_party\webview2\lib\x64\WebView2LoaderStatic.lib ^
    /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] Launcher build failed.
    pause
    exit /b 1
)

echo Building SelfUpdater.exe ...
cl /nologo /std:c++17 /O2 /utf-8 /EHsc /W3 /DUNICODE /D_UNICODE ^
    src\SelfUpdater.cpp ^
    /Fe:SelfUpdater.exe ^
    /link shell32.lib user32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] SelfUpdater build failed.
    pause
    exit /b 1
)

echo.
echo Build finished: Launcher.exe / SelfUpdater.exe
if not "%1"=="--nopause" pause
