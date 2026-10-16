#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CloudUpdate 管理服务器（独立程序）。

功能：
  - 提供文件完整性清单 /api/manifest.json
  - 提供基于 HotPatcher 产物的版本索引 /api/versions 与更新描述 /api/version/{id}
  - 作为云端文件仓库：/files/packages/... 与 /files/versions/...（local 模式）
  - 远程存储（s3）模式下，下载 URL 为对象存储的 presigned URL，客户端直连下载
  - 支持 HTTPS（自签名证书自动生成，或加载用户证书）
  - 管理操作可通过网页控制台（默认首页）或命令行子命令完成：
        serve              启动服务（供启动器读取版本与下载）
        reindex            重建版本索引
        gen-manifest       重新生成完整性清单
        upload             上传文件到存储（local 写磁盘 / s3 传对象）
        import-hotpatcher  导入 HotPatcher 产物
        publish-launcher   发布启动器自升级版本
        set-enabled        设置开放版本（客户端可见/可下载）
        gen-cert           生成自签名证书
        version-delete     删除版本（回收站 + 撤销快照 + 远程删对象）

启动示例：
  python run_server.py --config config.json serve
  python run_server.py --config config.json --host 0.0.0.0 --port 8711 serve
"""
import argparse
from email.utils import formatdate
import contextlib
import datetime
import re
import hashlib
import io
import json
import mimetypes
import os
import shutil
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse

from config import (
    resolve_server_path,
    validate_platforms,
    platform_capabilities,
    workspace_paths,
    workspace_platform_paths,
    resolve_base_package_path,
    load_config,
    ensure_dirs,
    safe_join,
    parse_multipart,
    version_key,
    get_base_packages,
    get_latest_base_version,
    get_base_dir,
    launcher_bg_dir,
)
from manifest import (
    hash_file,
    matches_any,
    generate_manifest,
    build_base_descriptor,
)
from versions import (
    _read_json,
    build_versions_index,
    load_versions_index,
    filter_index_by_enabled,
    read_descriptor,
    filter_index_by_platform,
    add_version_channels,
    _revoked_entry_from_descriptor,
    load_revoked_store,
    save_revoked_store,
)
from storage import get_storage
from packages import list_installers as catalog_installers, package_kind
from importer import (
    run_import, import_version, base_import_source,
    find_android_apk, validate_android_apk, inspect_application_source, split_mobile_release,
    iter_resource_files, is_resource_file, validate_resource_tree, inspect_base_source, patch_import_plan,
)
import certgen

BASE_DIR = Path(__file__).resolve().parent
# PyInstaller 单文件打包后 __file__ 指向临时解压目录，改用 exe 实际所在目录作为基准
if getattr(sys, "frozen", False):
    BASE_DIR = Path(sys.executable).resolve().parent
SERVER_START_TIME = time.time()
# 供「重启服务器」接口重新拉起同一进程
_APP_ARGV = []
# 是否为双击启动（无子命令）：用于决定要不要自动打开浏览器 / 结束时暂停
_DOUBLE_CLICKED = False


def _is_double_clicked():
    return _DOUBLE_CLICKED


_CFG_MTIME_CACHE = {}


def reload_cfg_if_changed():
    """若 config.json 比内存中的副本更新，则重新加载，使手动/外部修改立即生效。

    修复：服务进程只在启动时 load_config 一次，之后手动编辑 config.json 不会反映到
    运行时（表现为“路径配置没同步”“一键导入读不到 hotpatcher 源”）。改为每个请求前
    比对文件 mtime，变化即重新加载并刷新 UpdateHandler.cfg。
    """
    cfg = UpdateHandler.cfg
    cfg_path = cfg.get("_config_path") if cfg else None
    if not cfg_path or not os.path.isfile(cfg_path):
        return
    try:
        mtime = os.path.getmtime(cfg_path)
    except OSError:
        return
    if _CFG_MTIME_CACHE.get(cfg_path) == mtime:
        return
    try:
        new_cfg = load_config(cfg_path)
    except Exception:
        return
    if UpdateHandler.cfg is None or new_cfg["_config_path"] == UpdateHandler.cfg.get("_config_path"):
        UpdateHandler.cfg = new_cfg
    _CFG_MTIME_CACHE[cfg_path] = mtime


# ======================================================================
# 多项目管理：同一监听端口，按 URL 前缀（如 /cc/）路由到不同项目的配置
# ======================================================================
PROJECT_REGISTRY = {"default": "main", "projects": {}}   # 项目注册表（内存态，由 projects.json 落地）
SERVER_DIR = ""                                            # 主配置文件所在目录
_PROJECT_CFG_CACHE = {}                                    # key -> (cfg, mtime)
_REGISTRY_MTIME = 0.0
RESERVED_PREFIXES = {"api", "web", "files", "platforms"}                # 这些前缀永远留给路由/静态资源，不能当项目名

# 接口注册表：供「服务器管理 → 接口管理」列出与启停。
# 仅登记可被停用的数据/管理类接口；服务器管理自身（/api/projects、/api/server/*）与
# 核心控制台（/web、/files、/api/status）不登记，永远可用，避免把自己锁死。
API_ENDPOINTS = [
    {"id": "platform_config", "method": "POST", "path": "/api/platform/config", "scope": "admin", "name": "平台配置", "desc": "保存当前平台的基础包、补丁来源和顺序"},
    {"id": "versions",         "method": "GET",    "path": "/api/versions",          "scope": "client", "name": "版本列表",     "desc": "返回所有版本及其元信息"},
    {"id": "version_detail",   "method": "GET",    "path": "/api/version/*",         "scope": "client", "name": "版本详情",     "desc": "返回单个版本的描述与下载信息"},
    {"id": "manifest",         "method": "GET",    "path": "/api/manifest.json",     "scope": "client", "name": "完整性清单",   "desc": "返回文件完整性清单"},
    {"id": "files",            "method": "GET",    "path": "/api/files",             "scope": "client", "name": "云端文件浏览", "desc": "浏览基础包目录"},
    {"id": "config_get",       "method": "GET",    "path": "/api/config",            "scope": "admin",  "name": "项目配置读取", "desc": "返回当前项目配置（不含密钥）"},
    {"id": "enabled_versions", "method": "GET",    "path": "/api/enabled_versions",  "scope": "client", "name": "启用版本列表", "desc": "返回客户端应拉取的启用版本"},
    {"id": "launcher_version", "method": "GET",    "path": "/api/launcher/version",  "scope": "client", "name": "启动器版本信息", "desc": "返回启动器版本号"},
    {"id": "launcher_bg",      "method": "GET",    "path": "/api/launcher/background","scope": "client", "name": "启动器背景",   "desc": "返回背景图列表"},
    {"id": "launcher_versions","method": "GET",    "path": "/api/launcher/versions", "scope": "admin",  "name": "启动器版本列表", "desc": "返回启动器历史版本"},
    {"id": "launcher_runtime", "method": "GET",    "path": "/api/launcher/runtime",  "scope": "client", "name": "启动器运行时", "desc": "返回运行时下载信息"},
    {"id": "dirs_list",        "method": "GET",    "path": "/api/dirs/list",         "scope": "admin",  "name": "目录位置列表", "desc": "返回关键目录位置"},
    {"id": "upload",           "method": "POST",   "path": "/api/upload",            "scope": "admin",  "name": "上传版本包",   "desc": "上传并导入版本包"},
    {"id": "versions_reindex", "method": "POST",   "path": "/api/versions/reindex",  "scope": "admin",  "name": "重建版本索引", "desc": "重新扫描版本库"},
    {"id": "manifest_gen",     "method": "POST",   "path": "/api/manifest/generate", "scope": "admin",  "name": "生成完整性清单", "desc": "重新生成清单"},
    {"id": "import_hotpatcher", "method": "POST",  "path": "/api/import/hotpatcher",  "scope": "admin",  "name": "导入 HotPatcher", "desc": "从 HotPatcher 源导入"},
    {"id": "enabled_set",      "method": "POST",   "path": "/api/enabled_versions",  "scope": "admin",  "name": "设置启用版本", "desc": "更新启用版本列表"},
    {"id": "config_update",    "method": "POST",   "path": "/api/config/update",     "scope": "admin",  "name": "更新项目配置", "desc": "保存项目配置"},
    {"id": "storage_test",     "method": "POST",   "path": "/api/storage/test",      "scope": "admin",  "name": "存储连通测试", "desc": "测试对象存储连接"},
    {"id": "launcher_add",     "method": "POST",   "path": "/api/launcher/versions", "scope": "admin",  "name": "新增启动器版本", "desc": "登记启动器版本"},
    {"id": "launcher_publish", "method": "POST",   "path": "/api/launcher/publish",  "scope": "admin",  "name": "发布启动器",   "desc": "发布启动器到下载"},
    {"id": "launcher_bg_dir",  "method": "POST",   "path": "/api/launcher/background/dir", "scope": "admin", "name": "设置背景目录", "desc": "设置背景图目录"},
    {"id": "launcher_bg_up",   "method": "POST",   "path": "/api/launcher/background","scope": "admin",  "name": "上传背景图",   "desc": "上传背景图"},
    {"id": "version_del",      "method": "DELETE", "path": "/api/version/*",         "scope": "admin",  "name": "删除版本",     "desc": "删除单个版本"},
    {"id": "launcher_ver_del", "method": "DELETE", "path": "/api/launcher/versions/*","scope": "admin",  "name": "删除启动器版本", "desc": "删除启动器版本"},
]


def match_endpoint(method, path):
    """按 方法+路径 匹配接口注册表中的条目（支持 /api/x/* 前缀匹配）。"""
    for ep in API_ENDPOINTS:
        if ep["method"] != method:
            continue
        p = ep["path"]
        if p.endswith("/*"):
            if path.startswith(p[:-1]):
                return ep
        elif p == path:
            return ep
    return None


def reserved_prefixes():
    """路由保留前缀（api/web/files）始终保留；访问格式配置可扩展更多保留项。"""
    extra = set()
    try:
        acc = (UpdateHandler.cfg or {}).get("access") or {}
        for x in (acc.get("reservedPrefixes") or []):
            if isinstance(x, str) and x:
                extra.add(x)
    except Exception:
        pass
    return RESERVED_PREFIXES | extra



def init_projects(main_cfg_path):
    """根据主配置文件初始化项目注册表。无 projects.json 时退化为单项目（主配置即 'main'）。"""
    global PROJECT_REGISTRY, SERVER_DIR, _REGISTRY_MTIME
    SERVER_DIR = os.path.dirname(os.path.abspath(main_cfg_path))
    default_entry = {"display": "CodeBuild", "config": os.path.basename(main_cfg_path)}
    pf = os.path.join(SERVER_DIR, "projects.json")
    if os.path.isfile(pf):
        try:
            with open(pf, encoding="utf-8") as handle:
                reg = json.load(handle)
        except Exception:
            reg = {}
        if not isinstance(reg, dict):
            reg = {}
        reg.setdefault("default", "main")
        reg.setdefault("projects", {})
        if not isinstance(reg["projects"], dict):
            reg["projects"] = {}
        dk = reg.get("default", "main")
        if dk not in reg["projects"]:
            reg["projects"][dk] = default_entry
        PROJECT_REGISTRY = reg
    else:
        PROJECT_REGISTRY = {"default": "main", "projects": {"main": default_entry}}
    _REGISTRY_MTIME = os.path.getmtime(pf) if os.path.isfile(pf) else 0.0
    # 预热默认项目配置，保证首个请求前 self.cfg 可用
    try:
        load_project_cfg(PROJECT_REGISTRY["default"])
    except Exception:
        pass


def save_registry():
    global _REGISTRY_MTIME
    pf = os.path.join(SERVER_DIR, "projects.json")
    payload = {
        "default": PROJECT_REGISTRY.get("default", "main"),
        "server_admin_token": PROJECT_REGISTRY.get("server_admin_token", ""),
        "projects": PROJECT_REGISTRY.get("projects", {}),
    }
    with open(pf, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    _REGISTRY_MTIME = os.path.getmtime(pf)


def reload_projects_if_changed():
    global PROJECT_REGISTRY, _REGISTRY_MTIME
    pf = os.path.join(SERVER_DIR, "projects.json")
    if not os.path.isfile(pf):
        return
    try:
        mtime = os.path.getmtime(pf)
    except OSError:
        return
    if _REGISTRY_MTIME == mtime:
        return
    try:
        with open(pf, encoding="utf-8") as handle:
            reg = json.load(handle)
    except Exception:
        return
    if not isinstance(reg, dict):
        return
    reg.setdefault("default", "main")
    reg.setdefault("projects", {})
    if not isinstance(reg["projects"], dict):
        reg["projects"] = {}
    global PROJECT_REGISTRY
    PROJECT_REGISTRY = reg
    _REGISTRY_MTIME = mtime
    # 剔除已注销项目的配置缓存
    live = set(reg["projects"].keys())
    for k in list(_PROJECT_CFG_CACHE.keys()):
        if k not in live:
            _PROJECT_CFG_CACHE.pop(k, None)


def load_project_cfg(key):
    """加载（并按 mtime 缓存）指定项目的配置；key 不存在时回退到默认项目。"""
    projects = PROJECT_REGISTRY.get("projects", {})
    entry = projects.get(key) or projects.get(PROJECT_REGISTRY.get("default", "main"))
    if not entry:
        return None
    cfg_path = os.path.abspath(os.path.join(SERVER_DIR, entry["config"]))
    mtime = os.path.getmtime(cfg_path) if os.path.isfile(cfg_path) else 0.0
    cached = _PROJECT_CFG_CACHE.get(key)
    if cached and cached[1] == mtime and not cached[0].get("workspace_dir"):
        return cached[0]
    cfg = load_config(cfg_path)
    _PROJECT_CFG_CACHE[key] = (cfg, mtime)
    return cfg


def resolve_project_path(raw_path):
    """解析 URL 路径得到 (项目key, 去掉前缀后的剩余路径)。无前缀或非项目前缀 -> 默认项目。"""
    parts = [p for p in raw_path.split("/") if p]
    projects = PROJECT_REGISTRY.get("projects", {})
    # 用 reserved_prefixes()（含访问格式管理扩展的保留前缀），保证“保留前缀”在路由解析层与创建层一致识别
    if parts and parts[0] in projects and parts[0] not in reserved_prefixes():
        key = parts[0]
        rest = "/" + "/".join(parts[1:]) if len(parts) > 1 else "/"
        return key, rest
    return PROJECT_REGISTRY.get("default", "main"), raw_path


class UpdateHandler(BaseHTTPRequestHandler):
    server_version = "CloudUpdateServer/2.5"
    cfg = None
    server = None

    # ---------- 基础工具 ----------
    def log_message(self, fmt, *args):
        sys.stdout.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), fmt % args))
        sys.stdout.flush()

    def _send_json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, text, status=200, content_type="text/plain; charset=utf-8"):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, abs_path, download_name=None):
        if not os.path.isfile(abs_path):
            self._send_json({"ok": False, "error": "文件不存在"}, status=404)
            return
        with open(abs_path, "rb") as f:
            stat = os.fstat(f.fileno())
            size = stat.st_size
            etag = f'"{stat.st_mtime_ns:x}-{size:x}"'
            modified = formatdate(stat.st_mtime, usegmt=True)
            start, end, status = 0, size - 1, 200
            range_header = self.headers.get("Range", "")
            if_range = self.headers.get("If-Range")
            if range_header and (not if_range or if_range in (etag, modified)):
                match = re.fullmatch(r"bytes=(\d*)-(\d*)", range_header.strip())
                valid = bool(match and any(match.groups()) and size > 0)
                if valid:
                    first, last = match.groups()
                    if first:
                        start = int(first)
                        end = min(int(last), size - 1) if last else size - 1
                    else:
                        suffix = int(last)
                        valid = suffix > 0
                        start = max(0, size - suffix)
                    valid = valid and start <= end and start < size
                if not valid:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("ETag", etag)
                    self.end_headers()
                    return
                status = 206
            ctype = mimetypes.guess_type(abs_path)[0] or "application/octet-stream"
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(max(0, end - start + 1)))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", etag)
            self.send_header("Last-Modified", modified)
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            if download_name:
                self.send_header("Content-Disposition", f'attachment; filename="{download_name}"')
            self.end_headers()
            f.seek(start)
            remaining = end - start + 1
            try:
                while remaining > 0:
                    chunk = f.read(min(1024 * 1024, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # An interrupted client will resume with Range on its next request.

    # ---------- 路由（仅启动器所需的只读 API + 本地文件下发）----------
    def do_GET(self):
        reload_cfg_if_changed()   # 让手动/外部修改根 config.json 立即生效
        orig_path = self.path
        if not self._enter_project():
            return
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        query = parse_qs(parsed.query)

        # 接口启停门控：被服务器管理员停用的数据/管理类接口返回 403（服务器管理自身与核心控制台不登记，永远可用）
        if not self._endpoint_allowed(path):
            self._send_json({"ok": False, "error": "该接口已被服务器管理员停用"}, status=403)
            return

        # 根路径重定向：仅当“原始请求”就是裸根 /（无项目前缀）时才重定向，
        # 避免默认项目自身 URL（如 /codebuild/）被剥离前缀后再次命中而陷入重定向环。
        access = (UpdateHandler.cfg or {}).get("access") or {}
        orig_unquoted = unquote(urlparse(orig_path).path)
        if orig_unquoted in ("/", "/index.html"):
            dk = PROJECT_REGISTRY.get("default", "main")
            if dk != "main" and access.get("rootBehavior", "redirect") == "redirect":
                target = ("/" + dk + "/") if orig_unquoted == "/" else ("/" + dk + "/index.html")
                self.send_response(302)
                self.send_header("Location", target)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

        # 网页管理控制台：默认首页与静态资源（其余请求继续走 API / 文件下发）
        if path in ("/", "/index.html") or path.startswith("/web/"):
            rel = "index.html" if path in ("/", "/index.html") else path[len("/web/"):]
            self._serve_web_file(rel)
            return

        if path == "/api/status":
            self._api_status(query)
        elif path == "/api/versions":
            self._api_versions(query)
        elif path.startswith("/api/version/"):
            self._api_version(path[len("/api/version/"):], query)
        elif path == "/api/manifest.json":
            self._api_manifest(query)
        elif path == "/api/files":
            self._api_files(query)
        elif path == "/api/workspace":
            self._api_workspace()
        elif path == "/api/platform/config":
            self._api_platform_config()
        elif path == "/api/config":
            self._api_config()
        elif path == "/api/enabled_versions":
            self._api_enabled_versions()
        elif path == "/api/installers":
            self._api_installers()
        elif path.startswith("/api/installer/"):
            self._api_installer(path[len("/api/installer/"):])
        elif path == "/api/launcher/version":
            self._api_launcher_version()
        elif path == "/api/launcher/background":
            self._api_launcher_background()
        elif path == "/api/launcher/versions":
            self._api_launcher_versions()
        elif path == "/api/launcher/runtime":
            self._api_launcher_runtime()
        elif path == "/api/dirs/list":
            self._api_dirs_list(query)
        elif path == "/api/projects":
            self._api_projects_get()
        elif path == "/api/server/apis":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._api_server_apis()
        elif path == "/api/server/cert":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._api_server_cert()
        elif path == "/api/server/access":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._api_server_access()
        elif path.startswith("/files/packages/"):
            self._serve_package_file(path[len("/files/packages/"):])
        elif path.startswith("/files/versions/"):
            self._serve_version_file(path[len("/files/versions/"):])
        elif path.startswith("/files/installers/"):
            self._serve_installer_file(path[len("/files/installers/"):])
        elif path.startswith("/files/background/"):
            self._serve_background_file(path[len("/files/background/"):])
        elif path.startswith("/files/launcher/"):
            self._serve_launcher_file(path[len("/files/launcher/"):])
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    # ---------- API ----------
    def _api_status(self, query):
        index = filter_index_by_platform(load_versions_index(self.cfg), getattr(self, "platform", self.cfg.get("default_platform", self.cfg["platforms"][0])))
        index = add_version_channels(index)
        manifests = {}
        for platform in self.cfg["platforms"]:
            manifests[platform] = []
            packages = get_base_packages(self.cfg, platform)
            for version in sorted(packages.keys(), key=version_key):
                path = os.path.join(self.cfg["manifests_dir"], f"{self.cfg['project']}_{platform}_{version}.json")
                info = {"baseVersion": version, "exists": os.path.exists(path), "path": packages[version]}
                if info["exists"]:
                    m = _read_json(path) or {}
                    info.update({"fileCount": m.get("fileCount", 0), "generatedAt": m.get("generatedAt", "")})
                manifests[platform].append(info)
        storage = get_storage(self.cfg)
        self._send_json({
            "ok": True,
            "server": "CloudUpdateServer",
            "consoleVersion": "2.7",
            "gameVersion": index["gameVersion"],
            "resourceVersion": index["resourceVersion"],
            "activePlatform": getattr(self, "platform", self.cfg["platforms"][0]),
            "storageBackend": "s3" if storage.is_remote else "local",
            "authRequired": bool(self.cfg.get("admin_token")),
            "https": bool((UpdateHandler.cfg or {}).get("https", {}).get("enabled")),
            "project": self.cfg["project"],
            "projectKey": self.project,
            "projectPrefix": self.project_prefix,
            "projects": self._projects_list(),
            "platforms": self.cfg["platforms"],
            "pid": os.getpid(),
            "uptimeSeconds": int(time.time() - SERVER_START_TIME),
            "startedAt": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(SERVER_START_TIME)),
            "python": sys.version.split()[0],
            "host": self.cfg.get("host", "0.0.0.0"),
            "port": self._listen_port(),
            "configPath": self.cfg.get("_config_path", ""),
            "enabledVersions": self.cfg.get("enabled_versions", {}),
            "currentVersion": index.get("current", ""),
            "versionCount": len(index.get("versions", [])),
            "manifests": manifests,
            "versionLibraryDir": self.cfg.get("version_library_dir", ""),
            "basePackageDirs": {p: get_base_dir(self.cfg, p) for p in self.cfg["platforms"]},
            "basePackages": self.cfg.get("base_packages", {}),
            "validBaseVersions": load_versions_index(self.cfg).get("baseVersions", {}),
            "installerVersions": {p: [i["versionId"] for i in catalog_installers(self.cfg, p)] for p in self.cfg["platforms"]},
            "platformSettings": self.cfg.get("platform_settings", {}),
            "serverTime": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        })

    def _api_versions(self, query):
        index = load_versions_index(self.cfg, rebuild=query.get("rebuild", ["0"])[0] == "1")
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        if platform not in self.cfg["platforms"]:
            self._send_json({"error": "未知平台"}, status=400)
            return
        index = filter_index_by_platform(index, platform)
        if query.get("all", ["0"])[0] != "1":
            index = filter_index_by_enabled(index, self.cfg)
        for version in index.get("versions", []):
            version["url"] = self._public_url(version.get("url", ""))
        index = add_version_channels(index)
        index["platform"] = platform
        self._send_json(index)

    def _api_version(self, version_id, query):
        try:
            desc = read_descriptor(self.cfg, version_id, query.get("platform", [self.cfg["platforms"][0]])[0], query.get("kind", [None])[0])
        except (ValueError, RuntimeError) as exc:
            self._send_json({"error": str(exc)}, status=400); return
        if not desc:
            self._send_json({"ok": False, "error": f"版本 {version_id} 不存在"}, status=404)
            return
        desc["platform"] = query["platform"][0]
        for file in desc.get("files", []):
            for field in ("url", "fallbackUrl"):
                if file.get(field): file[field] = self._public_url(file[field])
        self._send_json(desc)

    def _api_manifest(self, query):
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        force = query.get("refresh", ["0"])[0] == "1"
        try:
            manifest = generate_manifest(self.cfg, platform, base_version=base_version, force=force)
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=400)
            return
        self._send_json(manifest)

    def _api_files(self, query):
        """浏览云端基础包目录。GET /api/files?platform=Windows&baseVersion=1.0&path=CodeBuild/Content"""
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        rel_dir = query.get("path", [""])[0]
        kind = query.get("kind", ["base"])[0]
        if kind == "installer":
            items = self._list_installers(platform)
            if base_version:
                items = [item for item in items if item["versionId"] == base_version]
            if not items:
                self._send_json({"error": "当前平台没有这个安装包版本"}, status=404); return
            if not base_version:
                item = max(items, key=lambda item: version_key(item["versionId"]))
                base_version = item["versionId"]
            else:
                item = items[0]
            normalized = rel_dir.replace("\\", "/").strip("/")
            if normalized and any(p in (".", "..") for p in normalized.split("/")):
                self._send_json({"error": "目录不存在或路径非法"}, status=404); return
            prefix = normalized + "/" if normalized else ""
            entries, seen = [], set()
            for entry in item["files"]:
                rel = entry["targetRelativePath"]
                if not rel.startswith(prefix): continue
                tail = rel[len(prefix):]
                name = tail.split("/")[0]
                if name in seen: continue
                seen.add(name)
                directory = "/" in tail
                entries.append({"name":name,"type":"dir" if directory else "file","size":0 if directory else entry["size"],
                                "modified":item["date"],"path":prefix + name,
                                "downloadUrl":"" if directory else self._public_url(entry["url"]),
                                "gameVersion":item["gameVersion"],"resourceVersion":item["resourceVersion"]})
            if normalized and not entries:
                self._send_json({"error":"程序包目录不存在"}, status=404); return
            self._send_json({"ok": True, "platform": platform, "kind": "installer", "baseVersion": base_version,
                             "path": normalized, "parent": normalized.rsplit("/",1)[0] if "/" in normalized else "", "entries": entries}); return
        if kind != "base":
            self._send_json({"error": "云端文件类型必须是 base 或 installer"}, status=400); return
        raw_platform = (self.cfg.get("base_packages", {}) or {}).get(platform) or {}
        if base_version:
            # 显式指定了版本号：必须已在配置中，否则视为无效版本返回 404，
            # 避免回退到最新版本导致“请求不存在的版本却返回最新版内容”的串味。
            if base_version not in raw_platform:
                paths = {str(v): str(p) for v, p in raw_platform.items()}
                self._send_json({
                    "ok": False,
                    "error": f"平台 {platform} 未配置基础包版本 {base_version}",
                    "configuredPaths": paths,
                }, status=404)
                return
            resolved = self._resolve_path(raw_platform[base_version])
            if not os.path.isdir(resolved):
                self._send_json({
                    "ok": False,
                    "error": f"基础包 {platform}/{base_version} 已配置，但目录在磁盘上不存在：{resolved}",
                    "configuredPaths": {base_version: resolved},
                }, status=404)
                return
        root = get_base_dir(self.cfg, platform, base_version)
        if not root or not os.path.isdir(root):
            # get_base_packages 会过滤掉磁盘上不存在的目录，因此这里回查原始配置，
            # 区分「压根没配置」和「配置了但目录不存在」，避免给出误导性的提示。
            raw = (self.cfg.get("base_packages", {}) or {}).get(platform) or {}
            if raw:
                paths = {str(v): str(p) for v, p in raw.items()}
                detail = "；".join(f"{v} → {p}" for v, p in paths.items())
                self._send_json({
                    "ok": False,
                    "error": f"平台 {platform} 的基础包目录已在 config.json 配置，但磁盘上不存在（尚未导入内容？）：{detail}",
                    "configuredPaths": paths,
                }, status=404)
            else:
                self._send_json({
                    "ok": False,
                    "error": f"平台 {platform} 未在 config.json 的 base_packages 中配置基础包目录",
                }, status=404)
            return
        abs_dir = safe_join(root, rel_dir)
        if abs_dir is None or not os.path.isdir(abs_dir):
            self._send_json({"ok": False, "error": "目录不存在或路径非法"}, status=404)
            return
        storage = get_storage(self.cfg)
        entries = []
        for name in sorted(os.listdir(abs_dir)):
            item_path = os.path.join(abs_dir, name)
            rel = os.path.relpath(item_path, root).replace("\\", "/")
            if rel == ".cloudupdate.json":
                continue
            if not is_resource_file(rel):
                continue
            if matches_any(rel, self.cfg["manifest_exclude_patterns"]):
                continue
            if os.path.isdir(item_path):
                entries.append({"name": name, "type": "dir", "size": 0,
                                "modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(item_path))),
                                "path": rel})
            else:
                base_for_url = base_version or get_latest_base_version(self.cfg, platform)
                entries.append({
                    "name": name, "type": "file", "size": os.path.getsize(item_path),
                    "modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(item_path))),
                    "path": rel,
                    "downloadUrl": self._public_url(storage.url_for("packages", platform=platform, version=base_for_url, rel=rel)),
                })
        parent = ""
        if rel_dir:
            parent = "/".join(rel_dir.replace("\\", "/").split("/")[:-1])
        self._send_json({
            "ok": True, "platform": platform,
            "baseVersion": base_version or get_latest_base_version(self.cfg, platform),
            "root": root, "path": rel_dir.replace("\\", "/"), "parent": parent, "entries": entries,
        })

    @staticmethod
    def _pending_workspace_versions(folder, existing, required=None):
        if not os.path.isdir(folder):
            return []
        return [v for v in sorted(os.listdir(folder))
                if re.fullmatch(r"[A-Za-z0-9_.-]+", v) and not v.startswith(".")
                and v not in existing and os.path.isdir(os.path.join(folder, v))
                and os.listdir(os.path.join(folder, v))
                and (not required or os.path.isfile(os.path.join(folder, v, required)))]

    def _api_workspace(self):
        if not self.cfg.get("workspace_dir"):
            self._send_json({"ok": True, "configured": False, "suggestedRoot": os.path.join(SERVER_DIR, "workspace", self.project)})
            return
        paths = workspace_paths(self.cfg)
        platforms = []
        index = load_versions_index(self.cfg)
        for platform in self.cfg["platforms"]:
            item = {"platform": platform, **workspace_platform_paths(self.cfg, platform)}
            item["baseVersions"] = list(self.cfg["base_packages"].get(platform, {}))
            item["baseValidation"] = {}
            if platform in self.cfg["platforms"]:
                for version, folder in self.cfg["base_packages"].get(platform, {}).items():
                    try:
                        validate_resource_tree(folder)
                        item["baseValidation"][version] = {"valid": True}
                    except (ValueError, OSError) as exc:
                        item["baseValidation"][version] = {"valid": False, "error": str(exc)}
            item["capabilities"] = platform_capabilities(platform)
            if item["capabilities"]["applicationPackage"]:
                item["installerVersions"] = [i["versionId"] for i in self._list_installers(platform)]
            item["sources"] = {k: v for k, v in ((self.cfg.get("import_sources") or {}).get(platform) or {}).items() if k in (("base", "patch", "application", "apk") if platform == "Android" else ("base", "patch", "application"))}
            item["patchVersions"] = [v["versionId"] for v in filter_index_by_platform(index, platform).get("versions", []) if package_kind(v) == "patch"]
            item["pendingBases"] = self._pending_workspace_versions(item["baseImport"], item["baseVersions"])
            if item["capabilities"]["apkInstaller"]:
                item["pendingApks"] = self._pending_workspace_versions(item["apkImport"], item["installerVersions"])
            item["pendingApplications"] = self._pending_workspace_versions(item["applicationImport"], item["installerVersions"])
            platforms.append(item)
        pending = self._pending_workspace_versions(paths["launcherImport"], self.cfg["launcher_versions"], "Launcher.exe")
        self._send_json({"ok": True, "configured": True, "paths": paths, "platforms": platforms, "pendingLaunchers": pending})

    def _admin_workspace(self):
        data = self._json_body()
        if data is None: return
        value = data.get("root")
        if not isinstance(value, str) or not value.strip():
            self._send_json({"error": "请选择工作空间目录"}, status=400); return
        root = os.path.realpath(self._resolve_path(value.strip()))
        shared = os.path.realpath(os.path.join(SERVER_DIR, "workspace"))
        if os.path.normcase(root) == os.path.normcase(shared):
            self._send_json({"error": "请选择项目独立目录，例如 workspace/" + self.project}, status=400); return
        current = self.cfg.get("workspace_dir")
        def overlaps(left, right):
            left, right = os.path.normcase(os.path.realpath(left)), os.path.normcase(os.path.realpath(right))
            try:
                return os.path.commonpath([left, right]) in (left, right)
            except ValueError:
                return False
        for key in PROJECT_REGISTRY.get("projects", {}):
            if key == self.project: continue
            entry = PROJECT_REGISTRY["projects"][key]
            if not os.path.isfile(os.path.join(SERVER_DIR, entry["config"])):
                continue
            other = load_project_cfg(key)
            if other and other.get("workspace_dir") and overlaps(root, other["workspace_dir"]):
                self._send_json({"error": "工作空间不能与其它项目的工作空间重叠"}, status=400); return
        changing = current and os.path.normcase(os.path.realpath(current)) != os.path.normcase(root)
        if os.path.exists(root) and not os.path.isdir(root):
            self._send_json({"error": "工作空间目标必须是目录，不能是文件"}, status=400); return
        if changing and overlaps(current, root):
            self._send_json({"error": "新旧工作空间不能相互包含"}, status=400); return
        if changing and os.path.isdir(root) and os.listdir(root):
            self._send_json({"error": "新工作空间必须为空，避免覆盖已有内容"}, status=400); return
        moved = False
        try:
            if changing and os.path.isdir(current):
                if os.path.isdir(root): os.rmdir(root)
                os.makedirs(os.path.dirname(root), exist_ok=True)
                shutil.move(current, root)
                moved = True
            updates = {"workspace_dir": root}
            if changing:
                https = dict(self.cfg.get("https") or {})
                for key in ("certFile", "keyFile"):
                    path = https.get(key)
                    if path and overlaps(current, path) and os.path.normcase(os.path.realpath(path)) != os.path.normcase(os.path.realpath(current)):
                        https[key] = os.path.join(root, os.path.relpath(path, current))
                updates["https"] = https
            self.cfg = _apply_config_updates(updates, self.cfg)
            ensure_dirs(self.cfg)
            build_versions_index(self.cfg)
        except OSError as exc:
            if moved and (_read_json(self.cfg["_config_path"]) or {}).get("workspace_dir") != root:
                shutil.move(root, current)
            self._send_json({"error": "工作空间保存失败：" + str(exc)}, status=400); return
        self._send_json({"ok": True, "message": "项目工作空间已保存并创建所有目录"})

    def _check_import_source(self, source):
        selected = os.path.normcase(os.path.realpath(source))
        def overlaps(left, right):
            try:
                return os.path.commonpath([left, right]) in (left, right)
            except ValueError:
                return False
        for key, entry in PROJECT_REGISTRY.get("projects", {}).items():
            if not os.path.isfile(os.path.join(SERVER_DIR, entry["config"])):
                continue
            cfg = self.cfg if key == self.project else load_project_cfg(key)
            root = cfg.get("workspace_dir")
            if not root:
                continue
            managed = os.path.normcase(os.path.realpath(root))
            imports = os.path.join(managed, "imports")
            if overlaps(selected, managed):
                if key == self.project and selected.startswith(imports + os.sep):
                    continue
                raise ValueError("请选择外部产物目录或当前项目的 imports 子目录，不能使用项目已管理的数据作为导入来源")

    def _admin_workspace_inspect(self):
        data = self._json_body()
        if data is None: return
        platform = self._body_platform(data)
        if not platform: return
        try:
            source = data.get("source")
            if not isinstance(source, str) or not source.strip():
                raise ValueError("请选择来源")
            source = self._resolve_path(source.strip())
            self._check_import_source(source)
            category = data.get("category")
            if category == "base":
                result = inspect_base_source(source, platform)
                if result["apk"]:
                    result["apk"] = os.path.basename(result["apk"])
            elif category == "application":
                result = inspect_application_source(source, platform)
            elif category == "apk":
                if not platform_capabilities(platform)["apkInstaller"]:
                    raise ValueError("APK 安装包仅适用于 Android")
                apk = find_android_apk(source)
                if not apk: raise ValueError("未找到 APK，请选择具体文件或包含 APK 的目录")
                result = validate_android_apk(apk)
            elif category == "patch":
                root, versions, _cook = patch_import_plan(source, platform)
                result = {"versions": versions}
            else:
                raise ValueError("来源检测支持 base、patch 或 application")
            self._send_json({"ok": True, "platform": platform, "inspection": result})
        except (ValueError, OSError) as exc:
            self._send_json({"error": str(exc)}, status=400)

    def _admin_workspace_import(self):
        data = self._json_body()
        if data is None: return
        if not self.cfg.get("workspace_dir"):
            self._send_json({"error": "请先初始化工作空间"}, status=400); return
        category = data.get("category")
        if category == "patch":
            self._admin_import_payload(data); return
        platform = self._body_platform(data)
        if not platform: return
        caps = platform_capabilities(platform)
        if category == "apk" and not caps["apkInstaller"]:
            self._send_json({"error": "APK 安装包仅适用于 Android"}, status=400); return
        if category == "launcher" and not caps["windowsLauncher"]:
            self._send_json({"error": "Windows 启动器仅适用于 Windows"}, status=400); return
        version = data.get("version", "")
        if not isinstance(version, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", version) or version.startswith("."):
            self._send_json({"error": "请选择有效版本号"}, status=400); return
        if category in ("base", "apk", "application"):
            paths = workspace_platform_paths(self.cfg, platform)
            source = os.path.join(paths["baseImport" if category == "base" else "applicationImport"], version)
            target = os.path.join(paths["bases" if category == "base" else "installers"], version)
        elif category == "launcher":
            paths = workspace_paths(self.cfg)
            source = os.path.join(paths["launcherImport"], version)
            target = os.path.join(paths["launcher"], version)
        else:
            self._send_json({"error": "导入类型必须是 base、application、patch 或 launcher"}, status=400); return
        try:
            if "source" in data:
                if not isinstance(data["source"], str) or not data["source"].strip():
                    raise ValueError("请选择来源")
                source = self._resolve_path(data["source"].strip())
            self._check_import_source(source)
            repair_backup = None
            if os.path.exists(target):
                repairable = False
                if category == "base" and caps["baseKind"] == "resources" and os.path.isdir(target):
                    try:
                        validate_resource_tree(target)
                    except (ValueError, OSError):
                        repairable = True
                if not repairable:
                    self._send_json({"error": "这个版本已经存在，请使用新的版本号"}, status=409); return
                repair_backup = os.path.join(self.cfg["data_dir"], "trash", "invalid-bases", platform, version + "-" + str(time.time_ns()))
            if category in ("apk", "application"):
                self._import_apk_package(source, target, platform, version, data); return
            selected_source = source
            info = inspect_base_source(source, platform) if category == "base" else None
            if info:
                source = info["source"]
            elif not os.path.isfile(os.path.join(source, "Launcher.exe")):
                raise ValueError("启动器导入目录中缺少 Launcher.exe")
            self._check_import_source(source)
            if not os.path.isdir(source) or not os.listdir(source):
                raise ValueError("来源目录不存在或为空")
            # Validate all APK input before publishing either resource or installer.
            apk = info.get("apk") if info else None
            apk_target = os.path.join(self.cfg["workspace_dir"], "installers", platform, version) if apk else None
            if apk_target and os.path.exists(apk_target):
                raise ValueError("同版本安装包已存在，请单独导入不含 APK 的资源目录，或使用新版本号")
            import tempfile
            os.makedirs(os.path.dirname(target), exist_ok=True)
            stage = tempfile.mkdtemp(prefix=".import-", dir=os.path.dirname(target))
            apk_stage = None
            apk_published = False
            try:
                if info and caps["baseKind"] == "resources":
                    for abs_path, rel in iter_resource_files(source):
                        dst = os.path.join(stage, rel)
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        shutil.copy2(abs_path, dst)
                    validate_resource_tree(stage)
                    metadata = os.path.join(info["selectedSource"], "version.json")
                    if os.path.isfile(metadata) and not os.path.isfile(os.path.join(stage, "version.json")):
                        shutil.copy2(metadata, os.path.join(stage, "version.json"))
                else:
                    shutil.copytree(source, stage, dirs_exist_ok=True)
                if category == "base":
                    metadata = {"resourceVersion": version}
                    for field in ("requiredGameVersion", "gameVersion"):
                        value = data.get(field)
                        if value is not None:
                            if not isinstance(value, str) or len(value) > 128: raise ValueError("版本字段无效：" + field)
                            if value.strip(): metadata[field] = value.strip()
                    with open(os.path.join(stage, ".cloudupdate.json"), "w", encoding="utf-8") as handle:
                        json.dump(metadata, handle)
                    self._sync_staged_package(stage, platform, version, "packages")
                if apk:
                    os.makedirs(os.path.dirname(apk_target), exist_ok=True)
                    apk_stage = tempfile.mkdtemp(prefix=".import-apk-", dir=os.path.dirname(apk_target))
                    self._stage_apk(apk, apk_stage, platform, version, dict(data, gameVersion=data.get("gameVersion") or data.get("requiredGameVersion") or version))
                    os.replace(apk_stage, apk_target)
                    apk_published = True
                try:
                    if repair_backup:
                        os.makedirs(os.path.dirname(repair_backup), exist_ok=True)
                        os.replace(target, repair_backup)
                    os.replace(stage, target)
                except OSError:
                    if apk_published:
                        os.replace(apk_target, apk_stage)
                    if repair_backup and os.path.isdir(repair_backup) and not os.path.exists(target):
                        os.replace(repair_backup, target)
                    raise
            finally:
                for temporary in (stage, apk_stage):
                    if temporary and os.path.isdir(temporary): shutil.rmtree(temporary)
            self.cfg = load_config(self.cfg["_config_path"])
            _PROJECT_CFG_CACHE[self.project] = (self.cfg, os.path.getmtime(self.cfg["_config_path"]))
            build_versions_index(self.cfg)
            if category == "base":
                storage = get_storage(self.cfg)
                if storage.is_remote: _sync_versions_to_s3(self.cfg, storage)
                if "source" in data: self._remember_import_source(platform, category, selected_source)
            self._send_json({"ok": True, "message": "已导入 " + category + " / " + version + ("，已保留不合格旧包的备份" if repair_backup else "") + ("，并导入 APK" if apk else "")})
        except (ValueError, OSError) as exc:
            self._send_json({"error": "导入失败：" + str(exc)}, status=400)

    def _sync_staged_package(self, stage, platform, version, category):
        storage = get_storage(self.cfg)
        if not storage.is_remote: return
        if category == "installers":
            files = [f["targetRelativePath"] for f in (_read_json(os.path.join(stage, "installer.json")) or {}).get("files", [])]
        else:
            from manifest import matches_any
            files = [rel for _, rel in iter_resource_files(stage) if rel != ".cloudupdate.json" and not matches_any(rel, self.cfg["manifest_exclude_patterns"])]
        for rel in files:
            if not storage.upload_file(category, src_path=os.path.join(stage, rel), platform=platform, version=version, rel=rel):
                raise ValueError("对象存储上传失败，未发布该包：" + rel)

    def _stage_apk(self, apk_path, stage, platform, version, data=None):
        meta = inspect_application_source(apk_path, platform)
        source = meta["path"]
        if platform == "Windows":
            shutil.copytree(source, stage, dirs_exist_ok=True)
        else:
            shutil.copy2(source, os.path.join(stage, meta["fileName"]))
        data = data or {}
        bundled = data.get("bundledResourceVersion", "") or version
        game = data.get("gameVersion") or version
        for value in (bundled, game):
            if not isinstance(value, str) or not value.strip() or len(value) > 128: raise ValueError("程序版本或内置资源版本无效")
        files = []
        from manifest import hash_file
        for folder, dirs, names in os.walk(stage):
            for name in names:
                path = os.path.join(folder, name)
                rel = os.path.relpath(path, stage).replace("\\", "/")
                if rel == "installer.json": continue
                digest, size = hash_file(path, self.cfg["manifest_hash"])
                files.append({"targetRelativePath": rel, "size": size, "hash": digest})
        files.sort(key=lambda entry: entry["targetRelativePath"])
        descriptor = {"schemaVersion": 2, "platform": platform, "versionId": version,
                      "gameVersion": game.strip(), "resourceVersion": bundled.strip(),
                      "type": "install", "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "files": files,
                      "fileName": meta["fileName"], "size": sum(f["size"] for f in files)}
        with open(os.path.join(stage, "installer.json"), "w", encoding="utf-8") as handle:
            json.dump(descriptor, handle, ensure_ascii=False, indent=2)
        self._sync_staged_package(stage, platform, version, "installers")

    def _import_apk_package(self, source, target, platform, version, data):
        """Stage a platform-specific program without mixing it into resource bases."""
        if os.path.exists(target):
            self._send_json({"error": "该程序包版本已存在，请使用新的版本号"}, status=409); return
        meta = inspect_application_source(source, platform)
        import tempfile
        os.makedirs(os.path.dirname(target), exist_ok=True)
        stage = tempfile.mkdtemp(prefix=".import-application-", dir=os.path.dirname(target))
        try:
            self._stage_apk(meta["path"], stage, platform, version, data)
            os.replace(stage, target)
        finally:
            if os.path.isdir(stage): shutil.rmtree(stage)
        self.cfg = load_config(self.cfg["_config_path"])
        _PROJECT_CFG_CACHE[self.project] = (self.cfg, os.path.getmtime(self.cfg["_config_path"]))
        build_versions_index(self.cfg)
        if "source" in data:
            self._remember_import_source(platform, "application", source)
        self._send_json({"ok": True, "message": "已导入程序包 " + version + " / " + meta["fileName"]})

    def _remember_import_source(self, platform, category, source):
        sources = dict(self.cfg.get("import_sources") or {})
        sources[platform] = dict(sources.get(platform) or {}, **{category: source})
        self.cfg = _apply_config_updates({"import_sources": sources}, self.cfg)

    def _admin_import_payload(self, data):
        platform = self._body_platform(data)
        if not platform: return
        try:
            if "source" in data:
                if not isinstance(data["source"], str) or not data["source"].strip():
                    raise ValueError("请选择补丁包来源目录")
                source = self._resolve_path(data["source"].strip())
                if not os.path.isdir(source):
                    raise ValueError("补丁包来源目录不存在")
                self._check_import_source(source)
                managed = os.path.normcase(os.path.realpath(self.cfg["versions_dir"]))
                selected = os.path.normcase(os.path.realpath(source))
                try:
                    overlapping = os.path.commonpath([managed, selected]) in (managed, selected)
                except ValueError:
                    overlapping = False
                if overlapping:
                    raise ValueError("补丁来源不能与项目内部补丁存储目录重叠")
                options = dict(self.cfg.get("platform_settings") or {})
                options[platform] = dict(options.get(platform) or {}, patchSourceDir=source)
                _do_import_core(dict(self.cfg, platform_settings=options), platform)
                self._remember_import_source(platform, "patch", source)
            else:
                _do_import_core(self.cfg, platform)
            self._send_json({"ok": True, "message": platform + " 补丁导入完成"})
        except (Exception, SystemExit) as exc:
            self._send_json({"error": "补丁导入失败：" + str(exc)}, status=400)

    def _platform_config(self):
        platform = getattr(self, "platform", self.cfg.get("default_platform", self.cfg["platforms"][0]))
        options = self.cfg.get("platform_settings", {}).get(platform, {})
        selected = ((self.cfg.get("import_sources") or {}).get(platform) or {}).get("patch")
        source = self._resolve_path(selected or options.get("patchSourceDir", ""))
        source_ready, source_error = False, ""
        try:
            patch_import_plan(source, platform)
            source_ready = True
        except (ValueError, OSError) as exc:
            source_error = str(exc)
        root = self._resolve_path(options.get("basePackagesRoot", ""))
        packages = self.cfg.get("base_packages", {}).get(platform, {})
        return {"platform": platform, "patchSourceDir": source, "sourceExists": bool(source and os.path.isdir(source)),
                "sourceReady": source_ready, "sourceError": source_error,
                "basePackagesRoot": root, "hotpatcherOrder": options.get("hotpatcherOrder", ""),
                "basePackages": packages, "packageStatus": {v: os.path.isdir(path) for v, path in packages.items()},
                "apiBase": self.project_prefix + "/" + platform + "/api"}

    def _api_platform_config(self):
        self._send_json({"ok": True, "config": self._platform_config()})

    def _api_config(self):
        config = self._public_config()
        config["platformConfig"] = self._platform_config()
        self._send_json({"ok": True, "config": config})

    def _api_enabled_versions(self):
        self._send_json({"ok": True, "enabledVersions": self.cfg.get("enabled_versions", {})})

    def _installer_root(self, platform=None):
        platform = platform or getattr(self, "platform", self.cfg.get("default_platform", self.cfg["platforms"][0]))
        return os.path.join(self.cfg.get("workspace_dir") or "", "installers", platform)

    def _list_installers(self, platform):
        return [dict(item, url=self._public_url(item["url"])) for item in catalog_installers(self.cfg, platform)]

    def _api_installers(self):
        platform = getattr(self, "platform", self.cfg.get("default_platform", self.cfg["platforms"][0]))
        if platform not in self.cfg["platforms"] or not self.cfg.get("workspace_dir"):
            self._send_json({"error": "程序包需要已配置工作空间和有效平台"}, status=400); return
        self._send_json({"ok": True, "platform": platform, "installers": self._list_installers(platform)})

    def _api_installer(self, version_id):
        platform = getattr(self, "platform", self.cfg.get("default_platform", self.cfg["platforms"][0]))
        if platform not in self.cfg["platforms"] or not self.cfg.get("workspace_dir"):
            self._send_json({"error": "程序包需要已配置工作空间和有效平台"}, status=400); return
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", version_id or "") or version_id in (".", ".."):
            self._send_json({"ok": False, "error": "无效版本号"}, status=400); return
        folder = os.path.join(self._installer_root(platform), version_id)
        if not os.path.isdir(folder):
            self._send_json({"ok": False, "error": "安装包不存在"}, status=404); return
        items = [i for i in self._list_installers(platform) if i.get("versionId") == version_id]
        if not items:
            self._send_json({"ok": False, "error": "程序包文件缺失或元数据无效"}, status=404); return
        self._send_json({"ok": True, "installer": items[0]})

    def _serve_installer_file(self, rel):
        """下发安装包：/files/installers/<platform>/<version>/<file>"""
        parts = [p for p in str(rel).replace("\\", "/").split("/") if p and p != "."]
        if len(parts) < 3 or any(".." in p for p in parts):
            self._send_json({"ok": False, "error": "无效安装包路径"}, status=400); return
        platform, version_id, filename = parts[0], parts[1], "/".join(parts[2:])
        try:
            platform = validate_platforms([platform])[0]
        except ValueError:
            self._send_json({"ok": False, "error": "无效平台"}, status=400); return
        if platform not in self.cfg["platforms"] or not self.cfg.get("workspace_dir") or (self.explicit_platform and self.platform != platform):
            self._send_json({"error": "安装包下载平台不符"}, status=400); return
        published = next((i for i in self._list_installers(platform) if i["versionId"] == version_id), None)
        if not published or filename not in {f["targetRelativePath"] for f in published["files"]}:
            self._send_json({"error": "安装包不存在"}, status=404); return
        root = os.path.abspath(self._installer_root(platform))
        target = os.path.abspath(os.path.join(root, version_id, filename))
        if not target.startswith(root + os.sep) and target != root:
            self._send_json({"ok": False, "error": "无效安装包路径"}, status=400); return
        if not os.path.isfile(target):
            self._send_json({"ok": False, "error": "安装包文件不存在"}, status=404); return
        self._send_file(target)

    def _api_dirs_list(self, query):
        """列出服务器本地目录，供管理页面文件夹选择器使用。"""
        if not (self._auth_ok() or self._server_admin_ok()):
            self._send_json({"ok": False, "error": "未授权：需要 Bearer 管理令牌"}, status=401)
            return
        raw = (query.get("path") or [""])[0].strip()
        try:
            p = os.path.abspath(os.path.expanduser(raw)) if raw else ""
        except Exception as exc:
            self._send_json({"ok": False, "error": f"路径无效：{exc}"}, status=400)
            return
        warning = ""
        if p and not os.path.isdir(p):
            # 配置的目录在磁盘上不存在：沿上级目录逐级回退到第一个存在的目录，
            # 让文件夹选择器仍能打开并修正，而不是直接报错导致“选择包目录”卡死。
            cur = p
            while cur and not os.path.isdir(cur):
                nxt = os.path.dirname(cur.rstrip("\\/"))
                if nxt == cur:
                    cur = ""
                    break
                cur = nxt
            if cur and os.path.isdir(cur):
                warning = f"原路径不存在，已回退到：{cur}"
                p = cur
        if not p:
            if os.name == "nt":
                drives = []
                for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
                    d = letter + ":\\"
                    if os.path.isdir(d):
                        drives.append({"name": d.rstrip("\\") + "\\", "path": d, "isDir": True})
                self._send_json({"ok": True, "path": "", "parent": None, "entries": drives, "warning": warning})
                return
            p = "/"
        if not os.path.isdir(p):
            self._send_json({"ok": False, "error": f"路径不存在或不是目录：{p}"}, status=400)
            return
        try:
            names = sorted(os.listdir(p), key=str.lower)
        except OSError as exc:
            self._send_json({"ok": False, "error": f"无法读取目录：{exc}"}, status=400)
            return
        entries = []
        for name in names:
            if name.startswith("."):
                continue
            full = os.path.join(p, name)
            try:
                if os.path.isdir(full):
                    entries.append({"name": name, "path": full, "isDir": True})
                elif query.get("extensions", [""])[0] in (".apk", ".ipa") and name.lower().endswith(query["extensions"][0]) and os.path.isfile(full):
                    entries.append({"name": name, "path": full, "isDir": False})
            except OSError:
                pass
        stripped = p.rstrip("\\/")
        parent = os.path.dirname(stripped) if stripped else None
        if parent == p or not parent:
            parent = None
        self._send_json({"ok": True, "path": p, "parent": parent, "entries": entries, "warning": warning})

    def _public_storage_config(self):
        storage = self.cfg.get("storage") or {}
        s3 = storage.get("s3") or {}
        return {
            "backend": (storage.get("backend") or "local"),
            "s3": {
                "provider": s3.get("provider", "oss"),
                "endpoint": s3.get("endpoint", ""),
                "region": s3.get("region", ""),
                "service": s3.get("service", "s3"),
                "bucket": s3.get("bucket", ""),
                "accessKeyId": s3.get("accessKeyId", ""),
                "secretKeySet": bool(s3.get("secretAccessKey")),
                "prefix": s3.get("prefix", ""),
                "publicBaseUrl": s3.get("publicBaseUrl", ""),
                "addressingStyle": s3.get("addressingStyle", "virtual"),
                "presignExpires": s3.get("presignExpires", 3600),
            },
        }

    def _public_config(self):
        return {
            "host": self.cfg.get("host"),
            "port": self.cfg.get("port"),
            "project": self.cfg.get("project"),
            "projectKey": self.project,
            "projectPrefix": self.project_prefix,
            "projects": self._projects_list(),
            "platforms": self.cfg.get("platforms"),
            "platformCapabilities": {p: platform_capabilities(p) for p in self.cfg["platforms"]},
            "defaultPlatform": self.cfg.get("default_platform", self.cfg["platforms"][0]),
            "workspaceDir": self.cfg.get("workspace_dir", ""),
            "dataDir": self.cfg.get("data_dir"),
            "versionLibraryDir": self.cfg.get("version_library_dir", ""),
            "basePackageDirs": {p: get_base_dir(self.cfg, p) for p in self.cfg["platforms"]},
            "basePackages": self.cfg.get("base_packages", {}),
            "installerVersions": {p: [i["versionId"] for i in catalog_installers(self.cfg, p)] for p in self.cfg["platforms"]},
            "validBaseVersions": load_versions_index(self.cfg).get("baseVersions", {}),
            "platformSettings": self.cfg.get("platform_settings", {}),
            "manifestExcludePatterns": self.cfg.get("manifest_exclude_patterns", []),
            "manifestHash": self.cfg.get("manifest_hash", "md5"),
            "maxUploadMb": self.cfg.get("max_upload_mb", 2048),
            "enabledVersions": self.cfg.get("enabled_versions", {}),
            "launcherVersions": self.cfg.get("launcher_versions", {}),
            "backgroundDir": self.cfg.get("background_dir", ""),
            "storageBackend": "s3" if get_storage(self.cfg).is_remote else "local",
            "storage": self._public_storage_config(),
            "adminTokenSet": bool(self.cfg.get("admin_token")),
        }

    def _api_launcher_version(self):
        """启动器自升级版本信息。data/launcher/version.json: {version, url}"""
        version_path = os.path.join(self.cfg["data_dir"], "launcher", "version.json")
        info = _read_json(version_path)
        if not info:
            self._send_json({"ok": False, "error": "未发布启动器版本"}, status=404)
            return
        info["url"] = self._public_url(get_storage(self.cfg).url_for("launcher", rel="Launcher.exe"))
        self._send_json(info)

    def _api_launcher_publish(self, version):
        """发布启动器版本（由 CLI 子命令 publish-launcher 触发，逻辑见 cmd_publish_launcher）。"""
        pass

    def _api_launcher_runtime(self):
        """运行时资源清单：ui/ + SelfUpdater.exe + launcher_config.json。客户端首次运行据此下载。"""
        root = os.path.join(self.cfg["data_dir"], "launcher")
        storage = get_storage(self.cfg)
        candidates = []
        ui_dir = os.path.join(root, "ui")
        if os.path.isdir(ui_dir):
            for dirpath, _dirnames, filenames in os.walk(ui_dir):
                for name in sorted(filenames):
                    full = os.path.join(dirpath, name)
                    rel = os.path.relpath(full, root).replace("\\", "/")
                    candidates.append((rel, full))
        for name in ("SelfUpdater.exe", "launcher_config.json"):
            full = os.path.join(root, name)
            if os.path.isfile(full):
                candidates.append((name, full))
        files = []
        for rel, full in sorted(candidates):
            digest, size = hash_file(full, "md5")
            files.append({"path": rel, "url": self._public_url(storage.url_for("launcher", rel=rel)), "hash": digest, "size": size})
        digest_ctx = hashlib.md5()
        for f in files:
            digest_ctx.update(f["path"].encode("utf-8")); digest_ctx.update(b"\0")
            digest_ctx.update(str(f["size"]).encode("utf-8")); digest_ctx.update(b"\0")
            digest_ctx.update(f["hash"].encode("utf-8")); digest_ctx.update(b"\0")
        self._send_json({"ok": True, "version": digest_ctx.hexdigest()[:16] if files else "", "files": files})

    def _api_launcher_versions(self):
        versions = self.cfg.get("launcher_versions") or {}
        items = []
        for version, path in sorted(versions.items(), key=lambda kv: version_key(kv[0]), reverse=True):
            resolved = self._resolve_path(path) if path else ""
            items.append({
                "version": version, "path": resolved,
                "exists": os.path.isdir(resolved) if resolved else False,
                "launcherExe": os.path.isfile(os.path.join(resolved, "Launcher.exe")) if resolved else False,
                "selfUpdaterExe": os.path.isfile(os.path.join(resolved, "SelfUpdater.exe")) if resolved else False,
                "exeSize": os.path.getsize(os.path.join(resolved, "Launcher.exe")) if resolved and os.path.isfile(os.path.join(resolved, "Launcher.exe")) else 0,
            })
        published = (_read_json(os.path.join(self.cfg["data_dir"], "launcher", "version.json")) or {}).get("version", "")
        self._send_json({"ok": True, "versions": items, "published": published})

    def _api_launcher_background(self):
        bg_dir = launcher_bg_dir(self.cfg)
        storage = get_storage(self.cfg)
        files = []
        max_mtime = 0
        if os.path.isdir(bg_dir):
            for name in sorted(os.listdir(bg_dir)):
                if not name.lower().endswith(".png"):
                    continue
                path = os.path.join(bg_dir, name)
                digest, size = hash_file(path, self.cfg.get("manifest_hash", "md5"))
                mtime = os.path.getmtime(path)
                max_mtime = max(max_mtime, mtime)
                files.append({"name": name, "url": self._public_url(storage.url_for("background", name=name)), "hash": digest, "size": size})
        meta = _read_json(os.path.join(self.cfg["data_dir"], "launcher", "background.json")) or {}
        fps = int(meta.get("frameFps", 12) or 12)
        digest_ctx = hashlib.md5()
        for f in files:
            digest_ctx.update(f["name"].encode("utf-8")); digest_ctx.update(b"\0")
            digest_ctx.update(str(f["size"]).encode("utf-8")); digest_ctx.update(b"\0")
            digest_ctx.update(f["hash"].encode("utf-8")); digest_ctx.update(b"\0")
        digest_ctx.update(f"fps={fps}".encode("utf-8"))
        version = digest_ctx.hexdigest()[:16] if files else ""
        self._send_json({"ok": True, "version": version, "frameFps": fps,
                         "dir": bg_dir if os.path.isdir(bg_dir) else "", "files": files})

    # ---------- 文件下发（local 模式读取磁盘；remote 模式 URL 已是 presigned，不会走到这里）----------
    def _serve_package_file(self, rel):
        parts = rel.split("/", 2)
        if len(parts) != 3 or parts[0] not in self.cfg["platforms"]:
            self._send_json({"error": "基础包路径应为 平台/版本/文件"}, status=400)
            return
        platform, version, path = parts
        if not is_resource_file(path) or path.replace("\\", "/") == ".cloudupdate.json" or matches_any(path, self.cfg["manifest_exclude_patterns"]):
            self._send_json({"error": "该文件不属于资源包，请通过程序包接口管理程序文件"}, status=400); return
        if getattr(self, "route_platform", None) and platform != self.platform:
            self._send_json({"error": "文件平台与路由不一致"}, status=400)
            return
        absolute = get_storage(self.cfg).local_path("packages", platform=platform, version=version, rel=path)
        if absolute is None or not os.path.isfile(absolute):
            self._send_json({"error": "基础包文件不存在"}, status=404)
            return
        self._send_file(absolute)

    def _serve_version_file(self, rel):
        parts = rel.split("/", 2)
        if len(parts) != 3 or parts[0] not in self.cfg["platforms"] or parts[1] in ("", ".", ".."):
            self._send_json({"error": "补丁路径应为 平台/版本/文件"}, status=400)
            return
        platform, version, path = parts
        if getattr(self, "route_platform", None) and platform != self.platform:
            self._send_json({"error": "文件平台与路由不一致"}, status=400)
            return
        absolute = get_storage(self.cfg).local_path("versions", platform=platform, version=version, rel=path)
        if absolute is None or not os.path.isfile(absolute):
            self._send_json({"error": "补丁文件不存在"}, status=404)
            return
        self._send_file(absolute)

    def _serve_background_file(self, rel):
        abs_path = get_storage(self.cfg).local_path("background", name=os.path.basename(rel))
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        self._send_file(abs_path)

    def _serve_launcher_file(self, rel):
        abs_path = get_storage(self.cfg).local_path("launcher", rel=rel)
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        self._send_file(abs_path)

    # ---------- 网页控制台静态资源 ----------
    def _serve_web_file(self, rel):
        web_dir = self.cfg.get("web_dir") or os.path.join(SERVER_DIR, "web")
        if not os.path.isdir(web_dir) and os.path.isdir(os.path.join(SERVER_DIR, "web")):
            web_dir = os.path.join(SERVER_DIR, "web")
        if not os.path.isdir(web_dir):
            # 子项目配置里的 web_dir 可能解析到了自身目录（projects/<key>/web 不存在），
            # 前端是共享的，回退到 BASE_DIR/web；冻结 exe 下再回退到 _MEIPASS/web。
            fallback = os.path.join(BASE_DIR, "web")
            if os.path.isdir(fallback):
                web_dir = fallback
            else:
                meipass = getattr(sys, "_MEIPASS", "")
                if meipass and os.path.isdir(os.path.join(meipass, "web")):
                    web_dir = os.path.join(meipass, "web")
        if not rel or rel == "index.html":
            rel = "index.html"
        abs_path = safe_join(web_dir, rel)
        if abs_path is None or not os.path.isfile(abs_path):
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)
            return
        # 主页面注入当前项目前缀与项目清单，使前端 API/资源自动走 /<项目>/ 前缀（无需改打包文件）
        if rel == "index.html":
            with open(abs_path, "r", encoding="utf-8") as f:
                text = f.read()
            injection = (
                "<script>window.__PREFIX__=%s;window.__PROJECT__=%s;window.__DEFAULT_PROJECT__=%s;window.__PROJECTS_LIST__=%s;"
                "window.__PLATFORM__=%s;window.__CONSOLE_VERSION__='2.5';"
                "window.__SWITCH_PROJECT__=function(k){if(k)location.pathname='/' + k + '/';};"
                "</script>"
                % (json.dumps(self.project_prefix), json.dumps(self.project),
                   json.dumps(PROJECT_REGISTRY.get("default", "main")),
                   json.dumps(self._projects_list()), json.dumps(getattr(self, "platform", "")))
            )
            if "</head>" in text:
                text = text.replace("</head>", injection + "</head>", 1)
            else:
                text = injection + text
            payload = text.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
            return
        self._send_file(abs_path)

    # ---------- 管理操作（HTTP 接口，需管理令牌）----------
    def _auth_ok(self):
        token = self.cfg.get("admin_token")
        if not token:
            return True
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            auth = auth[len("Bearer "):].strip()
        return auth == token

    def _json_body(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            self._send_json({"ok": False, "error": "空请求体"}, status=400)
            return None
        raw = self.rfile.read(length)
        try:
            data = json.loads(raw.decode("utf-8"))
            if not isinstance(data, dict):
                raise ValueError("请求体必须是 JSON 对象")
            return data
        except Exception as exc:
            self._send_json({"ok": False, "error": f"JSON 解析失败：{exc}"}, status=400)
            return None

    def do_POST(self):
        reload_cfg_if_changed()   # 让手动/外部修改根 config.json 立即生效
        if not self._enter_project():
            return
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        query = parse_qs(parsed.query)
        # 接口启停门控
        if not self._endpoint_allowed(path):
            self._send_json({"ok": False, "error": "该接口已被服务器管理员停用"}, status=403)
            return
        # 项目注册管理需要“服务器管理员令牌”，独立于单个项目的 admin_token
        if path == "/api/projects":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_create_project()
            return
        if path == "/api/projects/default":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_set_default_project()
            return
        if path == "/api/projects/platforms":
            if not self._server_admin_ok():
                self._send_json({"error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_project_platforms()
            return
        if path == "/api/projects/rename":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_rename_project()
            return
        if path == "/api/workspace":
            if not self._server_admin_ok():
                self._send_json({"error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_workspace()
            return
        if path == "/api/server/apis":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._api_server_apis_post()
            return
        if path == "/api/server/cert":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._api_server_cert_post()
            return
        if path == "/api/server/access":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._api_server_access_post()
            return
        if not self._auth_ok():
            self._send_json({"ok": False, "error": "未授权：需要 Bearer 管理令牌"}, status=401)
            return
        if path == "/api/versions/reindex":
            self._admin_reindex()
        elif path == "/api/manifest/generate":
            self._admin_manifest_generate(query)
        elif path == "/api/import/hotpatcher":
            self._admin_import()
        elif path == "/api/enabled_versions":
            self._admin_set_enabled()
        elif path == "/api/upload":
            self._admin_upload(query)
        elif path == "/api/launcher/versions":
            self._admin_launcher_add_version()
        elif path == "/api/launcher/publish":
            self._admin_launcher_publish()
        elif path == "/api/launcher/background/dir":
            self._admin_launcher_bg_dir()
        elif path == "/api/launcher/background/clear":
            self._admin_launcher_bg_clear()
        elif path == "/api/launcher/background":
            self._admin_launcher_bg()
        elif path == "/api/workspace/inspect":
            self._admin_workspace_inspect()
        elif path == "/api/workspace/import":
            self._admin_workspace_import()
        elif path == "/api/platform/config":
            self._admin_platform_config()
        elif path == "/api/config/update":
            self._admin_config_update()
        elif path == "/api/storage/test":
            self._admin_storage_test()
        elif path == "/api/server/restart":
            self._admin_restart()
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    def do_DELETE(self):
        reload_cfg_if_changed()   # 让手动/外部修改根 config.json 立即生效
        if not self._enter_project():
            return
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        query = parse_qs(parsed.query)
        if not self._endpoint_allowed(path):
            self._send_json({"ok": False, "error": "该接口已被服务器管理员停用"}, status=403)
            return
        if path.startswith("/api/projects/"):
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_delete_project(path[len("/api/projects/"):], query)
            return
        if not self._auth_ok():
            self._send_json({"ok": False, "error": "未授权：需要 Bearer 管理令牌"}, status=401)
            return
        if path.startswith("/api/version/"):
            self._admin_delete_version(path[len("/api/version/"):], query["platform"][0], query.get("kind", [None])[0])
        elif path.startswith("/api/launcher/versions/"):
            self._admin_delete_launcher_version(path[len("/api/launcher/versions/"):])
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    # ---------- 多项目：请求级项目解析与项目注册管理 ----------
    def _enter_project(self):
        """项目和平台路由在请求入口统一解析，未指定时使用项目默认平台。"""
        reload_projects_if_changed()
        parsed = urlparse(self.path)
        key, rest = resolve_project_path(unquote(parsed.path))
        cfg = load_project_cfg(key)
        if cfg is None:
            self._send_json({"error": "项目不存在"}, status=404)
            return False
        self.project = key
        self.cfg = cfg
        default = PROJECT_REGISTRY.get("default", "main")
        root_behavior = (UpdateHandler.cfg or {}).get("access", {}).get("rootBehavior", "redirect")
        self.project_prefix = "" if key == default and (root_behavior == "serve_at_root" or key == "main") else "/" + key
        parts = rest.strip("/").split("/")
        route_platform = None
        if len(parts) >= 2 and parts[0] == "platforms":
            route_platform = parts[1]
            rest = "/" + "/".join(parts[2:])
        elif len(parts) >= 2 and parts[0] not in reserved_prefixes() and parts[1] in ("api", "files"):
            route_platform = parts[0]
            rest = "/" + "/".join(parts[1:])
        query = parse_qs(parsed.query, keep_blank_values=True)
        supplied = ([route_platform] if route_platform else []) + query.get("platform", [])
        if self.headers.get("X-Platform"):
            supplied.append(self.headers["X-Platform"])
        canonical = {p.lower(): p for p in cfg["platforms"]}
        selected = []
        for value in supplied:
            platform = canonical.get(value.lower())
            if not platform:
                self._send_json({"error": "项目未配置平台：" + value, "platforms": cfg["platforms"]}, status=400)
                return False
            selected.append(platform)
        if len(set(selected)) > 1:
            self._send_json({"error": "路径、查询参数和请求头的平台不一致"}, status=400)
            return False
        self.platform = selected[0] if selected else cfg.get("default_platform", cfg["platforms"][0])
        self.route_platform = route_platform
        if (rest.startswith("/api/launcher/") or rest.startswith("/files/launcher/") or rest.startswith("/files/background/")) and not platform_capabilities(self.platform)["windowsLauncher"]:
            self._send_json({"error": "Windows 启动器仅适用于 Windows"}, status=400)
            return False
        self.explicit_platform = bool(supplied)
        query["platform"] = [self.platform]
        from urllib.parse import urlencode
        self.path = rest + "?" + urlencode(query, doseq=True)
        return True

    def _resolve_path(self, path):
        return resolve_server_path(path, os.path.dirname(self.cfg["_config_path"]))

    def _body_platform(self, data):
        value = data.get("platform", getattr(self, "platform", self.cfg.get("default_platform", self.cfg["platforms"][0])))
        canonical = {p.lower(): p for p in self.cfg["platforms"]}
        platform = canonical.get(str(value).lower())
        if not platform or (getattr(self, "explicit_platform", False) and platform != self.platform):
            self._send_json({"error": "请求平台未配置或与路由平台不一致"}, status=400)
            return None
        return platform

    def _public_url(self, url):
        """把根相对下载链接（/files/...）按当前项目前缀改写，避免跨项目串味。"""
        if url and url.startswith("/") and not url.startswith("//"):
            return self.project_prefix + url
        return url

    def _listen_port(self):
        """返回服务器真实监听端口（所有项目共用同一端口），而非各子项目配置里的 port。"""
        try:
            srv = getattr(self, "server", None)
            if srv is not None:
                return srv.server_address[1]
        except Exception:
            pass
        return self.cfg.get("port", 8710)

    def _projects_list(self):
        default = PROJECT_REGISTRY.get("default", "main")
        return [{
            "key": k,
            "display": (v.get("display") or k),
            "prefix": "" if (k == default and default == "main") else "/" + k,
            "isDefault": k == default,
        } for k, v in PROJECT_REGISTRY.get("projects", {}).items()]

    def _server_admin_ok(self):
        """项目注册管理（增删）需要“服务器管理员令牌”：优先 projects.json 的 server_admin_token，
        否则回退到默认项目的 admin_token。两者皆空则放行（与 _auth_ok 一致）。"""
        token = PROJECT_REGISTRY.get("server_admin_token") or ""
        if not token:
            try:
                token = load_project_cfg(PROJECT_REGISTRY.get("default", "main")).get("admin_token", "")
            except Exception:
                token = ""
        if not token:
            return True
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            auth = auth[len("Bearer "):].strip()
        return auth == token

    def _endpoint_allowed(self, path):
        """接口启停门控（服务器级，对所有项目统一生效）：
        接口配置写在根 config（UpdateHandler.cfg），故此处必须读全局配置而非 self.cfg——
        否则仅默认项目受控，子项目仍会绕开门控。未登记（核心/服务器管理接口）一律放行。"""
        ep = match_endpoint(self.command, path)
        if ep is None:
            return True
        overrides = (UpdateHandler.cfg or {}).get("api_endpoints") or {}
        if ep["id"] in overrides:
            return bool(overrides[ep["id"]])
        return bool(ep.get("enabled", True))

    # ---------- 服务器管理：接口管理 / 证书管理 / 访问格式管理 ----------
    def _api_server_apis(self):
        overrides = (self.cfg or {}).get("api_endpoints") or {}
        out = []
        for ep in API_ENDPOINTS:
            st = overrides.get(ep["id"])
            enabled = bool(st) if st is not None else bool(ep.get("enabled", True))
            out.append({"id": ep["id"], "method": ep["method"], "path": ep["path"],
                        "scope": ep["scope"], "name": ep["name"], "desc": ep["desc"],
                        "enabled": enabled})
        self._send_json({"ok": True, "endpoints": out})

    def _api_server_apis_post(self):
        data = self._json_body()
        if data is None:
            return
        toggles = data.get("toggles")
        if not isinstance(toggles, dict):
            self._send_json({"ok": False, "error": "toggles 必须是 {接口id: true/false}"}, status=400)
            return
        known = {ep["id"] for ep in API_ENDPOINTS}
        clean = {k: bool(v) for k, v in toggles.items() if k in known}
        if not clean:
            self._send_json({"ok": False, "error": "没有有效的接口 id"}, status=400)
            return
        # 防御性合并：以磁盘现有 api_endpoints 为基础，仅覆盖本次提交的接口 id，
        # 避免任何情况下（包括前端只提交部分开关）丢失其它接口已保存的启停状态。
        existing = dict((UpdateHandler.cfg or {}).get("api_endpoints") or {})
        existing.update(clean)
        _apply_config_updates({"api_endpoints": existing}, UpdateHandler.cfg)
        self._send_json({"ok": True, "message": "接口启停已保存（立即生效）", "api_endpoints": clean})

    def _cert_info(self, cert_path):
        try:
            from cryptography import x509
            with open(cert_path, "rb") as f:
                cert = x509.load_pem_x509_certificate(f.read())
            cn = None
            try:
                cn = cert.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)[0].value
            except Exception:
                pass
            issuer = None
            try:
                issuer = cert.issuer.get_attributes_for_oid(x509.NameOID.COMMON_NAME)[0].value
            except Exception:
                pass
            try:
                not_after = cert.not_valid_after_utc
            except AttributeError:
                not_after = cert.not_valid_after
            try:
                not_before = cert.not_valid_before_utc
            except AttributeError:
                not_before = cert.not_valid_before
            now = datetime.datetime.now(datetime.timezone.utc)
            na = not_after
            if na.tzinfo is None:
                na = na.replace(tzinfo=datetime.timezone.utc)
            return {"commonName": cn, "issuer": issuer,
                    "notBefore": not_before.isoformat(),
                    "notAfter": na.isoformat(),
                    "daysLeft": (na - now).days,
                    "selfSigned": bool(cn and cn == issuer)}
        except Exception as exc:
            return {"error": str(exc)}

    def _api_server_cert(self):
        https = dict((self.cfg or {}).get("https") or {})
        info = {"enabled": bool(https.get("enabled", False)),
                "mode": "off",
                "certFile": https.get("certFile", ""),
                "keyFile": https.get("keyFile", ""),
                "commonName": https.get("commonName", ""),
                "country": https.get("country", "CN"),
                "autoGenSelfSigned": bool(https.get("autoGenSelfSigned", True))}
        cert_path = None
        key_path = info["keyFile"]
        if info["enabled"]:
            try:
                cf, kf = certgen.load_cert_pair(https.get("certFile", ""), https.get("keyFile", ""))
                cert_path, key_path = cf, kf
            except RuntimeError:
                if https.get("autoGenSelfSigned", True):
                    ssl_dir = os.path.join(self.cfg.get("data_dir", ""), "ssl")
                    try:
                        cf, kf = certgen.ensure_self_signed_cert(ssl_dir, https.get("commonName", "CloudUpdate"), https.get("country", "CN"))
                        cert_path, key_path = cf, kf
                    except Exception:
                        pass
        if cert_path and os.path.isfile(cert_path):
            ci = self._cert_info(cert_path)
            info.update(ci)
            info["mode"] = "selfsigned" if ci.get("selfSigned") else "custom"
            info["certFile"] = cert_path
            info["keyFile"] = key_path
        self._send_json({"ok": True, "cert": info})

    def _api_server_cert_post(self):
        data = self._json_body()
        if data is None:
            return
        https = dict((self.cfg or {}).get("https") or {})
        action = str(data.get("action", "enable")).strip()
        if action == "disable":
            https["enabled"] = False
        elif action == "enable":
            https["enabled"] = True
        elif action == "setCustom":
            https["enabled"] = True
            https["autoGenSelfSigned"] = False
            https["certFile"] = str(data.get("certFile", ""))
            https["keyFile"] = str(data.get("keyFile", ""))
            try:
                certgen.load_cert_pair(https["certFile"], https["keyFile"])
            except RuntimeError as e:
                self._send_json({"ok": False, "error": f"证书文件无效：{e}"}, status=400)
                return
        elif action == "regenerate":
            https["enabled"] = True
            https["autoGenSelfSigned"] = True
            commonName = str(data.get("commonName") or https.get("commonName") or "CloudUpdate")
            country = str(data.get("country") or https.get("country") or "CN")
            ssl_dir = os.path.join(self.cfg.get("data_dir", ""), "ssl")
            for fn in ("server.crt", "server.key"):
                p = os.path.join(ssl_dir, fn)
                if os.path.isfile(p):
                    try:
                        os.remove(p)
                    except OSError:
                        pass
            cf, kf = certgen.ensure_self_signed_cert(ssl_dir, commonName, country)
            https["certFile"] = cf
            https["keyFile"] = kf
            https["commonName"] = commonName
            https["country"] = country
        else:
            self._send_json({"ok": False, "error": f"未知 action: {action}"}, status=400)
            return
        _apply_config_updates({"https": https}, UpdateHandler.cfg)
        self._send_json({"ok": True, "message": "证书配置已保存，HTTPS 将在重启服务器后生效", "https": https})

    def _api_server_access(self):
        access = dict((self.cfg or {}).get("access") or {})
        access.setdefault("prefixPattern", "/{key}/")
        access.setdefault("reservedPrefixes", list(RESERVED_PREFIXES))
        access.setdefault("rootBehavior", "redirect")
        access.setdefault("defaultRedirect", access.get("rootBehavior", "redirect") == "redirect")
        self._send_json({"ok": True, "access": access})

    def _api_server_access_post(self):
        data = self._json_body()
        if data is None:
            return
        cur = dict((self.cfg or {}).get("access") or {})
        if "rootBehavior" in data:
            rb = str(data["rootBehavior"]).strip()
            if rb not in ("redirect", "serve_at_root"):
                self._send_json({"ok": False, "error": "rootBehavior 仅支持 redirect 或 serve_at_root"}, status=400)
                return
            cur["rootBehavior"] = rb
        if "reservedPrefixes" in data:
            rp = data["reservedPrefixes"]
            if not isinstance(rp, list):
                self._send_json({"ok": False, "error": "reservedPrefixes 必须是字符串数组"}, status=400)
                return
            rp = [str(x).strip() for x in rp if str(x).strip()]
            if not rp:
                self._send_json({"ok": False, "error": "reservedPrefixes 不能为空（至少保留 api/web/files）"}, status=400)
                return
            for must in ("api", "web", "files"):
                if must not in rp:
                    rp.append(must)
            cur["reservedPrefixes"] = rp
        if "prefixPattern" in data:
            cur["prefixPattern"] = str(data["prefixPattern"])
        _apply_config_updates({"access": cur}, UpdateHandler.cfg)
        self._send_json({"ok": True, "message": "访问格式配置已保存（rootBehavior/保留前缀立即生效）", "access": cur})

    def _projects_detail(self):
        """返回每个项目的“空间”详情：相对数据目录、版本数、访问地址等。"""
        default = PROJECT_REGISTRY.get("default", "main")
        out = []
        for k, v in PROJECT_REGISTRY.get("projects", {}).items():
            entry = {
                "key": k,
                "display": v.get("display") or k,
                "prefix": "" if (k == default and default == "main") else "/" + k,
                "isDefault": k == default,
                "configPath": v.get("config", ""),
                "dataDir": "",
                "versionCount": 0,
            }
            try:
                cfg = load_project_cfg(k)
            except Exception:
                cfg = None
            if cfg:
                entry["platforms"] = cfg["platforms"]
                entry["defaultPlatform"] = cfg.get("default_platform", cfg["platforms"][0])
                cfg_path = cfg.get("_config_path", "")
                cfg_dir = os.path.dirname(os.path.abspath(cfg_path)) if cfg_path else SERVER_DIR
                entry["workspaceDir"] = os.path.relpath(cfg["workspace_dir"], SERVER_DIR).replace("\\", "/") if cfg.get("workspace_dir") else ""
                dd = cfg.get("data_dir") or ""
                if dd:
                    data_abs = os.path.abspath(os.path.join(cfg_dir, dd))
                    entry["dataDir"] = os.path.relpath(data_abs, SERVER_DIR).replace("\\", "/")
                try:
                    idx = load_versions_index(cfg)
                    entry["versionCount"] = len(idx.get("versions") or [])
                except Exception:
                    entry["versionCount"] = 0
            out.append(entry)
        return out

    def _api_projects_get(self):
        self._send_json({
            "ok": True,
            "current": self.project,
            "default": PROJECT_REGISTRY.get("default", "main"),
            "projects": self._projects_detail(),
        })

    def _sync_launcher_config(self, key, template=None):
        """同步客户端启动器配置到本项目：复制模板项目的 launcher 运行时资产到
        本项目 workspace/data/launcher/，并把 launcher_config.json 的 serverUrl 追加 /<key> 前缀，
        否则本项目客户端会下载到默认项目的 launcher_config（serverUrl 无前缀）而串到默认项目。"""
        if "Windows" not in load_project_cfg(key)["platforms"]:
            return
        source_cfg = load_project_cfg(template or PROJECT_REGISTRY.get("default", "main"))
        src = os.path.join(source_cfg["data_dir"], "launcher")
        if not os.path.isdir(src):
            return
        dst = os.path.join(load_project_cfg(key)["data_dir"], "launcher")
        if os.path.isdir(dst) and os.listdir(dst):
            return  # 已存在则保留（可能已手动调整）
        import shutil
        try:
            shutil.copytree(src, dst, dirs_exist_ok=True)
        except Exception:
            return
        cfg_path = os.path.join(dst, "launcher_config.json")
        if not os.path.isfile(cfg_path):
            return
        # 新项目的 serverUrl 基于请求 Host 的“服务器根地址”（兄弟关系），
        # 不再拼接默认项目 URL，避免子项目前缀被嵌套成 /codebuild/cc。
        host = self.headers.get("Host", "")
        base = ("http://" + host.strip()) if host.strip() else ""
        if not base:
            return
        new_url = base.rstrip("/") + "/" + key
        try:
            with open(cfg_path, encoding="utf-8-sig") as f:
                cc = json.load(f) or {}
            cc["serverUrl"] = new_url
            with open(cfg_path, "w", encoding="utf-8-sig") as f:
                json.dump(cc, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _admin_create_project(self):
        data = self._json_body()
        if data is None:
            return
        key = str(data.get("key", "")).strip()
        display = str(data.get("display", "")).strip() or key
        copy_from = str(data.get("copyFrom", "")).strip()
        if not re.match(r"^[A-Za-z0-9_]+$", key):
            self._send_json({"ok": False, "error": "项目名只能包含字母、数字、下划线"}, status=400)
            return
        if key in reserved_prefixes():
            self._send_json({"ok": False, "error": f"项目名 '{key}' 为保留前缀，不可使用"}, status=400)
            return
        if key in PROJECT_REGISTRY.get("projects", {}):
            self._send_json({"ok": False, "error": f"项目 '{key}' 已存在"}, status=400)
            return
        # 选择模板配置：copyFrom 指定项目，否则默认项目
        projects = PROJECT_REGISTRY.get("projects", {})
        if copy_from and copy_from in projects:
            src_cfg_path = os.path.abspath(os.path.join(SERVER_DIR, projects[copy_from]["config"]))
        else:
            src_cfg_path = os.path.abspath(os.path.join(SERVER_DIR, projects[PROJECT_REGISTRY.get("default", "main")]["config"]))
        src = _read_json(src_cfg_path) or {}
        try:
            platforms = validate_platforms(data.get("platforms", src.get("platforms", ["Windows"])))
            default_platform = data.get("defaultPlatform", platforms[0])
            if default_platform not in platforms:
                raise ValueError("默认平台必须在平台列表中")
        except ValueError as exc:
            self._send_json({"error": str(exc)}, status=400)
            return
        # 构造新项目配置：数据目录互相隔离；host/port 由统一监听端口决定，不在此设置
        storage = json.loads(json.dumps(src.get("storage", {"backend": "local", "s3": {}})))
        if storage.get("backend") == "s3":
            options = storage.setdefault("s3", {})
            options["prefix"] = (options.get("prefix", "").strip("/") + "/projects/" + key).lstrip("/")
        new_cfg = {
            "project": key,
            "platforms": platforms,
            "default_platform": default_platform,
            "workspace_dir": os.path.relpath(os.path.join(SERVER_DIR, "workspace", key), os.path.join(SERVER_DIR, "projects", key)),
            "web_dir": os.path.abspath(os.path.join(SERVER_DIR, "web")),
            "platform_settings": {p: {"hotpatcherOrder": ""} for p in platforms},
            "enabled_versions": {},
            "admin_token": src.get("admin_token", ""),
            "storage": storage,
            "manifest_hash": src.get("manifest_hash", "md5"),
            "manifest_exclude_patterns": src.get("manifest_exclude_patterns", []),
            "max_upload_mb": src.get("max_upload_mb", 2048),
        }
        proj_dir = os.path.join(SERVER_DIR, "projects", key)
        os.makedirs(proj_dir, exist_ok=True)
        cfg_path = os.path.join(proj_dir, "config.json")
        with open(cfg_path, "w", encoding="utf-8") as f:
            json.dump(new_cfg, f, ensure_ascii=False, indent=2)
        PROJECT_REGISTRY.setdefault("projects", {})[key] = {"display": display, "config": f"projects/{key}/config.json"}
        save_registry()
        ensure_dirs(load_project_cfg(key))
        self._sync_launcher_config(key, copy_from or None)
        self._send_json({"ok": True,
                         "message": f"已创建项目 {display}（{key}），访问地址：/{key}/",
                         "key": key, "prefix": "/" + key})

    def _admin_set_default_project(self):
        """设置默认项目（访问根 / 时进入的项目）。需要服务器管理员令牌。"""
        data = self._json_body()
        if data is None:
            return
        key = str(data.get("key", "")).strip()
        if key not in PROJECT_REGISTRY.get("projects", {}):
            self._send_json({"ok": False, "error": f"项目 '{key}' 不存在"}, status=404)
            return
        PROJECT_REGISTRY["default"] = key
        save_registry()
        self._send_json({"ok": True, "message": f"已将 {key} 设为默认项目（根 / 进入该项目）"})

    def _admin_delete_project(self, key, query=None):
        key = unquote(key).strip("/")
        cleanup = bool(query) and query.get("cleanup", ["0"])[0] in ("1", "true", "yes")
        if key == PROJECT_REGISTRY.get("default", "main"):
            self._send_json({"ok": False, "error": "默认项目不可删除"}, status=400)
            return
        if key not in PROJECT_REGISTRY.get("projects", {}):
            self._send_json({"ok": False, "error": f"项目 '{key}' 不存在"}, status=404)
            return
        PROJECT_REGISTRY["projects"].pop(key, None)
        _PROJECT_CFG_CACHE.pop(key, None)
        save_registry()
        msg = f"已注销项目 {key}"
        if cleanup:
            disk = os.path.join(SERVER_DIR, "projects", key)
            if os.path.isdir(disk):
                import shutil
                shutil.rmtree(disk, ignore_errors=True)
                msg += "（已删除项目空间目录）"
        self._send_json({"ok": True, "message": msg})

    def _admin_rename_project(self):
        """重命名项目：可仅改显示名，或连同标识(key/URL前缀)一起改。
        改标识时会迁移 projects/<old_key>/ 目录到 projects/<new_key>/（默认项目配置在根目录则不移动）；
        并同步修正该项目启动器配置的 serverUrl，保持兄弟关系。"""
        data = self._json_body()
        if data is None:
            return
        old_key = str(data.get("key", "")).strip()
        new_key = str(data.get("newKey", "")).strip()
        display = str(data.get("display", "")).strip()
        projects = PROJECT_REGISTRY.get("projects", {})
        if old_key not in projects:
            self._send_json({"ok": False, "error": f"项目 '{old_key}' 不存在"}, status=404)
            return
        # 仅更新显示名称
        if not new_key or new_key == old_key:
            if display:
                projects[old_key]["display"] = display
                save_registry()
                self._send_json({"ok": True, "message": f"已更新显示名：{display}"})
            else:
                self._send_json({"ok": False, "error": "未提供新标识或显示名"}, status=400)
            return
        if not re.match(r"^[A-Za-z0-9_]+$", new_key):
            self._send_json({"ok": False, "error": "新标识只能包含字母、数字、下划线"}, status=400)
            return
        if new_key in reserved_prefixes():
            self._send_json({"ok": False, "error": f"标识 '{new_key}' 为保留前缀，不可使用"}, status=400)
            return
        if new_key in projects:
            self._send_json({"ok": False, "error": f"标识 '{new_key}' 已存在"}, status=400)
            return
        # 迁移磁盘目录（仅当项目配置位于 projects/<old_key>/ 下时）
        old_dir = os.path.join(SERVER_DIR, "projects", old_key)
        new_dir = os.path.join(SERVER_DIR, "projects", new_key)
        moved = False
        if os.path.isdir(old_dir):
            if os.path.exists(new_dir):
                self._send_json({"ok": False, "error": f"目标目录已存在：projects/{new_key}"}, status=400)
                return
            import shutil
            try:
                shutil.move(old_dir, new_dir)
            except Exception as exc:
                self._send_json({"ok": False, "error": f"目录迁移失败：{exc}"}, status=500)
                return
            moved = True
        # 更新注册表（默认项目配置在根目录，config 路径保持不变）
        entry = projects.pop(old_key)
        if display:
            entry["display"] = display
        cfg_rel = (entry.get("config") or "").replace("\\", "/")
        if cfg_rel.startswith(f"projects/{old_key}/"):
            entry["config"] = f"projects/{new_key}/config.json"
        projects[new_key] = entry
        if PROJECT_REGISTRY.get("default") == old_key:
            PROJECT_REGISTRY["default"] = new_key
        _PROJECT_CFG_CACHE.pop(old_key, None)
        save_registry()
        load_project_cfg(new_key)
        # 同步修正该项目启动器配置的 serverUrl（兄弟关系，避免串项目）
        self._rewrite_launcher_serverurl(old_key, new_key, moved)
        self._send_json({"ok": True,
                         "message": f"已重命名 {old_key} → {new_key}（目录迁移：{moved}）",
                         "key": new_key, "prefix": "/" + new_key})

    def _rewrite_launcher_serverurl(self, old_key, new_key, moved):
        """重命名后修正该项目启动器配置里的 serverUrl 末尾前缀段。"""
        cfg_path = os.path.join(load_project_cfg(new_key)["data_dir"], "launcher", "launcher_config.json")
        if not os.path.isfile(cfg_path):
            return
        try:
            with open(cfg_path, encoding="utf-8-sig") as f:
                lc = json.load(f) or {}
            url = lc.get("serverUrl", "")
            # 鲁棒替换末尾的 /<old_key> 或 /<old_key>/（serverUrl 可能带或不带尾斜杠）
            sfx1 = "/" + old_key
            sfx2 = sfx1 + "/"
            new_url = None
            if url.endswith(sfx2):
                new_url = url[: -len(sfx2)] + sfx2.replace(old_key, new_key)
            elif url.endswith(sfx1):
                new_url = url[: -len(sfx1)] + sfx1.replace(old_key, new_key)
            if new_url and new_url != url:
                lc["serverUrl"] = new_url
                with open(cfg_path, "w", encoding="utf-8-sig") as f:
                    json.dump(lc, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _admin_reindex(self):
        try:
            idx = build_versions_index(self.cfg)
            self._send_json({"ok": True, "message": f"索引已重建：当前 {idx['current']}"})
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)

    def _admin_manifest_generate(self, query):
        try:
            platform = query.get("platform", [self.cfg["platforms"][0]])[0]
            base_version = query.get("baseVersion", [""])[0] or None
            m = generate_manifest(self.cfg, platform, base_version=base_version, force=True)
            self._send_json({"ok": True, "message": f"已生成清单 {platform}：{m['fileCount']} 个文件"})
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)

    def _admin_import(self):
        data = self._json_body() if int(self.headers.get("Content-Length", "0") or "0") else {}
        if data is not None:
            self._admin_import_payload(data)

    def _admin_set_enabled(self):
        data = self._json_body()
        if data is None:
            return
        platform = self._body_platform(data)
        if not platform:
            return
        versions = data.get("versions")
        if not platform or not isinstance(versions, list):
            self._send_json({"ok": False, "error": "缺少 platform 或 versions"}, status=400)
            return
        ids = [str(x).strip() for x in versions if str(x).strip()]
        ev = dict(self.cfg.get("enabled_versions") or {})
        ev[platform] = ids
        self.cfg = _apply_config_updates({"enabled_versions": ev}, self.cfg)
        index = load_versions_index(self.cfg)
        filtered = filter_index_by_enabled(filter_index_by_platform(index, platform), self.cfg)
        client_versions = [v.get("versionId") for v in (filtered.get("versions") or [])]
        self._send_json({"ok": True, "message": f"已开放 {platform}：{ids}", "clientVersions": client_versions})

    def _admin_delete_version(self, version_id, platform, kind=None):
        try:
            ok, msg = delete_version(self.cfg, version_id, platform, kind)
            self.cfg = load_config(self.cfg["_config_path"])
            self._send_json({"ok": ok, "message": msg}, status=200 if ok else (404 if "不存在" in msg else 400))
        except (ValueError, OSError) as exc:
            self._send_json({"error": "删除失败：" + str(exc)}, status=400)

    def _admin_upload(self, query):
        target = query.get("target", [""])[0]
        if target == "package" and query.get("kind", ["base"])[0] != "base":
            self._send_json({"error": "请通过程序包导入管理，不可上传到资源基础包"}, status=400); return
        if target in ("launcher", "background") and not platform_capabilities(self.platform)["windowsLauncher"]:
            self._send_json({"error": "Windows 启动器仅适用于 Windows"}, status=400); return
        if target not in ("package", "version", "launcher", "background"):
            self._send_json({"ok": False, "error": "target 仅支持 package/version/launcher/background"}, status=400)
            return
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            self._send_json({"ok": False, "error": "空请求体"}, status=400)
            return
        body = self.rfile.read(length)
        parts = parse_multipart(self.headers.get("Content-Type", ""), body)
        if not parts:
            self._send_json({"ok": False, "error": "未解析到上传文件（需 multipart/form-data，字段名 file）"}, status=400)
            return
        max_mb = int(self.cfg.get("max_upload_mb", 2048) or 2048)
        storage = get_storage(self.cfg)
        saved = []
        for name, filename, content in parts:
            if name != "file" or not filename:
                continue
            if len(content) > max_mb * 1024 * 1024:
                self._send_json({"ok": False, "error": f"文件超过上限 {max_mb}MB"}, status=413)
                return
            kw = self._upload_kw(target, query, filename)
            if kw is None:
                self._send_json({"ok": False, "error": "参数不完整（缺少 platform/versionId 等）"}, status=400)
                return
            if target in ("package", "version"):
                rel = kw["rel"].replace("\\", "/")
                resource_rel = rel[:-6] if target == "version" and rel.lower().endswith(".patch") else rel
                if not is_resource_file(resource_rel) or rel == ".cloudupdate.json":
                    self._send_json({"error": "资源上传不能包含程序包或原生二进制，请使用程序包导入"}, status=400); return
            category = {"package":"packages", "version":"versions"}.get(target, target)
            import tempfile
            tmp = tempfile.NamedTemporaryFile(delete=False, suffix=os.path.splitext(filename)[1])
            try:
                tmp.write(content)
                tmp.close()
                if storage.upload_file(category, src_path=tmp.name, **kw) is False:
                    self._send_json({"error": "上传路径无效"}, status=400)
                    return
                saved.append(filename)
            finally:
                if os.path.exists(tmp.name):
                    try:
                        os.remove(tmp.name)
                    except OSError:
                        pass
        if not saved:
            self._send_json({"ok": False, "error": "没有可保存的文件"}, status=400)
            return
        if target == "package":
            generate_manifest(self.cfg, kw["platform"], kw["version"], force=True)
            build_versions_index(self.cfg)
        self._send_json({"ok": True, "message": f"已上传 {len(saved)} 个文件", "note": ", ".join(saved)})

    def _upload_kw(self, target, query, filename):
        if target == "package":
            platform = query.get("platform", [self.cfg["platforms"][0]])[0]
            base_version = query.get("baseVersion", [""])[0] or ""
            path = query.get("path", [""])[0] or ""
            rel = (path + "/" + filename) if path else filename
            return {"platform": platform, "version": base_version, "rel": rel}
        if target == "version":
            version_id = query.get("versionId", [""])[0] or ""
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", version_id) or version_id in (".", ".."):
                return None
            platform = query["platform"][0]
            path = query.get("path", ["files"])[0] or "files"
            rel = (path + "/" + os.path.basename(filename)) if path else os.path.basename(filename)
            return {"platform": platform, "version": version_id, "rel": rel}
        if target == "launcher":
            path = query.get("path", [""])[0] or filename
            return {"rel": path}
        if target == "background":
            return {"name": filename}
        return None

    def _admin_launcher_add_version(self):
        data = self._json_body()
        if data is None:
            return
        version = (data.get("version") or "").strip()
        dirpath = (data.get("dir") or "").strip()
        if not version or not dirpath:
            self._send_json({"ok": False, "error": "缺少 version 或 dir"}, status=400)
            return
        lv = dict(self.cfg.get("launcher_versions") or {})
        lv[version] = dirpath
        self.cfg = _apply_config_updates({"launcher_versions": lv}, self.cfg)
        self._send_json({"ok": True, "message": f"已添加启动器版本 {version} -> {dirpath}"})

    def _admin_delete_launcher_version(self, version):
        version = unquote(version)
        lv = dict(self.cfg.get("launcher_versions") or {})
        if version not in lv:
            self._send_json({"ok": False, "error": f"启动器版本 {version} 不存在"}, status=404)
            return
        if self.cfg.get("workspace_dir"):
            source = lv[version]
            expected = os.path.join(workspace_paths(self.cfg)["launcher"], version)
            if os.path.abspath(source) != os.path.abspath(expected):
                self._send_json({"ok": False, "error": "启动器版本目录不属于工作空间"}, status=400)
                return
            target = os.path.join(self.cfg["data_dir"], "trash", "launcher", version + "-" + str(time.time_ns()))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.move(source, target)
            self.cfg = load_config(self.cfg["_config_path"])
            self._send_json({"ok": True, "message": f"启动器版本 {version} 已移入回收站"})
            return
        lv.pop(version, None)
        self.cfg = _apply_config_updates({"launcher_versions": lv}, self.cfg)
        self._send_json({"ok": True, "message": f"已移除启动器版本 {version}"})

    def _admin_launcher_publish(self):
        data = self._json_body()
        if data is None:
            return
        version = (data.get("version") or "").strip()
        if not version:
            self._send_json({"ok": False, "error": "缺少 version"}, status=400)
            return
        try:
            ns = type("NS", (), {"version": version})()
            cmd_publish_launcher(ns, self.cfg)
            self._send_json({"ok": True, "message": f"启动器版本已发布：{version}"})
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)

    def _admin_launcher_bg_dir(self):
        data = self._json_body()
        if data is None:
            return
        dirpath = (data.get("dir") or "").strip()
        if not dirpath:
            self._send_json({"ok": False, "error": "缺少 dir"}, status=400)
            return
        self.cfg = _apply_config_updates({"background_dir": dirpath}, self.cfg)
        self._send_json({"ok": True, "message": f"已设置背景目录：{dirpath}"})

    def _admin_launcher_bg(self):
        data = self._json_body()
        if data is None:
            return
        try:
            fps = int(data.get("frameFps"))
        except (TypeError, ValueError):
            self._send_json({"ok": False, "error": "frameFps 必须为整数"}, status=400)
            return
        if fps <= 0:
            self._send_json({"ok": False, "error": "frameFps 必须大于 0"}, status=400)
            return
        bg_json = os.path.join(self.cfg["data_dir"], "launcher", "background.json")
        meta = _read_json(bg_json) or {}
        meta["frameFps"] = fps
        os.makedirs(os.path.dirname(bg_json), exist_ok=True)
        with open(bg_json, "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        self._send_json({"ok": True, "message": f"背景帧率已设为 {fps} FPS"})

    def _admin_launcher_bg_clear(self):
        self.cfg = _apply_config_updates({"background_dir": ""}, self.cfg)
        bg_json = os.path.join(self.cfg["data_dir"], "launcher", "background.json")
        if os.path.isfile(bg_json):
            try:
                os.remove(bg_json)
            except OSError:
                pass
        self._send_json({"ok": True, "message": "已清除背景目录配置"})

    def _admin_project_platforms(self):
        data = self._json_body()
        if data is None:
            return
        try:
            key = data.get("key", self.project)
            if key not in PROJECT_REGISTRY["projects"]:
                raise ValueError("项目不存在")
            platforms = validate_platforms(data.get("platforms"))
            default = data.get("defaultPlatform", platforms[0])
            if default not in platforms:
                raise ValueError("默认平台必须在平台列表中")
            cfg = load_project_cfg(key)
            # 保存已有数据，仅改变可用平台；删除平台后重新添加可恢复其配置。
            new_cfg = _apply_config_updates({"platforms": platforms, "default_platform": default}, cfg)
            _PROJECT_CFG_CACHE[key] = (new_cfg, os.path.getmtime(new_cfg["_config_path"]))
            build_versions_index(new_cfg)
            self._send_json({"ok": True, "message": "平台已保存，已有目录与文件保留", "platforms": platforms, "defaultPlatform": default})
        except (ValueError, TypeError) as exc:
            self._send_json({"error": str(exc)}, status=400)

    def _admin_platform_config(self):
        data = self._json_body()
        if data is None:
            return
        if self.cfg.get("workspace_dir") and any(key in data for key in ("basePackagesRoot", "patchSourceDir", "basePackages")):
            self._send_json({"error": "目录由工作空间统一管理，请使用工作空间导入接口"}, status=400)
            return
        platform = self._body_platform(data)
        if not platform:
            return
        try:
            options = dict(self.cfg.get("platform_settings", {}))
            selected = dict(options.get(platform, {}))
            for key in ("patchSourceDir", "basePackagesRoot", "hotpatcherOrder"):
                if key in data:
                    if not isinstance(data[key], str):
                        raise ValueError(key + " 必须是文本")
                    selected[key] = data[key].strip()
            options[platform] = selected
            packages = dict(self.cfg.get("base_packages", {}))
            updates = {"platform_settings": options}
            warnings = []
            if "basePackages" in data:
                entries = data["basePackages"]
                if not isinstance(entries, dict):
                    raise ValueError("基础包配置必须是 {版本号:目录}")
                normalized = {}
                base_root = self._resolve_path(selected.get("basePackagesRoot", ""))
                for version, path in entries.items():
                    if not re.fullmatch(r"[A-Za-z0-9_.-]+", version) or not isinstance(path, str) or not path.strip():
                        raise ValueError("基础包版本号或目录无效")
                    absolute = resolve_base_package_path(path, base_root or os.path.dirname(self.cfg["_config_path"]))
                    normalized[version] = absolute
                    if not os.path.isdir(absolute):
                        warnings.append("基础包 " + version + " 目录不存在：" + absolute)
                packages[platform] = normalized
                updates["base_packages"] = packages
            source = self._resolve_path(selected.get("patchSourceDir", ""))
            if source and not os.path.isdir(source):
                warnings.append("补丁来源目录不存在：" + source)
            self.cfg = _apply_config_updates(updates, self.cfg)
            _PROJECT_CFG_CACHE[self.project] = (self.cfg, os.path.getmtime(self.cfg["_config_path"]))
            build_versions_index(self.cfg)
            self._send_json({"ok": True, "message": platform + " 平台配置已保存", "warnings": warnings})
        except (ValueError, TypeError) as exc:
            self._send_json({"error": str(exc)}, status=400)

    def _admin_config_update(self):
        data = self._json_body()
        if data is None:
            return
        updates = {}
        missing_bp = []
        try:
            if "storage" in data:
                updates["storage"] = self._storage_from_payload(data)
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=400)
            return
        if "project" in data:
            updates["project"] = str(data["project"])
        if "host" in data:
            updates["host"] = str(data["host"])
        if "port" in data:
            updates["port"] = int(data["port"])
        if "dataDir" in data:
            if not isinstance(data["dataDir"], str) or not data["dataDir"].strip():
                self._send_json({"error": "项目数据目录不能为空"}, status=400)
                return
            updates["data_dir"] = data["dataDir"].strip()
        if "versionLibraryDir" in data:
            if not isinstance(data["versionLibraryDir"], str) or not data["versionLibraryDir"].strip():
                self._send_json({"error": "补丁文件库目录不能为空"}, status=400)
                return
            updates["version_library_dir"] = data["versionLibraryDir"].strip()
        if "manifestHash" in data:
            updates["manifest_hash"] = str(data["manifestHash"])
        if "maxUploadMb" in data:
            updates["max_upload_mb"] = int(data["maxUploadMb"])
        if "manifestExcludePatterns" in data:
            updates["manifest_exclude_patterns"] = list(data["manifestExcludePatterns"])
        if data.get("clearAdminToken"):
            updates["admin_token"] = ""
        elif data.get("adminToken"):
            updates["admin_token"] = str(data["adminToken"])
        if not updates:
            self._send_json({"ok": False, "error": "没有可更新的字段"}, status=400)
            return
        try:
            new_cfg = _apply_config_updates(updates, self.cfg)
            self.cfg = new_cfg
            build_versions_index(new_cfg)
            _PROJECT_CFG_CACHE[self.project] = (new_cfg, os.path.getmtime(new_cfg["_config_path"]))
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)
            return
        msg = "配置已保存并重新加载（host/port/https 等需重启服务器生效）"
        if missing_bp:
            msg += "；以下基础包目录在服务器上不存在、将被忽略（请确认路径或先放入内容）：" + "；".join(missing_bp)
        # 校验 versionLibraryDir 与 dataDir 是否同一根目录，避免版本/清单数据分家
        vlib = (self.cfg.get("version_library_dir") or "").strip()
        ddir = (self.cfg.get("data_dir") or "").strip()
        if vlib and ddir:
            try:
                av, ad = os.path.abspath(vlib), os.path.abspath(ddir)
                if os.path.commonpath([av, ad]) not in (av, ad):
                    msg += ("；⚠ 注意：versionLibraryDir 与 dataDir 不在同一根目录，"
                            "版本库将存入前者、清单/启动器/背景存入后者，可能造成数据“看似丢失”，请确认是否符合预期")
            except ValueError:
                pass
        self._send_json({"ok": True, "message": msg})

    def _storage_from_payload(self, data):
        """把管理页面提交的 storage 负载整理为可写入 config.json 的规范化配置。

        secretAccessKey 不会随 GET /api/config 返回，因此提交为空时沿用磁盘旧值，
        避免前端拿不到旧密钥导致误清空。
        """
        raw = data.get("storage")
        if raw is None:
            return None
        if not isinstance(raw, dict):
            raise ValueError("storage 必须是对象")
        backend = str(raw.get("backend") or "local").strip()
        if backend not in ("local", "s3"):
            raise ValueError("storage.backend 仅支持 local 或 s3")
        s3 = raw.get("s3") or {}
        if not isinstance(s3, dict):
            raise ValueError("storage.s3 必须是对象")
        disk = _read_json(self.cfg.get("_config_path")) or {}
        cur_s3 = ((disk.get("storage") or {}).get("s3") or {}) if isinstance(disk, dict) else {}
        merged = {k: v for k, v in cur_s3.items()}
        for key in ("provider", "endpoint", "region", "service", "bucket", "accessKeyId",
                    "prefix", "publicBaseUrl", "addressingStyle", "presignExpires"):
            if key in s3:
                merged[key] = s3[key]
        secret = str(s3.get("secretAccessKey") or "").strip()
        if secret:
            merged["secretAccessKey"] = secret
        if not str(merged.get("accessKeyId") or "").strip() and cur_s3.get("accessKeyId"):
            merged["accessKeyId"] = cur_s3["accessKeyId"]
        if backend == "s3":
            try:
                merged["presignExpires"] = max(60, int(merged.get("presignExpires") or 3600))
            except (TypeError, ValueError):
                raise ValueError("presignExpires 必须是数字")
            if not str(merged.get("endpoint") or "").strip():
                raise ValueError("OSS/S3 模式必须填写 Endpoint")
            if not str(merged.get("bucket") or "").strip():
                raise ValueError("OSS/S3 模式必须填写 Bucket")
            if not str(merged.get("accessKeyId") or "").strip():
                raise ValueError("OSS/S3 模式必须填写 AccessKey ID")
            if not str(merged.get("secretAccessKey") or "").strip():
                raise ValueError("OSS/S3 模式必须填写 AccessKey Secret（首次配置）")
        return {"backend": backend, "s3": merged}

    def _admin_storage_test(self):
        data = self._json_body()
        if data is None:
            return
        try:
            storage_cfg = self._storage_from_payload(data)
            if storage_cfg is None:
                self._send_json({"ok": False, "error": "缺少 storage 配置"}, status=400)
                return
            test_cfg = dict(self.cfg)
            test_cfg["_storage"] = None
            test_cfg["storage"] = storage_cfg
            st = get_storage(test_cfg)
            if not st.is_remote:
                self._send_json({"ok": True, "message": "本地磁盘模式无需测试"})
                return
            st.test_connection()
            self._send_json({"ok": True, "message": "连接成功：endpoint / bucket / 凭据有效"})
        except Exception as exc:
            self._send_json({"ok": False, "error": f"连接失败：{exc}"}, status=400)

    def _admin_restart(self):
        try:
            self._send_json({"ok": True, "message": "服务器正在重启…"})
            self.wfile.flush()
        except Exception:
            pass
        srv = UpdateHandler.server
        if srv is not None:
            try:
                srv.server_close()
            except Exception:
                pass
        time.sleep(0.2)
        try:
            os.execv(sys.executable, [sys.executable] + list(_APP_ARGV))
        except Exception as exc:
            self._send_json({"ok": False, "error": f"重启失败：{exc}"}, status=500)


# ---------- 管理操作（命令行子命令）----------

def delete_version(cfg, version_id, platform=None, kind=None):
    platform = platform or cfg.get("default_platform", cfg["platforms"][0])
    if platform not in cfg["platforms"] or not isinstance(version_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", version_id) or version_id.startswith("."):
        return False, "平台或版本号无效"
    if kind not in (None, "base", "patch", "installer"):
        return False, "包类型无效"
    targets = {"patch": os.path.join(cfg["versions_dir"], platform, version_id)}
    base = (cfg.get("base_packages", {}).get(platform) or {}).get(version_id)
    if base: targets["base"] = base
    if cfg.get("workspace_dir"):
        targets["installer"] = os.path.join(cfg["workspace_dir"], "installers", platform, version_id)
    candidates = [k for k, path in targets.items() if os.path.isdir(path) and (kind is None or kind == k)]
    if not candidates: return False, f"{platform}/{version_id} 版本不存在"
    if len(candidates) != 1: return False, "同版本号有多个包，请指定 base、installer 或 patch"
    kind = candidates[0]
    source = targets[kind]
    try:
        entry = _revoked_entry_from_descriptor(dict(cfg, _selected_platform=platform, _selected_kind=kind), version_id) if kind != "installer" else None
    except (ValueError, RuntimeError):
        entry = None  # Invalid legacy bases can still be removed from management.
    if entry and entry.get("type") != "full":
        store = load_revoked_store(cfg)
        store[platform + "/" + kind + "/" + version_id] = entry
        save_revoked_store(cfg, store)
    if kind == "base" and not cfg.get("workspace_dir"):
        # Old external package sources are unregistered, never moved/deleted.
        packages = {p: dict(v) for p, v in cfg.get("base_packages", {}).items()}
        packages.get(platform, {}).pop(version_id, None)
        cfg = _apply_config_updates({"base_packages": packages}, cfg)
    else:
        managed = cfg.get("workspace_dir") or cfg["versions_dir"]
        source_real, managed_real = os.path.normcase(os.path.realpath(source)), os.path.normcase(os.path.realpath(managed))
        if os.path.commonpath([source_real, managed_real]) != managed_real or source_real == managed_real:
            return False, "版本目录不在项目管理范围内"
        trash = os.path.join(cfg["data_dir"], "trash", platform, kind)
        os.makedirs(trash, exist_ok=True)
        shutil.move(source, os.path.join(trash, version_id + "_" + str(time.time_ns())))
        if cfg.get("_config_path"): cfg = load_config(cfg["_config_path"])
    # Drop only this package's switch; preserve a same-ID app/resource counterpart.
    enabled = dict(cfg.get("enabled_versions") or {})
    if platform in enabled:
        enabled[platform] = [v for v in enabled[platform] if v != kind + ":" + version_id]
        if cfg.get("_config_path"): cfg = _apply_config_updates({"enabled_versions": enabled}, cfg)
    build_versions_index(cfg)
    return True, f"{platform}/{version_id}（{kind}）已从管理中移除，原文件已保留"


def build_ssl_context(cfg):
    https = cfg.get("https") or {}
    if not https.get("enabled"):
        return None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    except AttributeError:
        ctx.options |= ssl.OP_NO_SSLv2 | ssl.OP_NO_SSLv3 | ssl.OP_NO_TLSv1 | ssl.OP_NO_TLSv1_1
    cert_file, key_file = https.get("certFile", ""), https.get("keyFile", "")
    try:
        cert_file, key_file = certgen.load_cert_pair(cert_file, key_file)
    except RuntimeError:
        if https.get("autoGenSelfSigned", True):
            ssl_dir = os.path.join(cfg["data_dir"], "ssl")
            cert_file, key_file = certgen.ensure_self_signed_cert(
                ssl_dir, https.get("commonName", "CloudUpdate"), https.get("country", "CN"))
        else:
            raise
    ctx.load_cert_chain(cert_file, key_file)
    return ctx


def cmd_serve(args, cfg):
    build_versions_index(cfg)
    init_projects(cfg.get("_config_path"))
    UpdateHandler.cfg = cfg
    ctx = build_ssl_context(cfg)
    scheme = "https" if ctx else "http"
    server = None
    last_error = None
    for _ in range(20):
        try:
            server = ThreadingHTTPServer((cfg["host"], cfg["port"]), UpdateHandler)
            break
        except OSError as exc:
            last_error = exc
            time.sleep(0.5)
    if server is None:
        print(f"无法监听 {cfg['host']}:{cfg['port']}：{last_error}")
        sys.exit(1)
    if ctx:
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
    UpdateHandler.server = server
    storage_label = "S3 对象存储" if get_storage(cfg).is_remote else "本地磁盘"
    print("=" * 60)
    console_url = f"{scheme}://127.0.0.1:{cfg['port']}/"
    print("CloudUpdate 管理服务器已启动")
    print(f"  ★ 网页管理控制台 : {console_url}")
    print("    （把上面这个地址复制到浏览器打开即可管理）")
    print(f"  状态接口 : {scheme}://127.0.0.1:{cfg['port']}/api/status")
    print(f"  监听地址 : {scheme}://{cfg['host']}:{cfg['port']}")
    print(f"  存储后端 : {storage_label}")
    print(f"  项目     : {cfg['project']}  platforms: {','.join(cfg['platforms'])}")
    print(f"  配置文件 : {cfg.get('_config_path', '')}")
    print(f"  数据目录 : {cfg['data_dir']}")
    print("  按 Ctrl+C 停止")
    print("=" * 60)
    # 双击启动 或 显式 --open 时自动打开管理页面，省去手动复制地址
    want_open = _is_double_clicked() or bool(getattr(args, "open", False))
    if want_open and not cfg.get("no_open_browser"):
        try:
            import webbrowser
            threading.Timer(1.0, lambda: webbrowser.open(console_url)).start()
            print("正在为你自动打开浏览器...")
        except Exception:
            pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n正在停止...")
    finally:
        server.server_close()


def cmd_reindex(args, cfg):
    idx = build_versions_index(cfg)
    print(f"索引已重建：当前版本 {idx['current']}，更新链 {' -> '.join(idx['updateChain']) or '无'}")


def cmd_gen_manifest(args, cfg):
    platforms = [args.platform] if getattr(args, "platform", None) else cfg["platforms"]
    for platform in platforms:
        m = generate_manifest(cfg, platform, force=True)
        print(f"已生成清单 {platform}：{m['fileCount']} 个文件")


def cmd_upload(args, cfg):
    target = args.target
    src = args.file
    if not os.path.isfile(src):
        print(f"文件不存在：{src}")
        return
    storage = get_storage(cfg)
    kw = {}
    if target == "package":
        platform = args.platform or cfg["platforms"][0]
        base_version = args.version or get_latest_base_version(cfg, platform)
        kw = {"platform": platform, "version": base_version, "rel": args.path or os.path.basename(src)}
    elif target == "version":
        if not args.version:
            print("缺少 --version"); return
        kw = {"platform": args.platform or cfg.get("default_platform", cfg["platforms"][0]), "version": args.version, "rel": args.path or os.path.basename(src)}
    elif target == "launcher":
        kw = {"rel": args.path or os.path.basename(src)}
    elif target == "background":
        kw = {"name": os.path.basename(src)}
    else:
        print("target 仅支持 package / version / launcher / background"); return
    storage.upload_file({"package":"packages", "version":"versions"}.get(target, target), src_path=src, **kw)
    print(f"已上传 [{target}] -> {storage.url_for(target, **kw)}")


def _sync_versions_to_s3(cfg, storage):
    """按平台/版本描述同步发布文件。"""
    for platform in cfg["platforms"]:
        root = os.path.join(cfg["versions_dir"], platform)
        if not os.path.isdir(root):
            continue
        for version in os.listdir(root):
            desc = _read_json(os.path.join(root, version, "descriptor.json")) or {}
            for file in desc.get("files", []):
                url = file.get("url", "")
                prefix = f"/files/versions/{platform}/{version}/"
                if url.startswith(prefix):
                    rel = url[len(prefix):]
                    source = safe_join(os.path.join(root, version), rel)
                    if source and os.path.isfile(source):
                        storage.upload_file("versions", src_path=source, platform=platform, version=version, rel=rel)
                elif url.startswith("/files/packages/"):
                    segments = url[len("/files/packages/"):].split("/", 2)
                    if len(segments) == 3:
                        pf, base, rel = segments
                        source = safe_join(get_base_dir(cfg, pf, base), rel) if get_base_dir(cfg, pf, base) else None
                        if source and os.path.isfile(source):
                            storage.upload_file("packages", src_path=source, platform=pf, version=base, rel=rel)


def _do_import_core(cfg, platform=None):
    """执行 HotPatcher 导入 + 远程存储同步（打印到 stdout，供 CLI 与网页共用）。"""
    platform = platform or cfg.get("default_platform", cfg["platforms"][0])
    options = cfg.get("platform_settings", {}).get(platform, {})
    run_import(resolve_server_path(options.get("patchSourceDir", ""), os.path.dirname(cfg["_config_path"])), cfg["data_dir"], cfg["project"],
               platform, options.get("hotpatcherOrder", ""), config=cfg)
    storage = get_storage(cfg)
    if storage.is_remote:
        _sync_versions_to_s3(cfg, storage)


def cmd_import_hotpatcher(args, cfg):
    _do_import_core(cfg, args.platform)


def _apply_config_updates(updates, cfg=None):
    """把更新合并进磁盘配置并重新加载（同时刷新运行时 cfg）。供网页管理接口复用。"""
    cfg = cfg or UpdateHandler.cfg
    cfg_path = cfg.get("_config_path")
    if not cfg_path:
        return cfg
    disk = _read_json(cfg_path)
    if not isinstance(disk, dict):
        disk = {}
    disk.pop("_storage", None)
    disk.pop("_config_path", None)
    disk.update(updates)
    import tempfile
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=os.path.dirname(cfg_path), suffix=".tmp", delete=False) as handle:
        json.dump(disk, handle, ensure_ascii=False, indent=2)
        temporary = handle.name
    os.replace(temporary, cfg_path)
    new_cfg = load_config(cfg_path)
    if UpdateHandler.cfg is None or new_cfg["_config_path"] == UpdateHandler.cfg.get("_config_path"):
        UpdateHandler.cfg = new_cfg
    _CFG_MTIME_CACHE[cfg_path] = os.path.getmtime(cfg_path)
    ensure_dirs(new_cfg)
    return new_cfg


def cmd_publish_launcher(args, cfg):
    version = args.version.strip()
    versions = cfg.get("launcher_versions") or {}
    src_dir = versions.get(version, "")
    if not src_dir or not os.path.isdir(src_dir):
        print(f"启动器版本 {version} 未配置或文件夹不存在")
        return
    launcher_dir = os.path.join(cfg["data_dir"], "launcher")
    os.makedirs(launcher_dir, exist_ok=True)
    for name in ("Launcher.exe", "SelfUpdater.exe"):
        s = os.path.join(src_dir, name)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(launcher_dir, name))
    ui_src, ui_dst = os.path.join(src_dir, "ui"), os.path.join(launcher_dir, "ui")
    if os.path.isdir(ui_src):
        if os.path.isdir(ui_dst):
            shutil.rmtree(ui_dst, ignore_errors=True)
        shutil.copytree(ui_src, ui_dst)
    storage = get_storage(cfg)
    if storage.is_remote:
        for dirpath, _dirnames, filenames in os.walk(launcher_dir):
            for name in filenames:
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, launcher_dir).replace("\\", "/")
                storage.upload_file("launcher", src_path=full, rel=rel)
    info = {"version": version, "url": storage.url_for("launcher", rel="Launcher.exe"), "note": ""}
    with open(os.path.join(launcher_dir, "version.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=2)
    print(f"启动器版本已发布：{version}")


def cmd_set_enabled(args, cfg):
    platform = args.platform.strip()
    ids = [x.strip() for x in args.versions.split(",") if x.strip()]
    cfg_path = cfg.get("_config_path")
    # 仅读取磁盘上的原始配置，避免把运行时计算字段（_storage 等非序列化对象、
    # 绝对化后的路径等）写回文件导致配置损坏。
    disk = _read_json(cfg_path)
    if not isinstance(disk, dict):
        disk = {}
    disk.pop("_storage", None)
    disk.pop("_config_path", None)
    ev = dict(disk.get("enabled_versions") or {})
    ev[platform] = ids
    disk["enabled_versions"] = ev
    import tempfile
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=os.path.dirname(cfg_path), suffix=".tmp", delete=False) as handle:
        json.dump(disk, handle, ensure_ascii=False, indent=2)
        temporary = handle.name
    os.replace(temporary, cfg_path)
    cfg["enabled_versions"] = ev
    build_versions_index(cfg)
    print(f"已开放 {platform}：{ids}")


def cmd_gen_cert(args, cfg):
    https = cfg.get("https") or {}
    ssl_dir = os.path.join(cfg["data_dir"], "ssl")
    cert, key = certgen.ensure_self_signed_cert(
        ssl_dir, https.get("commonName", "CloudUpdate"), https.get("country", "CN"))
    print(f"自签名证书已生成：\n  cert: {cert}\n  key:  {key}")


def cmd_version_delete(args, cfg):
    ok, msg = delete_version(cfg, args.id, args.platform, args.kind)
    print(msg)
    sys.exit(0 if ok else 1)


def _resolve_default_config(exe_dir):
    """双击启动时，按 exe 所在目录 -> 上级目录（Server/）的顺序寻找 config.json。"""
    here = os.path.join(exe_dir, "config.json")
    if os.path.isfile(here):
        return here
    parent = os.path.join(os.path.dirname(exe_dir), "config.json")
    if os.path.isfile(parent):
        return parent
    return here


def _write_default_config(path):
    """在 exe 旁边生成一个最小可用的默认配置，避免双击后因缺配置而报错退出。"""
    default = {
        "host": "0.0.0.0",
        "port": 8710,
        "project": "MyGame",
        "platforms": ["Windows", "Android", "IOS"],
        "default_platform": "Windows",
        "platform_settings": {"Windows": {"hotpatcherOrder": ""}},
        "workspace_dir": "workspace/main",
        "https": {
            "enabled": False,
            "certFile": "",
            "keyFile": "",
            "autoGenSelfSigned": True,
            "country": "CN",
            "commonName": "CloudUpdate",
        },
        "storage": {"backend": "local", "s3": {}},
    }
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(default, f, ensure_ascii=False, indent=2)


def main():
    global _APP_ARGV
    _APP_ARGV = list(sys.argv)
    parser = argparse.ArgumentParser(prog="CloudUpdateServer", description="CloudUpdate 管理服务器（独立程序）")
    parser.add_argument("--config", default=None, help="config.json 路径")
    parser.add_argument("--host", default=None, help="覆盖监听地址")
    parser.add_argument("--port", type=int, default=None, help="覆盖监听端口")
    sub = parser.add_subparsers(dest="command")

    p_serve = sub.add_parser("serve", help="启动 HTTP/HTTPS 服务（供启动器读取版本与下载）")
    p_serve.add_argument("--open", action="store_true", help="启动后自动打开网页管理控制台")
    sub.add_parser("reindex", help="重建版本索引")
    p_gen = sub.add_parser("gen-manifest", help="重新生成完整性清单")
    p_gen.add_argument("--platform", default=None, help="平台，默认全部")
    p_up = sub.add_parser("upload", help="上传文件到存储（local 写磁盘 / s3 传对象）")
    p_up.add_argument("--target", required=True, choices=["package", "version", "launcher", "background"])
    p_up.add_argument("--file", required=True, help="本地文件路径")
    p_up.add_argument("--platform", default=None)
    p_up.add_argument("--version", default=None, help="版本号 / 基础包版本")
    p_up.add_argument("--path", default=None, help="对象内的相对路径（默认文件名）")
    p_import = sub.add_parser("import-hotpatcher", help="导入平台 HotPatcher 产物")
    p_import.add_argument("--platform", default=None)
    p_pl = sub.add_parser("publish-launcher", help="发布启动器自升级版本")
    p_pl.add_argument("--version", required=True)
    p_se = sub.add_parser("set-enabled", help="设置开放版本")
    p_se.add_argument("--platform", required=True)
    p_se.add_argument("--versions", required=True, help="逗号分隔的版本号")
    sub.add_parser("gen-cert", help="生成自签名证书")
    p_vd = sub.add_parser("version-delete", help="按类型移除版本，文件保留在回收目录")
    p_vd.add_argument("--id", required=True, help="版本号")
    p_vd.add_argument("--platform", default=None)
    p_vd.add_argument("--kind", choices=["base", "patch", "installer"], default=None, help="同号包分别指定资源基础包、资源补丁或程序包")

    args = parser.parse_args()

    # 双击（无子命令）时，默认进入 serve，让服务器真正跑起来而不是闪一下帮助就退出
    global _DOUBLE_CLICKED
    double_clicked = not args.command
    _DOUBLE_CLICKED = double_clicked
    if not args.command:
        args.command = "serve"

    # 解析配置文件：未指定则自动寻找 exe 旁 / 上级目录；都不存在则生成默认配置
    cfg_path = args.config or _resolve_default_config(str(BASE_DIR))
    if not os.path.isfile(cfg_path):
        _write_default_config(cfg_path)
        print(f"[初始化] 已生成默认配置：{cfg_path}")
        print(f"          请在需要时编辑 data_dir / base_packages / storage 等字段。")
    elif double_clicked and args.config is None:
        # 双击且使用了自动找到的配置，提示来源
        print(f"[配置] 使用：{cfg_path}")

    if args.command == "serve" and double_clicked:
        print("[双击启动] 未指定子命令，默认以 serve 模式启动服务器。")
        print("           如需其它命令（reindex / import-hotpatcher / set-enabled ...），请在终端中运行。")

    try:
        cfg = load_config(cfg_path)
    except Exception as exc:
        print(f"\n[配置错误] 无法加载 {cfg_path}：{type(exc).__name__}: {exc}")
        if double_clicked:
            input("\n按 Enter 退出...")
        sys.exit(1)

    if args.host:
        cfg["host"] = args.host
    if args.port:
        cfg["port"] = args.port
    ensure_dirs(cfg)

    dispatch = {
        "serve": cmd_serve,
        "reindex": cmd_reindex,
        "gen-manifest": cmd_gen_manifest,
        "upload": cmd_upload,
        "import-hotpatcher": cmd_import_hotpatcher,
        "publish-launcher": cmd_publish_launcher,
        "set-enabled": cmd_set_enabled,
        "gen-cert": cmd_gen_cert,
        "version-delete": cmd_version_delete,
    }

    try:
        dispatch[args.command](args, cfg)
    except SystemExit:
        raise
    except Exception as exc:
        import traceback
        print(f"\n[运行错误] {type(exc).__name__}: {exc}")
        traceback.print_exc()
        if double_clicked:
            input("\n按 Enter 退出...")
        sys.exit(1)


if __name__ == "__main__":
    main()
