# -*- coding: utf-8 -*-
"""配置加载、路径解析与版本号辅助（CloudUpdate 管理服务器）。

配置中的相对路径一律以 **config.json 所在目录** 为基准（而不是程序/exe 所在目录），
这样把 exe 放到 dist/ 等子目录、或用 --config 指定别处的配置时，
data、web、../HotPatherPack 这类相对路径依然指向同一批真实资源。
未加载配置时退化为本文件（或 exe）所在目录。
"""

import json
import os
import re
import sys
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
# PyInstaller 单文件打包后 __file__ 指向临时解压目录，改用 exe 实际所在目录作为基准
if getattr(sys, "frozen", False):
    BASE_DIR = Path(sys.executable).resolve().parent

# 相对路径基准：load_config() 会把它设为 config.json 所在目录。
# 在配置加载前（例如查找默认配置文件）退化为 BASE_DIR。
CONFIG_DIR = BASE_DIR


def set_config_dir(path):
    """设置相对路径基准目录（传入 config.json 路径或其所在目录）。"""
    global CONFIG_DIR
    p = Path(path).resolve()
    CONFIG_DIR = p.parent if p.is_file() else p
    return CONFIG_DIR


def resolve_server_path(path, base=None):
    """把配置中的路径解析为绝对路径（相对路径以 config.json 所在目录为基准）。"""
    p = os.path.expanduser(str(path).strip())
    # 注意：故意不调用 os.path.expandvars —— 配置路径里合法的 `%VAR%` / `$VAR` 字符
    # 会被当成环境变量静默改写（例如含 % 的普通目录名会被替换或拼出非法路径），
    # 造成“配置看似正确、实则指向错误目录”的隐患。家目录 `~` 仍按预期展开。
    if not p:
        return ""
    if not os.path.isabs(p):
        base_dir = Path(base).resolve() if base else CONFIG_DIR
        p = str((base_dir / p).resolve())
    return os.path.abspath(p)


def resolve_base_package_path(path, root=""):
    """基础包目录解析：root 给定且 path 非绝对时，相对 root 解析（全局基础包根目录）；
    否则按常规服务器路径解析。返回绝对路径。"""
    path = str(path or "").strip()
    if not path:
        return ""
    if root and not os.path.isabs(path):
        return os.path.abspath(os.path.join(str(resolve_server_path(root)), path))
    return resolve_server_path(path)



