#include "UpdateManager.h"
#include <fstream>
#include <iostream>
#include <sstream>

static bool require(bool value, const std::string& message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    LauncherConfig cfg;
    cfg.serverUrl = argv[1];
    cfg.gamePath = argv[2];
    UpdateManager manager(cfg);
    UpdateCheckResult result;
    std::string error;
    if (!require(manager.checkUpdates(result,error),error)) return 1;
    if (!require(result.pending.size()==2,"Expected independent program and resource base")) return 1;
    if (!require(result.pending[0].type=="full" && result.pending[1].type=="resource","Program must precede resources")) return 1;
    if (!require(result.pending[0].versionId==result.pending[1].versionId,"Fixture must exercise identical IDs")) return 1;
    std::atomic<bool> cancel(false);
    for (const auto& version : result.pending)
        if (!require(manager.applyVersion(version.versionId,nullptr,cancel,error,&version),error)) return 1;
    UpdateManager reloaded(cfg);
    UpdateCheckResult after;
    if (!require(reloaded.checkUpdates(after,error),error)) return 1;
    if (!require(!after.hasUpdate && reloaded.localVersion()=="2.0","Installed program and resources must not be offered again")) return 1;
    const auto path = joinPath(cfg.gamePath,"CodeBuild\\Saved\\CloudUpdate\\Windows\\local_version.json");
    std::ifstream in(path);
    std::stringstream text; text << in.rdbuf();
    Json record = Json::parse(text.str());
    if (!require(record.getString("installedGameVersion")=="2.0" && record.getString("resourceVersionId")=="2.0","Dual version record differs from plugin schema")) return 1;
    record.obj["resourceVersionId"] = Json::string("12.0");
    record.obj["versionId"] = Json::string("12.0");
    in.close();
    { std::ofstream out(path); out << record.dump(2); }
    UpdateCheckResult engineUpdate;
    if (!require(reloaded.checkUpdates(engineUpdate,error),error)) return 1;
    if (!require(reloaded.localVersion()=="12.0" && !engineUpdate.hasUpdate,"Engine resource update must preserve installed program version")) return 1;
    std::cout << "Typed program/resource downloads and shared dual version records passed\n";
    return 0;
}
