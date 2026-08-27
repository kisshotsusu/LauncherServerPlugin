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
    _revoked_entry_from_descriptor,
    load_revoked_store,
    save_revoked_store,
)
from storage import get_storage
from importer import run_import
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
    UpdateHandler.cfg = new_cfg
    _CFG_MTIME_CACHE[cfg_path] = mtime


def _rel_if_under(raw, abs_path, root):
    """若 abs_path 位于 root 之下，返回相对 root 的简写（正斜杠）；否则返回原始值（绝对路径）。"""
    if not root:
        return raw
    root_abs = resolve_server_path(root)
    if not root_abs:
        return raw
    ap = os.path.abspath(abs_path)
    try:
        rel = os.path.relpath(ap, root_abs)
    except ValueError:
        return raw
    rel = rel.replace(os.sep, "/")
    if rel and not rel.startswith("..") and not os.path.isabs(rel):
        return rel
    return raw


# ======================================================================
# 多项目管理：同一监听端口，按 URL 前缀（如 /cc/）路由到不同项目的配置
# ======================================================================
PROJECT_REGISTRY = {"default": "main", "projects": {}}   # 项目注册表（内存态，由 projects.json 落地）
SERVER_DIR = ""                                            # 主配置文件所在目录
_PROJECT_CFG_CACHE = {}                                    # key -> (cfg, mtime)
_REGISTRY_MTIME = 0.0
RESERVED_PREFIXES = {"api", "web", "files"}                # 这些前缀永远留给路由/静态资源，不能当项目名

