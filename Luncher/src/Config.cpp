#include "Config.h"

#include "Json.h"

#include <windows.h>
#include <shlwapi.h>

#include <fstream>
#include <sstream>

std::string exeDirectory() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos);
    return wideToUtf8(path);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

LauncherConfig ConfigStore::load(const std::string& path) {
    LauncherConfig cfg;
    std::ifstream in(path, std::ios::binary);
    if (!in) return cfg;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    Json root = Json::parse(ss.str(), &err);
    if (root.type != Json::Type::Object) return cfg;
    cfg.serverUrl = root.getString("serverUrl", cfg.serverUrl);
    cfg.platform = root.getString("platform", cfg.platform);
    cfg.gamePath = root.getString("gamePath", cfg.gamePath);
    cfg.frameDir = root.getString("frameDir", cfg.frameDir);
    cfg.frameFps = root.getNumber("frameFps", cfg.frameFps);
    cfg.speedLimitKBps = static_cast<long>(root.getNumber("speedLimitKBps", cfg.speedLimitKBps));
    cfg.launcherVersion = root.getString("launcherVersion", cfg.launcherVersion);
    cfg.autoCheckOnStart = root.getBool("autoCheckOnStart", cfg.autoCheckOnStart);
    cfg.autoRepairOnStart = root.getBool("autoRepairOnStart", cfg.autoRepairOnStart);
    return cfg;
}

void ConfigStore::save(const std::string& path, const LauncherConfig& cfg) {
    Json root = Json::object();
    root.obj["serverUrl"] = Json::string(cfg.serverUrl);
    root.obj["platform"] = Json::string(cfg.platform);
    root.obj["gamePath"] = Json::string(cfg.gamePath);
    root.obj["frameDir"] = Json::string(cfg.frameDir);
    root.obj["frameFps"] = Json::number(cfg.frameFps);
    root.obj["speedLimitKBps"] = Json::number(cfg.speedLimitKBps);
    root.obj["launcherVersion"] = Json::string(cfg.launcherVersion);
    root.obj["autoCheckOnStart"] = Json::boolean(cfg.autoCheckOnStart);
    root.obj["autoRepairOnStart"] = Json::boolean(cfg.autoRepairOnStart);
    std::ofstream out(path, std::ios::binary);
    out << root.dump(2);
}
