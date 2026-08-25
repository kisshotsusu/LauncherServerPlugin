#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "UpdateManager.h"

#include "Json.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

int64_t parseNum(const std::string& s) {
    return s.empty() ? 0 : _strtoi64(s.c_str(), nullptr, 10);
}

// 递归创建目录（兼容多级路径，如 CodeBuild/Binaries/Win64）
void createDirectories(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '\\' || c == '/') {
            if (!cur.empty()) CreateDirectoryA(cur.c_str(), nullptr);
        }
        cur += c;
    }
    if (!cur.empty()) CreateDirectoryA(cur.c_str(), nullptr);
}

// 清理 Paks 目录中不属于当前版本清单的旧补丁包（HotPatcher 命名 *_P.pak/ucas/utoc）
// 以及下载残留（*.download）
void cleanupStalePatchPaks(const std::string& paksDir, const std::set<std::string>& keepNames) {
    std::string pattern = joinPath(paksDir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        bool isPatch = low.size() > 6 && low.rfind("_p.pak") == low.size() - 6;
        isPatch = isPatch || (low.size() > 7 && low.rfind("_p.ucas") == low.size() - 7);
        isPatch = isPatch || (low.size() > 7 && low.rfind("_p.utoc") == low.size() - 7);
        bool isTmp = low.size() > 9 && low.rfind(".download") == low.size() - 9;
        if ((isPatch || isTmp) && !keepNames.count(low)) {
            DeleteFileA(joinPath(paksDir, name).c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

}  // namespace

// 检查文件是否被其他进程锁定（尝试以独占方式打开）
static bool isFileLocked(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,               // 不共享 = 独占访问
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION;
    }
    CloseHandle(h);
    return false;
}

bool versionNewer(const std::string& a, const std::string& b) {
    std::string aa = a, bb = b;
    std::replace(aa.begin(), aa.end(), '-', '.');
    std::replace(bb.begin(), bb.end(), '-', '.');
    auto ap = split(aa, '.');
    auto bp = split(bb, '.');
    size_t n = std::max(ap.size(), bp.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string& x = i < ap.size() ? ap[i] : "0";
        const std::string& y = i < bp.size() ? bp[i] : "0";
        bool xn = !x.empty() && std::all_of(x.begin(), x.end(), ::isdigit);
        bool yn = !y.empty() && std::all_of(y.begin(), y.end(), ::isdigit);
        if (xn && yn) {
            int64_t xi = parseNum(x), yi = parseNum(y);
            if (xi != yi) return xi > yi;
        } else if (x != y) {
            return x > y;
        }
    }
    return false;
}

UpdateManager::UpdateManager(const LauncherConfig& cfg)
    : cfg_(cfg) {
    if (!cfg_.gamePath.empty()) {
        gameRoot_ = cfg_.gamePath;
    } else {
        gameRoot_ = exeDirectory();
    }
    platform_ = cfg_.platform.empty() ? "Windows" : cfg_.platform;
    loadLocalVersion();
}

std::string UpdateManager::localVersion() const { return localVersion_; }

std::string UpdateManager::gameExePath() const {
    std::string selfExe;
    {
        char buf[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) selfExe = buf;
    }
    auto isLauncherExe = [](const std::string& name) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return n == "launcher.exe" || n == "selfupdater.exe" || n == "launcher_old.exe";
    };

    std::vector<std::string> exes;
    std::function<void(const std::string&, int)> walk =
        [&](const std::string& dir, int depth) {
            if (depth > 4) return;
            std::string pattern = joinPath(dir, "*");
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            std::vector<std::string> dirsHere;
            do {
                const std::string name = fd.cFileName;
                if (name == "." || name == "..") continue;
                std::string full = joinPath(dir, name);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    dirsHere.push_back(full);
                } else if (name.size() > 4 &&
                           _stricmp(name.c_str() + name.size() - 4, ".exe") == 0 &&
                           !isLauncherExe(name)) {
                    if (selfExe.empty() || _stricmp(full.c_str(), selfExe.c_str()) != 0) {
                        exes.push_back(full);
                    }
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            std::sort(dirsHere.begin(), dirsHere.end());
            for (const std::string& d : dirsHere) walk(d, depth + 1);
        };
    walk(gameRoot_, 0);

    if (exes.empty()) return "";
    // 顶层优先（路径层级少者优先），同级按文件名排序
    std::stable_sort(exes.begin(), exes.end(),
                     [](const std::string& a, const std::string& b) {
                         int da = static_cast<int>(std::count(a.begin(), a.end(), '\\'));
                         int db = static_cast<int>(std::count(b.begin(), b.end(), '\\'));
                         if (da != db) return da < db;
                         return a < b;
                     });
    return exes.front();
}

bool UpdateManager::loadLocalVersion() {
    // 优先读取 UE 插件写入的版本文件，其次启动器自身状态
    const std::string paths[] = {
        joinPath(gameRoot_, "CodeBuild\\Saved\\CloudUpdate\\local_version.json"),
        joinPath(gameRoot_, "launcher_state.json"),
    };
    for (const auto& p : paths) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        Json root = Json::parse(ss.str(), &err);
        if (root.type != Json::Type::Object) continue;
        std::string v = root.getString("versionId");
        if (v.empty()) v = root.getString("version");
        if (!v.empty()) {
            localVersion_ = v;
            return true;
        }
    }
    return false;
}

void UpdateManager::saveLocalVersion(const std::string& version) {
    Json root = Json::object();
    root.obj["versionId"] = Json::string(version);
    root.obj["updatedAt"] = Json::string("now");
    const std::string statePath = joinPath(gameRoot_, "launcher_state.json");
    {
        std::ofstream out(statePath, std::ios::binary);
        out << root.dump(2);
    }
    // 同步 UE 插件读取的版本文件，保持两边版本号一致
    const std::string pluginDir = joinPath(joinPath(gameRoot_, "CodeBuild\\Saved"), "CloudUpdate");
    createDirectories(pluginDir);
    const std::string pluginPath = joinPath(pluginDir, "local_version.json");
    {
        std::ofstream out(pluginPath, std::ios::binary);
        out << root.dump(2);
    }
    localVersion_ = version;
}

void UpdateManager::setLocalVersion(const std::string& version) {
    localVersion_ = version;
    saveLocalVersion(version);
}

std::string UpdateManager::manifestUrl() const {
    std::string bv = baseVersion_.empty() ? "" : baseVersion_;
    return httpUrlJoin(cfg_.serverUrl,
                       "api/manifest.json?platform=" + urlEncode(platform_) +
                       "&baseVersion=" + urlEncode(bv));
}

std::string UpdateManager::paksDirectory() const {
    return joinPath(joinPath(gameRoot_, "CodeBuild\\Content"), "Paks");
}

int UpdateManager::finalizePendingMerges(int* outTotal) const {
    if (outTotal) *outTotal = 0;
    const std::string paksDir = paksDirectory();

    WIN32_FIND_DATAA fd;
    const std::string pattern = joinPath(paksDir, "*.pending");
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int swapped = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (outTotal) ++(*outTotal);

        // "<base>.pending" -> "<base>"
        std::string name = fd.cFileName;
        const size_t suffixLen = strlen(".pending");
        if (name.size() <= suffixLen ||
            _stricmp(name.c_str() + name.size() - suffixLen, ".pending") != 0) {
            continue;  // 不应发生，防御性检查
        }
        std::string baseName = name.substr(0, name.size() - suffixLen);
        const std::string basePath = joinPath(paksDir, baseName);
        const std::string pendingPath = joinPath(paksDir, name);

        // 基础文件被占用（游戏运行中）则跳过，等下次启动器启动前再交换
        if (GetFileAttributesA(basePath.c_str()) != INVALID_FILE_ATTRIBUTES &&
            isFileLocked(basePath)) {
            continue;
        }

        DeleteFileA(basePath.c_str());
        if (MoveFileExA(pendingPath.c_str(), basePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ++swapped;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return swapped;
}

std::string UpdateManager::versionsUrl() const {
    return httpUrlJoin(cfg_.serverUrl, "api/versions");
}

std::string UpdateManager::versionUrl(const std::string& id) const {
    return httpUrlJoin(cfg_.serverUrl,
                       "api/version/" + urlEncode(id) + "?platform=" + urlEncode(platform_));
}

bool UpdateManager::checkUpdates(UpdateCheckResult& out, std::string& err) {
    HttpResult resp = http_.get(versionsUrl());
    if (!resp.ok()) {
        err = resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error;
        return false;
    }
    std::string jerr;
    Json root = Json::parse(resp.body, &jerr);
    if (root.type != Json::Type::Object) {
        err = "版本索引解析失败";
        return false;
    }

    std::vector<VersionInfo> all;
    for (const Json& v : root.get("versions").array()) {
        VersionInfo info;
        info.versionId = v.getString("versionId");
        info.baseVersionId = v.getString("baseVersionId");
        info.date = v.getString("date");
        info.type = v.getString("type");
        info.url = v.getString("url");
        info.totalSize = static_cast<int64_t>(v.getNumber("totalSizeBytes"));
        if (!info.versionId.empty()) all.push_back(info);
    }

    // 基础包版本列表
    std::vector<std::string> baseVersions;
    const Json& bvObj = root.get("baseVersions");
    if (bvObj.type == Json::Type::Object) {
        const Json& plat = bvObj.get(platform_);
        if (plat.type == Json::Type::Array) {
            for (const Json& v : plat.array()) {
                std::string id = v.asString();
                if (!id.empty()) baseVersions.push_back(id);
            }
        }
    }

    std::vector<std::string> chain;
    for (const Json& v : root.get("updateChain").array()) {
        std::string id = v.asString();
        if (!id.empty()) chain.push_back(id);
    }

    // 回滚检测：本地版本被服务器撤销（隐藏/删除）时删除本地更新文件并回退
    std::string rollbackNote = rollbackIfRevoked(root);

    out.latestVersion = root.getString("current");
    const std::string local = localVersion();

    // 第一步：基础包更新（整包优先）
    for (const std::string& baseId : baseVersions) {
        if (!versionNewer(baseId, local)) continue;
        bool found = false;
        for (const VersionInfo& info : all) {
            if (info.versionId == baseId && info.type == "full") {
                out.pending.push_back(info);
                found = true;
                break;
            }
        }
        if (!found) {
            VersionInfo info;
            info.versionId = baseId;
            info.type = "full";
            info.url = "api/version/" + urlEncode(baseId) + "?platform=" + urlEncode(platform_);
            out.pending.push_back(info);
        }
    }

    // 多个基础包整包时只保留最新一个（全新安装直接下最新整包）
    std::string maxPendingBase;
    for (const auto& v : out.pending) {
        if (v.type == "full" && (maxPendingBase.empty() || versionNewer(v.versionId, maxPendingBase))) {
            maxPendingBase = v.versionId;
        }
    }
    if (!maxPendingBase.empty()) {
        out.pending.erase(
            std::remove_if(out.pending.begin(), out.pending.end(),
                           [&](const VersionInfo& v) {
                               return v.type == "full" && v.versionId != maxPendingBase;
                           }),
            out.pending.end());
    }

    // 第二步：补丁更新
    int localIndex = -1;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (chain[i] == local) { localIndex = static_cast<int>(i); break; }
    }
    for (size_t i = static_cast<size_t>(localIndex + 1); i < chain.size(); ++i) {
        const std::string& vid = chain[i];
        // 不比本地版本新则跳过（本地是整包版本且不在链中时，避免误列旧补丁）
        if (!versionNewer(vid, local)) continue;
        // 待更新的基础包整包已包含该补丁 → 跳过
        if (!maxPendingBase.empty() && !versionNewer(vid, maxPendingBase)) continue;
        for (const VersionInfo& info : all) {
            if (info.versionId == vid) {
                out.pending.push_back(info);
                break;
            }
        }
    }

    out.hasUpdate = !out.pending.empty();
    if (out.hasUpdate) {
        int baseCount = 0;
        for (const auto& v : out.pending) if (v.type == "full") ++baseCount;
        if (baseCount > 0 && baseCount < static_cast<int>(out.pending.size()))
            out.message = "发现新基础包与补丁更新（请先更新基础包）";
        else if (baseCount > 0) out.message = "发现新基础包";
        else out.message = "发现新补丁";
    } else {
        out.message = "已是最新版本";
    }
    if (!rollbackNote.empty()) {
        out.message = rollbackNote + "；" + out.message;
    }
    return true;
}

std::string UpdateManager::rollbackIfRevoked(const Json& root) {
    const std::string local = localVersion();
    if (local.empty()) return "";
    const Json& revoked = root.get("revoked");
    if (revoked.type != Json::Type::Array) return "";

    std::vector<UpdateFileItem> files;
    std::string revokedVersion;
    for (const Json& r : revoked.array()) {
        const std::string rid = r.getString("versionId");
        if (rid.empty() || rid != local) continue;
        if (r.getString("type") == "full") return "";  // 基础包整包不回滚
        revokedVersion = rid;
        for (const Json& f : r.get("files").array()) {
            UpdateFileItem item;
            item.fileName = f.getString("fileName");
            item.url = f.getString("url");
            item.targetRelativePath = f.getString("targetRelativePath");
            item.hash = f.getString("hash");
            item.kind = f.getString("kind");
            item.size = static_cast<int64_t>(f.getNumber("size"));
            if (!item.fileName.empty()) files.push_back(item);
        }
        break;
    }
    if (revokedVersion.empty()) return "";

    // 回退目标：比被撤销版本低的最高可用版本；没有则取最新基础包
    std::vector<std::string> candidates;
    for (const Json& v : root.get("versions").array()) {
        const std::string id = v.getString("versionId");
        if (!id.empty()) candidates.push_back(id);
    }
    std::vector<std::string> baseVersions;
    const Json& bvObj = root.get("baseVersions");
    if (bvObj.type == Json::Type::Object) {
        const Json& plat = bvObj.get(platform_);
        if (plat.type == Json::Type::Array) {
            for (const Json& v : plat.array()) {
                const std::string id = v.asString();
                if (!id.empty()) {
                    baseVersions.push_back(id);
                    candidates.push_back(id);
                }
            }
        }
    }
    std::string previous;
    for (const std::string& c : candidates) {
        if (versionNewer(revokedVersion, c) && (previous.empty() || versionNewer(c, previous))) {
            previous = c;
        }
    }
    if (previous.empty()) {
        for (const std::string& b : baseVersions) {
            if (previous.empty() || versionNewer(b, previous)) previous = b;
        }
    }

    int deleted = 0, locked = 0;
    for (const UpdateFileItem& item : files) {
        std::string target;
        if (item.kind == "ExternFile") {
            target = joinPath(gameRoot_, item.targetRelativePath);
        } else {
            target = joinPath(joinPath(joinPath(gameRoot_, "CodeBuild\\Content"), "Paks"), item.fileName);
        }
        if (target.empty() || GetFileAttributesA(target.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (DeleteFileA(target.c_str())) {
            ++deleted;
        } else {
            ++locked;
        }
    }

    setLocalVersion(previous);
    std::string note = "本地版本 " + revokedVersion + " 已被服务器撤销，已删除本地更新文件";
    if (deleted > 0) note += "（删除 " + std::to_string(deleted) + " 个文件）";
    if (!previous.empty()) note += "，版本回退至 " + previous;
    if (locked > 0) note += "；" + std::to_string(locked) + " 个文件被占用未删除，重启后再检查一次即可";
    return note;
}

bool UpdateManager::downloadFile(const UpdateFileItem& item, const ProgressFn& progress,
                                 const std::atomic<bool>& cancel, std::string& err) {
    std::string target;
    if (item.kind == "ContentPak" || item.kind == "IoStore") {
        target = joinPath(gameRoot_, "CodeBuild\\Content\\Paks\\" + item.fileName);
    } else {
        target = joinPath(gameRoot_, item.targetRelativePath);
    }
    std::string dir = target;
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    createDirectories(dir);

    const std::string tmp = target + ".download";
    // 服务端返回的 url 可能是相对路径，需要拼接服务器地址
    std::string downloadUrl = item.url;
    if (downloadUrl.rfind("http", 0) != 0) {
        downloadUrl = httpUrlJoin(cfg_.serverUrl, downloadUrl);
    }
    bool ok = http_.download(downloadUrl, tmp, cfg_.speedLimitKBps, 120,
                             [&](int64_t done, int64_t total) {
                                 if (progress) progress("下载中", done, total, item.fileName);
                             },
                             cancel, err);
    if (!ok) return false;

    // 下载后校验哈希（重试一次）
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!item.hash.empty()) {
            std::string md5err;
            std::string md5 = fileMD5(tmp, &md5err);
            if (!md5.empty() && _stricmp(md5.c_str(), item.hash.c_str()) != 0) {
                err = "哈希校验失败: " + item.fileName;
                if (attempt == 0) {
                    if (!http_.download(downloadUrl, tmp, cfg_.speedLimitKBps, 120, nullptr, cancel, err)) return false;
                    continue;
                }
                return false;
            }
        }
        break;
    }

    DeleteFileA(target.c_str());
    if (!MoveFileExA(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = "替换文件失败: " + item.fileName;
        return false;
    }
    return true;
}

bool UpdateManager::applyVersion(const std::string& versionId, const ProgressFn& progress,
                                 const std::atomic<bool>& cancel, std::string& err) {
    HttpResult resp = http_.get(versionUrl(versionId));
    if (!resp.ok()) {
        err = "获取更新描述失败: " + (resp.error.empty() ? std::to_string(resp.status) : resp.error);
        return false;
    }
    std::string jerr;
    Json root = Json::parse(resp.body, &jerr);
    if (root.type != Json::Type::Object) {
        err = "更新描述解析失败";
        return false;
    }

    std::vector<UpdateFileItem> items;
    for (const Json& f : root.get("files").array()) {
        UpdateFileItem item;
        item.fileName = f.getString("fileName");
        item.url = f.getString("url");
        item.targetRelativePath = f.getString("targetRelativePath");
        item.hash = f.getString("hash");
        item.kind = f.getString("kind");
        item.size = static_cast<int64_t>(f.getNumber("size"));
        if (!item.fileName.empty() && !item.url.empty()) items.push_back(item);
    }
    if (items.empty()) {
        err = "该版本没有可下载文件";
        return false;
    }

    bool restartRequired = root.getBool("restartRequired");
    const std::string updateType = root.getString("type");

    // 保留文件集合（完整包更新时用于清理旧补丁包）
    std::set<std::string> keepNames;
    for (const UpdateFileItem& item : items) {
        std::string rel = (item.kind == "ContentPak" || item.kind == "IoStore")
                              ? item.fileName
                              : item.targetRelativePath;
        auto pos = rel.find_last_of("\\/");
        if (pos != std::string::npos) rel = rel.substr(pos + 1);
        std::string low = rel;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (!low.empty()) keepNames.insert(low);
    }

    int64_t totalBytes = 0;
    for (const UpdateFileItem& item : items) totalBytes += item.size;
    int64_t doneBytes = 0;
    int64_t doneCount = 0, totalCount = static_cast<int64_t>(items.size());
    for (const UpdateFileItem& item : items) {
        if (cancel.load()) { err = "已取消"; return false; }
        if (progress) progress("下载更新 " + std::to_string(doneCount + 1) + "/" + std::to_string(totalCount),
                               doneBytes, totalBytes, item.fileName);
        // 把单文件进度换算为整个版本的字节进度
        auto fileProgress = [&](const std::string& status, int64_t done, int64_t, const std::string& file) {
            if (progress) progress(status, doneBytes + done, totalBytes, file);
        };
        if (!downloadFile(item, fileProgress, cancel, err)) return false;
        doneBytes += item.size;
        ++doneCount;
        if (progress) progress("完成 " + std::to_string(doneCount) + "/" + std::to_string(totalCount),
                               doneBytes, totalBytes, item.fileName);
    }

    // 完整包更新：清理旧版本遗留的补丁包（如 1.3_Windows_001_P.pak）
    if (updateType == "full") {
        cleanupStalePatchPaks(paksDirectory(), keepNames);
    }

    // 更新完成后立即尝试交换暂存补丁（游戏未运行时文件未被锁定，可直接替换）
    finalizePendingMerges();

    setLocalVersion(versionId);
    if (restartRequired && progress) progress("更新完成（需要重启游戏生效）", totalCount, totalCount, "");
    return true;
}

bool UpdateManager::repair(const ProgressFn& progress, const std::atomic<bool>& cancel, std::string& err) {
    HttpResult resp = http_.get(manifestUrl());
    if (!resp.ok()) {
        err = "获取完整性清单失败: " + (resp.error.empty() ? std::to_string(resp.status) : resp.error);
        return false;
    }
    std::string jerr;
    Json root = Json::parse(resp.body, &jerr);
    if (root.type != Json::Type::Object) {
        err = "清单解析失败";
        return false;
    }
    const std::string baseVersion = root.getString("baseVersionId");
    if (!baseVersion.empty()) baseVersion_ = baseVersion;

    struct CheckItem {
        std::string path, hash;
        int64_t size = 0;
    };
    std::vector<CheckItem> files;
    for (const Json& f : root.get("files").array()) {
        CheckItem item;
        item.path = f.getString("path");
        item.hash = f.getString("hash");
        item.size = static_cast<int64_t>(f.getNumber("size"));
        if (!item.path.empty()) files.push_back(item);
    }

    // 清单保留文件集合：完整性修复时同步清理旧补丁包
    std::set<std::string> keepNames;
    for (const CheckItem& item : files) {
        std::string rel = item.path;
        auto pos = rel.find_last_of("\\/");
        if (pos != std::string::npos) rel = rel.substr(pos + 1);
        std::string low = rel;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (!low.empty()) keepNames.insert(low);
    }
    const std::string paksDir = joinPath(joinPath(gameRoot_, "CodeBuild\\Content"), "Paks");

    std::vector<CheckItem> broken;
    int64_t checked = 0;
    for (const CheckItem& item : files) {
        if (cancel.load()) { err = "已取消"; return false; }
        if (progress) progress("检查 " + std::to_string(checked + 1) + "/" + std::to_string(files.size()),
                               checked, static_cast<int64_t>(files.size()), item.path);
        std::string localPath = joinPath(gameRoot_, item.path);
        std::string md5err;
        std::string md5 = fileMD5(localPath, &md5err);
        if (md5.empty() || _stricmp(md5.c_str(), item.hash.c_str()) != 0) {
            broken.push_back(item);
        }
        ++checked;
    }

    if (broken.empty()) {
        cleanupStalePatchPaks(paksDir, keepNames);
        if (progress) progress("修复完成：所有文件完整", 1, 1, "");
        return true;
    }

    int64_t brokenTotal = 0;
    for (const CheckItem& item : broken) brokenTotal += item.size;
    if (progress) progress("发现 " + std::to_string(broken.size()) + " 个问题文件，开始下载修复", 0, brokenTotal, "");
    int64_t brokenDone = 0;
    int64_t done = 0;
    for (const CheckItem& item : broken) {
        if (cancel.load()) { err = "已取消"; return false; }
        UpdateFileItem file;
        file.fileName = item.path.substr(item.path.find_last_of("\\/") + 1);
        file.url = httpUrlJoin(cfg_.serverUrl, "files/packages/" + urlEncode(platform_) +
                               "/" + urlEncode(baseVersion) + "/" + item.path);
        file.targetRelativePath = item.path;
        file.hash = item.hash;
        file.size = item.size;
        file.kind = "ExternFile";
        if (progress) progress("修复 " + std::to_string(done + 1) + "/" + std::to_string(broken.size()),
                               brokenDone, brokenTotal, item.path);
        auto fileProgress = [&](const std::string& status, int64_t d, int64_t, const std::string& path) {
            if (progress) progress(status, brokenDone + d, brokenTotal, path);
        };
        if (!downloadFile(file, fileProgress, cancel, err)) return false;
        brokenDone += item.size;
        ++done;
    }
    cleanupStalePatchPaks(paksDir, keepNames);
    if (progress) progress("修复完成：成功 " + std::to_string(done) + " 个文件", brokenDone, brokenTotal, "");
    return true;
}

bool UpdateManager::launchGame(std::string& err, HANDLE* outProcess) const {
    std::string exe = gameExePath();
    if (exe.empty() || GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = exe.empty() ? "未找到游戏程序，请先下载游戏" : ("未找到游戏程序: " + exe);
        return false;
    }
    SHELLEXECUTEINFOA sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "open";
    sei.lpFile = exe.c_str();
    sei.lpDirectory = gameRoot_.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei) || reinterpret_cast<intptr_t>(sei.hProcess) <= 0) {
        err = "启动游戏失败";
        return false;
    }
    if (outProcess) {
        *outProcess = sei.hProcess;
    } else if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }
    return true;
}