# 接口注册表：供「服务器管理 → 接口管理」列出与启停。
# 仅登记可被停用的数据/管理类接口；服务器管理自身（/api/projects、/api/server/*）与
# 核心控制台（/web、/files、/api/status）不登记，永远可用，避免把自己锁死。
API_ENDPOINTS = [
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
            reg = json.load(open(pf, encoding="utf-8"))
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
        reg = json.load(open(pf, encoding="utf-8"))
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
    if cached and cached[1] == mtime:
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
    server_version = "CloudUpdateServer/2.0"
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
        size = os.path.getsize(abs_path)
        ctype, _ = mimetypes.guess_type(abs_path)
        if ctype is None:
            ctype = "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        if download_name:
            self.send_header("Content-Disposition", f'attachment; filename="{download_name}"')
        self.end_headers()
        with open(abs_path, "rb") as f:
            while True:
                chunk = f.read(1024 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)

    # ---------- 路由（仅启动器所需的只读 API + 本地文件下发）----------
    def do_GET(self):
        reload_cfg_if_changed()   # 让手动/外部修改根 config.json 立即生效
        orig_path = self.path
        self._enter_project()
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
        elif path == "/api/config":
            self._api_config()
        elif path == "/api/enabled_versions":
            self._api_enabled_versions()
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
        elif path.startswith("/files/background/"):
            self._serve_background_file(path[len("/files/background/"):])
        elif path.startswith("/files/launcher/"):
            self._serve_launcher_file(path[len("/files/launcher/"):])
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    # ---------- API ----------
    def _api_status(self, query):
        index = load_versions_index(self.cfg)
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
            "basePackageDirs": self.cfg.get("package_roots", {}),
            "basePackages": self.cfg.get("base_packages", {}),
            "basePackagesRoot": self.cfg.get("base_packages_root", ""),
            "patchSourceDir": self.cfg.get("hotpatcher_source", ""),
            "serverTime": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        })

    def _api_versions(self, query):
        index = load_versions_index(self.cfg, rebuild=query.get("rebuild", ["0"])[0] == "1")
        if query.get("all", ["0"])[0] != "1":
            index = filter_index_by_enabled(index, self.cfg)
        self._send_json(index)

    def _api_version(self, version_id, query):
        desc = read_descriptor(self.cfg, version_id)
        if not desc:
            self._send_json({"ok": False, "error": f"版本 {version_id} 不存在"}, status=404)
            return
        self._send_json(desc)

    def _api_manifest(self, query):
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        force = query.get("refresh", ["0"])[0] == "1"
        try:
            manifest = generate_manifest(self.cfg, platform, base_version=base_version, force=force)
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)
            return
        self._send_json(manifest)

    def _api_files(self, query):
        """浏览云端基础包目录。GET /api/files?platform=Windows&baseVersion=1.0&path=CodeBuild/Content"""
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        rel_dir = query.get("path", [""])[0]
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
            resolved = resolve_server_path(raw_platform[base_version])
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

    def _api_config(self):
        self._send_json({"ok": True, "config": self._public_config()})

    def _api_enabled_versions(self):
        self._send_json({"ok": True, "enabledVersions": self.cfg.get("enabled_versions", {})})

    def _api_dirs_list(self, query):
        """列出服务器本地目录，供管理页面文件夹选择器使用。"""
        if not self._auth_ok():
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
            "dataDir": self.cfg.get("data_dir"),
            "versionLibraryDir": self.cfg.get("version_library_dir", ""),
            "basePackageDirs": self.cfg.get("package_roots", {}),
            "basePackages": self.cfg.get("base_packages", {}),
            "basePackagesRoot": self.cfg.get("base_packages_root", ""),
            "patchSourceDir": self.cfg.get("hotpatcher_source", ""),
            "manifestExcludePatterns": self.cfg.get("manifest_exclude_patterns", []),
            "manifestHash": self.cfg.get("manifest_hash", "md5"),
            "maxUploadMb": self.cfg.get("max_upload_mb", 2048),
            "hotpatcherOrder": self.cfg.get("hotpatcher_order", ""),
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
            resolved = resolve_server_path(path) if path else ""
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
        parts = rel.split("/", 1)
        if len(parts) != 2:
            self._send_json({"ok": False, "error": "路径格式错误"}, status=400)
            return
        platform, rest = parts
        packages = get_base_packages(self.cfg, platform)
        # URL 形态有两种：
        #   /files/packages/<platform>/<baseVersion>/<rest>   （基础包整包，由 build_base_descriptor 生成）
        #   /files/packages/<platform>/<rest>                 （外部文件 ExternFile，无版本段）
        # 若 rest 的首段是已知基础包版本，则按版本定位目录，否则回退到最新基础包。
        rest_parts = rest.split("/", 1)
        if rest_parts[0] in packages:
            base_version, file_rel = rest_parts[0], (rest_parts[1] if len(rest_parts) > 1 else "")
        else:
            base_version, file_rel = get_latest_base_version(self.cfg, platform), rest
        abs_path = get_storage(self.cfg).local_path("packages", platform=platform, version=base_version, rel=file_rel)
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径或远程存储不支持本地下发"}, status=400)
            return
        self._send_file(abs_path)

    def _serve_version_file(self, rel):
        parts = rel.split("/", 1)
        if len(parts) != 2:
            self._send_json({"ok": False, "error": "路径格式错误"}, status=400)
            return
        version_id, file_rel = parts
        abs_path = get_storage(self.cfg).local_path("versions", version=version_id, rel=file_rel)
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径或远程存储不支持本地下发"}, status=400)
            return
        self._send_file(abs_path)

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
        web_dir = self.cfg.get("web_dir") or os.path.join(BASE_DIR, "web")
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
                "window.__SWITCH_PROJECT__=function(k){if(k)location.pathname='/' + k + '/';};"
                "</script>"
                % (json.dumps(self.project_prefix), json.dumps(self.project),
                   json.dumps(PROJECT_REGISTRY.get("default", "main")),
                   json.dumps(self._projects_list()))
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
            return json.loads(raw.decode("utf-8"))
        except Exception as exc:
            self._send_json({"ok": False, "error": f"JSON 解析失败：{exc}"}, status=400)
            return None

    def do_POST(self):
        reload_cfg_if_changed()   # 让手动/外部修改根 config.json 立即生效
        self._enter_project()
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
        if path == "/api/projects/rename":
            if not self._server_admin_ok():
                self._send_json({"ok": False, "error": "需要服务器管理员令牌"}, status=401)
                return
            self._admin_rename_project()
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
        self._enter_project()
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
            self._admin_delete_version(path[len("/api/version/"):])
        elif path.startswith("/api/launcher/versions/"):
            self._admin_delete_launcher_version(path[len("/api/launcher/versions/"):])
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    # ---------- 多项目：请求级项目解析与项目注册管理 ----------
    def _enter_project(self):
        """按 URL 前缀解析当前请求属于哪个项目，并切换 self.cfg / self.project_prefix。"""
        reload_projects_if_changed()
        parsed0 = urlparse(self.path)
        raw_path = unquote(parsed0.path)
        key, rest = resolve_project_path(raw_path)
        self.project = key
        # 默认项目无前缀（保持 / 根路径，完全兼容旧的单一项目部署）；
        # 其它项目才带 /<key> 前缀。两种方式访问默认项目都生效（/ 与 /main/）。
        default = PROJECT_REGISTRY.get("default", "main")
        # 历史单项目（默认名为 main）仍占用根路径 /；其余默认项目（如重命名后的 codebuild）
        # 与所有子项目统一走 /<key> 前缀，实现“每个项目独立地址”。
        # 访问格式管理可设置 rootBehavior=serve_at_root，让默认项目直接服务在根 /（前缀为空）。
        root_behavior = (UpdateHandler.cfg or {}).get("access", {}).get("rootBehavior", "redirect")
        self.project_prefix = "" if (key == default and root_behavior == "serve_at_root") else ("/" + key)
        cfg = load_project_cfg(key)
        if cfg is None:
            cfg = load_project_cfg(PROJECT_REGISTRY.get("default", "main")) or {}
        self.cfg = cfg
        # 重写 self.path 为去掉前缀后的路径（保留 query），下游 dispatch 继续按原逻辑匹配
        self.path = rest + (("?" + parsed0.query) if parsed0.query else "")

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
                cfg_path = cfg.get("_config_path", "")
                cfg_dir = os.path.dirname(os.path.abspath(cfg_path)) if cfg_path else SERVER_DIR
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

    def _sync_launcher_config(self, key):
        """同步客户端启动器配置到本项目：复制默认项目的 launcher 运行时资产到
        projects/<key>/data/launcher/，并把其中 launcher_config.json 的 serverUrl 追加 /<key> 前缀，
        否则本项目客户端会下载到默认项目的 launcher_config（serverUrl 无前缀）而串到默认项目。"""
        src = os.path.join(SERVER_DIR, "data", "launcher")
        if not os.path.isdir(src):
            return
        dst = os.path.join(SERVER_DIR, "projects", key, "data", "launcher")
        if os.path.isdir(dst):
            return  # 已存在则保留（可能已手动调整）
        import shutil
        try:
            shutil.copytree(src, dst)
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
        # 构造新项目配置：数据目录互相隔离；host/port 由统一监听端口决定，不在此设置
        new_cfg = {
            "project": key,
            "platforms": src.get("platforms", ["Windows"]),
            "data_dir": "data",
            "version_library_dir": "data/versions",
            "web_dir": os.path.abspath(os.path.join(SERVER_DIR, "web")),
            "base_packages_root": src.get("base_packages_root", ""),
            "base_packages": {},
            "enabled_versions": {},
            "hotpatcher_source": "",
            "hotpatcher_order": src.get("hotpatcher_order", ""),
            "admin_token": src.get("admin_token", ""),
            "storage": src.get("storage", {"backend": "local", "s3": {}}),
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
        load_project_cfg(key)  # 预热
        self._sync_launcher_config(key)  # 同步客户端启动器配置（serverUrl 加 /<key> 前缀）
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
        if moved:
            cfg_path = os.path.join(SERVER_DIR, "projects", new_key, "data", "launcher", "launcher_config.json")
        else:
            cfg_path = os.path.join(SERVER_DIR, "data", "launcher", "launcher_config.json")
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
        try:
            src = self.cfg.get("hotpatcher_source", "")
            if not src or not os.path.isdir(src):
                self._send_json({"ok": False, "error": f"未配置或找不到 HotPatcher 产物目录：{src}"}, status=400)
                return
            entries = [e for e in sorted(os.listdir(src))
                       if os.path.isfile(os.path.join(src, e, f"{e}_Release.json"))]
            if not entries:
                self._send_json({"ok": False, "error": f"在 {src} 未找到任何 HotPatcher 版本（缺少 *_Release.json）"}, status=400)
                return
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                _do_import_core(self.cfg)
            self._send_json({"ok": True, "output": buf.getvalue(), "message": "导入完成"})
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc), "output": ""}, status=500)

    def _admin_set_enabled(self):
        data = self._json_body()
        if data is None:
            return
        platform = (data.get("platform") or "").strip()
        versions = data.get("versions")
        if not platform or not isinstance(versions, list):
            self._send_json({"ok": False, "error": "缺少 platform 或 versions"}, status=400)
            return
        ids = [str(x).strip() for x in versions if str(x).strip()]
        ev = dict(self.cfg.get("enabled_versions") or {})
        ev[platform] = ids
        _apply_config_updates({"enabled_versions": ev})
        index = load_versions_index(self.cfg)
        filtered = filter_index_by_enabled(index, self.cfg)
        client_versions = [v.get("versionId") for v in (filtered.get("versions") or [])]
        self._send_json({"ok": True, "message": f"已开放 {platform}：{ids}", "clientVersions": client_versions})

    def _admin_delete_version(self, version_id):
        ok, msg = delete_version(self.cfg, version_id)
        self._send_json({"ok": ok, "message": msg}, status=200 if ok else 500)

    def _admin_upload(self, query):
        target = query.get("target", [""])[0]
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
            import tempfile
            tmp = tempfile.NamedTemporaryFile(delete=False, suffix=os.path.splitext(filename)[1])
            try:
                tmp.write(content)
                tmp.close()
                storage.upload_file(target, src_path=tmp.name, **kw)
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
            if not version_id:
                return None
            path = query.get("path", ["Windows"])[0] or "Windows"
            rel = (path + "/" + filename) if path else filename
            return {"version": version_id, "rel": rel}
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
        _apply_config_updates({"launcher_versions": lv})
        self._send_json({"ok": True, "message": f"已添加启动器版本 {version} -> {dirpath}"})

    def _admin_delete_launcher_version(self, version):
        version = unquote(version)
        lv = dict(self.cfg.get("launcher_versions") or {})
        if version not in lv:
            self._send_json({"ok": False, "error": f"启动器版本 {version} 不存在"}, status=404)
            return
        lv.pop(version, None)
        _apply_config_updates({"launcher_versions": lv})
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
        _apply_config_updates({"background_dir": dirpath})
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
        _apply_config_updates({"background_dir": ""})
        bg_json = os.path.join(self.cfg["data_dir"], "launcher", "background.json")
        if os.path.isfile(bg_json):
            try:
                os.remove(bg_json)
            except OSError:
                pass
        self._send_json({"ok": True, "message": "已清除背景目录配置"})

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
        if "platforms" in data:
            updates["platforms"] = [str(p).strip() for p in data["platforms"] if str(p).strip()]
        if "host" in data:
            updates["host"] = str(data["host"])
        if "port" in data:
            updates["port"] = int(data["port"])
        if "dataDir" in data:
            updates["data_dir"] = str(data["dataDir"])
        if "versionLibraryDir" in data:
            updates["version_library_dir"] = str(data["versionLibraryDir"])
        bp_root = (self.cfg.get("base_packages_root", "") or "")
        if "basePackagesRoot" in data:
            bp_root = str(data["basePackagesRoot"])
            updates["base_packages_root"] = bp_root
        if "basePackages" in data:
            bp = data["basePackages"]
            if not isinstance(bp, dict):
                self._send_json({"ok": False, "error": "basePackages 必须是对象（平台: {版本号: 目录}）"}, status=400)
                return
            norm_bp = {}
            for platform, versions in bp.items():
                if not isinstance(versions, dict):
                    self._send_json({"ok": False, "error": f"basePackages.{platform} 必须是对象（版本号: 目录）"}, status=400)
                    return
                for version, path in versions.items():
                    if not isinstance(path, str) or not str(path).strip():
                        self._send_json({"ok": False, "error": f"基础包 {platform}/{version} 的目录路径为空"}, status=400)
                        return
                    abs_path = resolve_base_package_path(path, bp_root)
                    if not os.path.isdir(abs_path):
                        missing_bp.append(f"{platform}/{version} → {path}")
                    # 全局根目录存在且该目录在其下时，存为相对子目录（更干净、可移植）；否则保留绝对路径
                    rel = _rel_if_under(path, abs_path, bp_root)
                    norm_bp.setdefault(str(platform), {})[str(version)] = rel
            updates["base_packages"] = norm_bp
        if "patchSourceDir" in data:
            updates["hotpatcher_source"] = str(data["patchSourceDir"])
        if "hotpatcherOrder" in data:
            updates["hotpatcher_order"] = str(data["hotpatcherOrder"])
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

