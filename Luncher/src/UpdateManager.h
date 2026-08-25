#pragma once

#include "Config.h"
#include "Network.h"
#include "Json.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct UpdateFileItem {
    std::string fileName;
    std::string url;
    std::string targetRelativePath;
    std::string hash;
    std::string kind;   // ContentPak / IoStore / ExternFile
    int64_t size = 0;
};

struct VersionInfo {
    std::string versionId;
    std::string baseVersionId;
    std::string date;
    std::string type;   // full / patch
    std::string url;
    int64_t totalSize = 0;
};

struct UpdateCheckResult {
    bool hasUpdate = false;
    std::string latestVersion;
    std::string message;
    std::vector<VersionInfo> pending;  // 基础包在前，补丁在后
};

using ProgressFn = std::function<void(const std::string& status, int64_t done, int64_t total, const std::string& file)>;

class UpdateManager {
public:
    explicit UpdateManager(const LauncherConfig& cfg);

    // 检查更新：先基础包后补丁
    bool checkUpdates(UpdateCheckResult& out, std::string& err);

    // 应用一个版本（基础包整包或补丁）
    bool applyVersion(const std::string& versionId, const ProgressFn& progress,
                      const std::atomic<bool>& cancel, std::string& err);

    // 完整性修复（按当前基础包版本清单）
    bool repair(const ProgressFn& progress, const std::atomic<bool>& cancel, std::string& err);

    // 扫描 Paks 目录中的 "*.pending" 暂存补丁，在游戏未运行（文件未被锁定）时交换回基础文件。
    // 返回成功交换的数量；outTotal 为发现的暂存补丁总数。
    // 应在启动游戏进程之前调用，确保基础文件未被引擎锁定。
    int finalizePendingMerges(int* outTotal = nullptr) const;

    // 启动游戏；outProcess 非空时返回进程句柄（调用方负责 CloseHandle）
    bool launchGame(std::string& err, HANDLE* outProcess = nullptr) const;

    std::string localVersion() const;
    void setLocalVersion(const std::string& version);
    std::string gameRoot() const { return gameRoot_; }
    std::string gameExePath() const;  // 检测游戏目录内首个 exe（排除启动器自身）
    std::string currentBaseVersion() const { return baseVersion_; }

private:
    LauncherConfig cfg_;
    std::string gameRoot_;
    std::string platform_;
    std::string localVersion_;
    std::string baseVersion_;
    HttpClient http_;

    bool loadLocalVersion();
    void saveLocalVersion(const std::string& version);
    std::string manifestUrl() const;
    std::string versionsUrl() const;
    std::string versionUrl(const std::string& id) const;
    bool downloadFile(const UpdateFileItem& item, const ProgressFn& progress,
                      const std::atomic<bool>& cancel, std::string& err);

    std::string paksDirectory() const;

    // 服务器撤销/关闭版本时：删除本地更新文件并回退版本号，返回提示文本
    std::string rollbackIfRevoked(const Json& root);
};

// 简易版本号比较：a > b
bool versionNewer(const std::string& a, const std::string& b);
