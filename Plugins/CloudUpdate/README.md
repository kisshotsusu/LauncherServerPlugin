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

### 启动实例（GameInstance）选择（2026-10-17）

项目设置 -> Cloud Update (云更新) -> 启动实例：

| 配置项 | 说明 |
| --- | --- |
| 启用按资源切换 GameInstance | 开启后，资源校验通过时把引擎的 GameInstance 切换为下面选择的"资源类"；否则使用"引导类"。关闭则完全使用项目/命令行配置的 GameInstance |
| 引导用 GameInstance 类 | **类选择器直接选择 UGameInstance 子类**（默认 `/Script/Engine.GameInstance`）。资源缺失、未激活、校验失败或安全模式时使用 |
| 资源激活后 GameInstance 类 | **类选择器直接选择蓝图类**（例如 `/Game/BiNanSuo/BNS_GI`）。资源校验通过后使用 |

- 两个字段都是 **`TSoftClassPtr<UGameInstance>`（软类引用）**：编辑器里是类选择器（下拉 + 资产选择），不再需要手工输入类路径；选择器只列出 `UGameInstance` 子类。
- 之所以用软引用而不是 `TSubclassOf`/硬引用：资源 GameInstance 通常位于**下载的基础包**里，硬引用会在 cook 时把 `/Game` 内容拉进 bootstrap 包，破坏分包。
- **运行时解析**：先按选中的软类同步加载；若旧 ini 里保存的是不带 `_C` 的蓝图资产路径，会先补 `_C` 再试，或按蓝图资产取 `GeneratedClass`，因此历史配置仍然兼容。
- Android 平台可在 `Config/Android/AndroidGame.ini` 覆盖（文本格式与类路径一致）：
  ```
  [/Script/CloudUpdate.CloudUpdateSettings]
  bSelectGameInstanceFromResources=True
  BootstrapGameInstanceClass=/Script/Engine.GameInstance
  ResourceGameInstanceClass=/Game/BiNanSuo/BNS_GI.BNS_GI_C
  ```
- 启动决策会写入 `Saved/CloudUpdate/<平台>/startup_gameinstance.json`（`mode`/`configuredClass`/`effectiveClass`/`reason`），
  更新面板的 `StartupStatusText` 也会显示同一摘要，便于真机排查"为什么没进资源实例"。

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
- `UI/BP_CloudUpdateGameMode`：继承 `CloudUpdateUIGameMode`，其 `UpdateWidgetClass` 已指定 `WBP_GameStart`。
- `UI/WBP_GameStart`：继承 `CloudUpdateScreen` 的更新面板，包含平台、游戏/资源版本、计划、总下载/当前文件/合并进度及操作按钮。
- `Source/CloudUpdate/Public/UI`、`Private/UI`：独立的运行时 UI 入口，无 UnrealEd 依赖。
- `Private/Tests`：任务生命周期回归测试，仅在开发测试构建中编译。

打开使用该 GameMode 的关卡并运行后，PlayerController 会自动创建 WBP_GameStart、加入视口、显示鼠标并设置 UI 输入模式，退出关卡时移除控件。用户点击“检查更新”后异步查询平台对应的完整更新计划；不会打开页面就自动下载。原项目默认启动地图保持现有配置。

CloudUpdateScreen 的 NativeConstruct / NativeDestruct 自动绑定和解除自身监听；WBP 不需重复绑定。按钮名 CheckButton（兼容 RetryButton）/ UpdateButton / CancelButton / CloseButton（兼容 ExitButton）/ RepairButton 对应 检查更新 / 更新资源 / 取消 / 关闭 / 修复。文本名 PlatformText / VersionsText / StatusText / SummaryText / DownloadText / FileProgressText / MergeText 和进度条 DownloadProgress / FileProgress / MergeProgress 可供自定义子控件复用。异步完成后才展示服务器结果，失败时可重试；需要游戏包更新或有待生效资源时禁用资源更新按钮，忙碌期间防止重复请求。未知总大小显示明确提示和循环进度，不伪造百分比；合并只有文件级进度。按钮固定在滚动内容下方，安卓使用 SafeZone。

