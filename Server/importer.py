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

from config import get_base_dir, get_base_packages, load_config, validate_platforms, platform_capabilities, safe_join
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


def source_platform_name(source, platform):
    """Resolve one source platform; never pick a GPU format arbitrarily."""
    platform = validate_platforms([platform])[0]
    if not os.path.isdir(source):
        return None
    candidates = [n for n in os.listdir(source) if os.path.isdir(os.path.join(source, n)) and cook_platform(n) == platform]
    if len(candidates) > 1:
        raise ValueError("存在多个目标平台目录或安卓 Cook 格式，请选择具体目录，不能合并不同格式的资源")
    return candidates[0] if candidates else None


def cook_platform(name):
    name = str(name).lower()
    if name in ("windows", "windowsnoeditor", "windowsclient", "win64"):
        return "Windows"
    if name in ("android", "android_astc", "android_etc2", "android_dxt", "android_multi", "android_openxr"):
        return "Android"
    if name in ("ios", "iosclient"):
        return "IOS"
    return None


def select_platform_source(source, platform):
    platform = validate_platforms([platform])[0]
    if not os.path.isdir(source):
        raise ValueError("来源目录不存在")
    declared = cook_platform(os.path.basename(os.path.normpath(source)))
    if declared and declared != platform:
        raise ValueError(f"来源目录属于 {declared}，与当前平台 {platform} 不符")
    selected = source_platform_name(source, platform)
    if selected:
        return os.path.join(source, selected)
    others = [cook_platform(n) for n in os.listdir(source) if os.path.isdir(os.path.join(source, n)) and cook_platform(n)]
    if others:
        raise ValueError(f"来源中没有 {platform} 产物，检测到：{', '.join(sorted(set(others)))}")
    return source


RESOURCE_EXTENSIONS = (".pak", ".utoc", ".ucas")
JUNK_TOP_LEVEL = frozenset({"devicelogs", "insightscaptures", "saved", "intermediate", "binaries",
                            "win-x64", "symbols", "screenshots", "crashreports"})
JUNK_EXTENSIONS = (".apk", ".ipa", ".obb", ".rar", ".zip", ".7z", ".so", ".pdb", ".exe", ".dll",
                   ".dylib", ".a", ".lib", ".bat", ".cmd", ".ps1", ".sh", ".log", ".utrace",
                   ".download", ".patch", ".tmp", ".bak")
JUNK_NAME_PREFIXES = ("symbolize", "uninstall_", "install_")
JUNK_NAME_CONTAINS = ("_symbols", "devicelogs", "insightscaptures")


def is_junk_resource_path(rel_path):
    """资源包导入时应跳过的发布附带物 / 调试产物。"""
    rel = str(rel_path).replace("\\", "/")
    parts = [p for p in rel.split("/") if p and p != "."]
    if not parts:
        return True
    lowered_parts = [p.lower() for p in parts]
    if any(p in JUNK_TOP_LEVEL or any(k in p for k in JUNK_NAME_CONTAINS) for p in lowered_parts):
        return True
    name = parts[-1]
    lower = name.lower()
    if lower.endswith(JUNK_EXTENSIONS):
        return True
    if any(lower.startswith(p) for p in JUNK_NAME_PREFIXES):
        return True
    return False


def is_resource_file(rel_path):
    """Preserve runtime assets, shaders, media and configuration; omit release tools."""
    return not is_junk_resource_path(rel_path)


def validate_resource_tree(root):
    """Validate retained containers within their own relative directory."""
    files = {rel.replace("\\", "/").lower(): path for path, rel in iter_resource_files(root)}
    containers = {rel: path for rel, path in files.items() if rel.endswith(RESOURCE_EXTENSIONS)}
    if not any(rel.endswith((".pak", ".utoc")) for rel in containers):
        raise ValueError("资源包缺少 pak / IoStore 数据，请选择 Data 或包含 Content/Paks 的资源目录；APK、IPA、OBB、压缩包和日志不能作为资源包")
    for rel, path in containers.items():
        if os.path.getsize(path) <= 0:
            raise ValueError("资源文件为空：" + rel)
        stem, ext = os.path.splitext(rel)
        if ext == ".utoc" and stem + ".ucas" not in containers:
            raise ValueError("IoStore 索引缺少同目录配对数据：" + rel)
        if ext == ".ucas":
            base = re.sub(r"_s\d+$", "", stem)
            if base + ".utoc" not in containers:
                raise ValueError("IoStore 数据缺少同目录索引：" + rel)
    return {"pakCount": sum(n.endswith(".pak") for n in containers),
            "utocCount": sum(n.endswith(".utoc") for n in containers),
            "ucasCount": sum(n.endswith(".ucas") for n in containers)}


