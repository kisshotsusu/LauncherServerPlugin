# CloudUpdate 多项目、多平台管理服务器

纯 Python 标准库提供管理控制台、版本分发、整包与补丁下载、完整性清单和对象存储。

## 启动

双击 `Start.bat` 启动并自动打开管理台（优先运行 `dist/CloudUpdateServer.exe`，不存在时使用 Python 源码）。保留启动窗口，在该窗口按 `Ctrl+C` 停止服务；服务运行期间可直接用浏览器访问管理台。

```bat
python run_server.py --config config.json serve
```

默认地址为 `http://127.0.0.1:8710/`。打包版本使用 `dist/CloudUpdateServer.exe --config config.json serve`。源码修改后须重新打包才能更新 EXE 内的后端；管理网页从 Server/web 加载，发布版本同时内置网页。

服务器目录只保留两个 BAT 入口：`Start.bat` 用于日常启动，`build_exe.bat` 用于打包。包导入在管理台“项目内部 → 包导入”完成，完整性清单在总览或清单管理中生成，版本开放在版本管理中操作。原有重复启动、打开网页、强制停止及导入/发布/清单快捷脚本已移除。

需要命令行操作时，可直接调用原有命令（`--platform` 按实际平台填写）：

```bat
python run_server.py --config config.json import-hotpatcher --platform Windows
python run_server.py --config config.json gen-manifest --platform Windows
```

打包环境将 `python run_server.py` 替换为 `dist/CloudUpdateServer.exe`。导入成功后再生成清单，版本是否对客户端开放仍由管理台控制。

## 配置流程

1. **项目管理 → 配置平台**：选择 Windows、Android、IOS，指定默认平台。新建项目时也能直接指定平台。
2. **服务器管理 → 存储管理**：选择项目，只配置该项目的工作空间根目录。此处不管理包来源或内部存储目录。
3. **项目内部 → 包导入**：选择平台，手动选择基础包产物目录并填写版本号，点击导入基础包。
4. 手动选择 HotPatcher 输出根目录，或包含 `<版本>_Release.json` 和 `<平台>/` 的单个版本文件夹，点击导入补丁包。来源文件保留，包复制后由工作空间管理。项目界面只展示来源与版本，不展示导入后的内部存储位置。
5. 把启动器放入 `imports/launcher/<版本>/`，导入后在启动器管理中发布。
6. 在 **版本管理** 开放需要下发的版本。空开放列表表示全部关闭；未设置开放列表表示全部开放。

修改工作空间位置会整体移动工作空间，目标必须为空，不能与当前目录相互包含。

## 工作空间结构

```text
workspace/<项目标识>/
  bases/<平台>/<版本>/              基础包文件
  patches/<平台>/<版本>/            补丁描述与 files/ 文件
  installers/<平台>/<版本>/          移动端安装包（APK）
  imports/<平台>/base/<版本>/       基础包导入入口
  imports/<平台>/apk/<版本>/        安装包导入入口
  imports/<平台>/patch/<版本>/      HotPatcher 导入入口
  imports/launcher/<版本>/          启动器导入入口
  launcher/versions/<版本>/         启动器版本源文件
  launcher/background/              背景序列帧
  data/
    versions.json                  平台更新链索引
    manifests/                     完整性清单
    revoked.json                   撤销记录
    trash/                         回收站
    launcher/                      启动器发布资源
```

只配置 `workspace_dir`，其相对路径以项目 config.json 所在目录为基准。基础包、补丁库、导入目录、启动器和数据路径均由工作空间派生；各项目、各平台互相隔离。`platform_settings` 仅保存每个平台的 `hotpatcherOrder`。网络、令牌和对象存储设置使用原有配置字段。

新项目默认使用 `Server/workspace/<项目标识>`。`Server/workspace` 仅作为容纳各项目的父目录，不能直接设为项目工作空间；项目工作空间不能互相重叠。模板启动器复制到目标项目的工作空间，下载地址使用目标项目的 URL 前缀；新建 S3 项目也使用独立的对象前缀。

## 平台请求分发

以下三种方式选择相同的平台：

```text
GET /codebuild/IOS/api/versions
GET /codebuild/api/versions?platform=IOS
GET /codebuild/api/versions   (请求头 X-Platform: IOS)
```

