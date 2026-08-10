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
  - 管理操作通过命令行子命令完成（不再依赖网页）：
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
  python run_server.py --config config.json serve --host 0.0.0.0 --port 8711
"""
import argparse
import hashlib
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
    load_config,
    ensure_dirs,
    safe_join,
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
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        query = parse_qs(parsed.query)

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
            "https": bool(self.cfg.get("https", {}).get("enabled")),
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
            "versionLibraryDir": self.cfg.get("version_library_dir", ""),
            "basePackageDirs": self.cfg.get("package_roots", {}),
            "basePackages": self.cfg.get("base_packages", {}),
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
        root = get_base_dir(self.cfg, platform, base_version)
        if not root or not os.path.isdir(root):
            self._send_json({"ok": False, "error": f"平台 {platform} 未配置基础包目录"}, status=404)
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
                    "downloadUrl": storage.url_for("packages", platform=platform, version=base_for_url, rel=rel),
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

    def _public_config(self):
        return {
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
            "enabledVersions": self.cfg.get("enabled_versions", {}),
            "launcherVersions": self.cfg.get("launcher_versions", {}),
            "backgroundDir": self.cfg.get("background_dir", ""),
            "storageBackend": "s3" if get_storage(self.cfg).is_remote else "local",
        }

    def _api_launcher_version(self):
        """启动器自升级版本信息。data/launcher/version.json: {version, url}"""
        version_path = os.path.join(self.cfg["data_dir"], "launcher", "version.json")
        info = _read_json(version_path)
        if not info:
            self._send_json({"ok": False, "error": "未发布启动器版本"}, status=404)
            return
        info.setdefault("url", get_storage(self.cfg).url_for("launcher", rel="Launcher.exe"))
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
            files.append({"path": rel, "url": storage.url_for("launcher", rel=rel), "hash": digest, "size": size})
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
                files.append({"name": name, "url": storage.url_for("background", name=name), "hash": digest, "size": size})
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
    print("=" * 60)
    print("CloudUpdate 管理服务器已启动")
    print(f"  地址     : {scheme}://{cfg['host']}:{cfg['port']}")
    print(f"  存储后端 : {'S3 对象存储' if get_storage(cfg).is_remote else '本地磁盘'}")
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


def cmd_reindex(args, cfg):
    idx = build_versions_index(cfg)
    print(f"索引已重建：当前版本 {idx['current']}，更新链 {' -> '.join(idx['updateChain']) or '无'}")


def cmd_gen_manifest(args, cfg):
    for platform in cfg["platforms"]:
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


def cmd_import_hotpatcher(args, cfg):
    run_import(cfg.get("hotpatcher_source", ""), cfg["data_dir"], cfg["project"],
               cfg["platforms"][0], cfg.get("hotpatcher_order", ""))
    # s3 模式：把补丁 pak 与外部文件同步到对象存储，否则客户端拿到的 presigned URL 会 404
    storage = get_storage(cfg)
    if not storage.is_remote:
        return
    versions_dir = cfg["versions_dir"]
    if not os.path.isdir(versions_dir):
        return
    platform0 = cfg["platforms"][0]
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
        print(f"  已同步版本 {entry} 到对象存储")


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


def main():
    parser = argparse.ArgumentParser(prog="CloudUpdateServer", description="CloudUpdate 管理服务器（独立程序）")
    parser.add_argument("--config", default=None, help="config.json 路径")
    parser.add_argument("--host", default=None, help="覆盖监听地址")
    parser.add_argument("--port", type=int, default=None, help="覆盖监听端口")
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("serve", help="启动 HTTP/HTTPS 服务（供启动器读取版本与下载）")
    sub.add_parser("reindex", help="重建版本索引")
    sub.add_parser("gen-manifest", help="重新生成完整性清单")
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
    if not args.command:
        parser.print_help()
        return
    cfg = load_config(args.config)
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
    dispatch[args.command](args, cfg)


if __name__ == "__main__":
    main()
