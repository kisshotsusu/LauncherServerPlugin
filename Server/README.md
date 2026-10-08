# CloudUpdate 多项目、多平台管理服务器

纯 Python 标准库提供管理控制台、版本分发、整包与补丁下载、完整性清单和对象存储。

## 启动

```bat
python run_server.py --config config.json serve
```

默认地址为 `http://127.0.0.1:8710/`。打包版本使用 `dist/CloudUpdateServer.exe --config config.json serve`。源码修改后须重新打包才能更新 EXE 内的后端；管理网页从 Server/web 加载，发布版本同时内置网页。

## 配置流程

1. **项目管理 → 配置平台**：选择 Windows、Linux、Mac、Android、IOS 或添加自定义标识，指定默认平台。新建项目时也能直接指定平台。
2. 打开 **目录位置**，选择一个项目工作空间，点击 **保存并初始化**。各平台目录自动创建。
3. 把基础包放入 `imports/<平台>/base/<版本>/`，刷新待导入包并点击导入基础包。
4. 把 HotPatcher 输出放入 `imports/<平台>/patch/<版本>/`，包含 `<版本>_Release.json` 与 `<平台>/`，点击对应平台的导入补丁。
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
{"category":"base","platform":"Linux","version":"1.0"}
```

`category` 支持 `base`、`patch`、`launcher`；补丁导入无需填写 version。

对象存储支持 OSS、COS、AWS S3 和 MinIO。endpoint、region、bucket 和密钥在「对象存储」中设置；Secret 留空保持已有值。平台补丁对象键为 `versions/<平台>/<版本>/files/<文件>`。

## 验证与构建

```bat
python -m unittest discover -s tests -v
python -m compileall -q .
build_venv/Scripts/python.exe -m PyInstaller --noconfirm build_exe.spec
```

回归测试使用临时项目验证真实 HTTP 分发、平台与项目隔离、独立删除、配置路径、开放列表和下载链接。