def delete_version(cfg, version_id):
    """删除版本：回收站 + 撤销快照（供客户端精确删除已下载内容）；远程存储同时删除对象。"""
    version_id = os.path.basename(version_id)
    version_dir = os.path.join(cfg["versions_dir"], version_id)
    if not os.path.isdir(version_dir):
        return False, f"版本 {version_id} 不存在"
    entry = _revoked_entry_from_descriptor(cfg, version_id)
    if entry and entry.get("type") != "full":
        store = load_revoked_store(cfg)
        store[version_id] = entry
        save_revoked_store(cfg, store)
    storage = get_storage(cfg)
    if storage.is_remote:
        desc = _read_json(os.path.join(version_dir, "descriptor.json")) or {}
        for f in desc.get("files", []) or []:
            url = f.get("url") or ""
            if url.startswith("/files/"):
                try:
                    storage.delete_object(url[len("/files/"):])
                except Exception as exc:
                    print(f"  [警告] 删除对象失败 {url}: {exc}")
    trash_dir = os.path.join(cfg["data_dir"], "trash")
    os.makedirs(trash_dir, exist_ok=True)
    target = os.path.join(trash_dir, version_id)
    if os.path.exists(target):
        target += "_" + time.strftime("%Y%m%d%H%M%S")
    shutil.move(version_dir, target)
    build_versions_index(cfg)
    return True, f"版本 {version_id} 已移入回收站（{target}），可手动恢复"


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
        kw = {"version": args.version, "rel": args.path or os.path.basename(src)}
    elif target == "launcher":
        kw = {"rel": args.path or os.path.basename(src)}
    elif target == "background":
        kw = {"name": os.path.basename(src)}
    else:
        print("target 仅支持 package / version / launcher / background"); return
    storage.upload_file(target, src_path=src, **kw)
    print(f"已上传 [{target}] -> {storage.url_for(target, **kw)}")