原 PreConstruct 单版本显示与 Construct 手工拆分版本字符串的入口已断开，原函数保留作参考。更新面板不改写本地版本，只有资源更新成功或启动后完成激活才改变资源记录。

`ApplyLatestUpdate` 现在只沿用服务端 updateChain 顺序，依次应用与当前游戏版本兼容的资源补丁。游戏整包（full）交给启动器在退出游戏后安装。失败或需重启时停止；OnUpdateFinished 的 VersionId 是实际应用到的版本，需重启时它不一定已达到服务器最新版本。重启并确认暂存补丁交换成功后，再调用以继续后续版本。

`QueryPendingUpdateSize` 逐个解析兼容资源补丁的描述文件，不包含游戏整包，查询成功且无更新时 bSuccess=true、字节数和待更新数均为 0；它只广播大小查询事件。大小是实际待下载文件清单的 size 汇总；存在未知大小时返回失败状态，已知大小仍可读取。

`ApplyBinaryPatchToBaseEx` 将结果暴露给蓝图：Success / Failed / StagedForRestart。旧 bool API 保留以兼容现有蓝图。自动合并、手动合并和更新请求通过忙碌状态避免重叠，空补丁目录视为成功。

完成事件广播前已解除对应任务的忙碌状态，允许在完成事件中直接启动下一项任务。取消更新不会再把未完成的版本写入本地版本记录；文件修复存在失败时返回失败。

打包时把 `/CloudUpdate/Maps/L_CloudUpdate` 加入“要打包的地图列表”，或者将其作为 GameDefaultMap。地图通过 GameMode 直接引用 WBP_GameStart，可随地图一起 Cook。插件内容和项目内容的虚拟路径分别为 /CloudUpdate/ 与 /Game/。

### 目录检查与限制

现有根目录 `BP_ThirdPersonCharacter` 是角色示例，不属于更新 UI 核心功能，保留其原路径以免破坏已有引用。把角色示例迁入独立 Examples 目录前，应通过编辑器重命名和引用检查，而不要直接移动 uasset 文件。Binaries / Intermediate 是本地编译产物，不应作为插件源代码分发；发布插件建议使用 BuildPlugin 生成目标引擎版本的分发包，并一同提供所需 HotPatcher 依赖。

`.pending` 补丁交换仍需要发生在引擎挂载 Pak/IoStore 之前。当前 EngineSubsystem 初始化中的尽力交换晚于挂载，单纯重启不保证能交换已被占用的基础容器；可靠方案应由外部启动器在游戏进程启动前执行交换。运行时 UI 关卡不能解决这一时序问题。

测试命令：`UnrealEditor-Cmd.exe CodeBuild.uproject -unattended -NullRHI -ExecCmds="Automation RunTests CloudUpdate.Service.TaskLifecycle" -TestExit="Automation Test Queue Empty"`。


## 双版本与详细进度 API（2026-10-09）

**游戏版本**来自打包时的 Project Settings -> Description -> Project Version，或插件设置中的 GameVersionOverride。更改 C++ 并发布新游戏包时应提升该版本；资源更新永远不修改游戏版本。**资源版本**来自 Saved/CloudUpdate/<自动检测通道>/local_version.json，初始值使用 InitialResourceVersionId，留空兼容旧 CurrentVersionId。旧 GetLocalVersion / SetLocalVersion 现在明确表示资源版本；二者都不会改写游戏包版本。资源版本写入带 platform 字段的 JSON，不回写项目的初始配置。