def find_android_apk(source):
    """Only a single APK is unambiguous. A file path selects it explicitly."""
    if os.path.isfile(source):
        return os.path.abspath(source) if source.lower().endswith(".apk") else None
    if not os.path.isdir(source):
        return None
    source = select_platform_source(source, "Android")
    candidates = []
    for folder, dirs, names in os.walk(source):
        dirs[:] = [n for n in dirs if not is_junk_resource_path(n + "/marker") and (not cook_platform(n) or cook_platform(n) == "Android")]
        for name in names:
            if name.lower().endswith(".apk"):
                candidates.append(os.path.abspath(os.path.join(folder, name)))
    if len(candidates) > 1:
        raise ValueError("检测到多个 APK，请明确选择一个 APK 文件或只包含一个 APK 的目录")
    return candidates[0] if candidates else None


def validate_android_apk(apk_path):
    """校验 APK：ZIP 魔数 + AndroidManifest.xml 存在。返回元数据 dict。"""
    path = str(apk_path or "")
    if not path or not os.path.isfile(path):
        raise ValueError("安装包文件不存在")
    if not path.lower().endswith(".apk"):
        raise ValueError("安装包必须是 .apk 文件")
    size = os.path.getsize(path)
    if size <= 0:
        raise ValueError("安装包文件为空")
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic[:2] != b"PK":
        raise ValueError("安装包不是有效的 APK（ZIP 魔数校验失败）")
    import zipfile
    try:
        with zipfile.ZipFile(path, "r") as zf:
            names = set(zf.namelist())
            if "AndroidManifest.xml" not in names:
                raise ValueError("安装包缺少 AndroidManifest.xml")
            # 排除资源目录以外的损坏成员
            bad = zf.testzip()
            if bad is not None:
                raise ValueError(f"安装包损坏，首个坏文件: {bad}")
    except zipfile.BadZipFile as exc:
        raise ValueError(f"安装包 ZIP 结构损坏: {exc}") from exc
    return {
        "fileName": os.path.basename(path),
        "size": size,
        "path": os.path.abspath(path),
    }


