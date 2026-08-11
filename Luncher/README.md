# CloudUpdate 游戏启动器（WebView2 + C++ 核心）

原生 Windows 游戏启动器，采用 **WebView2（Chromium）前端 + C++ 核心** 的现代架构：
前端负责二游风格的动效界面，C++ 层负责更新、修复、启动、自升级等核心逻辑。

## 功能

- **启动游戏**：自动检测游戏目录内的首个 exe（排除启动器自身）并启动
- **大版本更新**：整包更新基础包（按基础包版本下载全部文件并替换）
- **小版本更新**：按 HotPatcher 补丁链依次下载补丁（ContentPak / IoStore / 外部文件）
- **检查更新顺序**：先检查基础包是否有新版本，再检查补丁更新
- **完整性检查与修复**：对照服务器清单，缺失/损坏文件自动下载修复
- **下载限速**：设置 KB/s，0 = 不限速
- **自定义游戏位置**：设置面板可指定游戏目录或浏览选择
- **启动自检**：启动器启动时自动检查自身版本与游戏更新；发现启动器新版本时
  自动下载并由 `SelfUpdater.exe` 替换重启，无需手动操作
- **背景资源自动更新**：启动自检同时检查服务器背景资源清单
  （`/api/launcher/background`），有新版时自动下载 PNG 序列帧到本地
  `Background/` 并应用服务器配置的 FPS，本地版本记录在 `background_version.json`
- **单文件首运行**：部署只需要 `Launcher.exe`；首次运行会连接服务器
  （`/api/launcher/runtime`）自动下载 `ui/`、`SelfUpdater.exe`、背景图等到
  程序目录下的 `app/` 子文件夹，无需手动放置任何资源文件
- **一键下载/启动**：未安装游戏时主按钮为「下载游戏」，点击可选择下载目录并自动拉取
  全部文件；已设置游戏目录时直接下载、不再重复选择目录；已安装且无更新时为
  「启动游戏」，有更新时自动切换为「开始更新」
- **整包更新自动清理旧补丁包**：升级到新的基础包整包后，会自动删除
  `Paks` 目录中不属于新版本的旧 HotPatcher 补丁包（`*_P.pak/ucas/utoc`）和下载残留；
  「完整性修复」也会同步清理，避免旧补丁与新整包冲突
- **版本撤销自动回滚**：检查更新时读取 `/api/versions` 的 `revoked` 列表，若本地版本
  已被服务器删除或关闭开放，自动删除该版本下载的 Pak / IoStore / 外部文件，并把本地
  版本回退到上一可用版本（基础包整包不回滚）；版本号同时写回 `launcher_state.json`
  与 UE 插件的 `Saved/CloudUpdate/local_version.json`；文件被占用时提示重启后再检查
- **更新链修正**：本地已是整包版本（如 1.4）时不再误列旧补丁（如 1.3）；
  存在更新整包时跳过其中已包含的补丁；全新安装直接下载最新整包
- **进度全部上主按钮**：下载/检查/修复/自检的进度都直接显示在主按钮上
  （「下载中 42% · 12.3 MB/s」「修复中 48%」…），带按钮内进度条，无任何全屏弹窗；
  操作期间工具区显示「取消」按钮
- **运行状态检测**：启动游戏后按钮变为红色「关闭游戏」，点击即结束游戏进程；
  游戏自行退出时按钮自动恢复为「启动游戏」；每 2 秒持续检测，能识别已运行的
  游戏实例（唯一实例，不会重复启动），启动器重启后也能自动接上运行中的游戏
- **刷新按钮**：状态卡右上角提供刷新按钮，可随时重新检查更新并刷新游戏安装状态
- **二游风格 UI**：全屏序列帧/动态光效背景、玻璃拟态卡片、发光主按钮、粒子氛围、
  进度条、通知 Toast、滑动设置面板，无边框圆角窗口
- **智能主按钮**：右下角主按钮随状态自动切换「下载游戏 / 开始更新 / 启动游戏」，
  未安装时为蓝色「下载游戏」，有更新时显示数量角标并以琥珀色高亮
- **修复 · 目录菜单**：修复与游戏目录选择合并为一个下拉按钮，右上角仅保留设置入口
- **原生窗口拖动**：标题栏中间区域由系统原生处理拖动，双击可最大化/还原
- **取消操作**：更新/修复过程中可随时取消
- **极简状态区**：左下角仅保留游戏版本号，无悬浮窗；下载进度显示在主按钮上，
  检查/修复/自升级的进度与取消按钮显示在忙碌提示中

## 技术栈

- **UI**：HTML / CSS / JavaScript，由 WebView2（Edge Chromium 内核）渲染
- **宿主**：C++17 / Win32（无边框窗口、DPI 感知、圆角、自绘标题栏）
- **WebView2 SDK**：已内置到 `third_party/webview2`（静态链接，无需额外安装 SDK）
- **网络**：WinHTTP + 原生 JSON 解析，无第三方运行时依赖

> 运行要求：Windows 10/11 需已安装 Microsoft Edge WebView2 Runtime
> （Win11 及新版本 Windows 10 通常已自带）。