也支持 `/codebuild/platforms/IOS/api/versions`。选择器不一致或平台未配置时返回 HTTP 400。未指定时使用项目默认平台。所有 URL 的项目部分由 projects.json 中的项目标识决定。

| 方法 | 项目内路径 | 用途 |
| --- | --- | --- |
| GET | `/api/status` | 服务和平台状态 |
| GET | `/api/config` | 项目公共配置 |
| GET / POST | `/api/workspace` | 工作空间目录、待导入包、初始化与整体迁移 |
| POST | `/api/workspace/import` | 导入基础包、补丁或启动器 |
| GET | `/api/versions` | 当前平台开放版本及更新链；`all=1` 查看全部 |
| GET | `/api/version/<版本>` | 当前平台描述，下载链接携带项目和平台 |
| DELETE | `/api/version/<版本>` | 回收当前平台补丁 |
| GET | `/api/manifest.json?baseVersion=<版本>` | 基础包完整性清单 |
| POST | `/api/manifest/generate?baseVersion=<版本>` | 生成清单 |
| GET | `/api/files?baseVersion=<版本>&path=<目录>` | 基础包文件浏览 |
| POST | `/api/import/hotpatcher` | 当前平台导入，JSON 可指定 platform |
| POST | `/api/enabled_versions` | 当前平台开放列表，JSON `{versions:[]}` |
| POST | `/api/upload?target=version&versionId=<版本>&path=files` | 上传当前平台补丁文件 |
| GET | `/files/versions/<平台>/<版本>/files/<文件>` | 补丁文件下载 |
| GET | `/files/packages/<平台>/<基础版本>/<文件>` | 基础包文件下载 |
| POST | `/api/projects/platforms` | 保存某个项目的平台列表与默认平台，需服务器管理权限 |

工作空间导入请求示例：

```json
{"category":"base","platform":"IOS","version":"1.0","source":"D:/Builds/IOS"}
```

`category` 支持 `base`、`application`、`patch`、`launcher`（兼容 Android 的 `apk` 别名）；基础包、安装包、补丁包通过 source 选择本地来源目录；补丁导入无需填写 version。来源位置通过 import_sources 按平台和包类型保存。工作空间保存使用服务器管理权限，包导入使用项目管理权限。

对象存储支持 OSS、COS、AWS S3 和 MinIO。endpoint、region、bucket 和密钥在「对象存储」中设置；Secret 留空保持已有值。平台补丁对象键为 `versions/<平台>/<版本>/files/<文件>`。

## 验证与构建

```bat
python -m unittest discover -s tests -v
node tests/test_console.cjs
python -m compileall -q .
build_venv/Scripts/python.exe -m PyInstaller --noconfirm build_exe.spec
```

回归测试使用临时项目验证真实 HTTP 分发、平台与项目隔离、独立删除、配置路径、开放列表和下载链接。

管理台版本列表支持按版本号、依赖版本搜索，以及按已开放、已关闭、异常包筛选。“开放筛选结果”和“关闭筛选结果”只修改当前筛选命中的有效包，其他包保持原状态。保存期间锁定开放操作，失败后恢复操作按钮。

读取请求超过 20 秒会提示超时；写入操作不自动超时或重试，避免大型导入尚未完成时重复提交。写入期间连接中断时，请刷新核对结果后再操作。页面切到后台后暂停状态轮询，返回时立即检查连接。前端行为测试需要 Node.js；服务器运行仍无需 Node.js 或第三方 Python 依赖。

## 工作空间校验与导入反馈

项目包导入手动选择来源，导入期间按钮锁定，防止重复提交。来源文件保留。启动器仍从指定导入入口发现有效、未导入的版本。

迁移目标必须为目录，不允许与其它项目工作空间重叠；保存配置失败时移回原目录。工作空间内的证书路径随迁移更新。工作空间启用后不再通过平台配置接口指定独立目录。

补丁描述包含子目录中的 Pak、utoc 和 ucas，下载链接与相对路径保留目录层级。资产统计使用当前项目的 game_asset_prefixes，Diff 元数据支持放在对应版本目录内。

## v2.5 多平台资源导入

