# CloudUpdate 云端更新系统

一套面向 UE 游戏的云更新方案，包含三个部分：

| 目录 | 作用 |
| --- | --- |
| `Plugins/CloudUpdate/` | UE 运行时插件：完整性检查、HotPatcher 补丁更新、云端修复 |
| `Luncher/` | C++ 启动器（WebView2 二游风格 UI）：检查/下载/更新/修复/启动游戏/自升级 |
| `Server/` | Python 管理服务器（零第三方依赖）：版本索引、完整性清单、文件仓库、网页管理台 |

## 架构与更新流程

```
玩家机器                                   服务器 E:\SVN\LauncherServerPlugin\Server (:8710)
─────────────────────                    ─────────────────────────────
Launcher.exe（单文件）                     run_server.py / CloudUpdateServer.exe
  ├─ 首次运行自动下载 app/ui、             ├─ /api/versions 版本索引
  │  SelfUpdater、背景到 app/ 子目录       ├─ /api/version/{id} 更新描述
  ├─ 启动自检顺序：                        ├─ /api/manifest.json 完整性清单
  │  ① 启动器自身版本                      ├─ /files/packages... 文件仓库
  │  ② 运行时资源(ui/SelfUpdater)          ├─ /api/launcher/runtime 启动器资源
  │  ③ 背景序列帧                          ├─ /api/launcher/background 背景清单
  │  ④ 游戏更新                            └─ 网页管理台 /
  ├─ 整包更新 / 补丁链更新 / 完整性修复
  ├─ 版本被服务器删除/关闭时自动回滚
  └─ 启动 CodeBuild.exe

游戏内                                    Plugins/CloudUpdate
  └─ 运行时完整性检查 / HotPatcher JSON 更新 / 云端修复
```

## 快速开始（本地联调）

1. **启动服务器**：运行 `Server\启动服务器.bat`（或 `Server\build_exe.bat` 打包后运行
   `Server\dist\CloudUpdateServer.exe`），打开 http://127.0.0.1:8710/
2. **构建启动器**：`Luncher\build.bat`（自动探测 VS vcvars64）
3. **部署**：只需把 `Launcher.exe` 拷到目标机器，首次运行自动从服务器拉取
   `ui/`、`SelfUpdater.exe`、背景图等到程序目录的 `app/` 子文件夹
4. **管理台**：http://127.0.0.1:8710/ 采用左侧分类导航（总览 / 游戏版本 / 存储管理 /
   服务器管理 / 启动器管理 / API 说明），可完成版本管理、开放版本、存储与对象存储配置、
   目录选择、启动器发布、背景资源设置、服务器控制

## 组件说明

各部分的详细文档见：

- [Luncher/README.md](Luncher/README.md) —— 启动器功能、构建、配置、IPC
- [Server/README.md](Server/README.md) —— 服务器 API、管理台、开放版本、启动器/背景管理
- [Plugins/CloudUpdate/README.md](Plugins/CloudUpdate/README.md) —— UE 插件蓝图 API

### 启动器（Luncher）要点

- 原生 Win32 + WebView2 前端，无边框圆角窗口、玻璃拟态、二游风格动效
- **单文件首运行**：根目录只有 `Launcher.exe`，其余资源运行时从服务器下载到 `app/`
- **主按钮状态机**：下载游戏 → 开始更新（含数量角标）→ 启动游戏 / 关闭游戏；
  下载/检查/修复/自检进度与实时速度直接显示在主按钮上，无全屏弹窗
- **启动自检**：启动器版本 → 运行时资源 → 背景序列帧 → 游戏更新
- **整包更新自动清理旧补丁包**：`Paks` 中不属于新版本的 `*_P.pak/ucas/utoc` 与下载残留自动删除
- **版本撤销自动回滚**：服务器删除或关闭某版本后，启动器检查更新时自动删除该版本
  下载的本地文件并回退到上一可用版本（基础包整包不回滚）
- **唯一实例检测**：每 2 秒扫描游戏进程，已运行时不重复启动，按钮变为「关闭游戏」
- **背景 Canvas 播放器**：帧未就绪保持上一帧，无黑屏无闪烁；背景同步会清理清单外的旧图
- 发布：`Luncher\发布启动器.bat`（或 `publish_launcher.ps1 -SyncToServer`）自动构建并生成干净发布包

### 服务器（Server）要点

