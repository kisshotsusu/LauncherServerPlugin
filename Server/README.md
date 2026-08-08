# CloudUpdate 管理服务器

零第三方依赖的 Python 管理服务器，负责：

- 托管打包目录（作为云端文件仓库，客户端缺失/损坏文件从这里下载修复）
- 生成并托管**完整性清单**（路径 / 大小 / MD5）
- 导入并索引 **HotPatcher 产物**（PatchConfig / Release / PakFilesInfo / Diff / Pak），生成客户端可用的更新描述
- 提供**网页管理控制台**与 REST API（版本管理、云端文件浏览/上传、清单生成、一键导入 HotPatcher）

## 目录结构

```
Server/
  run_server.py                HTTP 服务（纯标准库）
  config.json                  配置
  data/
    versions.json              版本索引（自动生成）
    versions/<版本>/            导入的 HotPatcher 产物 + descriptor.json
    manifests/                 完整性清单缓存
  scripts/
    import_hotpatcher.py       导入 HotPatcher 产物
    generate_manifest.py       重新生成完整性清单
  web/
    index.html                 管理页面
```

## 启动

```bat
cd /d D:\test\CodeBuild\Server
python run_server.py
```

默认监听 `http://0.0.0.0:8710`，**网页管理控制台**：`http://127.0.0.1:8710/`

控制台功能：

- 概览：项目、当前版本、更新链、版本数、清单状态
- 版本管理：查看更新描述、删除版本（移入 `data/trash` 回收站）、上传新版本文件、一键导入 HotPatcher
- 云端文件：按平台浏览打包目录、上传文件到任意目录、复制下载链接
- 完整性清单：按基础包版本查看/重新生成/下载清单
- 服务器设置：项目名、平台、监听地址/端口、数据目录、版本文件库/补丁包位置、**基础包多版本（每平台可增删多个版本目录）**、版本顺序、清单哈希与排除模式、上传上限、管理员令牌，全部可在网页上修改并保存
- 网页一键重启服务器（修改监听地址/端口后无需手动重启）
- API 说明：内置全部接口速查

若 `config.json` 配置了 `admin_token`，控制台右上角填写令牌后管理操作自动携带认证头。

## 首次配置

编辑 `config.json`：

| 配置项 | 说明 |
| --- | --- |
| `host` / `port` | 监听地址与端口 |
| `project` | 项目名，与插件设置一致（CodeBuild） |
| `platforms` | 支持的平台列表 |
| `version_library_dir` | **版本文件库位置**：存储导入的版本 JSON / Pak / 描述文件（可相对 Server 目录或绝对路径） |
| `base_packages` | **基础包位置（多版本）**：`{平台: {版本号: 目录}}`，每个版本独立清单与修复源；旧版 `package_roots` 会自动迁移为 `{ "1.0": 目录 }` |
| `hotpatcher_source` | **补丁包位置**：HotPatcher 产物目录（一键导入的数据源） |
| `manifest_exclude_patterns` | 清单排除模式（fnmatch 风格） |
| `admin_token` | 管理操作令牌，留空则不校验（生产环境建议设置） |
| `max_upload_mb` | 网页上传大小上限（MB），默认 2048 |
| `hotpatcher_order` | 一键导入时的版本顺序 |

这些设置在网页管理控制台的 **服务器设置** 页修改（保存后立即生效并写回 `config.json`），
或通过 `POST /api/config/update` 修改；监听地址/端口保存后点击“重启服务器”即可生效。

## 导入 HotPatcher 版本

HotPatcher 打包/补丁后，产物位于 `D:\test\CodeBuild\Saved\HotPatcher\`，执行：

```bat
python scripts\import_hotpatcher.py ^
  --source D:\test\CodeBuild\Saved\HotPatcher ^
  --data data ^
  --project CodeBuild --platform Windows ^
  --order 1.0,1.1,1.2,1.3,1.4
