#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
导入 HotPatcher 产物到管理服务器数据目录。

用法：
  python scripts/import_hotpatcher.py ^
      --source D:/test/CodeBuild/Saved/HotPatcher ^
      --data D:/test/CodeBuild/Server/data ^
      --project CodeBuild --platform Windows ^
      --order 1.0,1.1,1.2,1.3,1.4

说明：
  - 自动识别 Saved/HotPatcher 下的版本目录（含 {id}_Release.json）
  - 复制 PatchConfig / Release / PakFilesInfo / PakResults / Diff / Pak 到 data/versions/{id}
  - 解析 HotPatcher JSON 生成 descriptor.json（客户端 /api/version/{id} 使用）
  - 重建 data/versions.json 索引
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
SERVER_DIR = SCRIPT_DIR.parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from config import get_base_dir, load_config  # noqa: E402
from manifest import hash_file  # noqa: E402
from versions import build_versions_index  # noqa: E402


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
    """从 baseVersion.filePath 形如 .../1.0/1.0_Release.json 中提取 1.0"""
    if not path:
        return ""
    name = os.path.basename(path)
    m = re.match(r"^(.+)_Release\.json$", name)
    return m.group(1) if m else ""


def count_asset_map(diff_obj):
    """统计 Diff JSON 中 assetsDependenciesMap 的资产总数（跨模块）。"""
    if not diff_obj:
        return 0
    modules = diff_obj.get("assetsDependenciesMap", {})
    total = 0
    for module in modules.values():
        if isinstance(module, dict):
            total += len(module.get("assetDependencyDetails", {}) or {})
    return total


def parse_diff_counts(diff):
    if not diff:
        return 0, 0
    asset_diff = diff.get("assetDiffInfo", {})
    add = count_asset_map(asset_diff.get("addAssetDependInfo", {}))
    modify = count_asset_map(asset_diff.get("modifyAssetDependInfo", {}))
    delete = count_asset_map(asset_diff.get("deleteAssetDependInfo", {}))
    return add + modify, delete