def _sync_versions_to_s3(cfg, storage):
    """把版本库中的补丁 pak 与外部文件同步到对象存储（remote 模式），否则客户端 presigned URL 会 404。"""
    versions_dir = cfg["versions_dir"]
    if not os.path.isdir(versions_dir):
        return
    for entry in sorted(os.listdir(versions_dir)):
        vdir = os.path.join(versions_dir, entry)
        if not os.path.isdir(vdir):
            continue
        wd = os.path.join(vdir, "Windows")
        if os.path.isdir(wd):
            for name in sorted(os.listdir(wd)):
                if name.lower().endswith((".pak", ".utoc", ".ucas")):
                    full = os.path.join(wd, name)
                    storage.upload_file("versions", src_path=full, version=entry, rel=f"Windows/{name}")
        desc = _read_json(os.path.join(vdir, "descriptor.json")) or {}
        for f in desc.get("files", []) or []:
            url = f.get("url") or ""
            if not url.startswith("/files/packages/"):
                continue
            rest = url[len("/files/packages/"):]
            segs = rest.split("/", 2)
            if len(segs) < 2:
                continue
            pf = segs[0]
            if len(segs) >= 3 and segs[1] in get_base_packages(cfg, pf):
                bv, rel = segs[1], segs[2]
            else:
                bv, rel = "", segs[1]
            base = get_base_dir(cfg, pf, bv) if bv else get_base_dir(cfg, pf)
            src = safe_join(base, rel) if base else None
            if src and os.path.isfile(src):
                storage.upload_file("packages", src_path=src, platform=pf, version=bv, rel=rel)


