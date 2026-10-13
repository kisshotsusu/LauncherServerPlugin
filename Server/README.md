# CloudUpdate 多项目、多平台管理服务器

纯 Python 标准库提供管理控制台、版本分发、整包与补丁下载、完整性清单和对象存储。

## 启动

```bat
python run_server.py --config config.json serve
```

默认地址为 `http://127.0.0.1:8710/`。打包版本使用 `dist/CloudUpdateServer.exe --config config.json serve`。源码修改后须重新打包才能更新 EXE 内的后端；管理网页从 Server/web 加载，发布版本同时内置网页。

## 配置流程

1. **项目管理 → 配置平台**：选择 Windows、Linux、Mac、Android、IOS 或添加自定义标识，指定默认平台。新建项目时也能直接指定平台。
2. **服务器管理 → 存储管理**：选择项目，只配置该项目的工作空间根目录。此处不管理包来源或内部存储目录。
3. **项目内部 → 包导入**：选择平台，手动选择基础包产物目录并填写版本号，点击导入基础包。
4. 手动选择 HotPatcher 输出根目录，或包含 `<版本>_Release.json` 和 `<平台>/` 的单个版本文件夹，点击导入补丁包。来源文件保留，包复制后由工作空间管理。项目界面只展示来源与版本，不展示导入后的内部存储位置。
5. 把启动器放入 `imports/launcher/<版本>/`，导入后在启动器管理中发布。
6. 在 **版本管理** 开放需要下发的版本。空开放列表表示全部关闭；未设置开放列表表示全部开放。

修改工作空间位置会整体移动工作空间，目标必须为空，不能与当前目录相互包含。

## 工作空间结构

```text
workspace/
  bases/<平台>/<版本>/              基础包文件
  patches/<平台>/<版本>/            补丁描述与 files/ 文件
  imports/<平台>/base/<版本>/       基础包导入入口
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

## 平台请求分发

以下三种方式选择相同的平台：

```text
GET /codebuild/Linux/api/versions
GET /codebuild/api/versions?platform=Linux
GET /codebuild/api/versions   (请求头 X-Platform: Linux)
```

也支持 `/codebuild/platforms/Linux/api/versions`。选择器不一致或平台未配置时返回 HTTP 400。未指定时使用项目默认平台。所有 URL 的项目部分由 projects.json 中的项目标识决定。

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
{"category":"base","platform":"Linux","version":"1.0","source":"D:/Builds/Linux"}
```

`category` 支持 `base`、`patch`、`launcher`；基础包、补丁包通过 source 选择本地来源目录；补丁导入无需填写 version。来源位置通过 import_sources 按平台和包类型保存。工作空间保存使用服务器管理权限，包导入使用项目管理权限。

对象存储支持 OSS、COS、AWS S3 和 MinIO。endpoint、region、bucket 和密钥在「对象存储」中设置；Secret 留空保持已有值。平台补丁对象键为 `versions/<平台>/<版本>/files/<文件>`。

## 验证与构建

```bat
python -m unittest discover -s tests -v
python -m compileall -q .
build_venv/Scripts/python.exe -m PyInstaller --noconfirm build_exe.spec
```

回归测试使用临时项目验证真实 HTTP 分发、平台与项目隔离、独立删除、配置路径、开放列表和下载链接。

## 工作空间校验与导入反馈

项目包导入手动选择来源，导入期间按钮锁定，防止重复提交。来源文件保留。启动器仍从指定导入入口发现有效、未导入的版本。

迁移目标必须为目录，不允许与其它项目工作空间重叠；保存配置失败时移回原目录。工作空间内的证书路径随迁移更新。工作空间启用后不再通过平台配置接口指定独立目录。

补丁描述包含子目录中的 Pak、utoc 和 ucas，下载链接与相对路径保留目录层级。资产统计使用当前项目的 game_asset_prefixes，Diff 元数据支持放在对应版本目录内。

## v2.5 多平台资源导入

平台通道按精确名称隔离。Windows、Linux、Mac、IOS 和 Android 的各个 Cook flavor 可以独立配置；Android_ASTC 与 Android_ETC2 不互相回退。CloudUpdate 已移除项目配置中的 Platform 属性：桌面与 IOS 使用运行平台标识，安卓从 APK 的 CookFlavor 元数据自动识别 ASTC/ETC2/DXT/Multi/OpenXR。旧 APK 使用引擎的 CookedFlavors 元数据作为兼容来源。服务端通道名称应与 GetUpdatePlatform 返回值一致，不需要按平台改写项目设置。

基础包来源支持包含平台子目录的发布根目录。安卓 APK + Data 发布目录自动使用 Data 子树，避免错误生成 Data/Game/... 的目标路径。仅有 APK 的目录会拒绝导入；普通 UE NoObbs Archive 不含数据时，请选择项目 Saved/StagedBuilds/Android_ASTC 或使用核心打包脚本生成的完整 Data。APK 安装与游戏 C++ 二进制更新仍需要完整应用更新。

补丁来源支持 HotPatcher 根目录或单版本目录。混合平台根目录中没有当前平台产物的版本会跳过，无可导入产物或空资源目录则报错；平台级 Release/PatchConfig 元数据优先于共享文件。补丁的外部资源按其 baseVersionId 查找该平台的基础版本，禁止使用最新基础版本冒充依赖版本。

支持 pak、utoc、ucas 及其 .patch 差分文件。差分与完整文件同时存在时只发布一个差分条目，完整文件作为 fallbackUrl 下发，并提供独立 fallbackSize/fallbackHash。发布的大小和 MD5 取实际文件，不使用旧 PakFilesInfo 中的过期值。IoStore 文件会自动要求重启。

CloudUpdate 对版本索引、描述文件和完整性清单增加平台一致性校验，拒绝明确标记为其它平台的响应；没有平台字段的旧单平台响应保持兼容。完整文件回退下载使用其自己的哈希，不使用差分文件哈希。

v2.5 验证：42 项服务器测试、4 项 UE CloudUpdate 自动化测试通过，插件 Win64 编辑器构建与安卓运行时代码编译通过；打包 EXE 实测 Windows/Android_ASTC 双平台导入、索引、差分/回退 HTTP 下载及 ETC2 隔离。
