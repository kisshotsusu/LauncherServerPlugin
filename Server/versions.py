# -*- coding: utf-8 -*-
"""版本索引、更新链与更新描述（CloudUpdate 管理服务器）。"""

import json
import os
import time
from urllib.parse import quote, unquote

from config import ensure_dirs, get_base_packages, get_latest_base_version, version_key
from manifest import build_base_descriptor
from storage import get_storage


def _read_json(path, default=None):
    if not os.path.exists(path):
        return default
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default



def _revoked_store_path(cfg):
    return os.path.join(cfg["data_dir"], "revoked.json")


def load_revoked_store(cfg):
    """已删除版本的快照库（versionId -> {versionId, type, files}），持久化在 data/revoked.json。"""
    return _read_json(_revoked_store_path(cfg)) or {}


def save_revoked_store(cfg, store):
    path = _revoked_store_path(cfg)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(store, f, ensure_ascii=False, indent=2)


def _revoked_entry_from_descriptor(cfg, version_id):
    """从描述文件生成"被撤销版本"条目（versionId + 文件清单），供客户端精确删除。"""
    desc = read_descriptor(cfg, version_id)
    if not desc:
        return None
    files = []
    for f in desc.get("files", []):
        fn = f.get("fileName")
        if not fn:
            continue
        files.append({
            "fileName": fn,
            "targetRelativePath": f.get("targetRelativePath", ""),
            "hash": f.get("hash", ""),
            "size": f.get("size", 0),
            "kind": f.get("kind", ""),
        })
    return {"versionId": version_id, "type": desc.get("type", ""), "files": files}


def _revoked_entry_from_base(cfg, platform, version_id):
    """从基础包整包描述生成"被撤销版本"条目（versionId + 文件清单），供客户端精确删除旧基础包残留文件。

    与 _revoked_entry_from_descriptor 的区别：基础包没有版本文件库中的 descriptor.json，
    而是动态生成的整包描述（build_base_descriptor）。
    """
    desc = build_base_descriptor(cfg, platform, version_id)
    if not desc:
        return None
    files = []
    for f in desc.get("files", []):
        trp = f.get("targetRelativePath", "")
        if not trp:
            continue
        files.append({
            "fileName": f.get("fileName") or os.path.basename(trp),
            "targetRelativePath": trp,
            "hash": f.get("hash", ""),
            "size": f.get("size", 0),
            "kind": f.get("kind", ""),
        })
    return {"versionId": version_id, "type": "full", "files": files}


