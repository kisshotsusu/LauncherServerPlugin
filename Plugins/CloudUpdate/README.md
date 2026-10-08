# CloudUpdate 插件（云更新与完整性检查）

UE 5.8 运行时插件，与项目中的 HotPatcher 配合使用：

- **运行时完整性检查**：从管理服务器下载文件清单（路径 / 大小 / MD5），与本地安装文件逐一对账，报告缺失、大小不符、哈希不符的文件。
- **云端修复**：把有问题的文件从服务器下载并替换回本地；下载后再次校验哈希。
- **HotPatcher 更新管理**：根据 HotPatcher 生成的 JSON（`*_PatchConfig.json` / `*_PakFilesInfo.json` / `*_Release.json`）解析更新包，下载内容 Pak / IoStore 容器 / 外部文件，挂载 Pak 并记录本地版本。
- **版本撤销自动回滚**：`Check For Updates` 检测到本地版本出现在服务器 `revoked` 列表
  （版本被删除或关闭开放）时，自动删除该版本下载的 Pak / IoStore / 外部文件，并把本地
  版本回退到上一可用版本（基础包整包不回滚）。
- 蓝图可直接调用，见下方 API。

## 目录

```
Plugins/CloudUpdate/
  CloudUpdate.uplugin
  Source/CloudUpdate/
    Public/
      CloudUpdateTypes.h        通用结构体与枚举
      CloudUpdateSettings.h     项目设置
      CloudUpdateSubsystem.h    蓝图 API 与事件
    Private/
      CloudUpdateService.*      核心服务（URL / 版本记录 / 取消 / 回滚）
      CloudUpdateHttp.cpp       HTTP 请求与下载
      CloudUpdateIntegrity.cpp  完整性检查与修复
      CloudUpdateUpdate.cpp     更新检测、应用与撤销回滚
      CloudUpdateUtil.*         通用工具
```

## 项目设置

打开 项目设置 -> Cloud Update (云更新)：

| 配置项 | 说明 |
| --- | --- |
| 服务器地址 | 管理服务器地址，默认 `http://127.0.0.1:8710` |
| 访问令牌 | 可选；非空时随所有请求发送 `Authorization: Bearer <token>`，用于需要鉴权的服务器 |
| 自定义请求头 | 可选；键值对数组，随所有请求附加，适合 API 网关 / 鉴权等场景 |
| 项目名 | 与 `Server/config.json` 的 `project` 一致，默认 CodeBuild |
| 平台 | Windows |
| HotPatcher JSON 直连根地址 | 可选。留空走管理服务器 `/api/version/{id}`；填写后客户端直接按 `{根}/{版本}/{版本}_PatchConfig.json` 与 `_PakFilesInfo.json` 解析 |
| 本地安装根目录覆盖 | 留空自动判断：打包版用安装根目录，编辑器用项目目录 |
| 当前版本号 | 初始版本，更新成功后自动写入 `Saved/CloudUpdate/local_version.json` |
| 忽略的路径前缀 | 完整性检查跳过这些前缀（例如 `Engine/`） |
| 启动时自动检查更新 / 完整性检查 | 可选 |
| 下载后校验哈希 | 默认开启 |
| HTTP 超时 / 重试次数 | 网络参数 |

## 蓝图 API

获取子系统：`Get Cloud Update Subsystem`

- `Check Integrity(CheckMode)`：完整性检查，完成后触发 `On Integrity Check Finished`，返回 `FCloudFileIssue` 数组。
- `Repair Issues()`：修复上次检查发现的问题文件，触发 `On Repair Progress` / `On Repair Finished`。
- `Check For Updates()`：检查更新，触发 `On Update Check Finished`（含待更新版本列表）。
- `Apply Update(VersionId)`：下载并应用指定版本，触发 `On Update Progress` / `On Update Finished`。
- `Get Local Version()` / `Set Local Version(VersionId)`：读取/写入本地版本。
- `Is Busy()` / `Abort Current Task()`：任务状态与控制。
- `Get Server Url()` / `Set Server Url(Url)`：运行时切换服务器。

## 工作流程

1. 启动管理服务器（见 `Server/README.md`）。
2. 游戏运行时调用 `Check For Updates`，发现新版本。
3. 调用 `Apply Update("1.4")`：插件解析 HotPatcher JSON -> 下载 Pak / IoStore 容器 / 外部文件到 `Content/Paks` -> 立即挂载 `.pak`，IoStore 容器建议重启生效 -> 写入本地版本。
4. 如怀疑本地文件损坏，调用 `Check Integrity`，再调用 `Repair Issues` 从云端恢复。

## 说明