```

也可以不带参数直接运行，路径自动读取 `config.json`（补丁包位置 / 版本文件库 / 基础包）：

```bat
python scripts\import_hotpatcher.py
```

脚本会：

1. 识别所有含 `{版本}_Release.json` 的版本目录；
2. 复制 JSON 与 `Windows/` 下的 `.pak/.ucas/.utoc` 到 `data/versions/{版本}/`；
3. 解析 `*_PatchConfig.json`（版本、基础版本、IoStore）、`*_PakFilesInfo.json`（Pak 哈希/大小）、`*_Diff.json`（变更统计）；
4. 生成 `descriptor.json`，重建 `data/versions.json` 索引。

新版本发布流程：HotPatcher 出包 -> 控制台“一键导入 HotPatcher”（或重新运行导入脚本）-> 客户端 `CheckForUpdates` 即可发现。

## 完整性清单

清单按基础包版本扫描生成（首次访问或手动触发），缓存文件名 `{项目}_{平台}_{版本}.json`：

```bat
python scripts\generate_manifest.py --platform Windows --force
```

或通过 API：

```
GET  /api/manifest.json?platform=Windows&baseVersion=1.0&refresh=1
POST /api/manifest/generate?platform=Windows&baseVersion=1.0
```

## API 一览

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 管理页面 |
| GET | `/api/status` | 服务器状态、版本数、清单状态 |
| GET | `/api/versions` | 版本索引与更新链 |
| GET | `/api/version/{id}?platform=Windows` | 更新描述；基础包版本返回整包文件列表（客户端整包替换） |
| GET | `/api/manifest.json?platform=Windows&baseVersion=1.0` | 按基础包版本的完整性清单 |
| GET | `/api/files?platform=Windows&baseVersion=1.0&path=CodeBuild/Content` | 云端文件目录浏览（按基础包版本） |
| GET | `/api/config` | 服务器配置（不含令牌） |
| GET | `/api/launcher/version` | 启动器自升级版本信息（data/launcher/version.json） |
| GET | `/files/packages/{平台}/{基础包版本}/{路径}` | 打包目录文件（修复下载源，兼容无版本号的旧式路径） |
| GET | `/files/versions/{版本}/{文件名}` | 更新包文件 |
| GET | `/files/launcher/{文件名}` | 启动器安装包（自升级下载源） |
| POST | `/api/versions/reindex` | 重建版本索引（需令牌，若配置） |
| POST | `/api/manifest/generate?platform=Windows` | 重新生成清单（需令牌，若配置） |
| POST | `/api/upload?target=package|version&platform=&baseVersion=&path=&versionId=` | 上传文件（multipart，字段名 file） |
| POST | `/api/import/hotpatcher` | 一键导入 HotPatcher 产物（需配置 hotpatcher_source） |
| POST | `/api/config/update` | 修改全部设置，含基础包多版本 `basePackages`（JSON，需令牌，若配置） |
| POST | `/api/server/restart` | 网页重启服务器（自动重试端口） |
| DELETE | `/api/version/{id}` | 删除版本（移入 data/trash 回收站） |

## 客户端对接

UE 插件 `CloudUpdate`（`Plugins/CloudUpdate`）直接使用上述 API：

1. `Check For Updates` -> `GET /api/versions`：**先比较基础包版本**（`baseVersions`，有更新的整包排在结果最前），**再比较补丁链**（`updateChain`）；
2. `Apply Update("2.0")` -> 基础包整包更新：按描述符 `files` 下载全部文件并替换本地，`restartRequired=true`；`Apply Update("1.4")` -> 补丁更新：ContentPak 进 `Content/Paks`，ExternFile 替换本地文件，IoStore 重启生效；
3. `Check Integrity` -> `GET /api/manifest.json?baseVersion=本地基础包版本`，`Repair Issues` -> `GET /files/packages/Windows/{基础包版本}/{路径}`。

插件也支持直连 HotPatcher JSON 模式（不经过管理服务器描述文件），在项目设置中填写
`HotPatcherBaseUrl` 为版本文件所在 HTTP 根地址即可。

## 启动器（Luncher）

`D:\test\CodeBuild\Luncher` 为 C++ 启动器工程（Win32 + GDI+ + WinHTTP），
支持序列帧背景、限速下载、自定义游戏位置、基础包整包更新/补丁更新/修复、自升级。
发布新启动器版本时把 `Launcher.exe`、`SelfUpdater.exe` 复制到
`data/launcher/`，并更新 `data/launcher/version.json` 即可。

## 注意事项

- 大型文件（如 300MB+ 的 Pak）由客户端 HTTP 缓冲下载，本服务器按流式分块发送；生产环境可前置 CDN/对象存储。
- `admin_token` 为空时管理接口可直接调用，仅建议内网使用。
- 基础包版本与补丁版本重名时，以基础包整包为准：索引会移除同名补丁条目，
  更新链也不会包含基础包版本，避免客户端整包后又重复打同名补丁

## 开放版本控制（管理页面「版本管理」）

- 每个版本有「开放下载」开关：关闭后客户端 `/api/versions` 不再返回该版本，
  启动器不会检测到它，也无法下载/更新到该版本
- 提供「全部开放 / 全部关闭」快捷按钮（按平台）
- 未做任何设置时默认全部开放；管理端始终用 `GET /api/versions?all=1` 查看完整列表
- 配置保存在 `config.json` 的 `enabled_versions` 字段：`{ "Windows": ["1.0", "1.2"] }`
- 专用接口：`GET /api/enabled_versions`（查询）、
  `POST /api/enabled_versions`（设置，请求体 `{platform, versions:[...]}`）
- 新增基础包会自动加入开放列表（客户端立即可下载）；删除基础包时自动移出

## 服务器控制（管理页面「服务器控制」）

- 实时显示进程 PID、运行时长、启动时间、Python 版本、监听地址、数据目录
- 一键「重启服务器」（先拉起新进程再关闭旧进程，约 3 秒恢复）
- 「重建版本索引」「重新生成全部完整性清单」「刷新状态」快捷操作

## 启动器管理（管理页面「启动器管理」）

- **启动器版本（历史管理）**：像游戏基础包一样维护「版本文件夹」，
  每个版本指向一个含 `Launcher.exe` 的文件夹（配置存于 `launcher_versions`）；
  点「发布」时把所选文件夹的 `Launcher.exe` / `SelfUpdater.exe` 快照到
  `data/launcher` 并写入 `version.json`，客户端启动自检自动升级
- **背景序列帧（指定文件夹）**：在管理页直接填写服务器上的背景图文件夹路径
  （PNG 序列帧，按文件名排序，配置存于 `background_dir`），无需逐张上传；
  客户端启动自检自动下载新版背景图到本地 `Background/`
- 背景版本为内容指纹（文件名+大小+MD5+FPS），内容不变版本不变，不会重复下载
- 相关接口：
  - `GET /api/launcher/versions` / `POST {version, dir}` / `DELETE /api/launcher/versions/<版本>`
  - `POST /api/launcher/publish`：发布启动器版本（`{version, note}`）
  - `GET /api/launcher/background`：背景资源清单（版本、FPS、文件与 MD5）
  - `POST /api/launcher/background/dir`：设置背景文件夹（`{dir}`）
  - `POST /api/launcher/background`：设置 FPS（`{frameFps}`）
  - `POST /api/launcher/background/clear`：清除背景目录配置（不删除磁盘文件）
  - `GET /api/launcher/version`：客户端自升级查询