平台配置属性 Platform 已删除。GetUpdatePlatform 自动返回 Windows、Android 或 IOS。安卓 Cook 格式不再参与 API 平台名。直接 HotPatcher 来源仍识别单一 Android_ASTC 等目录，但不会混合多个 Cook 格式。安卓/iOS 的 pak 与 IoStore 文件写入 Saved/Paks，外部文件默认写入 Saved/CloudUpdate/Files；安装包和原生二进制需要完整应用更新。

版本索引、描述和完整性检查请求自动携带相同的 platform 参数；服务器从该平台的版本链计算 gameVersion/resourceVersion，不使用项目级统一服务器版本。客户端已生效/待生效资源记录按通道隔离，明确标记为其它平台的记录不会加载。旧共用 local_version.json 只有明确携带匹配 platform 时才兼容读取；未标记平台的旧文件保留原样，不猜测它属于哪个平台，初次使用新记录时回到本构建的初始资源版本。

### 查询节点

| 节点 | 返回值 |
| --- | --- |
| GetLocalGameVersion | 当前安装的游戏包版本 |
| GetServerGameVersion | 最近一次成功查询的服务器游戏包版本 |
| GetLocalResourceVersion | 当前生效的本地资源版本 |
| GetServerResourceVersion | 服务器资源发布链最后一项的资源版本 |
| GetPendingResourceVersion | 下载完成但等待重启/启动前交换的资源版本，无等待时为空 |
| QueryUpdatePlan | 异步查询版本索引与逐版本文件清单，不下载内容文件 |
| GetUpdatePlan | 查询结果，包括是否有效、版本、资源文件数/字节数、游戏整包文件数/字节数、是否需要更新游戏或重启 |
| GetRequiredUpdateFileCount | 当前可应用资源补丁的逻辑文件数，不是版本数 |
| GetRequiredUpdateSizeBytes | 当前可应用资源补丁的待下载字节数 |

先绑定 OnUpdatePlanReady，再调用 QueryUpdatePlan。bSuccess=true 后用 GetUpdatePlan 或事件传入 Plan 读取数据。服务器版本 getter 是缓存节点，不会隐式发起请求。未查询或查询失败时检查 Plan.bValid，不能把默认 0 解读为无需更新。CheckForUpdates 也会解析清单并更新计划，然后触发原检查事件。

Plan.RequiredFileCount / RequiredBytes 只统计当前游戏版本可用的资源补丁；GamePackageFileCount / GamePackageBytes 单独提供最新游戏整包统计。bResourceUpdateRequiresGame 表示发布链存在要求其他游戏版本的补丁；不会跳过它继续应用后面的依赖补丁。bGameUpdateRequired 表示服务器有更新的游戏包。

### 下载进度

使用 GetDownloadProgress 或 OnDetailedDownloadProgress，结构为 FCloudDownloadProgressInfo：

- CompletedFiles / TotalFiles / FailedFiles：下载成功数、逻辑文件总数、失败数。
- DownloadedBytes / TotalBytes：当前会话的下载字节数和总字节数。
- FilePercent / BytePercent：文件数量百分比和字节百分比，范围 0–100。
- CurrentFile / CurrentFileBytes / CurrentFileTotalBytes / CurrentFilePercent：当前文件名、字节数、总字节数和百分比。
- FileProgress / ByteProgress / CurrentFileProgress：同一进度的 0–1 值，可直接连接 UMG ProgressBar。
- bActive / bTotalBytesKnown：是否正在执行、总大小是否已知。

字节百分比按字节加权，不按文件数平均。例如 10 字节文件已下载、90 字节文件下载到 45 字节时，文件数进度是 50%，字节进度是 55%。一键更新在不同版本之间持续累计，不从零重置。重试不重复计入逻辑下载字节；补丁失败回退整文件时重算这个逻辑文件的下载体积，预算和百分比可能调整。它不是包含重传流量的网卡流量统计。