- 更新下载的 Pak 放入 `Content/Paks`，下次启动引擎会自动按文件名顺序挂载；插件也会在运行时立即挂载 `.pak`（仅打包版）。
- IoStore（`.ucas/.utoc`）容器下载后会提示重启游戏生效。
- 插件依赖 HotPatcherRuntime 模块，请保证 HotPatcher 插件已启用。


## 更新入口关卡与 UI（2026-10-08）

插件使用 `CanContainContent=true`。项目 Plugins/CloudUpdate 是指向源插件的符号链接，所有修改应以源目录 `E:/SVN/LauncherServerPlugin/Plugins/CloudUpdate` 为准，不要重复复制一份同名插件。

内容浏览器开启“显示插件内容”，然后进入 CloudUpdate：

- `Maps/L_CloudUpdate`：非 World Partition 的空关卡，没有第三人称角色或场景；World Settings 已指定更新入口 GameMode。
- `UI/BP_CloudUpdateGameMode`：继承 `CloudUpdateUIGameMode`，其 `UpdateWidgetClass` 已指定 `WBP_CloudUpdate`。
- `UI/WBP_CloudUpdate`：继承 `CloudUpdateScreen` 的空白 UMG 画布，用来制作自己的更新界面。
- `Source/CloudUpdate/Public/UI`、`Private/UI`：独立的运行时 UI 入口，无 UnrealEd 依赖。
- `Private/Tests`：任务生命周期回归测试，仅在开发测试构建中编译。

打开关卡并运行后，PlayerController 会自动创建 WBP_CloudUpdate、加入视口、显示鼠标并设置 UI 输入模式，退出关卡时移除控件。空白控件目前不含按钮，也不会自动请求更新服务器。原项目默认启动地图保持现有配置。

在 WBP_CloudUpdate 的 Construct 中使用 `Get Update Subsystem`：先绑定 OnUpdateCheckFinished / OnUpdateProgress / OnDownloadProgress / OnUpdateFinished，再调用 CheckForUpdates；在 Destruct 中解除自己的事件绑定（不要 Unbind All，以免移除其他控件的监听）。按钮可调用 ApplyLatestUpdate、CheckIntegrity、RepairIssues、AbortCurrentTask，并根据 IsBusy 控制按钮可用性。

`ApplyLatestUpdate` 现在沿用服务端的基础包和 updateChain 顺序，依次应用每个版本。失败或需重启时停止；OnUpdateFinished 的 VersionId 是实际应用到的版本，需重启时它不一定已达到服务器最新版本。重启并确认暂存补丁交换成功后，再调用以继续后续版本。

`QueryPendingUpdateSize` 复用同一个待更新列表，包含基础包，查询成功且无更新时 bSuccess=true、字节数和待更新数均为 0；它只广播大小查询事件。大小是服务端 totalSizeBytes 的汇总，服务器未提供大小时不能当作实际下载大小。

`ApplyBinaryPatchToBaseEx` 将结果暴露给蓝图：Success / Failed / StagedForRestart。旧 bool API 保留以兼容现有蓝图。自动合并、手动合并和更新请求通过忙碌状态避免重叠，空补丁目录视为成功。

完成事件广播前已解除对应任务的忙碌状态，允许在完成事件中直接启动下一项任务。取消更新不会再把未完成的版本写入本地版本记录；文件修复存在失败时返回失败。

打包时把 `/CloudUpdate/Maps/L_CloudUpdate` 加入“要打包的地图列表”，或者将其作为 GameDefaultMap。地图通过 GameMode 直接引用 WBP_CloudUpdate，可随地图一起 Cook。插件内容和项目内容的虚拟路径分别为 /CloudUpdate/ 与 /Game/。

### 目录检查与限制

现有根目录 `BP_ThirdPersonCharacter` 是角色示例，不属于更新 UI 核心功能，保留其原路径以免破坏已有引用。把角色示例迁入独立 Examples 目录前，应通过编辑器重命名和引用检查，而不要直接移动 uasset 文件。Binaries / Intermediate 是本地编译产物，不应作为插件源代码分发；发布插件建议使用 BuildPlugin 生成目标引擎版本的分发包，并一同提供所需 HotPatcher 依赖。

`.pending` 补丁交换仍需要发生在引擎挂载 Pak/IoStore 之前。当前 EngineSubsystem 初始化中的尽力交换晚于挂载，单纯重启不保证能交换已被占用的基础容器；可靠方案应由外部启动器在游戏进程启动前执行交换。运行时 UI 关卡不能解决这一时序问题。

测试命令：`UnrealEditor-Cmd.exe CodeBuild.uproject -unattended -NullRHI -ExecCmds="Automation RunTests CloudUpdate.Service.TaskLifecycle" -TestExit="Automation Test Queue Empty"`。
