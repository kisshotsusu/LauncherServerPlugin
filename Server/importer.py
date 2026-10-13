# -*- coding: utf-8 -*-
"""导入 HotPatcher 产物到管理服务器数据目录（可被 CLI / exe 直接调用，无需 scripts 目录）。

原 scripts/import_hotpatcher.py 的导入逻辑已迁移至此，便于 PyInstaller 单文件打包。
通过 run_import(...) 调用；scripts/import_hotpatcher.py 仅作为命令行薄封装保留。
"""
import json
import os
import re
import shutil
import sys
import time
import tempfile
import uuid
from pathlib import Path

from config import get_base_dir, get_base_packages, load_config
from manifest import hash_file
from versions import build_versions_index


def read_json(path):
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            return json.load(f)
    except Exception as exc:
        print(f"  [警告] 读取 {path} 失败: {exc}")
        return None


def version_id_from_base_path(path):
    if not path:
        return ""
    name = os.path.basename(path)
    m = re.match(r"^(.+)_Release\.json$", name)
    return m.group(1) if m else ""


# 仅把这些前缀下的资产视为“游戏内容”的变更/删除；其余（/Engine/ 及 ControlRig/Niagara 等
# 引擎插件内置资产）属于不变更的内置内容——HotPatcher 的资产注册表 diff 会把它们误报为“删除”
# （实际并未改动，只是未重新写进补丁），故应排除，避免把 3000+ 内置资产错显成“删除”。
DEFAULT_GAME_ASSET_PREFIXES = ("/Game/",)


def count_asset_map(diff_obj, keep_prefixes=DEFAULT_GAME_ASSET_PREFIXES):
    if not diff_obj:
        return 0
    modules = diff_obj.get("assetsDependenciesMap", {})
    total = 0
    for module in modules.values():
        if isinstance(module, dict):
            for name in (module.get("assetDependencyDetails") or {}):
                if keep_prefixes and not any(name.startswith(p) for p in keep_prefixes):
                    continue
                total += 1
    return total


def parse_diff_counts(diff, game_prefixes=DEFAULT_GAME_ASSET_PREFIXES):
    if not diff:
        return 0, 0
    asset_diff = diff.get("assetDiffInfo", {})
    add = count_asset_map(asset_diff.get("addAssetDependInfo", {}), game_prefixes)
    modify = count_asset_map(asset_diff.get("modifyAssetDependInfo", {}), game_prefixes)
    delete = count_asset_map(asset_diff.get("deleteAssetDependInfo", {}), game_prefixes)
    return add + modify, delete


def find_diffs(source_dir, version_id, platform="Windows"):
    results = []
    for folder in (source_dir, os.path.join(source_dir, platform), os.path.join(source_dir, version_id), os.path.join(source_dir, version_id, platform)):
        if not os.path.isdir(folder):
            continue
        for name in os.listdir(folder):
            if not name.endswith("_Diff.json"):
                continue
            stem = name[: -len("_Diff.json")]
            if stem == version_id or stem.endswith("_" + version_id) or stem.startswith(version_id + "_"):
                results.append(os.path.join(folder, name))
    return sorted(set(results))


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def base_import_source(source, platform):
    """Accept a multi-platform release root or an Android APK + Data release."""
    platform_dir = os.path.join(source, platform)
    if os.path.isdir(platform_dir):
        source = platform_dir
    if platform.startswith("Android"):
        data = os.path.join(source, "Data")
        if os.path.isdir(data):
            source = data
        resource_files = [name for _, _, names in os.walk(source) for name in names
                          if name.lower().endswith((".pak", ".utoc", ".ucas", ".obb"))]
        if not resource_files:
            raise ValueError("安卓基础包目录缺少资源数据。请选择包含 pak 的完整 Data 或 StagedBuilds 目录；仅有 APK 的 Archive 目录不能作为资源基础包。")
    return source


def import_version(source_dir, dest_versions_dir, version_id, platform, package_root, config=None):
    """在临时目录完成导入后替换发布目录，重导入不保留已移除的旧补丁。"""
    if not re.fullmatch(r"[A-Za-z0-9_-]+", platform) or not re.fullmatch(r"[A-Za-z0-9_.-]+", version_id) or version_id in (".", ".."):
        raise ValueError("平台或版本标识无效")
    if not os.path.isdir(os.path.join(source_dir, version_id, platform)):
        raise ValueError(f"版本 {version_id} 缺少 {platform} 产物目录")
    root = os.path.abspath(os.path.join(dest_versions_dir, platform))
    os.makedirs(root, exist_ok=True)
    target = os.path.join(root, version_id)
    stage = tempfile.mkdtemp(prefix=".import-", dir=root)
    backup = os.path.join(root, ".replace-" + uuid.uuid4().hex)
    try:
        result = _import_version(source_dir, dest_versions_dir, version_id, platform, package_root, stage, config)
        if os.path.exists(target):
            os.replace(target, backup)
        try:
            os.replace(stage, target)
        except Exception:
            if os.path.exists(backup):
                os.replace(backup, target)
            raise
        return result
    finally:
        for temporary in (stage, backup):
            if os.path.isdir(temporary) and os.path.commonpath([root, os.path.abspath(temporary)]) == root:
                shutil.rmtree(temporary)