文件大小未知时 bTotalBytesKnown=false，ByteProgress / BytePercent=-1；当前文件大小未知时 CurrentFileTotalBytes / CurrentFileProgress / CurrentFilePercent=-1。UI 应显示“不确定进度/未知大小”，不要显示成 0 字节。服务器给出 size:0 时是已知空文件，成功下载可正常到 100%。

旧 OnDownloadProgress 保留，OverallProgress 现在表示当前会话字节进度；新 UI 建议使用详细事件。旧 OnUpdateProgress 保留其单版本进度形式。

### 合并进度

使用 GetMergeProgress 或 OnMergeProgress，结构为 FCloudMergeProgressInfo：CompletedFiles / TotalFiles、SucceededFiles / FailedFiles / SkippedFiles、Progress（0–1）、Percent（0–100）、CurrentFile、bActive、bCurrentFileInProgress、bRestartRequired。

这是真实的**文件级**进度：当前后端没有单文件内部的进度回调，一个文件在处理时百分比保持不变，完成时增加一个文件。bCurrentFileInProgress=true 时可在 UI 显示“正在合并当前文件”的动画。Percent=100 只表示全部合并任务处理完毕，是否成功还应检查失败/跳过数。

资源下载流程中的补丁合并已改到后台线程，进度与完成事件回到游戏线程，不阻塞 UI。AutoMergePatches / AutoMergePatchesInPaksDir 也更新统一进度。手动 UI 合并使用 ApplyBinaryPatchAsync，返回 false 表示当前任务忙碌；完成后触发 OnBinaryPatchFinished。旧 ApplyBinaryPatchToBase / Ex 是同步高级接口，不适合在 UI 中执行大文件合并。

需要重启的更新只记录 pendingResourceVersionId，不提前提升 GetLocalResourceVersion。下次初始化，只有记录的目标文件存在、无残留 .pending 且已提供的文件哈希匹配时，才提升生效资源版本。有等待生效的版本时禁止再次应用补丁。既有启动前交换时序限制仍存在，启动器须在引擎挂载之前处理被占用容器的 .pending。

### 服务端字段

Server/versions.py 和 run_server.py 已补充平台/开放状态过滤之后的 gameVersion、resourceVersion；旧服务端没有这些字段时，客户端按 full 和资源链推导。发布资源描述时可以指定：

```json
{
  "versionId": "1.2-resource",
  "type": "patch",
  "resourceVersion": "1.2",
  "requiredGameVersion": "1.0",
  "files": [
    {"fileName":"Game.pak.patch", "targetRelativePath":"Game.pak.patch",
     "url":"/files/Game.pak.patch", "kind":"ContentPak", "binaryPatch":true,
     "size":123456, "fallbackUrl":"/files/Game.pak", "fallbackSize":987654}
  ]
}
```

requiredGameVersion 是明确的游戏包版本依赖；baseVersionId 是资源差分依赖，不能把它当作游戏版本。旧描述未提供 requiredGameVersion 时作为旧格式兼容，不推断 C++ 是否兼容；新发布请显式填写。resourceVersion 留空时兼容 versionId；发布库的 versionId 是唯一标识，游戏整包和资源发布不能使用同一个库标识覆盖彼此。

所有 size / fallbackSize 都以字节为单位。可预先判断无法合并（无实现、未启用或基础文件缺失）时，计划使用整文件回退大小；缺少 fallbackSize 时标记大小未知。

修改服务端源码后重启服务；运行打包服务端时需重新构建其可执行文件。重新生成索引可更新已有版本条目的新增元信息。

### 验证

2026-10-09：编辑器编译、Win64 Shipping 编译成功；CloudUpdate.Service 三组自动化测试通过且无测试警告；服务端 22 项测试通过。HTTP 测试自动启动随机端口的本地 fixture，验证跨版本下载、数量/字节百分比分离、合并失败回退、游戏版本不改变、资源版本推进。fixture 只使用独立测试目录，测试结束后关闭进程并删除测试文件。

