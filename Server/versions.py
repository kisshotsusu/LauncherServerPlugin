# -*- coding: utf-8 -*-
"""版本索引、更新链与更新描述（CloudUpdate 管理服务器）。"""

import json
import os
import time
from urllib.parse import quote, unquote

from config import ensure_dirs, get_base_packages, get_latest_base_version, version_key
from manifest import build_base_descriptor, generate_manifest
from storage import get_storage
from packages import base_metadata, list_installers, installer_descriptor, package_kind, version_key_for_package, version_is_enabled


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
    desc = read_descriptor(cfg, version_id, cfg.get("_selected_platform"), cfg.get("_selected_kind"))
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
    return {"versionId": version_id, "platform": desc.get("platform", cfg["platforms"][0]), "type": desc.get("type", ""), "packageKind": desc.get("packageKind", "patch"), "files": files}


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
    return {"versionId": version_id, "platform": platform, "type": desc["type"], "packageKind": "base", "files": files}


def build_versions_index(cfg, explicit_order=None):
    """扫描版本文件库与基础包目录生成版本索引（补丁 + 基础包多版本）。"""
    ensure_dirs(cfg)
    patch_versions = []
    invalid_versions = []
    versions_dir = cfg["versions_dir"]
    for platform in cfg["platforms"]:
        platform_dir = os.path.join(versions_dir, platform)
        if not os.path.isdir(platform_dir):
            continue
        for entry in sorted(os.listdir(platform_dir)):
            if entry.startswith("."):
                continue
            desc = _read_json(os.path.join(platform_dir, entry, "descriptor.json"))
            if not desc:
                continue
            if desc.get("type") == "full" and not desc.get("files"):
                invalid_versions.append({"platform":platform,"versionId":entry,"packageKind":"patch","type":"resource",
                                         "valid":False,"error":"旧基础包描述没有文件，请重新导入资源基础包或程序包",
                                         "date":desc.get("date", ""),"fileCount":0,"totalSizeBytes":0,"changedAssetCount":0,"deletedAssetCount":0})
                continue
            patch_versions.append({
                "platform": platform, "versionId": entry,
                "baseVersionId": desc.get("baseVersionId", ""), "date": desc.get("date", ""),
                "type": "resource" if desc.get("type") == "full" else desc.get("type", "patch"),
                "packageKind": "patch",
                "gameVersion": desc.get("gameVersion", ""),
                "resourceVersion": desc.get("resourceVersion") or entry,
                "requiredGameVersion": desc.get("requiredGameVersion", ""),
                "fileCount": len(desc.get("files", [])),
                "url": f"/api/version/{quote(entry)}?platform={quote(platform)}&kind=patch",
                "changedAssetCount": desc.get("changedAssetCount", 0),
                "deletedAssetCount": desc.get("deletedAssetCount", 0),
                "totalSizeBytes": sum(f.get("size", 0) for f in desc.get("files", [])),
            })

    base_versions, installer_versions = {}, {}
    for platform in cfg["platforms"]:
        base_versions[platform] = []
        for version, folder in sorted(get_base_packages(cfg, platform).items(), key=lambda pair: version_key(pair[0])):
            metadata = base_metadata(cfg, platform, version)
            record = {"platform": platform, "versionId": version, "baseVersionId": "",
                      "type": "resource", "packageKind": "base", **metadata,
                      "url": f"/api/version/{quote(version)}?platform={quote(platform)}&kind=base",
                      "changedAssetCount": 0, "deletedAssetCount": 0}
            try:
                manifest = generate_manifest(cfg, platform, version)
            except (ValueError, OSError, RuntimeError) as exc:
                invalid_versions.append(dict(record, valid=False, error=str(exc), fileCount=0, totalSizeBytes=0, date=""))
                continue
            base_versions[platform].append(version)
            record.update(date=manifest.get("generatedAt", ""), fileCount=len(manifest.get("files", [])),
                          totalSizeBytes=sum(f.get("size", 0) for f in manifest.get("files", [])))
            patch_versions.append(record)
        installer_versions[platform] = []
        for item in list_installers(cfg, platform):
            installer_versions[platform].append(item["versionId"])
            patch_versions.append({"platform": platform, "versionId": item["versionId"], "baseVersionId": "",
                                   "type": "full", "packageKind": "installer", "gameVersion": item["gameVersion"],
                                   "resourceVersion": item["resourceVersion"], "date": item["date"],
                                   "fileCount": item["fileCount"], "totalSizeBytes": item["size"], "changedAssetCount": 0, "deletedAssetCount": 0,
                                   "url": f"/api/version/{quote(item['versionId'])}?platform={quote(platform)}&kind=installer"})

    # 补丁按日期降序，整体再按版本号降序（基础包 2.0 会排在补丁 1.4 之前）
    versions = sorted(patch_versions, key=lambda v: version_key(v["versionId"]), reverse=True)

    platform_chains = {}
    for platform in cfg["platforms"]:
        candidates = [v for v in versions if v.get("platform") == platform and v["type"] == "patch"]
        ids = {v["versionId"] for v in candidates}
        order = (cfg.get("platform_settings", {}).get(platform) or {}).get("hotpatcherOrder", explicit_order or "")
        if order:
            chain = list(dict.fromkeys(v.strip() for v in order.split(",") if v.strip() in ids))
        else:
            chain = [v["versionId"] for v in sorted(candidates, key=lambda v: (v["date"], version_key(v["versionId"])))]
        platform_chains[platform] = chain
    chain = platform_chains.get(cfg.get("default_platform", cfg["platforms"][0]), [])

    all_ids = [v["versionId"] for v in patch_versions]
    current = max(all_ids, key=version_key) if all_ids else ""
    index = {
        "schemaVersion": 2,
        "importPolicyVersion": 1,
        "packageProtocolVersion": 3,
        "platformUpdateChains": platform_chains,
        "project": cfg["project"],
        "platforms": cfg["platforms"],
        "current": current,
        "versions": versions,
        "updateChain": chain,
        "baseVersions": base_versions,
        "installerVersions": installer_versions,
        "invalidVersions": invalid_versions,
        "revoked": [],
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    # Publishing a newer resource base must not revoke files still used by older clients.
    revoked_base = {}

    # 注入已删除版本的快照：版本若被重新发布（仍在 all_ids 中）则从撤销库移除
    revoked_store = load_revoked_store(cfg)
    cleaned = {}
    for vid, entry in revoked_store.items():
        if any(v["versionId"] == entry.get("versionId", vid) and v.get("platform") == entry.get("platform", cfg["platforms"][0]) and package_kind(v) == package_kind(entry) for v in patch_versions):
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
    if index is None or rebuild or index.get("schemaVersion") != 2 or index.get("importPolicyVersion") != 1 or index.get("packageProtocolVersion") != 3 or any("platform" not in v for v in index.get("versions", [])):
        index = build_versions_index(cfg)
    return index



def filter_index_by_enabled(index, cfg):
    if not index: return index
    configured = {p: set(ids) for p, ids in (cfg.get("enabled_versions") or {}).items() if isinstance(ids, list)}
    result = dict(index)
    original = index.get("versions", [])
    result["versions"] = [v for v in original if v.get("platform") not in configured or version_is_enabled(v, configured[v["platform"]])]
    visible = {(v.get("platform"), package_kind(v), v["versionId"]) for v in result["versions"]}
    for name, kind in (("baseVersions", "base"), ("installerVersions", "installer")):
        result[name] = {p: [vid for vid in ids if (p, kind, vid) in visible] for p, ids in index.get(name, {}).items()}
    chains = {p: [vid for vid in chain if (p, "patch", vid) in visible] for p, chain in index.get("platformUpdateChains", {}).items()}
    result["platformUpdateChains"] = chains
    selected = result.get("platform", (result.get("platforms") or cfg["platforms"])[0])
    result["updateChain"] = chains.get(selected, [])
    result["current"] = max((v["versionId"] for v in result["versions"]), key=version_key, default="")
    revoked = list(index.get("revoked", []))
    seen = {(r.get("platform"), r.get("packageKind"), r.get("versionId")) for r in revoked}
    for version in original:
        identity = (version.get("platform"), package_kind(version), version["versionId"])
        if identity in visible or identity in seen or version.get("type") == "full": continue
        try:
            entry = _revoked_entry_from_descriptor(dict(cfg, _selected_platform=identity[0], _selected_kind=identity[1]), identity[2])
        except (ValueError, RuntimeError):
            entry = None
        if entry: revoked.append(entry); seen.add(identity)
    result["revoked"] = revoked
    return result


def _rewrite_descriptor_urls(cfg, desc):
    """远程存储（s3）下，把描述里 /files/... 相对路径改写为 presigned URL，供客户端直连对象存储下载。"""
    storage = get_storage(cfg)
    if not storage.is_remote or not desc:
        return
    for f in desc.get("files", []) or []:
        for field in ("url", "fallbackUrl"):
            url = f.get(field) or ""
            if url.startswith("/files/"):
                f[field] = storage.url_for_key(url[len("/files/"):])


def read_descriptor(cfg, version_id, platform=None, kind=None):
    platform = platform or cfg.get("default_platform", cfg["platforms"][0])
    if platform not in cfg["platforms"] or not version_id or version_id in (".", "..") or any(c in version_id for c in "/\\"):
        return None
    if kind not in (None, "base", "patch", "installer"):
        raise ValueError("包类型必须是 base、patch 或 installer")
    if kind in (None, "base") and version_id in get_base_packages(cfg, platform):
        return build_base_descriptor(cfg, platform, version_id)
    if kind in (None, "patch"):
        desc = _read_json(os.path.join(cfg["versions_dir"], platform, version_id, "descriptor.json"))
        if desc:
            desc["packageKind"] = "patch"
            if desc.get("type") == "full": desc["type"] = "resource"
            _rewrite_descriptor_urls(cfg, desc)
            return desc
    if kind in (None, "installer"):
        return installer_descriptor(cfg, platform, version_id)
    return None


def filter_index_by_platform(index, platform):
    result = dict(index)
    result["platform"] = platform
    result["versions"] = [v for v in index.get("versions", []) if v.get("platform", index.get("platforms", [platform])[0]) == platform]
    ids = {v["versionId"] for v in result["versions"] if v.get("type") == "patch"}
    result["updateChain"] = list(dict.fromkeys(v for v in index.get("platformUpdateChains", {}).get(platform, index.get("updateChain", [])) if v in ids))
    result["platformUpdateChains"] = {platform: result["updateChain"]}
    result["baseVersions"] = {platform: index.get("baseVersions", {}).get(platform, [])}
    result["installerVersions"] = {platform: index.get("installerVersions", {}).get(platform, [])}
    result["invalidVersions"] = [v for v in index.get("invalidVersions", []) if v.get("platform") == platform]
    result["current"] = max((v["versionId"] for v in result["versions"]), key=version_key, default="")
    result["revoked"] = [v for v in index.get("revoked", []) if v.get("platform", index.get("platforms", [platform])[0]) == platform]
    result["platforms"] = [platform]
    return result


def add_version_channels(index):
    result = dict(index)
    versions = result.get("versions", [])
    applications = [v for v in versions if v.get("type") == "full"]
    application = max(applications, key=lambda v: version_key(v.get("gameVersion") or v["versionId"]), default=None)
    result["gameVersion"] = (application.get("gameVersion") or application["versionId"]) if application else ""
    resources = [v for v in versions if v.get("type") == "resource"]
    if application: resources.append(application)  # Bundled resources are a valid baseline.
    baseline = max(resources, key=lambda v: version_key(v.get("resourceVersion") or v["versionId"]), default=None)
    patches = {v["versionId"]: v for v in versions if v.get("type") == "patch"}
    chain = [patches[vid] for vid in result.get("updateChain", []) if vid in patches]
    resource = chain[-1] if chain else baseline
    if baseline and resource and version_key(baseline.get("resourceVersion") or baseline["versionId"]) > version_key(resource.get("resourceVersion") or resource["versionId"]):
        resource = baseline
    result["resourceVersion"] = (resource.get("resourceVersion") or resource["versionId"]) if resource else ""
    result["versionChannelsSchema"] = 2
    result["gameVersions"] = applications
    result["resourceVersions"] = [v for v in versions if v.get("type") != "full"]
    return result
