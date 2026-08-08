#pragma once

#include <string>

#define CLOUD_LAUNCHER_VERSION "1.0.0"

struct LauncherConfig {
    std::string serverUrl = "http://127.0.0.1:8710";
    std::string platform = "Windows";
    std::string gamePath;               // 留空 = 启动器所在目录
    std::string frameDir = "Background"; // 序列帧目录（相对启动器）
    double frameFps = 12;
    long speedLimitKBps = 0;            // 0 = 不限速
    std::string launcherVersion = CLOUD_LAUNCHER_VERSION;
    bool autoCheckOnStart = true;
    bool autoRepairOnStart = false;
};

class ConfigStore {
public:
    static LauncherConfig load(const std::string& path);
    static void save(const std::string& path, const LauncherConfig& cfg);
};

std::string exeDirectory();
std::string joinPath(const std::string& a, const std::string& b);
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