### 断点续传与下载进度

下载默认保留目标旁的 `.download` 数据和 `.download.json` 元信息。网络中断、重试用尽或重新发起下载时，按实际残片长度发送 Range；有验证标记时同时发送 If-Range。清单哈希或大小变化会使旧残片失效；没有哈希时只在有服务器验证标记的情况下续传。

退出游戏后再次进入也属于"重新发起下载"：未完成的那个文件按 `.download` 残片长度续传；**已经下载并落地的文件**（或 IoStore 的 `.pending` 文件）只要大小与清单哈希匹配就直接确认完成、不再重下，避免每次进入都从第一个文件开始。无哈希时残片身份改用 URL 路径（不含主机/端口），局域网回退换掉主机后仍能续传。

严格验证 206 的 Content-Range 和大小，错误响应体不会写入残片。服务器返回 200 时安全覆盖旧片段；无效区间、416 或最终校验失败时重新下载。完整文件成功校验并安装后清理元信息，IoStore 保留原有 pending 重启生效流程。

每 0.1 秒在游戏线程报告实际接收字节，包含已有残片；校验由后台线程执行。清除无效数据或服务器要求重新下载时，进度按真实数据回退。服务端支持单段 Range、ETag、Last-Modified 和 If-Range。

2026-10-16：Android 设备 ResumeHTTP/ResumeStream 自动测试通过，覆盖普通断连、忽略 Range、错误区间、503、文件变化、重建服务、空文件、完整缓存。Windows 模块编译通过；服务端 107 项测试通过，部署 EXE 返回正确 206。IOS 共用实现，未进行 IOS 构建或设备验收。

## 局域网回退（2026-10-17）

**解决什么问题**：内网联调 / 局域网部署时，服务器地址常被配成 `127.0.0.1`、写成本机内网地址，或域名解析到了客户端自己的地址——设备访问"自己"必然失败。开启本功能后，插件会判断"服务器地址解析出的 IP 是否与本机地址相同"，相同则自动改用配置的局域网地址。

### 项目设置（项目设置 -> Cloud Update (云更新) -> 局域网回退）

| 配置项 | 说明 |
| --- | --- |
| 启用局域网回退 | 总开关，**默认关闭**。关闭时零开销，不产生 DNS / 网卡枚举 |
| 局域网服务器地址 | 回退目标，如 `http://192.168.1.10:8710`；可带路径前缀；**不写端口时沿用原 URL 的端口** |
| 编辑器内也应用 | 默认关闭。编辑器通常与服务器同机，`127.0.0.1` 本来就是对的，回退会打断联调。本工程 `Config/DefaultGame.ini` 已显式设为 **True**（本机域名会被代理解析成 fake-IP，编辑器联调也需要回退） |
| 回退判定缓存（秒） | 默认 30。DNS + 网卡枚举只在缓存未命中时执行，避免每个文件都解析一次；0 表示每次请求都重新判定 |

### 行为

- **判定规则**：把服务器 `host` 解析为 IPv4 列表，命中下列任一即认为"服务器就是本机/本机代理"：
  与本机网卡地址完全相同、回环 `127.0.0.0/8`、未指定 `0.0.0.0`、
  RFC2544 fake-IP（`198.18.0.0/15`，Clash/Mihomo 等代理默认段）、链路本地 `169.254.0.0/16`、
  与本机网卡同 `/24`。纯 IPv4 字面量直接参与比较；IPv6 不参与（回退目标请用 IPv4）。
- **连接失败兜底**：直连（DNS/连接/超时/5xx）失败时，会改用 `LanServerUrl` 再试一次；
  成功后记住该主机，后续请求（含文件下载）直接走局域网地址，不再先失败一次。
  这条兜底让"域名在局域网内不可达、但内网 IP 可达"的部署也能正常更新。
