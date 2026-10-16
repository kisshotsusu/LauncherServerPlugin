"""Package identity shared by version management, downloads and file browsing."""
import json
import os
from urllib.parse import quote
from storage import get_storage


def read_json(path):
    try:
        with open(path, encoding="utf-8") as handle:
            value = json.load(handle)
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def base_metadata(cfg, platform, version):
    folder = (cfg.get("base_packages", {}).get(platform) or {}).get(version, "")
    metadata = read_json(os.path.join(folder, "version.json"))
    metadata.update(read_json(os.path.join(folder, ".cloudupdate.json")))
    return {"resourceVersion": metadata.get("resourceVersion") or version,
            "gameVersion": metadata.get("gameVersion") or "",
            "requiredGameVersion": metadata.get("requiredGameVersion", "")}


def package_kind(version):
    return version.get("packageKind") or ("base" if version.get("type") == "full" else "patch")


def version_key_for_package(version):
    return package_kind(version) + ":" + version["versionId"]


def version_is_enabled(version, enabled):
    return version["versionId"] in enabled or version_key_for_package(version) in enabled


def _safe_relative(name):
    return isinstance(name, str) and bool(name) and not name.startswith(("/", "\\")) and ":" not in name and all(p not in ("", ".", "..") for p in name.replace("\\", "/").split("/"))


def list_installers(cfg, platform):
    if platform not in cfg["platforms"] or not cfg.get("workspace_dir"):
        return []
    root = os.path.join(cfg["workspace_dir"], "installers", platform)
    if not os.path.isdir(root): return []
    result = []
    storage = get_storage(cfg)
    for version in sorted(os.listdir(root)):
        if version.startswith(".") or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-" for c in version): continue
        folder = os.path.join(root, version)
        if not os.path.isdir(folder): continue
        path = os.path.join(folder, "installer.json")
        meta = read_json(path)
        if os.path.isfile(path) and (meta.get("platform") != platform or meta.get("versionId") != version): continue
        extension = {"Android": ".apk", "IOS": ".ipa", "Windows": ""}[platform]
        files = meta.get("files")
        if not isinstance(files, list):
            name = meta.get("fileName", "")
            if not name and not os.path.isfile(path) and extension:
                legacy = [n for n in os.listdir(folder) if n.lower().endswith(extension) and os.path.isfile(os.path.join(folder,n))]
                if len(legacy) == 1: name = legacy[0]
            files = [{"targetRelativePath":name, "size":meta.get("size"), "hash":meta.get("hash", "")}]
        published = []
        for entry in files:
            if not isinstance(entry, dict): break
            name = entry.get("targetRelativePath", "")
            if not _safe_relative(name) or name == "installer.json": break
            if extension and ("/" in name or "\\" in name or not name.lower().endswith(extension)): break
            disk = os.path.join(folder, name)
            if not os.path.isfile(disk) or os.path.commonpath([os.path.realpath(disk),os.path.realpath(folder)]) != os.path.realpath(folder): break
            size = os.path.getsize(disk)
            if (extension and size <= 0) or (entry.get("size") is not None and entry["size"] != size): break
            published.append({"fileName": os.path.basename(name), "targetRelativePath":name, "size":size,
                              "hash":entry.get("hash", ""), "kind":"Application",
                              "url":storage.url_for("installers", platform=platform, version=version, rel=name)})
        else:
            if not published or (extension and len(published) != 1): continue
            if platform == "Windows" and not any(f["targetRelativePath"].lower().endswith(".exe") for f in published): continue
            result.append({"platform":platform,"versionId":version,"fileName":published[0]["fileName"] if extension else version,
                           "size":sum(f["size"] for f in published),"fileCount":len(published),"files":published,
                           "gameVersion":meta.get("gameVersion") or version,"resourceVersion":meta.get("resourceVersion") or version,
                           "type":"full","packageKind":"installer","date":meta.get("date", ""),
                           "url":published[0]["url"] if extension else f"/api/version/{quote(version)}?platform={platform}&kind=installer"})
    return result


def installer_descriptor(cfg, platform, version):
    item = next((i for i in list_installers(cfg, platform) if i["versionId"] == version), None)
    if not item: return None
    from manifest import hash_file
    cache_path = os.path.join(cfg["manifests_dir"], f"installer_{platform}_{version}.json")
    cache = read_json(cache_path)
    changed = False
    files = []
    for entry in item["files"]:
        entry = dict(entry)
        if not entry["hash"]:
            path = os.path.join(cfg["workspace_dir"], "installers", platform, version, entry["targetRelativePath"])
            stat = os.stat(path)
            signature = [stat.st_size, stat.st_mtime_ns]
            cached = cache.get(entry["targetRelativePath"], {})
            if cached.get("signature") == signature and cached.get("hash"):
                entry["hash"] = cached["hash"]
            else:
                entry["hash"], _ = hash_file(path)
                cache[entry["targetRelativePath"]] = {"signature":signature,"hash":entry["hash"]}
                changed = True
        files.append(entry)
    if changed:
        import uuid
        os.makedirs(cfg["manifests_dir"], exist_ok=True)
        temporary = cache_path + "." + uuid.uuid4().hex + ".tmp"
        try:
            with open(temporary, "w", encoding="utf-8") as handle: json.dump(cache, handle)
            os.replace(temporary, cache_path)
        finally:
            if os.path.isfile(temporary): os.remove(temporary)
    return {"schemaVersion":2,"platform":platform,"versionId":version,"baseVersionId":"",
            "type":"full","packageKind":"installer","gameVersion":item["gameVersion"],
            "resourceVersion":item["resourceVersion"],"date":item["date"],"restartRequired":True,"files":files}
