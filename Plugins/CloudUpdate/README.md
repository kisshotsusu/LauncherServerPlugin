# CloudUpdate 插件（云更新与完整性检查）

UE 5.8 运行时插件，与项目中的 HotPatcher 配合使用：

- **运行时完整性检查**：从管理服务器下载文件清单（路径 / 大小 / MD5），与本地安装文件逐一对账，报告缺失、大小不符、哈希不符的文件。
- **云端修复**：把有问题的文件从服务器下载并替换回本地；下载后再次校验哈希。
- **HotPatcher 更新管理**：根据 HotPatcher 生成的 JSON（`*_PatchConfig.json` / `*_PakFilesInfo.json` / `*_Release.json`）解析更新包，下载内容 Pak / IoStore 容器 / 外部文件，挂载 Pak 并记录本地版本。
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
      CloudUpdateService.cpp    核心实现（HTTP、哈希、比对、更新）
```

## 项目设置

打开 项目设置 -> Cloud Update (云更新)：

| 配置项 | 说明 |
| --- | --- |
| 服务器地址 | 管理服务器地址，默认 `http://127.0.0.1:8710` |
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