更新平台仅支持 Windows、Android、IOS；安卓的 ASTC/ETC2 等 Cook 格式属于资源格式，不再作为更新平台。插件自动使用运行平台。导入时识别单一 Android_ASTC 等产物目录，并发布到 Android；同一版本有多个 Cook 格式时必须选择一个，禁止混合资源。

基础包来源支持包含平台子目录的发布根目录。安卓 APK + Data 发布目录自动使用 Data 子树，避免错误生成 Data/Game/... 的目标路径。

**移动端资源包（base）导入规则**：
- 优先识别 `Data/` 子树（核心打包脚本输出：APK + Data + version.json）或 StagedBuilds 的 Content/Paks 树。
- 保留资源容器、引擎配置、shader、图片、视频和其他运行资源；排除安装包、原生二进制、工具和调试产物，不再使用扩展名白名单误删运行资源。
- 自动过滤发布附带物与调试产物：`DeviceLogs/`、`InsightsCaptures/`、`*_Symbols_*`、`*.rar` / `*.zip`、`*.apk` / `*.obb`、`win-x64/` 工具、安装/采集脚本（`Install_*.bat`、`Capture-*` 等）、日志与截图。
- 校验资源树：必须存在非空 pak 或成对 IoStore；utoc 与 ucas 必须在同一相对目录配对，支持数据分片；空文件、孤儿索引或数据一律拒绝。
- 仅有 APK / OBB / RAR / 日志的目录不能作为资源基础包。

**移动端安装包（apk）导入规则**：
- `category: "apk"` 导入 Android APK，存入 `installers/<平台>/<版本>/`，与资源包隔离。
- 校验：`.apk` 后缀、ZIP 魔数（PK）、包含 `AndroidManifest.xml`、ZIP 完整性。
- 基础包导入时若来源含 APK，会自动校验并提取到同版本 installers 目录。
- 多个 APK 不自动挑选；使用 APK 文件选择器明确选择具体文件。自动提取会先校验并暂存全部文件，资源发布失败时回滚同次暂存的 APK，避免留下半个导入结果。
- 下载接口：`GET /api/installers`、`GET /api/installer/<版本>`、`GET /files/installers/<平台>/<版本>/<文件>`。

补丁来源支持 HotPatcher 根目录或单版本目录。混合平台根目录中没有当前平台产物的版本会跳过，无可导入产物或空资源目录则报错；平台级 Release/PatchConfig 元数据优先于共享文件。补丁的外部资源按其 baseVersionId 查找该平台的基础版本，禁止使用最新基础版本冒充依赖版本。

支持 pak、utoc、ucas 及其 .patch 差分文件。差分与完整文件同时存在时只发布一个差分条目，完整文件作为 fallbackUrl 下发，并提供独立 fallbackSize/fallbackHash。发布的大小和 MD5 取实际文件，不使用旧 PakFilesInfo 中的过期值。IoStore 文件会自动要求重启。

CloudUpdate 对版本索引、描述文件和完整性清单增加平台一致性校验，拒绝明确标记为其它平台的响应；没有平台字段的旧单平台响应保持兼容。完整文件回退下载使用其自己的哈希，不使用差分文件哈希。

v2.5 验证：42 项服务器测试、4 项 UE CloudUpdate 自动化测试通过，插件 Win64 编辑器构建与安卓运行时代码编译通过；打包 EXE 实测 Windows/Android_ASTC 双平台导入、索引、差分/回退 HTTP 下载及 ETC2 隔离。

2026-10-13 平台整理：平台选项限定为 Windows、Android、IOS；CodeBuild 数据迁移到 workspace/codebuild，Android 与 Android_ASTC 的不同版本归入 Android。51 项服务器测试通过；Win64 编辑器构建及 Android arm64 CloudUpdate 模块定向编译通过。UE 平台测试和 4 项服务测试通过，UI.ScreenStates 因蓝图缺少 StatusText 控件失败。尚未进行移动设备打包/运行或 iOS 原生编译验证。

## 2026-10-16 导入规则复核

平台能力由后端统一下发。Windows、Android、IOS 均独立管理程序包、资源基础包和资源补丁。程序包来源分别为 Windows 完整程序目录、Android APK、iOS IPA；资源基础包统一只发布资源。Windows 启动器管理仅对 Windows 可用。