def load_config(config_path=None):
    if config_path is None:
        config_path = BASE_DIR / "config.json"
    with open(config_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    cfg["_config_path"] = str(Path(config_path).resolve())
    # 后续所有相对路径都以 config.json 所在目录为基准
    set_config_dir(cfg["_config_path"])
    cfg.setdefault("host", "0.0.0.0")
    cfg.setdefault("port", 8710)
    cfg.setdefault("project", "CodeBuild")
    cfg.setdefault("platforms", ["Windows"])
    cfg.setdefault("data_dir", "data")
    cfg.setdefault("web_dir", "web")
    cfg.setdefault("admin_token", "")
    cfg.setdefault("package_roots", {})
    cfg.setdefault("base_packages", {})
    cfg.setdefault("manifest_exclude_patterns", [])
    cfg.setdefault("manifest_hash", "md5")
    cfg.setdefault("max_upload_mb", 2048)
    cfg.setdefault("hotpatcher_source", "")
    cfg.setdefault("hotpatcher_order", "")
    cfg.setdefault("version_library_dir", os.path.join(cfg["data_dir"], "versions"))
    cfg["data_dir"] = resolve_server_path(cfg["data_dir"])
    cfg["web_dir"] = resolve_server_path(cfg["web_dir"])
    cfg["version_library_dir"] = resolve_server_path(cfg["version_library_dir"])
    cfg["versions_dir"] = cfg["version_library_dir"]
    cfg["hotpatcher_source"] = resolve_server_path(cfg["hotpatcher_source"])

    # 全局基础包根目录：base_packages 中的条目若为非绝对路径，则相对它解析（全局地址）
    cfg.setdefault("base_packages_root", "")
    bp_root_abs = resolve_server_path(cfg.get("base_packages_root", ""))
    cfg["base_packages_root"] = bp_root_abs

    # 迁移旧的 package_roots（单基础包）到 base_packages（多版本基础包）
    if not cfg["base_packages"] and cfg.get("package_roots"):
        migrated = {}
        for platform, root in cfg["package_roots"].items():
            if isinstance(root, dict):
                migrated[platform] = {
                    str(k): resolve_base_package_path(v, bp_root_abs)
                    for k, v in root.items()
                }
            else:
                migrated[platform] = {"1.0": resolve_base_package_path(root, bp_root_abs)}
        cfg["base_packages"] = migrated
    else:
        resolved_bp = {}
        for platform, versions in cfg["base_packages"].items():
            resolved_bp[platform] = {
                str(version): resolve_base_package_path(path, bp_root_abs)
                for version, path in (versions or {}).items()
            }
        cfg["base_packages"] = resolved_bp

    # 兼容字段：package_roots 指向各平台最新基础包
    cfg["package_roots"] = {}
    for platform in cfg["platforms"]:
        latest = get_latest_base_version(cfg, platform)
        cfg["package_roots"][platform] = get_base_dir(cfg, platform, latest)
    cfg["manifests_dir"] = os.path.join(cfg["data_dir"], "manifests")

    # HTTPS 与对象存储（云 OSS/COS）配置默认值
    cfg.setdefault("https", {
        "enabled": False,
        "certFile": "",
        "keyFile": "",
        "autoGenSelfSigned": True,
        "country": "CN",
        "commonName": "CloudUpdate",
    })
    cfg.setdefault("storage", {"backend": "local", "s3": {}})

    # 启动器版本源目录：相对路径同样以 config.json 所在目录为基准，
    # 否则会跟随进程当前工作目录漂移（双击启动时尤其不可控）。
    launcher_versions = cfg.get("launcher_versions") or {}
    cfg["launcher_versions"] = {
        str(version): resolve_server_path(path)
        for version, path in launcher_versions.items()
        if str(path).strip()
    }

    # HTTPS 证书/私钥路径同理
    https_cfg = cfg.get("https") or {}
    for key in ("certFile", "keyFile"):
        if https_cfg.get(key):
            https_cfg[key] = resolve_server_path(https_cfg[key])
    cfg["https"] = https_cfg

    return cfg



def ensure_dirs(cfg):
    os.makedirs(cfg["versions_dir"], exist_ok=True)
    os.makedirs(cfg["manifests_dir"], exist_ok=True)



def safe_join(root, rel_path):
    """将 rel_path 安全地解析到 root 下，防止路径穿越。"""
    root = os.path.abspath(root)
    candidate = os.path.abspath(os.path.join(root, rel_path))
    if os.path.commonpath([root, candidate]) != root:
        return None
    return candidate



def parse_multipart(content_type, body):
    """极简 multipart/form-data 解析，返回 [(field_name, filename_or_None, bytes)]。"""
    if not content_type or "boundary=" not in content_type:
        return []
    boundary = content_type.split("boundary=", 1)[1].strip().strip('"').encode("utf-8")
    delimiter = b"--" + boundary
    parts = []
    for raw_part in body.split(delimiter):
        raw_part = raw_part.strip(b"\r\n")
        if not raw_part or raw_part == b"--":
            continue
        header_blob, _, content = raw_part.partition(b"\r\n\r\n")
        headers = {}
        for line in header_blob.split(b"\r\n"):
            key, _, value = line.partition(b":")
            headers[key.strip().lower().decode("utf-8", "replace")] = value.strip().decode("utf-8", "replace")
        disposition = headers.get("content-disposition", "")
        name = ""
        filename = None
        for token in disposition.split(";"):
            token = token.strip()
            if token.lower().startswith("name="):
                name = token[5:].strip('"')
            elif token.lower().startswith("filename="):
                filename = token[9:].strip('"')
        parts.append((name, filename, content))
    return parts



def version_key(version):
    """版本号比较键：1.0 < 1.2 < 1.4 < 2.0。"""
    key = []
    for part in re.split(r"[.\-_]", str(version)):
        key.append((1, int(part), "") if part.isdigit() else (0, 0, part))
    return tuple(key)



def get_base_packages(cfg, platform):
    """返回该平台的 {基础包版本号: 绝对路径}（自动过滤不存在的目录）。"""
    result = {}
    for version, path in (cfg.get("base_packages", {}).get(platform) or {}).items():
        path = str(path or "")
        if path and os.path.isdir(path):
            result[str(version)] = os.path.abspath(path)
    return result



def get_latest_base_version(cfg, platform):
    versions = list(get_base_packages(cfg, platform).keys())
    if not versions:
        return ""
    return max(versions, key=version_key)



def get_base_dir(cfg, platform, base_version=None):
    """获取基础包目录；base_version 为空时取该平台最新版本。"""
    packages = get_base_packages(cfg, platform)
    if not packages:
        return ""
    if base_version and base_version in packages:
        return packages[base_version]
    latest = get_latest_base_version(cfg, platform)
    return packages.get(latest, "")



def launcher_bg_dir(cfg):
    """背景序列帧来源目录：优先 background_dir 配置，其次 data/launcher/background。"""
    raw = cfg.get("background_dir") or ""
    if raw:
        resolved = resolve_server_path(raw)
        if os.path.isdir(resolved):
            return resolved
    return os.path.join(cfg["data_dir"], "launcher", "background")