- 纯 Python 标准库，零第三方依赖；可打包为单文件 `CloudUpdateServer.exe`
- **白主题左右布局管理台**：左侧分类导航，右侧详情面板；所有服务器路径输入框
  均可通过「选择」按钮弹出目录选择器（`/api/dirs/list`）
- **对象存储**：本地磁盘 / 阿里云 OSS / 腾讯云 COS / AWS S3 / 自建 MinIO，
  管理台一键切换并「测试连接」，客户端直连对象存储下载
- **版本撤销与回滚**：删除或关闭开放版本后，客户端索引的 `revoked` 携带文件快照，
  启动器与 UE 插件自动删除本地更新文件并回退版本
- **开放版本控制**：每个版本可开关「开放下载」，客户端只看到已开放版本；
  新增基础包自动加入开放列表
- **基础包与补丁重名处理**：以整包为准，索引移除同名补丁、更新链排除整包版本，
  避免整包后重复打补丁
- **启动器历史版本**：像基础包一样按「版本文件夹」管理（`launcher_versions`），
  发布时把 `Launcher.exe` / `SelfUpdater.exe` / `ui/` 快照到 `data/launcher`
- **背景资源**：`background_dir` 指定服务器上的序列帧文件夹，内容指纹版本，
  客户端自动同步并清理旧图

### UE 插件（Plugins/CloudUpdate）要点

- 运行时完整性检查（清单逐文件 MD5 对账）+ 云端修复 + HotPatcher 补丁更新
- 检查更新时检测 `revoked`：本地版本被服务器撤销时删除对应 Pak/外部文件并回退版本号
- 项目设置中配置服务器地址、项目名、当前版本号等；支持 HotPatcher JSON 直连模式

## 背景序列帧生成（本地 ComfyUI + MiniMax H3）

启动器背景是 PNG 序列帧（按文件名排序，24fps 循环播放）。
可以用本机的 ComfyUI（http://127.0.0.1:8188）+ MiniMax H3 生成走路/动作序列：

1. **预处理首帧**（关键：防拉伸）——首帧分辨率必须与输出一致，且人物保持原比例：
   - 新建 1344×768 画布
   - 人物按原始宽高比完整放置、居中、高度占满
   - 两侧空隙用原图缩放模糊填充（不可拉伸人物）
2. **上传图片**：
   ```
   curl -X POST http://127.0.0.1:8188/upload/image -F "image=@首帧.png" -F "overwrite=true"
   ```
3. **提交 MiniMaxH3ImageToVideo 任务**：`POST /prompt`，关键节点参数：
   - `MiniMaxH3ImageToVideo`：`width=1344, height=768, length=124`（≈5.2 秒 @24fps），
     `first_frame` 接 LoadImage；提示词示例：
     “2D anime game character walking in place, full-body walk cycle animation...”
   - 采样：`BasicScheduler(steps=20)` + `SamplerCustomAdvanced` + `KSamplerSelect(res_multistep)`
   - 输出：用核心 `SaveImage` 直接把 124 帧保存为 PNG（不需要 ffmpeg）
4. **放入背景目录**：把帧重命名 `frame_0001.png ... frame_0124.png` 放到
   `Server\config.json` 的 `background_dir` 指向的文件夹，客户端重启/刷新即自动同步
5. **验证**：管理台「启动器管理」可见版本与文件数；连拍检查无黑屏、画面在动

注意：输出分辨率与首帧不一致（例如竖图直接输出 16:9）会导致人物拉伸，务必先按第 1 步预处理。

## 发布流程

1. 修改版本号（`Luncher\src\Config.h` 的 `CLOUD_LAUNCHER_VERSION`）
2. 运行 `Luncher\发布启动器.bat`（或 `publish_launcher.ps1 -Version x.y.z -SyncToServer`）
3. 发布包结构：根目录 `Launcher.exe`（客户端单文件）+ `server/`（服务器侧资源）
4. 在管理台「启动器管理」添加版本文件夹并点「发布」，客户端下次启动自检自动升级

## 注意事项

- 客户端系统要求：Windows 10/11 x64、Microsoft Edge WebView2 Runtime、
  VC++ 2015-2022 x64 运行库
- 服务器地址默认 `http://127.0.0.1:8710`；部署到其他机器时可在服务器发布
  `launcher_config.json`，客户端首次运行自动拉取
- 后台服务：管理服务器 8710 需保持运行；ComfyUI 8188 仅在生成序列帧时需要
- 大版本整包更新可能数百 MB，建议配合下载限速使用
- IoStore 容器（.ucas/.utoc）与整包更新后需重启游戏生效