def _import_version(source_dir, dest_versions_dir, version_id, platform, package_root, destination, config=None):
    version_src = os.path.join(source_dir, version_id)
    dest = destination
    os.makedirs(dest, exist_ok=True)

    copied = []
    for suffix in ("_Release.json", "_PatchConfig.json", "_PakFilesInfo.json",
                   "_PakResults.json", "_ReleaseConfig.json"):
        candidates = [
            os.path.join(version_src, platform, f"{version_id}{suffix}"),
            os.path.join(version_src, f"{version_id}{suffix}"),
            os.path.join(source_dir, f"{version_id}{suffix}"),
        ]
        src = next((c for c in candidates if os.path.isfile(c)), None)
        if src and os.path.isfile(src):
            copy_file(src, os.path.join(dest, os.path.basename(src)))
            copied.append(os.path.basename(src))

    for diff_src in find_diffs(source_dir, version_id, platform):
        copy_file(diff_src, os.path.join(dest, os.path.basename(diff_src)))
        copied.append(os.path.basename(diff_src))

    platform_src = os.path.join(version_src, platform)
    files_dest = os.path.join(dest, "files")
    if not os.path.isdir(platform_src):
        raise ValueError(f"版本 {version_id} 缺少 {platform} 产物目录：{platform_src}")
    if os.path.isdir(platform_src):
        for dirpath, dirnames, filenames in os.walk(platform_src):
            for name in filenames:
                src = os.path.join(dirpath, name)
                rel = os.path.relpath(src, platform_src)
                copy_file(src, os.path.join(files_dest, rel))
                copied.append(f"files/{rel}")

    patch_config = read_json(os.path.join(dest, f"{version_id}_PatchConfig.json"))
    release = read_json(os.path.join(dest, f"{version_id}_Release.json"))
    pak_info = read_json(os.path.join(dest, f"{version_id}_PakFilesInfo.json"))

    base_version_id = ""
    if patch_config:
        base_obj = patch_config.get("baseVersion") or {}
        base_path = base_obj.get("filePath", "") if isinstance(base_obj, dict) else ""
        base_version_id = version_id_from_base_path(base_path)
    if not base_version_id and release:
        base_version_id = release.get("baseVersionId", "")
    # A patch must resolve external files from its own base, not the latest base
    # of this (or another) platform.
    if base_version_id and config is not None:
        package_root = get_base_packages(config, platform).get(base_version_id, "")

    b_io_store = False
    if patch_config:
        io_settings = patch_config.get("ioStoreSettings") or {}
        b_io_store = bool(io_settings.get("bIoStore", False))

    date = release.get("date", "") if release else ""
    vtype = "patch" if base_version_id else "full"

    files = []
    if os.path.isdir(files_dest):
        relative_files = sorted(os.path.relpath(os.path.join(folder, name), files_dest).replace("\\", "/")
                                for folder, _, names in os.walk(files_dest) for name in names)
        for name in relative_files:
            lower = name.lower()
            binary_patch = lower.endswith(".patch")
            resource_name = name[:-6] if binary_patch else name
            if not resource_name.lower().endswith((".pak", ".utoc", ".ucas")):
                continue
            # When a delta and its complete fallback are both present, publish
            # one transfer entry rather than overwriting the merged result twice.
            if not binary_patch and name + ".patch" in relative_files:
                continue
            abs_path = os.path.join(files_dest, name)
            digest, size = hash_file(abs_path)
            kind = "ContentPak" if resource_name.lower().endswith(".pak") else "IoStore"
            b_io_store |= kind == "IoStore"
            entry = {
                "fileName": os.path.basename(name),
                "url": f"/files/versions/{platform}/{version_id}/files/{name}",
                "targetRelativePath": name,
                "hash": digest,
                "size": size,
                "kind": kind,
            }
            if binary_patch:
                entry["binaryPatch"] = True
                fallback = os.path.join(files_dest, resource_name)
                if os.path.isfile(fallback):
                    fallback_hash, fallback_size = hash_file(fallback)
                    entry.update(fallbackUrl=f"/files/versions/{platform}/{version_id}/files/{resource_name}",
                                 fallbackSize=fallback_size, fallbackHash=fallback_hash)
            files.append(entry)

    if not files:
        raise ValueError(f"版本 {version_id} 的 {platform} 目录没有 pak / IoStore / 资源差分文件")

    extern_count = 0
    if release and package_root and os.path.isdir(package_root):
        platform_assets = release.get("platformAssets", {})
        for pf, pf_data in platform_assets.items():
            if pf.lower() != platform.lower():
                continue
            for item in (pf_data.get("addExternFileToPak") or []):
                file_path = (item.get("filePath") or {}).get("filePath", "") if isinstance(item.get("filePath"), dict) else item.get("filePath", "")
                mount_path = item.get("mountPath", "")
                parts = [p for p in mount_path.replace("\\", "/").split("/") if p not in ("", ".")]
                while parts and parts[0] == "..":
                    parts.pop(0)
                rel = "/".join(parts)
                src_abs = os.path.join(package_root, rel)
                if not os.path.isfile(src_abs):
                    print(f"  [警告] 外部文件 {mount_path} 不在打包目录中，跳过")
                    continue
                digest, size = hash_file(src_abs)
                # URL 含基础包版本，确保多基础包版本时从该版本目录下发，且与对象存储 key 对齐
                ext_url = (f"/files/packages/{platform}/{base_version_id}/{rel}"
                           if base_version_id else f"/files/packages/{platform}/{rel}")
                files.append({
                    "fileName": os.path.basename(rel),
                    "url": ext_url,
                    "targetRelativePath": rel,
                    "hash": digest,
                    "size": size,
                    "kind": "ExternFile",
                })
                extern_count += 1

    diff = None
    for name in os.listdir(dest):
        if name.endswith("_Diff.json"):
            diff = read_json(os.path.join(dest, name))
            if diff:
                break
    # 仅统计游戏内容资产（排除 /Engine/ 等内置资产被 HotPatcher 误报为“删除”）
    game_prefixes = tuple(
        (config if config is not None else load_config()).get("game_asset_prefixes") or list(DEFAULT_GAME_ASSET_PREFIXES)
    )
    changed, deleted = parse_diff_counts(diff, game_prefixes)

    descriptor = {
        "schemaVersion": 2,
        "versionId": version_id,
        "baseVersionId": base_version_id,
        "date": date,
        "type": vtype,
        "changedAssetCount": changed,
        "deletedAssetCount": deleted,
        "restartRequired": b_io_store,
        "ioStoreEnabled": b_io_store,
        "files": files,
    }
    descriptor["platform"] = platform
    for field in ("gameVersion", "resourceVersion", "requiredGameVersion"):
        value = (release or {}).get(field) or (patch_config or {}).get(field)
        if value:
            descriptor[field] = value
    with open(os.path.join(dest, "descriptor.json"), "w", encoding="utf-8") as f:
        json.dump(descriptor, f, ensure_ascii=False, indent=2)

    return descriptor, copied