- **只替换主机与端口**，`路径`与查询串保持不变：
  `http://127.0.0.1:8710/files/a.pak?x=1` → `http://192.168.1.10:8710/files/a.pak?x=1`
- **覆盖所有请求**：清单、版本索引、版本描述、资源包下载、完整性修复。注入点是
  `FCloudUpdateService::ResolveRemoteUrl()`，因此**服务器自己返回的绝对地址**（例如描述文件 `files[].url` 写成 `127.0.0.1`）同样会被回退。
- **幂等**：地址已经是局域网地址时不再改写（避免自转发）。
- 每次判定写一条日志：`局域网回退判定 <原主机> -> <说明>`（`LogCloudUpdate`，按主机缓存命中不重复刷）。

### 蓝图 API

| 节点 | 说明 |
| --- | --- |
| `GetEffectiveServerUrl` | 实际生效的服务器地址（未回退时等于 `GetServerUrl`） |
| `IsLanFallbackActive` | 当前是否在使用局域网地址 |
| `GetLanFallbackReason` | 最近一次判定说明（含命中的本机 IP），适合打到设备日志 / 调试 UI |
| `RefreshLanFallback` | 切换网络后强制重新判定（清空缓存） |

### 验证

自动化测试 `CloudUpdate.LanFallback.Decision` 覆盖 URL 分解与回填、IPv4 字面量判定、回退决策（命中 / 不命中 / 端口继承 / 幂等 / 非法配置），全部为**纯函数、不访问网络**，可在编辑器直接跑：

```
UnrealEditor-Cmd.exe BiNanSuo.uproject -unattended -NullRHI -ExecCmds="Automation RunTests CloudUpdate.LanFallback" -TestExit="Automation Test Queue Empty"
```

2026-10-17 第 3 批（本机实测）：`www.susudesu.top` 解析为 `198.18.0.124`（本机代理 fake-IP），
`GetEffectiveServerUrl()` 返回 `http://192.168.1.10:8710`、`IsLanFallbackActive()==True`，
判定说明为"服务器地址 198.18.0.124 指向本机/本机代理"。自动化测试新增 fake-IP / 回环 / 同网段 /
"回环随机端口不改写"用例；完整 `CloudUpdate` 套件 12/12 通过。

> 回环保护：`127.0.0.1`/`localhost` 只有端口与 `LanServerUrl` 一致时才回退，
> 以免把本机随机端口的联调服务/自动化 fixture 改写到局域网地址。

## 容器集（.pak / .utoc / .ucas）一致性修复（2026-10-17）

### 背景：为什么"新 .pak + 旧容器"会直接炸

引擎把 `.pak` 当作 IoStore 的**挂载入口**：

- `FPakPlatformFile::Mount()` 只接受 `.pak`，然后自己用 `ChangeExtension(.pak → .utoc)` 去找容器
  （`IPlatformFilePak.cpp:6123-6141`）。**直接 Mount 一个 `.utoc` 既不需要也不受支持。**
- 找不到 `.utoc` 时 `IoStoreSuccess = NotFound`，随后 `Pak.SafeRelease()` —— **整个 `.pak` 都被丢弃不挂载**
  （`:6186-6203`，`GPak_AllowMissingUToc` 默认 `false`，`:231`）。

这就是"下载完成、重启后资源错误"的机制：旧逻辑里 `.utoc/.ucas` 一律暂存为 `.pending`，而 `.pak`
直接落到生效目录；下次启动引擎先挂载 Paks 目录，于是出现"新 `.pak` 找不到自己的容器"→ 该包整包
不被挂载 → 记录却显示资源版本已安装 → 切到资源 GameInstance 后加载资源崩溃。

### 本次改动

1. **`.utoc/.ucas` 的 `.pending` 语义保持不变**（没有改成"新组直接落地"）：
   IoStore 容器在运行时无法安全地原地替换（可能正被引擎读取），必须等重启——这点刻意保留。
   一致性改由"**在引擎挂载之前完成成组交换**"来保证（见下）。
