<#
  CloudUpdate 启动器发布脚本
  生成干净的发布目录：客户端只需 Launcher.exe，其余资源放到 server/ 子目录供服务器发布。

  用法：
    powershell -NoProfile -ExecutionPolicy Bypass -File publish_launcher.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File publish_launcher.ps1 -Version 1.1.0 -SkipBuild
    powershell -NoProfile -ExecutionPolicy Bypass -File publish_launcher.ps1 -SyncToServer
#>
[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$OutDir = "",
    [switch]$SkipBuild,
    [switch]$SyncToServer,
    [string]$ServerDir = "D:\test\CodeBuild\Server",
    [string]$Note = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step([string]$msg) {
    Write-Host "[发布] $msg" -ForegroundColor Cyan
}

# 1) 构建
if (-not $SkipBuild) {
    Write-Step "正在构建 Launcher.exe / SelfUpdater.exe ..."
    Push-Location $root
    try {
        & cmd.exe /c "call build.bat --nopause"
        if ($LASTEXITCODE -ne 0) { throw "build.bat 构建失败" }
    } finally {
        Pop-Location
    }
}

$exe = Join-Path $root "Launcher.exe"
$updater = Join-Path $root "SelfUpdater.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "未找到 Launcher.exe，请先构建" }
if (-not (Test-Path -LiteralPath $updater)) { throw "未找到 SelfUpdater.exe" }

# 2) 版本号：参数 > src/Config.h > 默认
if (-not $Version) {
    $configText = Get-Content -Raw -Encoding UTF8 (Join-Path $root "src\Config.h")
    if ($configText -match '#define\s+CLOUD_LAUNCHER_VERSION\s+"([^"]+)"') {
        $Version = $Matches[1]
    }
}
if (-not $Version) { $Version = "1.0.0" }

# 3) 输出目录：默认放在脚本同目录的 Release 文件夹下
if (-not $OutDir) {
    $OutDir = Join-Path $root "Release"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 4) 客户端：单文件
Write-Step "客户端：仅 Launcher.exe"
Copy-Item -LiteralPath $exe -Destination (Join-Path $OutDir "Launcher.exe") -Force

# 5) 服务器侧资源
Write-Step "服务器资源：ui / SelfUpdater / 背景 / 版本配置"
$serverOut = Join-Path $OutDir "server"
$serverUi = Join-Path $serverOut "ui"
$serverBg = Join-Path $serverOut "background"
New-Item -ItemType Directory -Force -Path $serverUi, $serverBg | Out-Null

Copy-Item -LiteralPath $exe -Destination (Join-Path $serverOut "Launcher.exe") -Force
Copy-Item -LiteralPath $updater -Destination (Join-Path $serverOut "SelfUpdater.exe") -Force
Copy-Item -Path (Join-Path $root "ui\*") -Destination $serverUi -Recurse -Force
Get-ChildItem -LiteralPath (Join-Path $root "Background") -Filter "*.png" -File -ErrorAction SilentlyContinue |
    Copy-Item -Destination $serverBg -Force

$versionJson = @{ version = $Version; url = "/files/launcher/Launcher.exe"; note = $Note } | ConvertTo-Json -Depth 4
Set-Content -Path (Join-Path $serverOut "version.json") -Value $versionJson -Encoding UTF8

$cfgJson = @{
    serverUrl         = "http://127.0.0.1:8710"
    platform          = "Windows"
    gamePath          = ""
    frameDir          = "Background"
    frameFps          = 12
    speedLimitKBps    = 0
    launcherVersion   = $Version
    autoCheckOnStart  = $true
    autoRepairOnStart = $false
} | ConvertTo-Json -Depth 4
Set-Content -Path (Join-Path $serverOut "launcher_config.json") -Value $cfgJson -Encoding UTF8

# 6) 发布说明
$readme = @"
CloudUpdate 启动器发布包 v$Version
====================================

【客户端部署】
只需把根目录的 Launcher.exe 拷到目标机器即可。
首次运行会自动连接服务器（http://127.0.0.1:8710）下载 ui/、SelfUpdater.exe、
背景图等到程序目录的 app/ 子文件夹。
系统要求：Windows 10/11 x64 + Microsoft Edge WebView2 Runtime + VC++ 2015-2022 x64 运行库。

【服务器部署】
1. 把 server/ 目录里的内容复制到服务器：Server\data\launcher\
   （Launcher.exe / SelfUpdater.exe / ui/ / version.json / launcher_config.json）
2. 背景图在 server\background\：在管理页「启动器管理」里把背景目录指向它
3. 打开管理页面（http://127.0.0.1:8710/），进入「启动器管理」：
   - 添加/更新版本文件夹（可指向本包 server\ 目录，须含 Launcher.exe）
   - 点击「发布」推送客户端升级
4. 客户端下次启动自检会自动完成升级与资源同步

【本包结构】
Launcher.exe      客户端单文件
server/           服务器侧启动器资源
  Launcher.exe        自升级源
  SelfUpdater.exe     自升级替换器
  ui/                 前端界面（首次运行拉取）
  background/         背景序列帧（配置为服务器背景目录）
  version.json        自升级版本
  launcher_config.json 客户端默认配置（首次运行拉取，本地已有配置则本地优先）
"@
Set-Content -Path (Join-Path $OutDir "发布说明.txt") -Value $readme -Encoding UTF8

# 7) 可选：直接同步到本地服务器
if ($SyncToServer) {
    if (-not (Test-Path -LiteralPath $ServerDir)) { throw "服务器目录不存在：$ServerDir" }
    $dst = Join-Path $ServerDir "data\launcher"
    New-Item -ItemType Directory -Force -Path $dst, (Join-Path $dst "ui"), (Join-Path $dst "background") | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $dst "Launcher.exe") -Force
    Copy-Item -LiteralPath $updater -Destination (Join-Path $dst "SelfUpdater.exe") -Force
    Copy-Item -Path (Join-Path $root "ui\*") -Destination (Join-Path $dst "ui") -Recurse -Force
    Copy-Item -Path (Join-Path $root "Background\*.png") -Destination (Join-Path $dst "background") -Force
    Set-Content -Path (Join-Path $dst "version.json") -Value $versionJson -Encoding UTF8
    Set-Content -Path (Join-Path $dst "launcher_config.json") -Value $cfgJson -Encoding UTF8
    Write-Step "已同步到服务器：$dst"
    Write-Host "  提示：请在管理页发布版本，并把背景目录设为 $dst\background" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "发布完成：$OutDir" -ForegroundColor Green
Write-Host "  客户端：$(Join-Path $OutDir 'Launcher.exe')"
Write-Host "  服务器：$(Join-Path $OutDir 'server')"