def build_versions_index(cfg, explicit_order=None):
    """扫描版本文件库与基础包目录生成版本索引（补丁 + 基础包多版本）。"""
    ensure_dirs(cfg)
    patch_versions = []
    versions_dir = cfg["versions_dir"]
    if os.path.isdir(versions_dir):
        for entry in sorted(os.listdir(versions_dir)):
            desc_path = os.path.join(versions_dir, entry, "descriptor.json")
            desc = _read_json(desc_path)
            if not desc:
                continue
            info = {
                "versionId": desc.get("versionId", entry),
                "baseVersionId": desc.get("baseVersionId", ""),
                "date": desc.get("date", ""),
                "type": desc.get("type", "patch"),
                "url": f"/api/version/{quote(entry)}",
                "changedAssetCount": desc.get("changedAssetCount", 0),
                "deletedAssetCount": desc.get("deletedAssetCount", 0),
                "totalSizeBytes": sum(f.get("size", 0) for f in desc.get("files", [])),
            }
            patch_versions.append(info)

    # 基础包版本（多版本整包）
    base_versions = {}
    base_ids_all = set()
    for platform in cfg["platforms"]:
        base_versions[platform] = []
        packages = get_base_packages(cfg, platform)
        for version in sorted(packages.keys(), key=version_key):
            base_versions[platform].append(version)
            base_ids_all.add(version)
            # 基础包整包优先：移除版本库中同名的补丁/整包条目，避免重复
            patch_versions = [v for v in patch_versions if v["versionId"] != version]
            manifest = _read_json(os.path.join(
                cfg["manifests_dir"], f"{cfg['project']}_{platform}_{version}.json"))
            total_size = sum(f.get("size", 0) for f in (manifest or {}).get("files", [])) if manifest else 0
            patch_versions.append({
                "versionId": version,
                "baseVersionId": "",
                "date": (manifest or {}).get("generatedAt", ""),
                "type": "full",
                "url": f"/api/version/{quote(version)}?platform={quote(platform)}",
                "changedAssetCount": 0,
                "deletedAssetCount": 0,
                "totalSizeBytes": total_size,
            })

    # 补丁按日期降序，整体再按版本号降序（基础包 2.0 会排在补丁 1.4 之前）
    versions = sorted(patch_versions, key=lambda v: version_key(v["versionId"]), reverse=True)

    chain = []
    if explicit_order:
        order = [x.strip() for x in explicit_order.split(",") if x.strip()]
        for vid in order:
            for v in patch_versions:
                if v["versionId"] == vid and v["type"] != "full":
                    chain.append(vid)
                    break
    else:
        # 无显式顺序时：按日期升序取 patch 版本作为更新链
        chain = [v["versionId"] for v in sorted(patch_versions, key=lambda v: v["date"]) if v["type"] == "patch"]
    # 基础包版本不进更新链（整包已包含其内容，避免客户端整包后再打同名补丁）
    chain = [vid for vid in chain if vid not in base_ids_all]

    all_ids = [v["versionId"] for v in patch_versions]
    current = max(all_ids, key=version_key) if all_ids else ""
    index = {
        "schemaVersion": 1,
        "project": cfg["project"],
        "platforms": cfg["platforms"],
        "current": current,
        "versions": versions,
        "updateChain": chain,
        "baseVersions": base_versions,
        "revoked": [],
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    # 被更高基础包版本取代的旧基础包：自动注入 revoked，让客户端删除其整包残留文件
    # （例如更新到 1.5.3 后，旧的 1.4 基础包目录下的 Windows_1.4_PatchConfig.json 应被清理）。
    revoked_base = {}
    revoke_superseded = cfg.get("revoke_superseded_base_packages", True)
    enabled_map = {}
    for _plat, _ids in (cfg.get("enabled_versions") or {}).items():
        if isinstance(_ids, list):
            enabled_map[str(_plat)] = set(str(x) for x in _ids)
    for platform in cfg["platforms"]:
        pkgs = base_versions.get(platform) or []
        if len(pkgs) <= 1:
            continue
        latest_base = max(pkgs, key=version_key)
        for v in pkgs:
            if v == latest_base:
                continue
            # 显式在 enabled_versions 中保留的旧基础包：只在开启自动回收时撤销
            kept = (platform in enabled_map) and (v in enabled_map[platform])
            if revoke_superseded or not kept:
                entry = _revoked_entry_from_base(cfg, platform, v)
                if entry:
                    revoked_base[v] = entry

    # 注入已删除版本的快照：版本若被重新发布（仍在 all_ids 中）则从撤销库移除
    revoked_store = load_revoked_store(cfg)
    cleaned = {}
    for vid, entry in revoked_store.items():
        if vid in all_ids:
            continue
        cleaned[vid] = entry
    # 合并被取代/被隐藏的旧基础包（被取代的基础包即便仍配置也应撤销，故不按 all_ids 跳过）
    for vid, entry in revoked_base.items():
        cleaned[vid] = entry
    if cleaned:
        index["revoked"] = [cleaned[k] for k in sorted(cleaned.keys(), key=version_key)]
    if cleaned != revoked_store or revoked_base:
        save_revoked_store(cfg, cleaned)
    index_path = os.path.join(cfg["data_dir"], "versions.json")
    with open(index_path, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2)
    return index



def load_versions_index(cfg, rebuild=False):
    index_path = os.path.join(cfg["data_dir"], "versions.json")
    index = _read_json(index_path)
    if index is None or rebuild:
        index = build_versions_index(cfg)
    return index



def filter_index_by_enabled(index, cfg):
    """按 enabled_versions 配置过滤客户端可见版本。
    未配置任何开放列表 = 全部开放；配置后只开放列表内的版本。
    管理端（?all=1）不受影响，始终看到完整索引。
    """
    if not index:
        return index
    ev = cfg.get("enabled_versions") or {}
    configured = {}
    for platform, ids in ev.items():
        if isinstance(ids, list) and ids:
            configured[str(platform)] = set(str(x) for x in ids)
    if not configured:
        return index
    allowed_any = set()
    for ids in configured.values():
        allowed_any |= ids

    original_versions = list(index.get("versions") or [])

    index = dict(index)
    index["versions"] = [v for v in (index.get("versions") or [])
                         if v.get("versionId") in allowed_any]
    index["updateChain"] = [x for x in (index.get("updateChain") or [])
                            if x in allowed_any]
    bv = dict(index.get("baseVersions") or {})
    for platform, ids in bv.items():
        if platform in configured:
            bv[platform] = [x for x in ids if x in configured[platform]]
    index["baseVersions"] = bv
    all_ids = [v.get("versionId") for v in index.get("versions", [])]
    if all_ids:
        index["current"] = max(all_ids, key=version_key)

    # 被隐藏的版本（含整包基础包）：客户端若已下载应删除。从描述文件快照文件清单注入 revoked
    hidden_ids = [v.get("versionId") for v in original_versions
                  if v.get("versionId") not in allowed_any]
    revoked = list(index.get("revoked") or [])
    seen = {r.get("versionId") for r in revoked}
    for vid in hidden_ids:
        if vid in seen:
            continue
        entry = _revoked_entry_from_descriptor(cfg, vid)
        if entry:
            revoked.append(entry)
            seen.add(vid)
    if revoked:
        index["revoked"] = revoked
    return index



def _rewrite_descriptor_urls(cfg, desc):
    """远程存储（s3）下，把描述里 /files/... 相对路径改写为 presigned URL，供客户端直连对象存储下载。"""
    storage = get_storage(cfg)
    if not storage.is_remote or not desc:
        return
    for f in desc.get("files", []) or []:
        url = f.get("url") or ""
        if url.startswith("/files/"):
            f["url"] = storage.url_for_key(url[len("/files/"):])


def read_descriptor(cfg, version_id):
    version_id = os.path.basename(unquote(version_id))
    # 基础包版本：在任一平台中找到则动态生成整包描述（优先于版本文件库中的同名 full 描述）
    for platform in cfg["platforms"]:
        if version_id in get_base_packages(cfg, platform):
            return build_base_descriptor(cfg, platform, version_id)
    desc_path = os.path.join(cfg["versions_dir"], version_id, "descriptor.json")
    desc = _read_json(desc_path)
    if desc:
        _rewrite_descriptor_urls(cfg, desc)
        return desc
    return None