def inspect_application_source(source, platform):
    """Select and validate a complete program, separately from hot-update resources."""
    platform = validate_platforms([platform])[0]
    if platform == "Android":
        path = find_android_apk(source)
        if not path: raise ValueError("未找到 APK，请选择具体 APK 文件或包含单个 APK 的目录")
        return validate_android_apk(path)
    if platform == "IOS":
        import zipfile
        import plistlib
        if os.path.isdir(source):
            source = select_platform_source(source, platform)
            candidates = [os.path.join(folder, n) for folder, _, names in os.walk(source) for n in names if n.lower().endswith(".ipa")]
            if len(candidates) != 1: raise ValueError("请选择具体 IPA 文件或只包含单个 IPA 的目录")
            source = candidates[0]
        if not os.path.isfile(source) or not source.lower().endswith(".ipa"):
            raise ValueError("iOS 程序包必须是 IPA 文件")
        try:
            with zipfile.ZipFile(source) as archive:
                infos = [n for n in archive.namelist() if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
                if len(infos) != 1: raise ValueError("IPA 必须包含单个 Payload/*.app/Info.plist")
                info = plistlib.loads(archive.read(infos[0]))
                executable = info.get("CFBundleExecutable")
                if not isinstance(executable, str) or "/" in executable or "\\" in executable or not executable:
                    raise ValueError("IPA 缺少有效 CFBundleExecutable")
                member = infos[0].rsplit("/", 1)[0] + "/" + executable
                if member not in archive.namelist() or archive.getinfo(member).file_size <= 0:
                    raise ValueError("IPA 缺少应用可执行文件")
                if archive.testzip(): raise ValueError("IPA 文件已损坏")
        except (zipfile.BadZipFile, plistlib.InvalidFileException, KeyError) as exc:
            raise ValueError("IPA 文件结构无效：" + str(exc)) from exc
        return {"path": os.path.abspath(source), "fileName": os.path.basename(source), "size": os.path.getsize(source)}
    source = select_platform_source(source, platform)
    validate_source_platform_files(source, platform)
    if not os.path.isdir(source): raise ValueError("Windows 程序包请选择完整游戏目录")
    executables = []
    for folder, dirs, names in os.walk(source):
        dirs[:] = [n for n in dirs if n.lower() not in JUNK_TOP_LEVEL - {"binaries"}]
        for name in names:
            path = os.path.join(folder, name)
            if name.lower().endswith(".exe"):
                with open(path, "rb") as handle:
                    if handle.read(2) == b"MZ": executables.append(path)
    if not executables: raise ValueError("Windows 程序包缺少有效 EXE，请选择完整游戏打包目录")
    return {"path": source, "fileName": os.path.basename(source), "format": "directory", "executables": len(executables)}


def split_mobile_release(source):
    source = select_platform_source(source, "Android")
    apk = find_android_apk(source)
    data = os.path.join(source, "Data")
    return (data if os.path.isdir(data) else source), apk


def base_import_source(source, platform):
    return inspect_base_source(source, platform)["source"]


def inspect_base_source(source, platform):
    platform = validate_platforms([platform])[0]
    source = select_platform_source(source, platform)
    selected = source
    apk = None
    if platform == "Android":
        source, apk = split_mobile_release(source)
    elif platform == "IOS":
        if source.lower().endswith(".app"):
            apps = [source]
        else:
            app_root = os.path.join(source, "Payload") if os.path.isdir(os.path.join(source, "Payload")) else source
            apps = [os.path.join(app_root, n) for n in os.listdir(app_root) if n.lower().endswith(".app") and os.path.isdir(os.path.join(app_root, n))]
        if len(apps) > 1:
            raise ValueError("检测到多个 iOS 应用目录，请选择具体 .app 目录")
        if apps:
            source = os.path.join(apps[0], "cookeddata")
            if not os.path.isdir(source):
                raise ValueError("iOS 应用目录缺少 cookeddata 资源，请选择 StagedBuilds 或完整资源目录")
        elif os.path.isdir(os.path.join(source, "Data")):
            source = os.path.join(source, "Data")
    stats = None
    if platform_capabilities(platform)["baseKind"] == "resources":
        stats = validate_resource_tree(source)
    else:
        # Full Windows installations retain executables, DLLs and all content.
        files = [os.path.join(folder, n) for folder, _, names in os.walk(source) for n in names]
        if not any(n.lower().endswith((".exe", ".pak", ".utoc")) and os.path.getsize(n) > 0
                   and not any(part.lower() in JUNK_TOP_LEVEL or any(k in part.lower() for k in JUNK_NAME_CONTAINS)
                               for part in Path(n).relative_to(source).parts[:-1]) for n in files):
            raise ValueError("Windows 基础包缺少可执行文件或游戏资源，请选择完整 Windows 打包目录")
    validate_source_platform_files(source, platform)
    if source != selected:
        validate_source_platform_files(selected, platform)
    if apk:
        validate_android_apk(apk)
    return {"platform": platform, "source": source, "selectedSource": selected, "apk": apk,
            "resourceStats": stats, "baseKind": platform_capabilities(platform)["baseKind"]}


def validate_source_platform_files(source, platform):
    for folder, _, names in os.walk(source):
        for name in names:
            if name.lower().endswith(RESOURCE_EXTENSIONS + (".pak.patch", ".utoc.patch", ".ucas.patch")):
                marker = re.search(r"-(Windows(?:NoEditor|Client)?|Android(?:_ASTC|_ETC2|_DXT|_Multi|_OpenXR)?|IOS)(?=[._-]|$)", name, re.I)
                if marker and cook_platform(marker.group(1)) != platform:
                    raise ValueError(f"资源文件 {name} 属于其它平台，与 {platform} 不符")
    for name in os.listdir(source):
        if name == "version.json" or name.endswith(("_Release.json", "_PatchConfig.json")):
            metadata = read_json(os.path.join(source, name))
            if not isinstance(metadata, dict):
                raise ValueError("版本元数据不是有效 JSON 对象：" + name)
            declared = metadata.get("platform")
            targets = metadata.get("pakTargetPlatforms")
            if declared and cook_platform(declared) != platform:
                raise ValueError("版本元数据的平台与当前选择不符：" + name)
            if isinstance(targets, list) and targets and platform not in [cook_platform(n) for n in targets]:
                raise ValueError("补丁目标平台与当前选择不符：" + name)


def iter_resource_files(source):
    """遍历资源包应保留的文件，产出 (abs_path, rel_posix)。"""
    for dirpath, _, names in os.walk(source):
        for name in names:
            abs_path = os.path.join(dirpath, name)
            rel = os.path.relpath(abs_path, source).replace("\\", "/")
            if is_resource_file(rel):
                yield abs_path, rel


def import_version(source_dir, dest_versions_dir, version_id, platform, package_root, config=None, source_platform=None):
    """在临时目录完成导入后替换发布目录，重导入不保留已移除的旧补丁。"""
    if not re.fullmatch(r"[A-Za-z0-9_-]+", platform) or not re.fullmatch(r"[A-Za-z0-9_.-]+", version_id) or version_id in (".", ".."):
        raise ValueError("平台或版本标识无效")
    platform = validate_platforms([platform])[0]
    source_platform = source_platform or source_platform_name(os.path.join(source_dir, version_id), platform)
    if not source_platform:
        raise ValueError(f"版本 {version_id} 缺少 {platform} 产物目录")
    if cook_platform(source_platform) != platform or source_platform != os.path.basename(source_platform):
        raise ValueError("补丁来源平台目录无效")
    validate_source_platform_files(os.path.join(source_dir, version_id, source_platform), platform)
    root = os.path.abspath(os.path.join(dest_versions_dir, platform))
    os.makedirs(root, exist_ok=True)
    target = os.path.join(root, version_id)
    stage = tempfile.mkdtemp(prefix=".import-", dir=root)
    backup = os.path.join(root, ".replace-" + uuid.uuid4().hex)
    try:
        result = _import_version(source_dir, dest_versions_dir, version_id, platform, package_root, stage, config, source_platform)
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


def _import_version(source_dir, dest_versions_dir, version_id, platform, package_root, destination, config=None, source_platform=None):
    version_src = os.path.join(source_dir, version_id)
    source_platform = source_platform or source_platform_name(version_src, platform)
    dest = destination
    os.makedirs(dest, exist_ok=True)

    copied = []
    for suffix in ("_Release.json", "_PatchConfig.json", "_PakFilesInfo.json",
                   "_PakResults.json", "_ReleaseConfig.json"):
        candidates = [
            os.path.join(version_src, source_platform, f"{version_id}{suffix}"),
            os.path.join(version_src, f"{version_id}{suffix}"),
            os.path.join(source_dir, f"{version_id}{suffix}"),
        ]
        src = next((c for c in candidates if os.path.isfile(c)), None)
        if src and os.path.isfile(src):
            copy_file(src, os.path.join(dest, os.path.basename(src)))
            copied.append(os.path.basename(src))

    for diff_src in find_diffs(source_dir, version_id, source_platform):
        copy_file(diff_src, os.path.join(dest, os.path.basename(diff_src)))
        copied.append(os.path.basename(diff_src))

    platform_src = os.path.join(version_src, source_platform)
    files_dest = os.path.join(dest, "files")
    if not os.path.isdir(platform_src):
        raise ValueError(f"版本 {version_id} 缺少 {platform} 产物目录：{platform_src}")
    if os.path.isdir(platform_src):
        for dirpath, dirnames, filenames in os.walk(platform_src):
            for name in filenames:
                if not name.lower().endswith(RESOURCE_EXTENSIONS + (".pak.patch", ".utoc.patch", ".ucas.patch")):
                    continue
                src = os.path.join(dirpath, name)
                rel = os.path.relpath(src, platform_src)
                copy_file(src, os.path.join(files_dest, rel))
                copied.append(f"files/{rel}")

    validate_source_platform_files(dest, platform)
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
            if pf.lower() != source_platform.lower():
                continue
            for item in (pf_data.get("addExternFileToPak") or []):
                file_path = (item.get("filePath") or {}).get("filePath", "") if isinstance(item.get("filePath"), dict) else item.get("filePath", "")
                mount_path = item.get("mountPath", "")
                parts = [p for p in mount_path.replace("\\", "/").split("/") if p not in ("", ".")]
                while parts and parts[0] == "..":
                    parts.pop(0)
                rel = "/".join(parts)
                if not rel or any(p in (".", "..") or ":" in p for p in parts):
                    raise ValueError("外部资源挂载路径无效：" + mount_path)
                src_abs = safe_join(package_root, rel)
                if not src_abs:
                    raise ValueError("外部资源路径超出基础包：" + mount_path)
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


def patch_import_plan(source_dir, platform, only=""):
    platform = validate_platforms([platform])[0]
    source_dir = os.path.abspath(source_dir)
    if not os.path.isdir(source_dir):
        raise ValueError("补丁来源目录不存在")
    single_version = os.path.basename(source_dir)
    # A selected Cook directory may sit inside one version directory.
    parent_version = os.path.basename(os.path.dirname(source_dir))
    if cook_platform(single_version) == platform and any(os.path.isfile(p) for p in (
            os.path.join(os.path.dirname(source_dir), parent_version + "_Release.json"),
            os.path.join(source_dir, parent_version + "_Release.json"))):
        selected_cook = os.path.basename(source_dir)
        source_dir = os.path.dirname(source_dir)
        single_version = os.path.basename(source_dir)
    else:
        selected_cook = None
    pf = selected_cook or source_platform_name(source_dir, platform)
    if os.path.isfile(os.path.join(source_dir, f"{single_version}_Release.json")) or (pf and os.path.isfile(os.path.join(source_dir, pf, f"{single_version}_Release.json"))):
        source_dir = os.path.dirname(source_dir)
        entries = [single_version]
    else:
        entries = sorted(os.listdir(source_dir))
    selected = []
    for entry in entries:
        source_platform = selected_cook or source_platform_name(os.path.join(source_dir, entry), platform)
        if not source_platform:
            continue
        releases = [os.path.join(source_dir, entry, f"{entry}_Release.json"),
                    os.path.join(source_dir, entry, source_platform, f"{entry}_Release.json")]
        if not any(os.path.isfile(path) for path in releases):
            continue
        if only:
            allowed = [x.strip() for x in only.split(",") if x.strip()]
            if entry not in allowed:
                continue
        if not os.path.isdir(os.path.join(source_dir, entry, source_platform)):
            print(f"跳过版本 {entry}：没有 {platform} 产物")
            continue
        selected.append(entry)

    if not selected:
        raise ValueError(f"没有找到平台 {platform} 的可导入版本")
    for entry in selected:
        pf = selected_cook or source_platform_name(os.path.join(source_dir, entry), platform)
        folder = os.path.join(source_dir, entry, pf)
        validate_source_platform_files(folder, platform)
        # Shared version metadata must also target this platform.
        for suffix in ("_Release.json", "_PatchConfig.json"):
            path = os.path.join(source_dir, entry, entry + suffix)
            if os.path.isfile(path) and not os.path.isfile(os.path.join(folder, entry + suffix)):
                meta = read_json(path)
                if not isinstance(meta, dict):
                    raise ValueError("补丁元数据无效：" + path)
                if meta.get("platform") and cook_platform(meta["platform"]) != platform:
                    raise ValueError("补丁元数据平台不符")
                targets = meta.get("pakTargetPlatforms")
                if isinstance(targets, list) and targets and platform not in [cook_platform(n) for n in targets]:
                    raise ValueError("补丁目标平台不符")
        containers = [os.path.join(d, n) for d, _, ns in os.walk(folder) for n in ns
                      if n.lower().endswith(RESOURCE_EXTENSIONS + (".pak.patch", ".utoc.patch", ".ucas.patch"))]
        if not containers or any(os.path.getsize(n) == 0 for n in containers):
            raise ValueError("补丁资源缺失或为空：" + entry)
    return source_dir, selected, selected_cook


def run_import(source_dir, data_dir, project, platform, order="", only="", config=None):
    """执行导入，返回版本索引 dict。供 CLI / exe 直接调用。"""
    platform = validate_platforms([platform])[0]
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

    source_dir, selected, selected_cook = patch_import_plan(source_dir, platform, only)
    versions = []
    for entry in selected:
        print(f"导入版本 {entry} ...")
        desc, copied = import_version(source_dir, cfg["versions_dir"], entry, platform, package_root, config=cfg, source_platform=selected_cook)
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
