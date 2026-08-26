# -*- coding: utf-8 -*-
"""完整性清单生成与文件哈希（CloudUpdate 管理服务器）。"""

import fnmatch
import hashlib
import json
import os
import time

from config import get_base_dir, get_latest_base_version
from storage import get_storage


def hash_file(path, algo="md5", chunk_size=1024 * 1024):
    h = hashlib.new(algo)
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            size += len(chunk)
            h.update(chunk)
    return h.hexdigest(), size



def matches_any(rel_path, patterns):
    rel = rel_path.replace("\\", "/")
    for pat in patterns:
        p = pat.replace("\\", "/")
        if fnmatch.fnmatch(rel, p) or fnmatch.fnmatch(os.path.basename(rel), p):
            return True
        # 允许目录前缀模式，例如 Engine/Extras/
        if p.endswith("/") and rel.startswith(p):
            return True
    return False



def generate_manifest(cfg, platform, base_version=None, force=False):
    """扫描基础包目录生成完整性清单，返回清单 dict（按基础包版本缓存）。"""
    project = cfg["project"]
    # 显式指定了版本号但配置中不存在：直接报错（由调用方转为 404），
    # 避免回退到最新版本生成“张冠李戴”的清单。
    if base_version and base_version not in (cfg.get("base_packages", {}).get(platform) or {}):
        raise ValueError(f"平台 {platform} 未配置基础包版本 {base_version}")
    root = get_base_dir(cfg, platform, base_version)
    if not root or not os.path.isdir(root):
        raise RuntimeError(f"未配置或找不到平台 {platform} 的打包目录")
    version = base_version or get_latest_base_version(cfg, platform) or "base"

    cache_path = os.path.join(cfg["manifests_dir"], f"{project}_{platform}_{version}.json")
    latest_path = os.path.join(cfg["manifests_dir"], f"{project}_{platform}.json")
    for candidate in (cache_path, latest_path if version == get_latest_base_version(cfg, platform) else ""):
        if candidate and os.path.exists(candidate) and not force:
            with open(candidate, "r", encoding="utf-8") as f:
                cached = json.load(f)
            if cached.get("baseVersionId") == version:
                return cached
            # 缓存属于其他基础包版本，忽略并重新生成

    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            abs_path = os.path.join(dirpath, name)
            rel = os.path.relpath(abs_path, root).replace("\\", "/")
            if matches_any(rel, cfg["manifest_exclude_patterns"]):
                continue
            digest, size = hash_file(abs_path, cfg["manifest_hash"])
            files.append({
                "path": rel,
                "size": size,
                "hash": digest,
                "hashType": cfg["manifest_hash"],
            })

    files.sort(key=lambda x: x["path"])
    manifest = {
        "schemaVersion": 1,
        "project": project,
        "platform": platform,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "baseVersionId": version,
        "fileCount": len(files),
        "files": files,
    }
    os.makedirs(cfg["manifests_dir"], exist_ok=True)
    with open(cache_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    if version == get_latest_base_version(cfg, platform):
        with open(latest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=2)
    return manifest



def build_base_descriptor(cfg, platform, base_version):
    """为基础包版本动态生成更新描述（整包文件列表，客户端据此整包替换）。"""
    manifest = generate_manifest(cfg, platform, base_version)
    files = []
    for entry in manifest.get("files", []):
        rel = entry.get("path", "")
        if not rel:
            continue
        files.append({
            "fileName": os.path.basename(rel),
            "url": get_storage(cfg).url_for("packages", platform=platform, version=base_version, rel=rel),
            "targetRelativePath": rel,
            "hash": entry.get("hash", ""),
            "size": entry.get("size", 0),
            "kind": "ExternFile",
        })
    return {
        "schemaVersion": 1,
        "versionId": base_version,
        "baseVersionId": "",
        "date": manifest.get("generatedAt", ""),
        "type": "full",
        "changedAssetCount": 0,
        "deletedAssetCount": 0,
        "restartRequired": True,
        "ioStoreEnabled": False,
        "files": files,
    }

