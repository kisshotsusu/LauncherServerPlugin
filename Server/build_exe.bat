@echo off
chcp 65001 >nul
REM 打包 CloudUpdateServer 为单文件 exe（需 Python 3.8+）
setlocal
cd /d "%~dp0"

echo [1/2] 安装依赖（pyinstaller + cryptography）...
python -m pip install --upgrade pip
python -m pip install pyinstaller cryptography
if errorlevel 1 (
    echo 依赖安装失败，请检查 Python 环境
    pause
    exit /b 1
)

echo [2/2] 打包 CloudUpdateServer.exe（单文件）...
REM 注意：传入 build_exe.spec 时不能再带 --onefile/--name/--hidden-import
REM （这些已在 spec 内定义）。否则 PyInstaller 会报 "makespec options not valid"。
python -m PyInstaller --clean --distpath dist --workpath build build_exe.spec

if exist "dist\CloudUpdateServer.exe" (
    echo.
    echo 打包完成：dist\CloudUpdateServer.exe
    echo 用法示例：
    echo   dist\CloudUpdateServer.exe --config config.json serve
    echo   dist\CloudUpdateServer.exe --config config.json gen-cert
) else (
    echo 打包失败，请查看上方日志
    pause
    exit /b 1
)
endlocal