## 目录结构

```
Luncher/
  Launcher.exe / SelfUpdater.exe    构建产物
  app/                              首次运行自动生成（ui / SelfUpdater / Background / 缓存）
  build.bat                         一键构建（自动探测 VS vcvars64）
  Launcher.vcxproj / SelfUpdater.vcxproj
  src/
    main.cpp / App.h / App.cpp      WebView2 宿主、窗口、IPC 桥
    Config.h / Config.cpp           配置读写
    Json.h / Json.cpp               极简 JSON
    Network.h / Network.cpp         WinHTTP 请求、限速下载、MD5
    UpdateManager.h / UpdateManager.cpp  更新/修复/启动游戏
    SelfUpdater.cpp                 自升级替换器
  ui/
    index.html / style.css / app.js 二游风格前端（改这里即可换皮肤）
    launcher.ico                    启动器图标
  third_party/webview2/             WebView2 SDK（内置）
  Background/frame_0001.png ...     示例序列帧（可替换）
  launcher_config.json              运行时生成
```

## 构建

```bat
cd /d E:\SVN\LauncherServerPlugin\Luncher
build.bat
```

或用 Visual Studio 打开两个 `.vcxproj` 编译。需要 VS2019/2022+ 的 C++ 桌面工作负载。

## 配置（launcher_config.json，与启动器同目录）

| 字段 | 说明 |
| --- | --- |
| `serverUrl` | 管理服务器地址，默认 `http://127.0.0.1:8710` |
| `platform` | 平台，默认 `Windows` |
| `gamePath` | 游戏安装目录；留空 = 启动器所在目录 |
| `frameDir` | 背景序列帧目录（相对启动器），默认 `Background` |
| `frameFps` | 序列帧播放帧率 |
| `speedLimitKBps` | 下载限速（KB/s），0 = 不限速 |
| `launcherVersion` | 启动器版本（自升级比较用） |
| `autoCheckOnStart` | 启动时自动检查更新 |
| `autoRepairOnStart` | 启动时自动完整性修复 |

以上均可在启动器「设置」面板中修改。

## UI 定制

所有界面都在 `ui/` 目录：

- `index.html`：页面结构（标题、状态卡、按钮、设置面板）
- `style.css`：主题、配色、动效（CSS 变量集中在 `:root`，改一处即可换主题）
- `app.js`：与 C++ 的 IPC 通信、背景轮播、粒子、交互逻辑

背景图放 `Background/`，支持任意 PNG 序列帧（按文件名排序）；
没有图片时自动降级为动态光效 + 粒子背景。

## IPC 协议（前端 ⇄ C++）

前端通过 `window.chrome.webview.postMessage()` 发送指令：

| 指令 | 说明 |
| --- | --- |
| `init` | 请求完整状态快照 |
| `check` / `apply` / `repair` | 检查更新 / 开始更新 / 完整性修复 |
| `launch` | 启动游戏 |
| `download_game` | 选择下载目录并下载游戏（未安装时主按钮触发） |
| `self_update` | 自升级（兼容保留，正常由启动自检触发） |
| `cancel` | 取消当前操作 |
| `browse` | 浏览选择游戏目录 |
| `settings_save` | 保存设置（gamePath/serverUrl/speed/frameDir/frameFps/自动选项） |
| `window_minimize` / `window_maximize` / `window_close` / `window_drag` | 窗口控制 |

C++ 推送事件：`init`、`status`、`busy`、`progress`、`pending`、`result`、`folder_selected`。

## 发布启动器新版本（自升级）

1. 修改 `src/Config.h` 中的 `CLOUD_LAUNCHER_VERSION`，重新 `build.bat`
2. 把新 `Launcher.exe` 与 `SelfUpdater.exe` 复制到服务器 `launcher/` 目录
3. 更新服务器 `launcher/version.json`：
   ```json
   { "version": "1.0.1", "url": "/files/launcher/Launcher.exe" }
   ```
4. 客户端点「自升级」即自动下载替换并重启

## 对接服务器接口

- `GET /api/versions`：版本索引（含 `baseVersions` 与 `updateChain`）
- `GET /api/versions` 的 `revoked`：被删除/关闭的补丁版本文件快照（回滚依据）
- `GET /api/version/{id}?platform=Windows`：基础包整包 / 补丁更新描述
- `GET /api/manifest.json?platform=Windows&baseVersion=1.0`：完整性清单
- `GET /files/packages/Windows/{基础包版本}/{路径}`：修复下载源
- `GET /api/launcher/version` 与 `/files/launcher/...`：自升级

本项目的本地管理服务器即 `E:\SVN\LauncherServerPlugin\Server`（Python，端口 8710），
启动器默认通过 `http://127.0.0.1:8710` 对接。

## 注意事项

- 大版本更新会整包下载（数百 MB），建议配合限速使用
- IoStore 容器（.ucas/.utoc）与基础包整包更新后需重启游戏生效
- 自升级依赖同目录 `SelfUpdater.exe`，请随启动器一起发布
- `WebView2Data/` 是 WebView2 用户数据缓存，可安全删除，下次运行自动重建