2. **成组交换 + 整组回滚 + 崩溃恢复**：`FCloudBinaryMerge::FinalizePendingMerges` 现在按容器组
   （`<base>.utoc` + `<base>.ucas`）处理——
   - 交换前先把现有基础文件备份为 `<base>.swapbak`，任一步失败就把**整组**回滚回"全旧"；
   - 容器组不完整（只有 `.utoc` 没有 `.ucas`，或反之）时**拒绝交换**，保留 `.pending`，绝不制造半套容器；
   - 启动时自动恢复上次中断的交换（只有 `.swapbak` 而没有基础文件 → 先恢复；两者都在 → 清理备份）。
3. **Android 补上"挂载之前"的交换**：`BNSAndroidStorage.cpp` 的 JNI 早期钩子（`nativeConfigureBNSAndroidStorage`，
   引擎初始化之前）用 POSIX `rename` 递归交换外部存储下的 `*.pending`，并删除 0 字节残片。
   Windows 侧原本就有 `BNSWindowsStorageBootstrap.inl`（`main` 之前）；`FinalizePendingMerges` 作为兜底。
4. **启动校验增加"容器集必须成套"**：`ValidateStartupManifest` 里**非 global 的** `.utoc`/`.ucas`
   要求 `.pak`/`.utoc`/`.ucas` 三者齐全，否则判定资源不可用 → 回退 bootstrap 引导包
   （升级/修复入口仍然可达），而不是带着残缺容器切到资源 GameInstance 崩溃。
   - **例外（2026-10-17 真机修复）**：`global.utoc`/`global.ucas` 是 IoStore 的全局脚本/数据容器，
     引擎不通过 `.pak` 挂载，**不存在也不需要 `global.pak`**；只要求 `global.ucas`（如有）配 `global.utoc`。
     把 global 也按"三件套"校验会让已安装的基础包每次启动都被判为"容器集不完整"，
     GameInstance 永远回退 bootstrap（真机 `startup_gameinstance.json` 的 reason 可证）。
   - 支持 `..._s0.ucas` 这类 IoStore 分片：按去掉 `_sN` 后缀的容器名归组，不要求 `..._s0.pak`。
   `OutPaks` 仍然只收集 `.pak`（容器会随同 basename 的 `.pak` 一起被引擎挂载）；"只有版本号、没有任何文件"
   依然不算安装证据。

### 验证

- 编译：`Build.bat BiNanSuoEditor Win64 Development` → `Result: Succeeded`。
- 自动化测试（11 项 `CloudUpdate.*`）：
  `CloudUpdate.Service.IoStoreMergeRestart`（成组交换 + 回归旧断言）、`CloudUpdate.Startup.LocalResourceManifest`
  （容器集校验）、`CloudUpdate.LanFallback.Decision`、`MockHTTPIntegration`、`ResumeHTTP/ResumeStream` 等
  全部通过（`CloudUpdate.UI.ScreenStates` 的既有失败已在下一批修复，见下）。

## P1/P2 修复（2026-10-17 第 2 批）

### P1 超时按大小估算

`HttpTimeoutSeconds` 在 UE 里是**整个请求的总超时**（`HttpRequestCommon.cpp` 的 TotalTimeoutTimer），不是空闲超时。
固定 60s 会让大 pak/utoc 在慢网络下必然被掐断，而重试只有 `DownloadRetryCount` 次 ——
表现就是"进度到 100% 却报失败"。现在：

```
超时 = clamp( max(HttpTimeoutSeconds, 剩余字节 / DownloadMinBytesPerSecond), HttpTimeoutSeconds, DownloadTimeoutMaxSeconds )
```

新增：`DownloadMinBytesPerSecond`（默认 32768，0 = 不估算）、`DownloadTimeoutMaxSeconds`（默认 1800，0 = 不限）。