def find_diffs(source_dir, version_id):
    """查找 {base}_{id}_Diff.json 或 {id}_{base}_Diff.json。"""
    results = []
    for folder in (source_dir, os.path.join(source_dir, "Windows")):
        if not os.path.isdir(folder):
            continue
        for name in os.listdir(folder):
            if not name.endswith("_Diff.json"):
                continue
            stem = name[: -len("_Diff.json")]
            if stem.endswith("_" + version_id) or stem.startswith(version_id + "_"):
                results.append(os.path.join(folder, name))
    return sorted(set(results))


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def import_version(source_dir, dest_versions_dir, version_id, platform, package_root):
    """导入单个版本，返回 (descriptor, copied_files)。"""
    version_src = os.path.join(source_dir, version_id)
    dest = os.path.join(dest_versions_dir, version_id)
    os.makedirs(dest, exist_ok=True)

    # 1. 复制 JSON 描述文件（可能位于版本目录内，也可能位于 Saved/HotPatcher 根目录）
    copied = []
    for suffix in ("_Release.json", "_PatchConfig.json", "_PakFilesInfo.json",
                   "_PakResults.json", "_ReleaseConfig.json"):
        candidates = [
            os.path.join(source_dir, f"{version_id}{suffix}"),
            os.path.join(version_src, f"{version_id}{suffix}"),
        ]
        src = next((c for c in candidates if os.path.isfile(c)), None)
        if src and os.path.isfile(src):
            copy_file(src, os.path.join(dest, os.path.basename(src)))
            copied.append(os.path.basename(src))

    # 2. 复制 Diff
    for diff_src in find_diffs(source_dir, version_id):
        copy_file(diff_src, os.path.join(dest, os.path.basename(diff_src)))
        copied.append(os.path.basename(diff_src))

    # 3. 复制 {version}/Windows 下的产物（pak / utoc / ucas / txt ...）
    windows_src = os.path.join(version_src, "Windows")
    windows_dest = os.path.join(dest, "Windows")
    if os.path.isdir(windows_src):
        for dirpath, dirnames, filenames in os.walk(windows_src):
            for name in filenames:
                src = os.path.join(dirpath, name)
                rel = os.path.relpath(src, windows_src)
                copy_file(src, os.path.join(windows_dest, rel))
                copied.append(f"Windows/{rel}")

    # 4. 解析 HotPatcher JSON
    patch_config = read_json(os.path.join(dest, f"{version_id}_PatchConfig.json"))
    release = read_json(os.path.join(dest, f"{version_id}_Release.json"))
    pak_info = read_json(os.path.join(dest, f"{version_id}_PakFilesInfo.json"))

    base_version_id = ""
    if patch_config:
        base_path = ""
        base_obj = patch_config.get("baseVersion") or {}
        if isinstance(base_obj, dict):
            base_path = base_obj.get("filePath", "")
        base_version_id = version_id_from_base_path(base_path)
    if not base_version_id and release:
        base_version_id = release.get("baseVersionId", "")

    b_io_store = False
    if patch_config:
        io_settings = patch_config.get("ioStoreSettings") or {}
        b_io_store = bool(io_settings.get("bIoStore", False))

    date = release.get("date", "") if release else ""
    vtype = "patch" if base_version_id else "full"

    # 5. 收集 Pak 文件（hash 优先取 PakFilesInfo）
    pak_hashes = {}
    if pak_info:
        platform_map = pak_info.get("pakFilesMap", {})
        for pf, pf_data in platform_map.items():
            if pf.lower() != platform.lower():
                continue
            for item in pf_data.get("pakFileInfos", []):
                pak_hashes[item.get("fileName", "")] = {
                    "hash": item.get("hash", ""),
                    "size": item.get("fileSize", 0),
                }

    files = []
    if os.path.isdir(windows_dest):
        for name in sorted(os.listdir(windows_dest)):
            lower = name.lower()
            if not (lower.endswith(".pak") or lower.endswith(".utoc") or lower.endswith(".ucas")):
                continue
            abs_path = os.path.join(windows_dest, name)
            digest, size = hash_file(abs_path)
            if lower.endswith(".pak"):
                known = pak_hashes.get(name, {})
                digest = known.get("hash") or digest
                size = known.get("size") or size
                kind = "ContentPak"
            else:
                kind = "IoStore"
            files.append({
                "fileName": name,
                "url": f"/files/versions/{version_id}/Windows/{name}",
                "targetRelativePath": name,
                "hash": digest,
                "size": size,
                "kind": kind,
            })

    # 6. 外部文件（Release JSON platformAssets.*.addExternFileToPak）
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
                files.append({
                    "fileName": os.path.basename(rel),
                    "url": f"/files/packages/{platform}/{rel}",
                    "targetRelativePath": rel,
                    "hash": digest,
                    "size": size,
                    "kind": "ExternFile",
                })
                extern_count += 1

    # 7. Diff 统计
    diff = None
    for name in os.listdir(dest):
        if name.endswith("_Diff.json"):
            diff = read_json(os.path.join(dest, name))
            if diff:
                break
    changed, deleted = parse_diff_counts(diff)

    descriptor = {
        "schemaVersion": 1,
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
    with open(os.path.join(dest, "descriptor.json"), "w", encoding="utf-8") as f:
        json.dump(descriptor, f, ensure_ascii=False, indent=2)

    return descriptor, copied


def main():
    parser = argparse.ArgumentParser(description="导入 HotPatcher 产物到管理服务器")
    parser.add_argument("--source", default=None, help="补丁包位置（HotPatcher 产物目录，默认取 Server/config.json）")
    parser.add_argument("--data", default=None, help="数据目录（默认取 Server/config.json）")
    parser.add_argument("--project", default=None)
    parser.add_argument("--platform", default=None)
    parser.add_argument("--order", default=None, help="版本顺序，逗号分隔（默认取 Server/config.json）")
    parser.add_argument("--only", default="", help="只导入指定版本，逗号分隔（可选）")
    args = parser.parse_args()

    server_cfg = load_config()
    source_dir = os.path.abspath(args.source or server_cfg.get("hotpatcher_source", ""))
    data_dir = os.path.abspath(args.data or server_cfg.get("data_dir", "data"))
    project = args.project or server_cfg.get("project", "CodeBuild")
    platform = args.platform or (server_cfg.get("platforms") or ["Windows"])[0]
    order = args.order if args.order is not None else server_cfg.get("hotpatcher_order", "")
    if not os.path.isdir(source_dir):
        print(f"错误：找不到 {source_dir}")
        sys.exit(1)

    cfg = {
        "project": project,
        "platforms": [platform],
        "data_dir": data_dir,
        "versions_dir": server_cfg.get("versions_dir") or os.path.join(data_dir, "versions"),
        "manifests_dir": os.path.join(data_dir, "manifests"),
        "package_roots": server_cfg.get("package_roots", {}),
        "manifest_exclude_patterns": [],
        "manifest_hash": "md5",
    }
    package_root = get_base_dir(server_cfg, platform)

    print("-" * 50)
    print(f"补丁包位置 : {source_dir}")
    print(f"版本文件库 : {cfg['versions_dir']}")
    print(f"基础包位置 : {package_root or '（未配置）'}")
    print("-" * 50)

    versions = []
    for entry in sorted(os.listdir(source_dir)):
        release_path = os.path.join(source_dir, entry, f"{entry}_Release.json")
        if not os.path.isfile(release_path):
            continue
        if args.only:
            allowed = [x.strip() for x in args.only.split(",") if x.strip()]
            if entry not in allowed:
                continue
        print(f"导入版本 {entry} ...")
        desc, copied = import_version(source_dir, cfg["versions_dir"], entry, platform, package_root)
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


if __name__ == "__main__":
    main()
