#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CloudUpdate 管理服务器（零第三方依赖，仅 Python 标准库）

功能：
  - 提供文件完整性清单 /api/manifest.json
  - 提供基于 HotPatcher 产物的版本索引 /api/versions 与更新描述 /api/version/{id}
  - 作为云端文件仓库：/files/packages/... 与 /files/versions/...
  - 管理页面：/（web/index.html）
  - 管理操作：重建版本索引、重新生成完整性清单

本文件为 HTTP 路由与请求处理层；底层逻辑拆分到：
  config.py   —— 配置加载、路径/版本号辅助
  manifest.py —— 完整性清单生成与哈希
  versions.py —— 版本索引与更新描述

启动：
  python run_server.py [--config config.json] [--host 0.0.0.0] [--port 8710]
"""

import argparse
import hashlib
import json
import mimetypes
import os
import shutil
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse

from config import (
    resolve_server_path,
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

BASE_DIR = Path(__file__).resolve().parent
SERVER_START_TIME = time.time()


class UpdateHandler(BaseHTTPRequestHandler):
    server_version = "CloudUpdateServer/1.0"
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

    def _check_token(self):
        token = self.cfg.get("admin_token", "")
        if not token:
            return True
        auth = self.headers.get("Authorization", "")
        q = parse_qs(urlparse(self.path).query)
        provided = auth.replace("Bearer ", "").strip() or (q.get("token", [""])[0])
        return provided == token

    # ---------- 路由 ----------
    def do_GET(self):
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        query = parse_qs(parsed.query)

        if path == "/" or path == "/index.html":
            self._serve_web("index.html")
        elif path == "/api/status":
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

    def do_POST(self):
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        if not self._check_token():
            self._send_json({"ok": False, "error": "管理员令牌无效"}, status=403)
            return
        if path == "/api/versions/reindex":
            self._api_reindex()
        elif path == "/api/manifest/generate":
            self._api_manifest_generate(parse_qs(parsed.query))
        elif path == "/api/upload":
            self._api_upload(parse_qs(parsed.query))
        elif path == "/api/import/hotpatcher":
            self._api_import_hotpatcher()
        elif path == "/api/config/update":
            self._api_config_update()
        elif path == "/api/enabled_versions":
            self._api_enabled_versions_update()
        elif path == "/api/launcher/publish":
            self._api_launcher_publish()
        elif path == "/api/launcher/background":
            self._api_launcher_background_update()
        elif path == "/api/launcher/background/dir":
            self._api_launcher_background_dir()
        elif path == "/api/launcher/background/clear":
            self._api_launcher_background_clear()
        elif path == "/api/launcher/versions":
            self._api_launcher_versions_update()
        elif path == "/api/server/restart":
            self._api_server_restart()
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    def do_DELETE(self):
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        if not self._check_token():
            self._send_json({"ok": False, "error": "管理员令牌无效"}, status=403)
            return
        if path.startswith("/api/launcher/versions/"):
            self._api_launcher_versions_delete(path[len("/api/launcher/versions/"):])
        elif path.startswith("/api/version/"):
            self._api_version_delete(path[len("/api/version/"):])
        else:
            self._send_json({"ok": False, "error": "404 Not Found"}, status=404)

    # ---------- 页面 ----------
    def _serve_web(self, name):
        abs_path = safe_join(self.cfg["web_dir"], name)
        if abs_path is None or not os.path.isfile(abs_path):
            self._send_text("index.html 未找到", status=404)
            return
        with open(abs_path, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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
        self._send_json({
            "ok": True,
            "server": "CloudUpdateServer",
            "project": self.cfg["project"],
            "platforms": self.cfg["platforms"],
            "pid": os.getpid(),
            "uptimeSeconds": int(time.time() - SERVER_START_TIME),
            "startedAt": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(SERVER_START_TIME)),
            "python": sys.version.split()[0],
            "host": self.cfg.get("host", "0.0.0.0"),
            "port": self.cfg.get("port", 8710),
            "configPath": self.cfg.get("_config_path", ""),
            "enabledVersions": self.cfg.get("enabled_versions", {}),
            "currentVersion": index.get("current", ""),
            "versionCount": len(index.get("versions", [])),
            "manifests": manifests,
            "authRequired": bool(self.cfg.get("admin_token", "")),
            "hotpatcherSourceConfigured": bool(self.cfg.get("hotpatcher_source", "")),
            "versionLibraryDir": self.cfg.get("version_library_dir", ""),
            "basePackageDirs": self.cfg.get("package_roots", {}),
            "basePackages": self.cfg.get("base_packages", {}),
            "patchSourceDir": self.cfg.get("hotpatcher_source", ""),
            "serverTime": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        })

    def _api_versions(self, query):
        index = load_versions_index(self.cfg, rebuild=query.get("rebuild", ["0"])[0] == "1")
        # 管理端 all=1 返回完整索引；客户端默认只返回已开放版本
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

    def _api_reindex(self):
        try:
            index = build_versions_index(self.cfg)
            self._send_json({"ok": True, "message": "版本索引已重建", "index": index})
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)

    def _api_manifest_generate(self, query):
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        try:
            manifest = generate_manifest(self.cfg, platform, base_version=base_version, force=True)
            self._send_json({
                "ok": True,
                "message": f"清单已重新生成（{platform} / 基础包 {manifest['baseVersionId']}，共 {manifest['fileCount']} 个文件）",
                "fileCount": manifest["fileCount"],
                "generatedAt": manifest["generatedAt"],
            })
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)

    def _api_config(self):
        self._send_json({"ok": True, "config": self._public_config()})

    def _api_enabled_versions(self):
        """当前开放版本配置。GET /api/enabled_versions"""
        self._send_json({
            "ok": True,
            "enabledVersions": self.cfg.get("enabled_versions", {}),
        })

    def _api_enabled_versions_update(self):
        """设置开放版本。POST JSON：{platform, versions:[...]} 或 {versionsByPlatform:{...}}"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception:
            self._send_json({"ok": False, "error": "请求体必须是 JSON"}, status=400)
            return

        cfg_path = self.cfg.get("_config_path")
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                disk = json.load(f)
        except Exception:
            disk = dict(self.cfg)

        clean = {}
        if "versionsByPlatform" in payload:
            raw = payload["versionsByPlatform"]
            if not isinstance(raw, dict):
                self._send_json({"ok": False, "error": "versionsByPlatform 必须是对象"}, status=400)
                return
            for platform, ids in raw.items():
                if not isinstance(ids, list):
                    self._send_json({"ok": False, "error": f"平台 {platform} 的开放版本必须是数组"}, status=400)
                    return
                clean[str(platform).strip()] = [str(x).strip() for x in ids if str(x).strip()]
        else:
            platform = str(payload.get("platform", "")).strip()
            if not platform:
                self._send_json({"ok": False, "error": "缺少 platform"}, status=400)
                return
            ids = payload.get("versions") or []
            if not isinstance(ids, list):
                self._send_json({"ok": False, "error": "versions 必须是数组"}, status=400)
                return
            clean[platform] = [str(x).strip() for x in ids if str(x).strip()]

        disk["enabled_versions"] = clean
        self.cfg["enabled_versions"] = clean
        try:
            with open(cfg_path, "w", encoding="utf-8") as f:
                json.dump(disk, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存 config.json 失败：{exc}"}, status=500)
            return

        build_versions_index(self.cfg)
        index = filter_index_by_enabled(load_versions_index(self.cfg), self.cfg)
        self._send_json({
            "ok": True,
            "message": "开放版本已更新",
            "enabledVersions": clean,
            "clientVersions": [v.get("versionId") for v in index.get("versions", [])],
        })

    def _public_config(self):
        public = {
            "host": self.cfg.get("host"),
            "port": self.cfg.get("port"),
            "project": self.cfg.get("project"),
            "platforms": self.cfg.get("platforms"),
            "dataDir": self.cfg.get("data_dir"),
            "versionLibraryDir": self.cfg.get("version_library_dir", ""),
            "basePackageDirs": self.cfg.get("package_roots", {}),
            "basePackages": self.cfg.get("base_packages", {}),
            "patchSourceDir": self.cfg.get("hotpatcher_source", ""),
            "manifestExcludePatterns": self.cfg.get("manifest_exclude_patterns", []),
            "manifestHash": self.cfg.get("manifest_hash", "md5"),
            "maxUploadMb": self.cfg.get("max_upload_mb", 2048),
            "hotpatcherOrder": self.cfg.get("hotpatcher_order", ""),
            "authRequired": bool(self.cfg.get("admin_token", "")),
            "adminTokenSet": bool(self.cfg.get("admin_token", "")),
            "enabledVersions": self.cfg.get("enabled_versions", {}),
            "launcherVersions": self.cfg.get("launcher_versions", {}),
            "backgroundDir": self.cfg.get("background_dir", ""),
        }
        # 兼容旧字段
        public["packageRoots"] = public["basePackageDirs"]
        public["hotpatcherSource"] = public["patchSourceDir"]
        return public

    def _api_config_update(self):
        """更新服务器路径设置（版本文件库 / 基础包 / 补丁包）。POST JSON。"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception:
            self._send_json({"ok": False, "error": "请求体必须是 JSON"}, status=400)
            return

        cfg_path = self.cfg.get("_config_path")
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                disk = json.load(f)
        except Exception:
            disk = dict(self.cfg)

        # 记录修改前的基础包版本，用于自动维护开放列表
        base_snapshot = {}
        for p, vmap in (disk.get("base_packages") or {}).items():
            base_snapshot[str(p)] = set(str(v) for v in (vmap or {}).keys())

        # 1. 版本文件库位置
        if "versionLibraryDir" in payload:
            raw = str(payload["versionLibraryDir"] or "").strip()
            if not raw:
                self._send_json({"ok": False, "error": "版本文件库位置不能为空"}, status=400)
                return
            resolved = resolve_server_path(raw)
            try:
                os.makedirs(resolved, exist_ok=True)
            except Exception as exc:
                self._send_json({"ok": False, "error": f"无法创建版本文件库目录：{exc}"}, status=400)
                return
            disk["version_library_dir"] = raw
            self.cfg["version_library_dir"] = resolved
            self.cfg["versions_dir"] = resolved

        # 2. 基础包位置（多版本）：{平台: {版本号: 路径}}
        if "basePackages" in payload:
            raw = payload["basePackages"]
            if not isinstance(raw, dict):
                self._send_json({"ok": False, "error": "basePackages 必须是 {平台: {版本: 路径}}"}, status=400)
                return
            new_base_packages = {}
            for platform, version_map in raw.items():
                if not isinstance(version_map, dict) or not version_map:
                    self._send_json({"ok": False, "error": f"平台 {platform} 的基础包版本不能为空"}, status=400)
                    return
                resolved_map = {}
                for version, path in version_map.items():
                    version = str(version).strip()
                    path = str(path or "").strip()
                    if not version or not path:
                        continue
                    resolved = resolve_server_path(path)
                    if not os.path.isdir(resolved):
                        self._send_json({"ok": False, "error": f"基础包目录不存在：{resolved}"}, status=400)
                        return
                    resolved_map[version] = resolved
                if not resolved_map:
                    self._send_json({"ok": False, "error": f"平台 {platform} 的基础包位置不能为空"}, status=400)
                    return
                new_base_packages[platform] = resolved_map
            disk["base_packages"] = new_base_packages
            self.cfg["base_packages"] = new_base_packages
            # 兼容字段：package_roots 指向各平台最新基础包
            self.cfg["package_roots"] = {
                p: get_base_dir(self.cfg, p, get_latest_base_version(self.cfg, p))
                for p in self.cfg.get("platforms", [])
            }
        elif "basePackageDirs" in payload:
            # 旧式单基础包：{平台: 路径}
            raw = payload["basePackageDirs"]
            if isinstance(raw, dict):
                for platform, path in raw.items():
                    path = str(path or "").strip()
                    if not path:
                        continue
                    resolved = resolve_server_path(path)
                    if not os.path.isdir(resolved):
                        self._send_json({"ok": False, "error": f"基础包目录不存在：{resolved}"}, status=400)
                        return
                    existing = self.cfg.get("base_packages", {}).get(platform, {})
                    matched_version = next((v for v, p in existing.items() if p == resolved), None)
                    if matched_version is None:
                        matched_version = "1.0"
                    self.cfg.setdefault("base_packages", {}).setdefault(platform, {})[matched_version] = resolved
                disk["base_packages"] = self.cfg["base_packages"]

        # 新基础包自动加入开放列表（客户端立即可下载）；删除的基础包同步移出开放列表
        if "basePackages" in payload or "basePackageDirs" in payload:
            old_bases = base_snapshot
            new_bases = {}
            for p, vmap in (self.cfg.get("base_packages") or {}).items():
                new_bases[str(p)] = set(str(v) for v in (vmap or {}).keys())
            ev = dict(disk.get("enabled_versions") or {})
            for p in set(old_bases) | set(new_bases):
                removed = old_bases.get(p, set()) - new_bases.get(p, set())
                added = new_bases.get(p, set()) - old_bases.get(p, set())
                ids = [x for x in (ev.get(p) or []) if x not in removed]
                for v in sorted(added, key=version_key):
                    if v not in ids:
                        ids.append(v)
                ev[p] = ids
            disk["enabled_versions"] = ev
            self.cfg["enabled_versions"] = ev

        # 3. 补丁包位置（HotPatcher 产物目录）
        if "patchSourceDir" in payload:
            raw = str(payload["patchSourceDir"] or "").strip()
            resolved = resolve_server_path(raw) if raw else ""
            if resolved and not os.path.isdir(resolved):
                self._send_json({"ok": False, "error": f"补丁包目录不存在：{resolved}"}, status=400)
                return
            disk["hotpatcher_source"] = raw
            self.cfg["hotpatcher_source"] = resolved

        # 4. 版本顺序
        if "hotpatcherOrder" in payload:
            order = str(payload["hotpatcherOrder"] or "").strip()
            disk["hotpatcher_order"] = order
            self.cfg["hotpatcher_order"] = order

        # 5. 项目名
        if "project" in payload:
            project = str(payload["project"] or "").strip()
            if not project:
                self._send_json({"ok": False, "error": "项目名不能为空"}, status=400)
                return
            disk["project"] = project
            self.cfg["project"] = project

        # 6. 平台列表
        if "platforms" in payload:
            platforms = [str(p).strip() for p in (payload["platforms"] or []) if str(p).strip()]
            if not platforms:
                self._send_json({"ok": False, "error": "平台列表不能为空"}, status=400)
                return
            disk["platforms"] = platforms
            self.cfg["platforms"] = platforms
            roots = dict(self.cfg.get("package_roots", {}))
            for p in platforms:
                roots.setdefault(p, "")
            disk["package_roots"] = roots
            self.cfg["package_roots"] = roots

        # 7. 数据目录（清单缓存、索引、回收站）
        if "dataDir" in payload:
            raw = str(payload["dataDir"] or "").strip()
            if not raw:
                self._send_json({"ok": False, "error": "数据目录不能为空"}, status=400)
                return
            resolved = resolve_server_path(raw)
            try:
                os.makedirs(resolved, exist_ok=True)
            except Exception as exc:
                self._send_json({"ok": False, "error": f"无法创建数据目录：{exc}"}, status=400)
                return
            disk["data_dir"] = raw
            self.cfg["data_dir"] = resolved
            self.cfg["manifests_dir"] = os.path.join(resolved, "manifests")
            os.makedirs(self.cfg["manifests_dir"], exist_ok=True)

        # 8. 上传大小上限（MB）
        if "maxUploadMb" in payload:
            try:
                max_mb = int(payload["maxUploadMb"])
            except (TypeError, ValueError):
                self._send_json({"ok": False, "error": "上传上限必须是数字"}, status=400)
                return
            if max_mb <= 0:
                self._send_json({"ok": False, "error": "上传上限必须大于 0"}, status=400)
                return
            disk["max_upload_mb"] = max_mb
            self.cfg["max_upload_mb"] = max_mb

        # 9. 清单哈希算法
        if "manifestHash" in payload:
            algo = str(payload["manifestHash"] or "md5").strip().lower()
            if algo not in ("md5", "sha1"):
                self._send_json({"ok": False, "error": "清单哈希只支持 md5 或 sha1"}, status=400)
                return
            disk["manifest_hash"] = algo
            self.cfg["manifest_hash"] = algo

        # 10. 清单排除模式
        if "manifestExcludePatterns" in payload:
            patterns = [str(p).strip() for p in (payload["manifestExcludePatterns"] or []) if str(p).strip()]
            disk["manifest_exclude_patterns"] = patterns
            self.cfg["manifest_exclude_patterns"] = patterns

        # 11. 管理员令牌（非空则设置；clearAdminToken=true 则清除）
        if "adminToken" in payload:
            token = str(payload["adminToken"] or "").strip()
            if token:
                disk["admin_token"] = token
                self.cfg["admin_token"] = token
        if payload.get("clearAdminToken"):
            disk["admin_token"] = ""
            self.cfg["admin_token"] = ""

        # 12. 监听地址 / 端口（重启后生效）
        if "host" in payload:
            host = str(payload["host"] or "").strip()
            if not host:
                self._send_json({"ok": False, "error": "监听地址不能为空"}, status=400)
                return
            disk["host"] = host
            self.cfg["host"] = host
        if "port" in payload:
            try:
                port = int(payload["port"])
            except (TypeError, ValueError):
                self._send_json({"ok": False, "error": "端口必须是数字"}, status=400)
                return
            if not (1 <= port <= 65535):
                self._send_json({"ok": False, "error": "端口必须在 1-65535 之间"}, status=400)
                return
            disk["port"] = port
            self.cfg["port"] = port

        # 13. 开放版本（客户端可见/可下载的版本）：{平台: [版本号...]}
        if "enabledVersions" in payload:
            raw = payload["enabledVersions"]
            if not isinstance(raw, dict):
                self._send_json({"ok": False, "error": "enabledVersions 必须是 {平台: [版本号]}"}, status=400)
                return
            clean = {}
            for platform, ids in raw.items():
                if not isinstance(ids, list):
                    self._send_json({"ok": False, "error": f"平台 {platform} 的开放版本必须是数组"}, status=400)
                    return
                clean[str(platform).strip()] = [str(x).strip() for x in ids if str(x).strip()]
            disk["enabled_versions"] = clean
            self.cfg["enabled_versions"] = clean

        # 14. 启动器历史版本（文件夹）：{版本号: 路径}
        if "launcherVersions" in payload:
            raw = payload["launcherVersions"]
            if not isinstance(raw, dict):
                self._send_json({"ok": False, "error": "launcherVersions 必须是 {版本号: 路径}"}, status=400)
                return
            clean_versions = {}
            for version, path in raw.items():
                version = str(version).strip()
                path = str(path or "").strip()
                if not version or not path:
                    continue
                resolved = resolve_server_path(path)
                if not os.path.isdir(resolved):
                    self._send_json({"ok": False, "error": f"启动器版本文件夹不存在：{resolved}"}, status=400)
                    return
                if not os.path.isfile(os.path.join(resolved, "Launcher.exe")):
                    self._send_json({"ok": False, "error": f"启动器版本文件夹缺少 Launcher.exe：{resolved}"}, status=400)
                    return
                clean_versions[version] = path
            disk["launcher_versions"] = clean_versions
            self.cfg["launcher_versions"] = {v: resolve_server_path(p) for v, p in clean_versions.items()}

        # 15. 背景序列帧来源文件夹
        if "backgroundDir" in payload:
            raw = str(payload.get("backgroundDir") or "").strip()
            if raw:
                resolved = resolve_server_path(raw)
                if not os.path.isdir(resolved):
                    self._send_json({"ok": False, "error": f"背景文件夹不存在：{resolved}"}, status=400)
                    return
                disk["background_dir"] = raw
                self.cfg["background_dir"] = resolved
            else:
                disk["background_dir"] = ""
                self.cfg["background_dir"] = ""

        try:
            with open(cfg_path, "w", encoding="utf-8") as f:
                json.dump(disk, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存 config.json 失败：{exc}"}, status=500)
            return

        build_versions_index(self.cfg)
        self._send_json({"ok": True, "message": "设置已保存并生效", "config": self._public_config()})

    def _api_server_restart(self):
        """重启服务器（先启动新进程，再关闭当前进程，端口自动重试）。"""
        cfg_path = self.cfg.get("_config_path", str(BASE_DIR / "config.json"))
        try:
            flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
            subprocess.Popen(
                [sys.executable, os.path.abspath(__file__), "--config", cfg_path],
                cwd=str(BASE_DIR),
                creationflags=flags,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                close_fds=True,
            )
        except Exception as exc:
            self._send_json({"ok": False, "error": f"启动新进程失败：{exc}"}, status=500)
            return
        threading.Timer(1.5, lambda: getattr(UpdateHandler.server, "shutdown", lambda: None)()).start()
        self._send_json({"ok": True, "message": "服务器正在重启，请稍候刷新页面"})

    def _api_files(self, query):
        """浏览云端基础包目录。GET /api/files?platform=Windows&baseVersion=1.0&path=CodeBuild/Content"""
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        rel_dir = query.get("path", [""])[0]
        root = get_base_dir(self.cfg, platform, base_version)
        if not root or not os.path.isdir(root):
            self._send_json({"ok": False, "error": f"平台 {platform} 未配置基础包目录"}, status=404)
            return
        abs_dir = safe_join(root, rel_dir)
        if abs_dir is None or not os.path.isdir(abs_dir):
            self._send_json({"ok": False, "error": "目录不存在或路径非法"}, status=404)
            return

        entries = []
        for name in sorted(os.listdir(abs_dir)):
            item_path = os.path.join(abs_dir, name)
            rel = os.path.relpath(item_path, root).replace("\\", "/")
            if os.path.isdir(item_path):
                entries.append({
                    "name": name,
                    "type": "dir",
                    "size": 0,
                    "modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(item_path))),
                    "path": rel,
                })
            else:
                base_for_url = base_version or get_latest_base_version(self.cfg, platform)
                entries.append({
                    "name": name,
                    "type": "file",
                    "size": os.path.getsize(item_path),
                    "modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(item_path))),
                    "path": rel,
                    "downloadUrl": f"/files/packages/{platform}/{base_for_url}/{rel}",
                })
        parent = ""
        if rel_dir:
            parent = "/".join(rel_dir.replace("\\", "/").split("/")[:-1])
        self._send_json({
            "ok": True,
            "platform": platform,
            "baseVersion": base_version or get_latest_base_version(self.cfg, platform),
            "root": root,
            "path": rel_dir.replace("\\", "/"),
            "parent": parent,
            "entries": entries,
        })

    def _api_upload(self, query):
        """上传文件。POST /api/upload?target=package|version|launcher|background&platform=Windows&baseVersion=&path=...&versionId="""
        target = query.get("target", ["package"])[0]
        platform = query.get("platform", [self.cfg["platforms"][0]])[0]
        base_version = query.get("baseVersion", [""])[0] or None
        rel_dir = query.get("path", [""])[0]
        version_id = query.get("versionId", [""])[0]

        if target == "package":
            root = get_base_dir(self.cfg, platform, base_version)
            if not root:
                self._send_json({"ok": False, "error": f"平台 {platform} 未配置基础包目录"}, status=404)
                return
            dest_dir = safe_join(root, rel_dir)
        elif target == "version":
            if not version_id:
                self._send_json({"ok": False, "error": "缺少 versionId"}, status=400)
                return
            root = os.path.join(self.cfg["versions_dir"], os.path.basename(version_id))
            dest_dir = safe_join(root, rel_dir)
        elif target == "launcher":
            root = os.path.join(self.cfg["data_dir"], "launcher")
            dest_dir = root
        elif target == "background":
            root = os.path.join(self.cfg["data_dir"], "launcher", "background")
            dest_dir = root
        else:
            self._send_json({"ok": False, "error": "target 只能是 package / version / launcher / background"}, status=400)
            return

        if dest_dir is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        os.makedirs(dest_dir, exist_ok=True)
        if target == "background":
            # 背景图整体替换：先清空旧图
            for old in os.listdir(dest_dir):
                old_path = os.path.join(dest_dir, old)
                if os.path.isfile(old_path) and old.lower().endswith(".png"):
                    try:
                        os.remove(old_path)
                    except OSError:
                        pass

        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            length = 0
        max_bytes = self.cfg.get("max_upload_mb", 2048) * 1024 * 1024
        if length <= 0 or length > max_bytes:
            self._send_json({"ok": False, "error": f"请求体大小不合法（上限 {self.cfg.get('max_upload_mb', 2048)} MB）"}, status=400)
            return

        body = self.rfile.read(length)
        parts = parse_multipart(self.headers.get("Content-Type", ""), body)
        saved = []
        for name, filename, content in parts:
            if name != "file" or not filename:
                continue
            safe_name = os.path.basename(filename.replace("\\", "/"))
            if not safe_name:
                continue
            if target == "background" and not safe_name.lower().endswith(".png"):
                continue
            dest_path = os.path.join(dest_dir, safe_name)
            with open(dest_path, "wb") as f:
                f.write(content)
            rel = os.path.relpath(dest_path, root).replace("\\", "/")
            if target == "launcher":
                dl_url = f"/files/launcher/{safe_name}"
            elif target == "background":
                dl_url = f"/files/launcher/background/{safe_name}"
            elif target == "version":
                dl_url = f"/files/versions/{version_id}/Windows/{safe_name}"
            else:
                dl_url = f"/files/packages/{platform}/{base_version or get_latest_base_version(self.cfg, platform)}/{rel}"
            saved.append({
                "fileName": safe_name,
                "path": rel,
                "size": len(content),
                "url": dl_url,
            })

        if not saved:
            self._send_json({"ok": False, "error": "没有收到文件（字段名应为 file）"}, status=400)
            return
        self._send_json({
            "ok": True,
            "message": f"已上传 {len(saved)} 个文件",
            "files": saved,
            "note": "上传后请重新生成完整性清单 / 重建版本索引",
        })

    def _api_import_hotpatcher(self):
        source = self.cfg.get("hotpatcher_source", "")
        if not source or not os.path.isdir(source):
            self._send_json({"ok": False, "error": "未在 config.json 中配置 hotpatcher_source"}, status=400)
            return
        script = os.path.join(BASE_DIR, "scripts", "import_hotpatcher.py")
        cmd = [sys.executable, script, "--source", source, "--data", self.cfg["data_dir"],
               "--project", self.cfg["project"], "--platform", self.cfg["platforms"][0]]
        order = self.cfg.get("hotpatcher_order", "")
        if order:
            cmd += ["--order", order]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600, cwd=str(BASE_DIR))
            output = (proc.stdout or "") + (proc.stderr or "")
            self._send_json({
                "ok": proc.returncode == 0,
                "returncode": proc.returncode,
                "output": output[-8000:],
            })
        except subprocess.TimeoutExpired:
            self._send_json({"ok": False, "error": "导入超时（>600 秒）"}, status=500)
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=500)

    def _api_version_delete(self, version_id):
        version_id = os.path.basename(unquote(version_id))
        version_dir = os.path.join(self.cfg["versions_dir"], version_id)
        if not os.path.isdir(version_dir):
            self._send_json({"ok": False, "error": f"版本 {version_id} 不存在"}, status=404)
            return
        # 删除前快照描述文件，供客户端精确删除已下载内容（整包不回滚）
        entry = _revoked_entry_from_descriptor(self.cfg, version_id)
        if entry and entry.get("type") != "full":
            store = load_revoked_store(self.cfg)
            store[version_id] = entry
            save_revoked_store(self.cfg, store)
        trash_dir = os.path.join(self.cfg["data_dir"], "trash")
        os.makedirs(trash_dir, exist_ok=True)
        target = os.path.join(trash_dir, version_id)
        if os.path.exists(target):
            target += "_" + time.strftime("%Y%m%d%H%M%S")
        shutil.move(version_dir, target)
        build_versions_index(self.cfg)
        self._send_json({"ok": True, "message": f"版本 {version_id} 已移入回收站（{target}），可手动恢复"})

    # ---------- 文件 ----------
    def _serve_package_file(self, rel):
        parts = rel.split("/", 1)
        if len(parts) != 2:
            self._send_json({"ok": False, "error": "路径格式错误"}, status=400)
            return
        platform, rest = parts
        # 兼容两种形式：/files/packages/{平台}/{基础包版本}/{路径} 或旧式 /files/packages/{平台}/{路径}
        first, _, remainder = rest.partition("/")
        packages = get_base_packages(self.cfg, platform)
        if first in packages:
            base_version = first
            file_rel = remainder
        else:
            base_version = get_latest_base_version(self.cfg, platform)
            file_rel = rest
        root = get_base_dir(self.cfg, platform, base_version)
        if not root:
            self._send_json({"ok": False, "error": f"平台 {platform} 未配置基础包目录"}, status=404)
            return
        abs_path = safe_join(root, file_rel)
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        self._send_file(abs_path)

    def _serve_version_file(self, rel):
        parts = rel.split("/", 1)
        if len(parts) != 2:
            self._send_json({"ok": False, "error": "路径格式错误"}, status=400)
            return
        version_id, file_rel = parts
        root = os.path.join(self.cfg["versions_dir"], os.path.basename(version_id))
        abs_path = safe_join(root, file_rel)
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        self._send_file(abs_path)

    def _api_launcher_version(self):
        """启动器自升级版本信息。data/launcher/version.json: {version, url}"""
        version_path = os.path.join(self.cfg["data_dir"], "launcher", "version.json")
        info = _read_json(version_path)
        if not info:
            self._send_json({"ok": False, "error": "未发布启动器版本"}, status=404)
            return
        info.setdefault("url", "/files/launcher/Launcher.exe")
        self._send_json(info)

    def _api_launcher_publish(self):
        """发布启动器版本：从历史版本文件夹快照 Launcher.exe / SelfUpdater.exe 到 data/launcher。
        POST JSON {version, note}"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception:
            self._send_json({"ok": False, "error": "请求体必须是 JSON"}, status=400)
            return
        version = str(payload.get("version", "")).strip()
        if not version:
            self._send_json({"ok": False, "error": "缺少版本号"}, status=400)
            return
        versions = self.cfg.get("launcher_versions") or {}
        src_dir = versions.get(version, "")
        if not src_dir or not os.path.isdir(src_dir):
            self._send_json({"ok": False, "error": f"版本 {version} 未配置或文件夹不存在，请先添加版本"}, status=400)
            return
        src_exe = os.path.join(src_dir, "Launcher.exe")
        if not os.path.isfile(src_exe):
            self._send_json({"ok": False, "error": f"版本文件夹中缺少 Launcher.exe：{src_dir}"}, status=400)
            return
        launcher_dir = os.path.join(self.cfg["data_dir"], "launcher")
        os.makedirs(launcher_dir, exist_ok=True)
        dest_exe = os.path.join(launcher_dir, "Launcher.exe")
        if os.path.abspath(src_exe) != os.path.abspath(dest_exe):
            shutil.copy2(src_exe, dest_exe)
        src_updater = os.path.join(src_dir, "SelfUpdater.exe")
        dest_updater = os.path.join(launcher_dir, "SelfUpdater.exe")
        if os.path.isfile(src_updater) and os.path.abspath(src_updater) != os.path.abspath(dest_updater):
            shutil.copy2(src_updater, dest_updater)
        # 同步 ui/ 前端资源（客户端首次运行会从这里拉取）
        src_ui = os.path.join(src_dir, "ui")
        dest_ui = os.path.join(launcher_dir, "ui")
        if os.path.isdir(src_ui):
            if os.path.isdir(dest_ui):
                shutil.rmtree(dest_ui, ignore_errors=True)
            shutil.copytree(src_ui, dest_ui)
        info = {
            "version": version,
            "url": "/files/launcher/Launcher.exe",
            "note": str(payload.get("note", "")).strip(),
        }
        try:
            with open(os.path.join(launcher_dir, "version.json"), "w", encoding="utf-8") as f:
                json.dump(info, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"写入 version.json 失败：{exc}"}, status=500)
            return
        self._send_json({"ok": True, "message": f"启动器版本已发布：{version}", "version": info})

    def _api_launcher_runtime(self):
        """运行时资源清单：ui/ + SelfUpdater.exe + launcher_config.json。
        客户端首次运行据此下载，只需 Launcher.exe 即可启动。"""
        root = os.path.join(self.cfg["data_dir"], "launcher")
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
            files.append({
                "path": rel,
                "url": f"/files/launcher/{rel}",
                "hash": digest,
                "size": size,
            })
        digest_ctx = hashlib.md5()
        for f in files:
            digest_ctx.update(f["path"].encode("utf-8"))
            digest_ctx.update(b"\0")
            digest_ctx.update(str(f["size"]).encode("utf-8"))
            digest_ctx.update(b"\0")
            digest_ctx.update(f["hash"].encode("utf-8"))
            digest_ctx.update(b"\0")
        self._send_json({
            "ok": True,
            "version": digest_ctx.hexdigest()[:16] if files else "",
            "files": files,
        })

    def _api_launcher_versions(self):
        """启动器历史版本列表。GET /api/launcher/versions"""
        versions = self.cfg.get("launcher_versions") or {}
        items = []
        for version, path in sorted(versions.items(), key=lambda kv: version_key(kv[0]), reverse=True):
            resolved = resolve_server_path(path) if path else ""
            info = {
                "version": version,
                "path": resolved,
                "exists": os.path.isdir(resolved) if resolved else False,
                "launcherExe": os.path.isfile(os.path.join(resolved, "Launcher.exe")) if resolved else False,
                "selfUpdaterExe": os.path.isfile(os.path.join(resolved, "SelfUpdater.exe")) if resolved else False,
                "exeSize": os.path.getsize(os.path.join(resolved, "Launcher.exe")) if resolved and os.path.isfile(os.path.join(resolved, "Launcher.exe")) else 0,
            }
            items.append(info)
        published = ""
        pub = _read_json(os.path.join(self.cfg["data_dir"], "launcher", "version.json")) or {}
        published = pub.get("version", "")
        self._send_json({"ok": True, "versions": items, "published": published, "publishedNote": pub.get("note", "")})

    def _api_launcher_versions_update(self):
        """添加/更新启动器历史版本。POST JSON {version, dir}"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception:
            self._send_json({"ok": False, "error": "请求体必须是 JSON"}, status=400)
            return
        version = str(payload.get("version", "")).strip()
        raw_dir = str(payload.get("dir", "")).strip()
        if not version or not raw_dir:
            self._send_json({"ok": False, "error": "版本号和文件夹路径不能为空"}, status=400)
            return
        resolved = resolve_server_path(raw_dir)
        if not os.path.isdir(resolved):
            self._send_json({"ok": False, "error": f"版本文件夹不存在：{resolved}"}, status=400)
            return
        if not os.path.isfile(os.path.join(resolved, "Launcher.exe")):
            self._send_json({"ok": False, "error": f"版本文件夹中缺少 Launcher.exe：{resolved}"}, status=400)
            return

        cfg_path = self.cfg.get("_config_path")
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                disk = json.load(f)
        except Exception:
            disk = dict(self.cfg)
        versions = dict(disk.get("launcher_versions") or {})
        versions[version] = raw_dir
        disk["launcher_versions"] = versions
        self.cfg["launcher_versions"] = {v: resolve_server_path(p) for v, p in versions.items()}
        try:
            with open(cfg_path, "w", encoding="utf-8") as f:
                json.dump(disk, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存 config.json 失败：{exc}"}, status=500)
            return
        self._send_json({"ok": True, "message": f"启动器版本 {version} 已保存", "versions": self.cfg["launcher_versions"]})

    def _api_launcher_versions_delete(self, version):
        """删除启动器历史版本（仅移除配置，不删除磁盘文件）。"""
        version = os.path.basename(unquote(version))
        cfg_path = self.cfg.get("_config_path")
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                disk = json.load(f)
        except Exception:
            disk = dict(self.cfg)
        versions = dict(disk.get("launcher_versions") or {})
        if version not in versions:
            self._send_json({"ok": False, "error": f"版本 {version} 不存在"}, status=404)
            return
        del versions[version]
        disk["launcher_versions"] = versions
        self.cfg["launcher_versions"] = {v: resolve_server_path(p) for v, p in versions.items()}
        try:
            with open(cfg_path, "w", encoding="utf-8") as f:
                json.dump(disk, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存 config.json 失败：{exc}"}, status=500)
            return
        self._send_json({"ok": True, "message": f"已移除版本 {version}（磁盘文件保留）"})

    def _api_launcher_background(self):
        """背景资源清单。GET /api/launcher/background（客户端据此自动更新背景图）"""
        bg_dir = launcher_bg_dir(self.cfg)
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
                files.append({
                    "name": name,
                    "url": f"/files/background/{name}",
                    "hash": digest,
                    "size": size,
                })
        meta = _read_json(os.path.join(self.cfg["data_dir"], "launcher", "background.json")) or {}
        fps = int(meta.get("frameFps", 12) or 12)
        # 内容指纹版本：文件名+大小+MD5+FPS，内容不变版本不变（避免重复下载）
        digest_ctx = hashlib.md5()
        for f in files:
            digest_ctx.update(f["name"].encode("utf-8"))
            digest_ctx.update(b"\0")
            digest_ctx.update(str(f["size"]).encode("utf-8"))
            digest_ctx.update(b"\0")
            digest_ctx.update(f["hash"].encode("utf-8"))
            digest_ctx.update(b"\0")
        digest_ctx.update(f"fps={fps}".encode("utf-8"))
        version = digest_ctx.hexdigest()[:16] if files else ""
        self._send_json({
            "ok": True,
            "version": version,
            "frameFps": fps,
            "dir": bg_dir if os.path.isdir(bg_dir) else "",
            "files": files,
        })

    def _api_launcher_background_dir(self):
        """设置背景序列帧来源文件夹。POST JSON {dir}"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception:
            self._send_json({"ok": False, "error": "请求体必须是 JSON"}, status=400)
            return
        raw = str(payload.get("dir", "")).strip()
        if not raw:
            self._send_json({"ok": False, "error": "文件夹路径不能为空"}, status=400)
            return
        resolved = resolve_server_path(raw)
        if not os.path.isdir(resolved):
            self._send_json({"ok": False, "error": f"背景文件夹不存在：{resolved}"}, status=400)
            return
        cfg_path = self.cfg.get("_config_path")
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                disk = json.load(f)
        except Exception:
            disk = dict(self.cfg)
        disk["background_dir"] = raw
        self.cfg["background_dir"] = resolved
        try:
            with open(cfg_path, "w", encoding="utf-8") as f:
                json.dump(disk, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存 config.json 失败：{exc}"}, status=500)
            return
        self._send_json({"ok": True, "message": f"背景目录已设为 {resolved}"})

    def _api_launcher_background_update(self):
        """更新背景资源设置（FPS）。POST JSON {frameFps}"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception:
            self._send_json({"ok": False, "error": "请求体必须是 JSON"}, status=400)
            return
        try:
            fps = int(payload.get("frameFps", 12))
        except (TypeError, ValueError):
            self._send_json({"ok": False, "error": "frameFps 必须是数字"}, status=400)
            return
        if fps <= 0:
            self._send_json({"ok": False, "error": "frameFps 必须大于 0"}, status=400)
            return
        path = os.path.join(self.cfg["data_dir"], "launcher", "background.json")
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"frameFps": fps}, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存失败：{exc}"}, status=500)
            return
        self._send_json({"ok": True, "message": f"背景序列帧 FPS 已设为 {fps}"})

    def _api_launcher_background_clear(self):
        """清除背景目录配置（不删除磁盘上的文件，客户端将不再更新背景）。"""
        cfg_path = self.cfg.get("_config_path")
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                disk = json.load(f)
        except Exception:
            disk = dict(self.cfg)
        disk["background_dir"] = ""
        self.cfg["background_dir"] = ""
        try:
            with open(cfg_path, "w", encoding="utf-8") as f:
                json.dump(disk, f, ensure_ascii=False, indent=2)
        except Exception as exc:
            self._send_json({"ok": False, "error": f"保存 config.json 失败：{exc}"}, status=500)
            return
        self._send_json({"ok": True, "message": "已清除背景目录配置（磁盘文件保留）"})

    def _serve_background_file(self, rel):
        root = launcher_bg_dir(self.cfg)
        abs_path = safe_join(root, os.path.basename(rel))
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        self._send_file(abs_path)

    def _serve_launcher_file(self, rel):
        root = os.path.join(self.cfg["data_dir"], "launcher")
        abs_path = safe_join(root, rel)
        if abs_path is None:
            self._send_json({"ok": False, "error": "非法路径"}, status=400)
            return
        self._send_file(abs_path)


def main():
    parser = argparse.ArgumentParser(description="CloudUpdate 管理服务器")
    parser.add_argument("--config", default=None, help="config.json 路径")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    args = parser.parse_args()

    cfg = load_config(args.config)
    if args.host:
        cfg["host"] = args.host
    if args.port:
        cfg["port"] = args.port
    ensure_dirs(cfg)

    # 首次启动自动重建索引，避免空数据
    build_versions_index(cfg)

    UpdateHandler.cfg = cfg

    # 端口可能被上一个正在退出的进程短暂占用，自动重试
    server = None
    last_error = None
    for _attempt in range(20):
        try:
            server = ThreadingHTTPServer((cfg["host"], cfg["port"]), UpdateHandler)
            break
        except OSError as exc:
            last_error = exc
            time.sleep(0.5)
    if server is None:
        print(f"无法监听 {cfg['host']}:{cfg['port']}：{last_error}")
        sys.exit(1)
    UpdateHandler.server = server
    print("=" * 60)
    print("CloudUpdate 管理服务器已启动")
    print(f"  地址     : http://{cfg['host']}:{cfg['port']}")
    print(f"  管理页面 : http://127.0.0.1:{cfg['port']}/")
    print(f"  项目     : {cfg['project']}  platforms: {','.join(cfg['platforms'])}")
    print(f"  数据目录 : {cfg['data_dir']}")
    print("  按 Ctrl+C 停止")
    print("=" * 60)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n正在停止...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