### P1 失败语义

失败消息不再只有"成功 N / 失败 M"，而是带上失败文件名（最多 5 个）：

`更新失败：成功 118 / 失败 2（pakchunk1-Android_ASTC.ucas、…）`

配合 `OnUpdateFinished` 的 `bRestartRequired`，UI 已区分 **失败**（红色状态）与 **需重启**
（"资源 X 已准备好，请关闭并重新打开应用使其生效"）。
说明：**没有**把某些文件降级成"可跳过"——缺 `.ucas` 就等于容器不可用，跳过会装出半套资源。

### P1 一键修复入口

- 新增 `UCloudUpdateSubsystem::RequestRepair()`：先 `CheckIntegrity(FullHash)`，发现问题自动 `RepairIssues()`
  （后者依赖同一次进程内检查的内存结果，串起来才不会用错）。
- `WBP_GameStart` 新增 `RepairButton` + `RepairLabel("修复")`，放在 `AttentionActions` 里与 检查更新/关闭 同排；
  `UCloudUpdateScreen` 按名字自动绑定，点击后状态栏显示校验/修复结果（监听 `OnRepairFinished`）。
  位置/样式可在设计师里自行调整；删掉该控件也不会报错（绑定是可选的）。

### P1 断点续传健壮性（三处）

1. **验证标记不再被清空**：206 未带 ETag/Last-Modified 时保留 `.download.json` 中原标记，
   否则下次续传没有 `If-Range`，服务器换了内容也会被追加到旧分片后面。
2. **`Content-Range: bytes 0-99/*`（总长未知）不再判为非法区间**，避免丢掉好分片并整文件重下。
3. **总大小未知时也能识别"分片已完整"**：把服务器给过的总大小记进 `.download.json` 的 `total`，
   下次启动用它判断，而不是发出 `bytes=<N>-` 触发 416。

### P2 安全模式（资源实例连续启动失败 → 回 bootstrap）

`SelectStartupGameInstance` 在切换到资源 GameInstance **之前**记一次"启动尝试"
（`Saved/CloudUpdate/<平台>/startup_attempt.json`）；`UCloudUpdateSubsystem` 启动后用 core ticker
在 `SafeModeStableSeconds`（默认 45s）后清除 —— **只有崩在稳定窗口内才累计**。同一版本连续失败达到
`SafeModeFailureThreshold`（默认 2，即第 3 次启动）时，本次强制使用 bootstrap 引导包，
保证更新/修复入口一定可见。蓝图：`IsSafeModeActive()` / `ExitSafeMode()`。

### P2 回滚记录一致性

`RollbackRevokedVersion` 之前无论文件是否删掉都 `SaveLocalVersion(TargetVersion)`；现在**有文件删不掉时
保持原记录并回报失败**（`OnRollbackFinished(bSuccess=false, …)`），避免"记录已回退、磁盘还是被撤销版本"。

### P2 控件资产名更正

`CloudUpdateUITests.inl` 与本文档原先引用 `/CloudUpdate/UI/WBP_CloudUpdate`（**该资产不存在**），
已改为真实的 `WBP_GameStart`；修正后 `CloudUpdate.UI.ScreenStates` 由失败转为通过。

### 验证（2026-10-17 第 2 批）

- `Build.bat BiNanSuoEditor Win64 Development` → `Result: Succeeded`
- `Build.bat BiNanSuo Android Development` → `Result: Succeeded`
- `Automation RunTests CloudUpdate` → **11 / 11 通过、0 失败**；新增断言覆盖：容器集不完整（有 `.utoc` 无 `.ucas`）
  被拒绝、成套容器被接受、安全模式计数读/写/清与换版本归零。
- 过程中发现并修正：`ValidateStartupManifest` 中那行 Error 级日志会被自动化框架记为测试失败，
  已改为 Warning（该条件本就安全回退 bootstrap，不是错误）。