每类来源可先点击“检测来源”。对应只读接口为 `POST /api/workspace/inspect`，请求 `{category:"base"|"patch"|"application", platform, source}`，复用实际导入的识别规则，不复制文件、不保存来源。实际导入仍重新校验。

检测拒绝错平台目录、冲突的 Cook 格式、错误平台元数据、空资源与不完整容器。HotPatcher 根、单版本及平台局部 Release 均可识别；批量导入前先检测所有候选版本。导入来源不得与任何项目的已管理工作空间重叠，当前项目的 imports 子目录除外。

旧移动端基础包会显示结构检查结果；仅有 APK/IPA/OBB/压缩包的旧目录保留原文件，但不再作为有效资源基础包发布。可用同一版本号导入正确资源修复这类不合格旧包，原目录备份到项目 data/trash/invalid-bases；已经有效的版本仍禁止覆盖。旧移动端完整性缓存按资源规则重建，安装包与调试产物不再混入资源清单。

本次复核：83 项服务端测试通过，网页脚本语法检查和三个平台的实际浏览器检查通过。服务端 EXE 已重新构建并部署；未检查或修改蓝图，未进行 UE/移动设备构建或运行验证。


## v2.7 程序与资源独立版本

三个平台的工作空间布局一致，项目分别使用 workspace/<项目>/，不共享导入来源或版本目录：

- installers/<平台>/<程序版本>/：Windows 完整程序目录（含有效 EXE）、Android APK 或 iOS IPA。IPA 校验 Payload/*.app/Info.plist、可执行文件与 ZIP 完整性；不进行签名或设备安装验证。
- bases/<平台>/<资源版本>/：资源基础包，Windows 也按资源规则过滤 EXE、DLL 等程序文件，发布 pak / IoStore、shader、媒体等运行资源。
- patches/<平台>/<资源版本>/：HotPatcher 资源补丁。

程序包通过 category=application 导入，version 为程序版本，bundledResourceVersion 为内置资源基线；资源包通过 category=base 导入，version 为资源版本，requiredGameVersion 为可选程序依赖。两种版本不要求相同。同号包可共存，管理页可以按程序包/资源包筛选并独立开放、关闭、查看和删除。

客户端 /api/versions 同时返回 gameVersion、resourceVersion、gameVersions、resourceVersions。每条记录带 packageKind（installer/base/patch）及明确的描述 URL，type 为 full（程序）、resource（资源基础包）或 patch（资源补丁）。客户端必须使用索引里的 URL，避免相同版本号取错包；/api/version/<版本>?kind=installer|base|patch 可精确获取。旧的纯版本号开放列表保持兼容，新管理页使用 installer:<版本>、base:<版本>、patch:<版本> 独立保存。

云端文件通过 kind=installer 或 base 分别查看程序文件与资源文件，Windows 程序目录可按子目录浏览。对象存储程序键为 installers/<平台>/<程序版本>/<相对文件>，资源键为 packages/<平台>/<资源版本>/<相对文件>；导入先上传，上传失败不发布本地版本。

DELETE /api/version/<版本>?platform=<平台>&kind=<类型> 仅移除指定包。项目工作空间内的包移入 data/trash/<平台>/<类型>/；外部旧基础包只解除配置，不移动原始来源。同号的其它包不受影响。不合格旧资源和没有文件的旧完整包描述在管理页显示为异常，供修复或删除，不作为有效客户端更新。

UE 插件将程序更新与资源更新分开统计，运行时仅安装兼容资源。Windows 启动器也独立判断程序版本与资源基线，使用带类型的描述 URL，按插件的 schemaVersion=2、installedGameVersion、resourceVersionId 和平台版本记录文件同步状态，避免引擎内更新资源后再次错误提示更新程序。


v2.7 验证：101 项服务端测试、5 项 UE 服务自动化测试、Windows 启动器同号程序/资源真实 HTTP 下载回归通过。Win64 插件、Android arm64 模块与 Windows 启动器构建通过；部署 EXE 与实际接口、三平台浏览器入口已核对。未修改蓝图；未进行移动设备运行、iOS 编译/签名验证，现有 APK/IPA 未重打包。
