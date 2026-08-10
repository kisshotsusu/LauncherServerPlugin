@echo off
chcp 65001 >nul
title CloudUpdate 服务器状态
cd /d "%~dp0"

echo Web 管理界面已移除：所有管理操作改为命令行子命令（不再使用网页）。
echo 常用命令（以打包后的 exe 为例）：
echo   dist\CloudUpdateServer.exe serve               启动 HTTP/HTTPS 服务
echo   dist\CloudUpdateServer.exe gen-cert            生成自签名证书
echo   dist\CloudUpdateServer.exe import-hotpatcher   导入 HotPatcher 更新包
echo   dist\CloudUpdateServer.exe version-delete --id 1.4   撤销（删除）版本
echo.
echo 服务器状态（若已运行）：
curl -sk -m 3 https://127.0.0.1:8710/api/status 2>nul || curl -s -m 3 http://127.0.0.1:8710/api/status 2>nul || echo   （服务器尚未启动）
pause
