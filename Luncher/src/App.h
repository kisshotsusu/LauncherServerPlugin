#pragma once

#include "Config.h"
#include "UpdateManager.h"

#include <windows.h>
#include <webview2.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Json;

struct UiState {
    std::mutex m;
    std::string status = "就绪";
    std::string currentFile;
    double progress = 0;
    bool showProgress = false;
    bool busy = false;
    std::string pendingText;
    std::string lastResult;
    std::vector<VersionInfo> pending;
};

// 跨线程投递到 UI 线程的事件
struct UiEvent {
    int kind = 0;            // 0=状态 1=进度 2=待更新 3=结果 4=忙碌 5=目录选择 6=刷新init 7=游戏运行状态 8=重载页面
    std::string text;
    double progress = 0;
    double total = 0;        // 总字节数（用于按钮上的速度/进度显示）
    bool flag = false;
    bool download = false;   // 是否为下载流程
    int mode = 0;            // 0=空闲 1=检查 2=下载/更新 3=修复 4=自检
};

class App {
public:
    App() = default;
    ~App();

    bool init(HINSTANCE hInstance);
    int run();
    HWND window() const { return hwnd_; }

    void onSize(int w, int h);
    void onTimer();
    void onUiEvent(UiEvent* ev);
    void onWebMessage(const wchar_t* json);
    WNDPROC origWebviewProc() const { return origWebviewProc_; }

    // 操作入口（工作线程或 UI 线程均可调用）
    void startCheck();
    void startApplyAll();
    void startRepair();
    void startLaunch();
    void startSelfUpdate();
    void startDownloadGame();
    void stopGame();
    void scanGameProcess();
    void checkBackgroundUpdate();
    bool bootstrapRuntime();
    bool syncRuntime(bool blocking, std::string& err);
    bool bootstrapActive() const { return bootstrapActive_; }
    void onPaintNative(HDC hdc, const RECT& rc);

    void sendInit();

private:
    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool coInit_ = false;
    HICON iconBig_ = nullptr;
    HICON iconSmall_ = nullptr;
    HWND webviewHwnd_ = nullptr;
    WNDPROC origWebviewProc_ = nullptr;

    LauncherConfig cfg_;
    std::unique_ptr<UpdateManager> updater_;
    UiState ui_;
    std::atomic<bool> cancel_{false};
    std::atomic<int> opMode_{0};
    std::atomic<bool> gameRunning_{false};
    HANDLE gameProcess_ = nullptr;
    bool checked_ = false;  // 是否已完成过一次更新检查
    bool startupCheckStarted_ = false;  // 启动自检是否已触发
    std::atomic<bool> applyAfterCheck_{false};  // 下载流程：检查完成后自动开始下载

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webviewController_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3_;
    EventRegistrationToken webMsgToken_{};
    std::wstring indexPath_;
    int navAttempts_ = 0;

    std::vector<std::string> frameFiles_;

    bool createWindow();
    void initWebView();
    void installDragSubclass();
    void setVirtualHostMappings();
    void loadFrames();
    void handleMessage(const std::string& json);
    void saveSettings(const Json& body);
    void browseGamePath();
    std::string browseForFolder(const wchar_t* title);
    bool findGameProcess(HANDLE* outHandle) const;
    void startupSelfCheck();
    bool runSelfUpdateCheck(bool silent);  // true = 已发现启动器更新并交给 SelfUpdater
    std::string runtimePath(const std::string& rel) const;  // exe目录/app/rel
    std::string readLocalBackgroundVersion() const;
    void saveLocalBackgroundVersion(const std::string& version);
    std::string readLocalRuntimeVersion() const;
    void saveLocalRuntimeVersion(const std::string& version);
    void pumpNativeMessages();
    void endBusySilent();
    bool gameInstalled() const;

    bool bootstrapActive_ = false;          // 首次初始化原生进度窗口
    std::atomic<int> bootstrapProgress_{0};
    std::string bootstrapText_;

    void spawn(std::function<void()> fn);
    void pushUiEvent(int kind, const std::string& text = "", double progress = 0,
                     bool flag = false, double total = 0, bool download = false, int mode = 0);
    void sendJson(const std::string& json);
    void reportProgress(const std::string& status, int64_t done, int64_t total,
                        const std::string& file, bool download = false);
    void applyOperationResult(bool ok, const std::string& message);

    std::string configPath() const;
    std::string buildInitJson();
};