def _do_import_core(cfg):
    """执行 HotPatcher 导入 + 远程存储同步（打印到 stdout，供 CLI 与网页共用）。"""
    run_import(cfg.get("hotpatcher_source", ""), cfg["data_dir"], cfg["project"],
               cfg["platforms"][0], cfg.get("hotpatcher_order", ""))
    storage = get_storage(cfg)
    if storage.is_remote:
        _sync_versions_to_s3(cfg, storage)


def cmd_import_hotpatcher(args, cfg):
    _do_import_core(cfg)


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
    disk.pop("package_roots", None)  # 兼容旧字段，由 load_config 重新计算，避免磁盘上残留陈旧值
    disk.update(updates)
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(disk, f, ensure_ascii=False, indent=2)
    new_cfg = load_config(cfg_path)
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
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(disk, f, ensure_ascii=False, indent=2)
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
    ok, msg = delete_version(cfg, args.id)
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
        "platforms": ["Windows"],
        "data_dir": "data",
        "base_packages": {"Windows": {}},
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
    sub.add_parser("import-hotpatcher", help="导入 HotPatcher 产物")
    p_pl = sub.add_parser("publish-launcher", help="发布启动器自升级版本")
    p_pl.add_argument("--version", required=True)
    p_se = sub.add_parser("set-enabled", help="设置开放版本")
    p_se.add_argument("--platform", required=True)
    p_se.add_argument("--versions", required=True, help="逗号分隔的版本号")
    sub.add_parser("gen-cert", help="生成自签名证书")
    p_vd = sub.add_parser("version-delete", help="删除版本（回收站 + 撤销快照 + 远程删对象）")
    p_vd.add_argument("--id", required=True, help="版本号")

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