def run_import(source_dir, data_dir, project, platform, order="", only="", config=None):
    """执行导入，返回版本索引 dict。供 CLI / exe 直接调用。"""
    source_dir = os.path.abspath(source_dir or "")
    data_dir = os.path.abspath(data_dir or "")
    if not os.path.isdir(source_dir):
        print(f"错误：找不到 {source_dir}")
        sys.exit(1)

    cfg = {
        "project": project,
        "platforms": [platform],
        "data_dir": data_dir,
        "versions_dir": os.path.join(data_dir, "versions"),
        "manifests_dir": os.path.join(data_dir, "manifests"),
        "package_roots": {},
        "manifest_exclude_patterns": [],
        "manifest_hash": "md5",
    }
    cfg = dict(config) if config is not None else cfg
    if platform not in cfg["platforms"]:
        raise ValueError(f"项目未配置平台 {platform}")
    if order:
        options = dict(cfg.get("platform_settings", {}))
        options[platform] = dict(options.get(platform, {}), hotpatcherOrder=order)
        cfg["platform_settings"] = options
    package_root = get_base_dir(config if config is not None else load_config(), platform)

    print("-" * 50)
    print(f"补丁包位置 : {source_dir}")
    print(f"版本文件库 : {cfg['versions_dir']}")
    print(f"基础包位置 : {package_root or '（未配置）'}")
    print("-" * 50)

    single_version = os.path.basename(source_dir)
    if os.path.isfile(os.path.join(source_dir, f"{single_version}_Release.json")):
        source_dir = os.path.dirname(source_dir)
        entries = [single_version]
    else:
        entries = sorted(os.listdir(source_dir))
    selected = []
    for entry in entries:
        releases = [os.path.join(source_dir, entry, f"{entry}_Release.json"),
                    os.path.join(source_dir, entry, platform, f"{entry}_Release.json")]
        if not any(os.path.isfile(path) for path in releases):
            continue
        if only:
            allowed = [x.strip() for x in only.split(",") if x.strip()]
            if entry not in allowed:
                continue
        if not os.path.isdir(os.path.join(source_dir, entry, platform)):
            print(f"跳过版本 {entry}：没有 {platform} 产物")
            continue
        selected.append(entry)
    if not selected:
        raise ValueError(f"没有找到平台 {platform} 的可导入版本")
    versions = []
    for entry in selected:
        print(f"导入版本 {entry} ...")
        desc, copied = import_version(source_dir, cfg["versions_dir"], entry, platform, package_root, config=cfg)
        versions.append(desc)
        print(f"  已复制 {len(copied)} 个文件，更新包 {len(desc['files'])} 个，base={desc['baseVersionId'] or '无'}")

    if not versions:
        print("没有找到任何版本")
        sys.exit(1)

    index = build_versions_index(cfg, explicit_order=order)
    print("-" * 50)
    print(f"共导入 {len(versions)} 个版本")
    print(f"更新链: {' -> '.join(index['updateChain']) or '无'}")
    print(f"当前版本: {index['current']}")
    print(f"索引文件: {os.path.join(data_dir, 'versions.json')}")
    return index
