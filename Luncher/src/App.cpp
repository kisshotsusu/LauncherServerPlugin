#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "App.h"

#include "../resource.h"
#include "Json.h"
#include "Network.h"

#include <webview2.h>
#include <WebView2EnvironmentOptions.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr UINT WM_APP_EVENT = WM_APP + 1;
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 760;
constexpr int kTitleBarHeight = 46;
// 左侧品牌区与右侧按钮区：这两个区域让鼠标事件透传给 WebView2
constexpr int kDragLeftZone = 210;
constexpr int kDragRightZone = 300;
// PROCESS_SYNCHRONIZE = 0x00100000（部分 SDK 未在默认 WINVER 下声明）
constexpr DWORD kProcessSynchronize = 0x00100000;

std::wstring u8w(const std::string& s) { return utf8ToWide(s); }

// 递归创建目录（用于背景图多级路径）
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

// 清理背景目录中不属于服务器清单的旧图
void cleanupStaleBackgroundFrames(const std::string& dir, const std::set<std::string>& keep) {
    std::string pattern = joinPath(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.size() < 5 || low.rfind(".png") != low.size() - 4) continue;
        if (!keep.count(low)) {
            DeleteFileA(joinPath(dir, name).c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

App* appFromHwnd(HWND hwnd) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// 子类化 WebView2 宿主窗口：标题栏中间区域返回 HTCAPTION，由系统原生拖动
LRESULT CALLBACK WebviewSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND parent = GetParent(hwnd);
    App* app = appFromHwnd(parent);
    if (app && app->window() == parent) {
        if (msg == WM_NCHITTEST) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetClientRect(parent, &rc);
            POINT cpt = pt;
            ScreenToClient(parent, &cpt);
            if (cpt.y >= 0 && cpt.y < kTitleBarHeight) {
                if (cpt.x >= kDragLeftZone && cpt.x <= rc.right - kDragRightZone) {
                    return HTCAPTION;
                }
            }
        } else if (msg == WM_NCLBUTTONDOWN && wp == HTCAPTION) {
            ReleaseCapture();
            SendMessageW(parent, WM_NCLBUTTONDOWN, HTCAPTION, lp);
            return 0;
        } else if (msg == WM_NCLBUTTONDBLCLK && wp == HTCAPTION) {
            SendMessageW(parent, WM_NCLBUTTONDBLCLK, HTCAPTION, lp);
            return 0;
        }
        if (app->origWebviewProc()) {
            return CallWindowProcW(app->origWebviewProc(), hwnd, msg, wp, lp);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = appFromHwnd(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_SIZE:
            if (app) app->onSize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_TIMER:
            if (app) app->onTimer();
            return 0;
        case WM_PAINT:
            if (app && app->bootstrapActive()) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                app->onPaintNative(hdc, ps.rcPaint);
                EndPaint(hwnd, &ps);
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = 980;
            mmi->ptMinTrackSize.y = 600;
            return 0;
        }
        case WM_APP_EVENT:
            if (app) app->onUiEvent(reinterpret_cast<UiEvent*>(wp));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

App::~App() {
    if (webviewHwnd_ && origWebviewProc_ && IsWindow(webviewHwnd_)) {
        SetWindowLongPtrW(webviewHwnd_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(origWebviewProc_));
    }
    if (webview_) {
        webview_->remove_WebMessageReceived(webMsgToken_);
    }
    webview_.Reset();
    webviewController_.Reset();
    env_.Reset();
    if (gameProcess_) CloseHandle(gameProcess_);
    if (iconBig_) DestroyIcon(iconBig_);
    if (iconSmall_) DestroyIcon(iconSmall_);
    if (coInit_) {
        CoUninitialize();
    }
}

bool App::init(HINSTANCE hInstance) {
    hInst_ = hInstance;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        coInit_ = true;
    }

    cfg_ = ConfigStore::load(configPath());
    if (cfg_.launcherVersion != CLOUD_LAUNCHER_VERSION) {
        cfg_.launcherVersion = CLOUD_LAUNCHER_VERSION;
        ConfigStore::save(configPath(), cfg_);
    }
    updater_ = std::make_unique<UpdateManager>(cfg_);

    // 优先使用内嵌资源图标（已编译进 exe，单文件首运行即可见），
    // 内嵌资源缺失时回退到 ui/launcher.ico 文件
    iconBig_ = static_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 64, 64, LR_SHARED));
    iconSmall_ = static_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 16, 16, LR_SHARED));
    if (!iconBig_ || !iconSmall_) {
        std::wstring iconPath = u8w(joinPath(joinPath(exeDirectory(), "ui"), "launcher.ico"));
        if (!iconBig_) iconBig_ = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 64, 64, LR_LOADFROMFILE));
        if (!iconSmall_) iconSmall_ = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hIcon = iconBig_;
    wc.lpszClassName = L"CloudLauncherWebViewWnd";
    RegisterClassW(&wc);

    if (!createWindow()) return false;
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall_));
    SetTimer(hwnd_, 2, 2000, nullptr);  // 每 2 秒检查游戏进程状态
    ShowWindow(hwnd_, SW_SHOW);  // 先显示窗口，用于首次初始化的进度

    // 首次运行：从服务器拉取界面资源到 app/ 子目录
    if (!bootstrapRuntime()) {
        DestroyWindow(hwnd_);
        return false;
    }
    loadFrames();
    // 首次运行后重新加载图标（app/ui/launcher.ico）
    {
        std::wstring iconPath2 = u8w(runtimePath("ui\\launcher.ico"));
        HICON iconBig2 = static_cast<HICON>(LoadImageW(nullptr, iconPath2.c_str(), IMAGE_ICON, 64, 64, LR_LOADFROMFILE));
        HICON iconSmall2 = static_cast<HICON>(LoadImageW(nullptr, iconPath2.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
        if (iconBig2) {
            if (iconBig_) DestroyIcon(iconBig_);
            iconBig_ = iconBig2;
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig_));
        }
        if (iconSmall2) {
            if (iconSmall_) DestroyIcon(iconSmall_);
            iconSmall_ = iconSmall2;
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall_));
        }
    }

    initWebView();
    UpdateWindow(hwnd_);
    return true;
}

int App::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

bool App::createWindow() {
    int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - kDefaultWidth) / 2);
    int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - kDefaultHeight) / 2);
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW, L"CloudLauncherWebViewWnd", L"CloudUpdate 启动器",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
        x, y, kDefaultWidth, kDefaultHeight, nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;

    // Win11 圆角窗口（不支持时忽略）
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2（Win11 圆角，旧系统静默失败）
    DWORD corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    return true;
}

void App::onSize(int w, int h) {
    if (!webviewHwnd_) installDragSubclass();
    if (!webviewController_) return;
    RECT rc = {0, 0, w, h};
    webviewController_->put_Bounds(rc);
}

void App::onTimer() {
    if (gameProcess_) {
        DWORD wr = WaitForSingleObject(gameProcess_, 0);
        if (wr == WAIT_OBJECT_0) {
            CloseHandle(gameProcess_);
            gameProcess_ = nullptr;
            // 主进程可能只是 stub，真正的游戏进程仍在运行（如 UE 的启动器 stub）
            HANDLE h = nullptr;
            if (findGameProcess(&h)) {
                gameProcess_ = h;
                return;
            }
            gameRunning_ = false;
            pushUiEvent(7, "", 0, false);
            pushUiEvent(0, "游戏已退出");
        }
    } else {
        scanGameProcess();
    }
}

/* ---------- WebView2 初始化 ---------- */

void App::initWebView() {
    Microsoft::WRL::ComPtr<CoreWebView2EnvironmentOptions> options =
        Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        options->put_AdditionalBrowserArguments(
            L"--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection");
        options->put_Language(L"zh-CN");
    }

    std::wstring dataDir = u8w(runtimePath("WebView2Data"));
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), options.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    MessageBoxW(hwnd_, L"WebView2 运行时不可用，请安装 Microsoft Edge WebView2 Runtime。",
                                L"CloudUpdate 启动器", MB_ICONERROR);
                    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    return S_OK;
                }
                env_ = env;
                return env->CreateCoreWebView2Controller(
                    hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result)) {
                                MessageBoxW(hwnd_, L"WebView2 控制器创建失败。", L"CloudUpdate 启动器",
                                            MB_ICONERROR);
                                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                                return S_OK;
                            }
                            webviewController_ = controller;
                            if (FAILED(controller->get_CoreWebView2(&webview_)) || !webview_) {
                                MessageBoxW(hwnd_, L"WebView2 核心对象获取失败。", L"CloudUpdate 启动器",
                                            MB_ICONERROR);
                                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                                return S_OK;
                            }

                            // 基础设置：去掉开发者工具、右键菜单、状态栏、快捷键与缩放
                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);

                                Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings3))) && settings3) {
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                                }
                            }

                            // 深色底色，避免加载白闪
                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
                                COREWEBVIEW2_COLOR color;
                                color.A = 255;
                                color.R = 5;
                                color.G = 7;
                                color.B = 13;
                                c2->put_DefaultBackgroundColor(color);
                            }

                            webview_->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                                            onWebMessage(raw);
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &webMsgToken_);

                            if (FAILED(webview_->QueryInterface(IID_PPV_ARGS(&webview3_)))) {
                                MessageBoxW(hwnd_, L"WebView2 版本过旧，无法加载本地界面。",
                                            L"CloudUpdate 启动器", MB_ICONERROR);
                                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                                return S_OK;
                            }

                            setVirtualHostMappings();
                            installDragSubclass();

                            RECT rc;
                            GetClientRect(hwnd_, &rc);
                            controller->put_Bounds({0, 0, rc.right, rc.bottom});
                            controller->put_ZoomFactor(1.0);

                            indexPath_ = L"file:///" + u8w(runtimePath("ui\\index.html"));
                            webview_->add_NavigationCompleted(
                                Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL ok = FALSE;
                                        args->get_IsSuccess(&ok);
                                        if (!ok && navAttempts_ < 3) {
                                            COREWEBVIEW2_WEB_ERROR_STATUS status =
                                                COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                            args->get_WebErrorStatus(&status);
                                            // 冷启动偶发超时，自动重试导航
                                            if (status == COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT ||
                                                status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                                                ++navAttempts_;
                                                webview_->Navigate(indexPath_.c_str());
                                            }
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            webview_->Navigate(indexPath_.c_str());
                            return S_OK;
                        }).Get());
            }).Get());

    if (FAILED(hr)) {
        MessageBoxW(hwnd_, L"WebView2 环境初始化失败。", L"CloudUpdate 启动器", MB_ICONERROR);
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

void App::installDragSubclass() {
    if (webviewHwnd_ || !hwnd_) return;
    HWND child = FindWindowExW(hwnd_, nullptr, L"Chrome_WidgetWin_0", nullptr);
    if (!child) return;
    webviewHwnd_ = child;
    origWebviewProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WebviewSubclassProc)));
}

void App::setVirtualHostMappings() {
    if (!webview3_) return;
    webview3_->ClearVirtualHostNameToFolderMapping(L"bgframes");
    if (!frameFiles_.empty()) {
        std::wstring frameDir = u8w(runtimePath(cfg_.frameDir));
        webview3_->SetVirtualHostNameToFolderMapping(
            L"bgframes", frameDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
}

/* ---------- 序列帧 ---------- */

void App::loadFrames() {
    frameFiles_.clear();
    std::string dir = runtimePath(cfg_.frameDir);
    std::string pattern = joinPath(dir, "*.png");
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            frameFiles_.push_back(fd.cFileName);
        }
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    std::sort(frameFiles_.begin(), frameFiles_.end());
}

/* ---------- 与前端通信 ---------- */

void App::sendJson(const std::string& json) {
    if (!webview_) return;
    webview_->PostWebMessageAsJson(u8w(json).c_str());
}

void App::pushUiEvent(int kind, const std::string& text, double progress, bool flag,
                      double total, bool download, int mode) {
    auto* ev = new UiEvent;
    ev->kind = kind;
    ev->text = text;
    ev->progress = progress;
    ev->total = total;
    ev->flag = flag;
    ev->download = download;
    ev->mode = mode;
    if (!PostMessageW(hwnd_, WM_APP_EVENT, reinterpret_cast<WPARAM>(ev), 0)) {
        delete ev;
    }
}

void App::onUiEvent(UiEvent* ev) {
    if (!ev) return;
    Json root = Json::object();
    switch (ev->kind) {
        case 0:  // 状态
            root.obj["type"] = Json::string("status");
            root.obj["text"] = Json::string(ev->text);
            break;
        case 1:  // 进度
            root.obj["type"] = Json::string("progress");
            root.obj["show"] = Json::boolean(ev->flag);
            root.obj["progress"] = Json::number(ev->progress);
            root.obj["total"] = Json::number(ev->total);
            root.obj["file"] = Json::string(ev->text);
            root.obj["download"] = Json::boolean(ev->download);
            root.obj["mode"] = Json::number(ev->mode);
            break;
        case 2: {  // 待更新
            root.obj["type"] = Json::string("pending");
            root.obj["text"] = Json::string(ev->text);
            Json items = Json::arrayValue();
            std::lock_guard<std::mutex> lock(ui_.m);
            for (const VersionInfo& v : ui_.pending) {
                Json item = Json::object();
                item.obj["versionId"] = Json::string(v.versionId);
                item.obj["type"] = Json::string(v.type);
                item.obj["date"] = Json::string(v.date);
                item.obj["totalSize"] = Json::number(static_cast<double>(v.totalSize));
                items.arr.push_back(item);
            }
            root.obj["items"] = items;
            break;
        }
        case 3:  // 结果
            root.obj["type"] = Json::string("result");
            root.obj["ok"] = Json::boolean(ev->flag);
            root.obj["message"] = Json::string(ev->text);
            break;
        case 4:  // 忙碌
            root.obj["type"] = Json::string("busy");
            root.obj["busy"] = Json::boolean(ev->flag);
            root.obj["mode"] = Json::number(ev->mode);
            break;
        case 5:  // 目录选择
            root.obj["type"] = Json::string("folder_selected");
            root.obj["path"] = Json::string(ev->text);
            break;
        case 6:  // 刷新完整状态（从工作线程回 UI 线程）
            setVirtualHostMappings();
            sendInit();
            delete ev;
            return;
        case 7:  // 游戏运行状态
            root.obj["type"] = Json::string("game_state");
            root.obj["running"] = Json::boolean(ev->flag);
            break;
        case 8:  // UI 资源已更新，重载页面让新版前端生效
            if (webview_) webview_->Reload();
            delete ev;
            return;
        default:
            break;
    }
    delete ev;
    sendJson(root.dump(0));
}

void App::onWebMessage(const wchar_t* json) {
    if (!json) return;
    handleMessage(wideToUtf8(json));
}

void App::handleMessage(const std::string& text) {
    std::string err;
    Json root = Json::parse(text, &err);
    if (root.type != Json::Type::Object) return;
    const std::string type = root.getString("type");

    if (type == "init") {
        sendInit();
    } else if (type == "check") {
        startCheck();
    } else if (type == "apply") {
        startApplyAll();
    } else if (type == "repair") {
        startRepair();
    } else if (type == "launch") {
        startLaunch();
    } else if (type == "self_update") {
        startSelfUpdate();
    } else if (type == "download_game") {
        startDownloadGame();
    } else if (type == "refresh") {
        sendInit();
        bool busy = false;
        {
            std::lock_guard<std::mutex> lock(ui_.m);
            busy = ui_.busy;
        }
        if (!busy) {
            spawn([this] {
                std::string runtimeErr;
                syncRuntime(false, runtimeErr);
                checkBackgroundUpdate();
                startCheck();
            });
        }
    } else if (type == "close_game") {
        stopGame();
    } else if (type == "cancel") {
        cancel_.store(true);
    } else if (type == "browse") {
        browseGamePath();
    } else if (type == "settings_save") {
        saveSettings(root);
    } else if (type == "set_game_path") {
        std::string path = root.getString("path");
        if (!path.empty() && path != cfg_.gamePath) {
            cfg_.gamePath = path;
            ConfigStore::save(configPath(), cfg_);
            updater_ = std::make_unique<UpdateManager>(cfg_);
            sendInit();
            pushUiEvent(3, "游戏目录已更新", 0, true);
        }
    } else if (type == "window_minimize") {
        ShowWindow(hwnd_, SW_MINIMIZE);
    } else if (type == "window_maximize") {
        if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        else ShowWindow(hwnd_, SW_MAXIMIZE);
    } else if (type == "window_close") {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (type == "window_drag") {
        ReleaseCapture();
        POINT pt;
        GetCursorPos(&pt);
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
    }
}

std::string App::buildInitJson() {
    Json root = Json::object();
    root.obj["type"] = Json::string("init");
    root.obj["serverUrl"] = Json::string(cfg_.serverUrl);
    root.obj["gamePath"] = Json::string(updater_ ? updater_->gameRoot() : cfg_.gamePath);
    root.obj["frameDir"] = Json::string(cfg_.frameDir);
    root.obj["frameFps"] = Json::number(cfg_.frameFps);
    root.obj["speedLimitKBps"] = Json::number(cfg_.speedLimitKBps);
    root.obj["launcherVersion"] = Json::string(cfg_.launcherVersion);
    root.obj["autoCheckOnStart"] = Json::boolean(cfg_.autoCheckOnStart);
    root.obj["autoRepairOnStart"] = Json::boolean(cfg_.autoRepairOnStart);
    root.obj["localVersion"] = Json::string(updater_ ? updater_->localVersion() : "");
    root.obj["checked"] = Json::boolean(checked_);
    root.obj["installed"] = Json::boolean(gameInstalled());
    root.obj["gameRunning"] = Json::boolean(gameRunning_);

    Json frames = Json::arrayValue();
    for (const std::string& n : frameFiles_) {
        frames.arr.push_back(Json::string("https://bgframes/" + n));
    }
    root.obj["frames"] = frames;

    {
        std::lock_guard<std::mutex> lock(ui_.m);
        root.obj["status"] = Json::string(ui_.status);
        root.obj["currentFile"] = Json::string(ui_.currentFile);
        root.obj["progress"] = Json::number(ui_.progress);
        root.obj["showProgress"] = Json::boolean(ui_.showProgress);
        root.obj["busy"] = Json::boolean(ui_.busy);
        root.obj["pendingText"] = Json::string(ui_.pendingText);
        root.obj["lastResult"] = Json::string(ui_.lastResult);

        Json pending = Json::arrayValue();
        for (const VersionInfo& v : ui_.pending) {
            Json item = Json::object();
            item.obj["versionId"] = Json::string(v.versionId);
            item.obj["type"] = Json::string(v.type);
            item.obj["date"] = Json::string(v.date);
            item.obj["totalSize"] = Json::number(static_cast<double>(v.totalSize));
            pending.arr.push_back(item);
        }
        root.obj["pending"] = pending;
    }
    return root.dump(0);
}

void App::sendInit() {
    if (!gameRunning_) scanGameProcess();
    sendJson(buildInitJson());
    if (!startupCheckStarted_) {
        startupCheckStarted_ = true;
        spawn([this] { startupSelfCheck(); });
    }
}

/* ---------- 状态上报 ---------- */

void App::reportProgress(const std::string& status, int64_t done, int64_t total,
                         const std::string& file, bool download) {
    double p = 0;
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        ui_.status = status;
        ui_.currentFile = file;
        ui_.showProgress = true;
        if (total > 0) p = static_cast<double>(done) / static_cast<double>(total);
        ui_.progress = p;
    }
    pushUiEvent(0, status);
    pushUiEvent(1, file, p, true, static_cast<double>(total), download, opMode_.load());
}

void App::applyOperationResult(bool ok, const std::string& message) {
    checked_ = true;
    opMode_ = 0;
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        ui_.busy = false;
        ui_.progress = 0;
        ui_.showProgress = false;
        ui_.currentFile.clear();
        ui_.lastResult = message;
    }
    pushUiEvent(4, "", 0, false);
    pushUiEvent(1, "", 0, false);
    pushUiEvent(3, message, 0, ok);
}

void App::spawn(std::function<void()> fn) {
    std::thread([fn = std::move(fn)] { fn(); }).detach();
}

/* ---------- 功能入口 ---------- */

void App::startCheck() {
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        if (ui_.busy) return;
        ui_.busy = true;
    }
    opMode_ = 1;
    pushUiEvent(4, "", 0, true, 0, false, 1);
    pushUiEvent(0, "正在检查更新…");
    spawn([this] {
        cancel_.store(false);
        UpdateCheckResult result;
        std::string err;
        bool ok = updater_->checkUpdates(result, err);
        if (ok) {
            std::string text = result.hasUpdate
                ? result.message + "（共 " + std::to_string(result.pending.size()) + " 个更新）"
                : result.message;
            {
                std::lock_guard<std::mutex> lock(ui_.m);
                ui_.pending = result.pending;
                ui_.pendingText = text;
            }
            pushUiEvent(0, result.hasUpdate ? "发现更新" : "已是最新版本");
            pushUiEvent(2, text);
            applyOperationResult(true, text);
            if (applyAfterCheck_.exchange(false)) {
                bool hasPending = false;
                {
                    std::lock_guard<std::mutex> lock(ui_.m);
                    hasPending = !ui_.pending.empty();
                }
                if (hasPending) {
                    startApplyAll();
                } else {
                    pushUiEvent(3, "服务器暂无可用版本，无法下载游戏", 0, false);
                }
            }
        } else {
            checked_ = true;
            applyOperationResult(false, err);
        }
    });
}

void App::startApplyAll() {
    std::vector<VersionInfo> pending;
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        if (ui_.busy) return;
        pending = ui_.pending;
        if (pending.empty()) {
            pushUiEvent(3, "没有待更新的版本，请先检查更新", 0, false);
            return;
        }
        ui_.busy = true;
    }
    opMode_ = 2;
    pushUiEvent(4, "", 0, true, 0, false, 2);
    pushUiEvent(0, "准备更新…");
    spawn([this, pending] {
        cancel_.store(false);
        for (const VersionInfo& info : pending) {
            if (cancel_.load()) {
                applyOperationResult(false, "操作已取消");
                return;
            }
            std::string err;
            auto progress = [this](const std::string& status, int64_t done, int64_t total, const std::string& file) {
                reportProgress(status, done, total, file, true);
            };
            bool ok = updater_->applyVersion(info.versionId, progress, cancel_, err);
            if (!ok) {
                if (cancel_.load()) {
                    applyOperationResult(false, "操作已取消");
                } else {
                    applyOperationResult(false, "更新 " + info.versionId + " 失败: " + err);
                }
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(ui_.m);
            ui_.pending.clear();
            ui_.pendingText.clear();
        }
        pushUiEvent(2, "");
        applyOperationResult(true, "全部更新完成");
        pushUiEvent(6, "", 0, false);  // 回 UI 线程刷新“已安装”状态
    });
}

void App::startRepair() {
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        if (ui_.busy) return;
        ui_.busy = true;
    }
    opMode_ = 3;
    pushUiEvent(4, "", 0, true, 0, false, 3);
    pushUiEvent(0, "正在完整性检查与修复…");
    spawn([this] {
        cancel_.store(false);
        std::string err;
        auto progress = [this](const std::string& status, int64_t done, int64_t total, const std::string& file) {
            reportProgress(status, done, total, file);
        };
        bool ok = updater_->repair(progress, cancel_, err);
        if (ok) {
            applyOperationResult(true, "修复完成");
        } else {
            applyOperationResult(false, cancel_.load() ? "操作已取消" : ("修复失败: " + err));
        }
    });
}

void App::startLaunch() {
    if (!gameRunning_) scanGameProcess();
    if (gameRunning_) {
        pushUiEvent(3, "游戏已在运行", 0, true);
        return;
    }

    // 启动游戏前交换上一轮因文件被占用而暂存的补丁（.pending -> 基础文件）。
    // 此时游戏进程尚未运行，pak/utoc 未被引擎锁定，Move 必然成功。
    int pendingTotal = 0;
    const int swapped = updater_->finalizePendingMerges(&pendingTotal);
    if (swapped > 0) {
        pushUiEvent(0, "已应用暂存更新（" + std::to_string(swapped) + " 个文件）");
    } else if (pendingTotal > 0) {
        pushUiEvent(0, "有 " + std::to_string(pendingTotal) + " 个暂存更新待下次启动时生效");
    }

    std::string err;
    HANDLE proc = nullptr;
    if (updater_->launchGame(err, &proc)) {
        gameProcess_ = proc;
        gameRunning_ = true;
        pushUiEvent(0, "游戏已启动");
        pushUiEvent(3, "游戏已启动", 0, true);
        pushUiEvent(7, "", 0, true);
    } else {
        pushUiEvent(0, "启动失败");
        pushUiEvent(3, err, 0, false);
    }
}

void App::stopGame() {
    if (gameProcess_) {
        TerminateProcess(gameProcess_, 0);
        CloseHandle(gameProcess_);
        gameProcess_ = nullptr;
    }
    gameRunning_ = false;
    pushUiEvent(7, "", 0, false);
    pushUiEvent(3, "游戏已关闭", 0, true);
}

bool App::findGameProcess(HANDLE* outHandle) const {
    if (!outHandle || !updater_) return false;
    std::wstring root = utf8ToWide(updater_->gameRoot());
    if (root.empty()) return false;
    if (root.back() != L'\\') root += L'\\';

    std::wstring selfPath;
    {
        wchar_t buf[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) selfPath = buf;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            std::wstring lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower == L"launcher.exe" || lower == L"selfupdater.exe") continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                                   kProcessSynchronize,
                                   FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t path[MAX_PATH] = {0};
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &len)) {
                std::wstring p = path;
                if (!selfPath.empty() && _wcsicmp(p.c_str(), selfPath.c_str()) == 0) {
                    CloseHandle(h);
                    continue;
                }
                // 路径在游戏目录内即视为游戏进程（覆盖 stub 与真实游戏进程）
                if (_wcsnicmp(p.c_str(), root.c_str(), root.size()) == 0) {
                    *outHandle = h;
                    found = true;
                    break;
                }
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

void App::scanGameProcess() {
    if (gameRunning_ || !updater_) return;
    HANDLE h = nullptr;
    if (findGameProcess(&h)) {
        gameProcess_ = h;
        gameRunning_ = true;
        pushUiEvent(7, "", 0, true);
        pushUiEvent(0, "检测到游戏已在运行");
    }
}

void App::startSelfUpdate() {
    spawn([this] { runSelfUpdateCheck(false); });
}

bool App::runSelfUpdateCheck(bool silent) {
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        if (ui_.busy) return false;
        ui_.busy = true;
    }
    opMode_ = 4;
    pushUiEvent(4, "", 0, true, 0, false, 4);
    pushUiEvent(0, silent ? "启动器自检中…" : "正在检查启动器更新…");

    cancel_.store(false);
    std::string url = httpUrlJoin(cfg_.serverUrl, "api/launcher/version");
    HttpResult resp = HttpClient().get(url, 15);
    if (!resp.ok()) {
        if (silent) {
            pushUiEvent(0, "启动器自检完成");
            endBusySilent();
        } else {
            applyOperationResult(false, "无法获取启动器版本信息");
        }
        return false;
    }
    std::string jerr;
    Json root = Json::parse(resp.body, &jerr);
    std::string remoteVersion = root.getString("version");
    std::string fileUrl = root.getString("url");
    if (remoteVersion.empty() || fileUrl.empty()) {
        if (silent) {
            pushUiEvent(0, "启动器自检完成");
            endBusySilent();
        } else {
            applyOperationResult(false, "启动器版本信息无效");
        }
        return false;
    }
    if (!versionNewer(remoteVersion, cfg_.launcherVersion)) {
        if (silent) pushUiEvent(0, "启动器已是最新版本");
        applyOperationResult(true, "启动器已是最新版本 " + cfg_.launcherVersion);
        return false;
    }
    pushUiEvent(0, "发现启动器新版本 " + remoteVersion + "，正在下载…");
    std::string downloadUrl = fileUrl;
    if (fileUrl.rfind("http", 0) != 0) downloadUrl = httpUrlJoin(cfg_.serverUrl, fileUrl);
    std::string dest = joinPath(exeDirectory(), "Launcher.exe.new");
    std::string err;
    std::string updaterExe = runtimePath("SelfUpdater.exe");
    bool ok = HttpClient().download(
        downloadUrl, dest, cfg_.speedLimitKBps, 120,
        [this](int64_t done, int64_t total) {
            reportProgress("正在下载启动器更新…", done, total, "Launcher.exe");
        },
        cancel_, err);
    if (!ok) {
        applyOperationResult(false, cancel_.load() ? "操作已取消" : ("启动器下载失败: " + err));
        return false;
    }
    if (GetFileAttributesA(updaterExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        applyOperationResult(false, "缺少 SelfUpdater.exe");
        return false;
    }
    char curExe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, curExe, MAX_PATH);
    std::string args = std::string("\"") + curExe + "\" \"" + dest + "\" relaunch";
    SHELLEXECUTEINFOA sei = {sizeof(sei)};
    sei.lpFile = updaterExe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) {
        applyOperationResult(false, "无法启动更新器");
        return false;
    }
    pushUiEvent(0, "启动器更新中，程序即将退出…");
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    return true;
}

void App::endBusySilent() {
    opMode_ = 0;
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        ui_.busy = false;
        ui_.progress = 0;
        ui_.showProgress = false;
        ui_.currentFile.clear();
    }
    pushUiEvent(4, "", 0, false);
    pushUiEvent(1, "", 0, false);
}

void App::startupSelfCheck() {
    // 1) 启动器自检（静默）
    if (runSelfUpdateCheck(true)) return;  // 发现启动器更新，即将退出
    // 2) 运行时资源自检（ui / SelfUpdater / 配置）
    std::string runtimeErr;
    syncRuntime(false, runtimeErr);
    // 3) 背景资源自检（自动更新背景图）
    checkBackgroundUpdate();
    // 4) 游戏更新检查（自动自检）
    if (cfg_.autoCheckOnStart) startCheck();
}

std::string App::readLocalBackgroundVersion() const {
    std::ifstream in(runtimePath("background_version.json"), std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    Json root = Json::parse(ss.str(), &err);
    return root.getString("version");
}

void App::saveLocalBackgroundVersion(const std::string& version) {
    Json root = Json::object();
    root.obj["version"] = Json::string(version);
    std::ofstream out(runtimePath("background_version.json"), std::ios::binary);
    out << root.dump(2);
}

std::string App::runtimePath(const std::string& rel) const {
    return joinPath(joinPath(exeDirectory(), "app"), rel);
}

std::string App::readLocalRuntimeVersion() const {
    std::ifstream in(runtimePath("runtime_version.json"), std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    Json root = Json::parse(ss.str(), &err);
    return root.getString("version");
}

void App::saveLocalRuntimeVersion(const std::string& version) {
    Json root = Json::object();
    root.obj["version"] = Json::string(version);
    std::ofstream out(runtimePath("runtime_version.json"), std::ios::binary);
    out << root.dump(2);
}

void App::pumpNativeMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void App::onPaintNative(HDC hdc, const RECT& /*rc*/) {
    RECT wnd;
    GetClientRect(hwnd_, &wnd);
    FillRect(hdc, &wnd, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    HFONT font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(220, 230, 245));

    RECT textRect = wnd;
    textRect.top = (wnd.bottom - wnd.top) / 2 - 60;
    textRect.bottom = textRect.top + 40;
    DrawTextW(hdc, u8w(bootstrapText_).c_str(), -1, &textRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // 进度条
    int bw = std::min<int>(520, (wnd.right - wnd.left) - 120);
    int bx = (wnd.right - wnd.left - bw) / 2;
    int by = textRect.bottom + 18;
    int bh = 10;
    RECT track = {bx, by, bx + bw, by + bh};
    HBRUSH trackBrush = CreateSolidBrush(RGB(30, 38, 52));
    FillRect(hdc, &track, trackBrush);
    DeleteObject(trackBrush);
    int pct = bootstrapProgress_.load();
    if (pct > 0) {
        RECT fill = {bx, by, bx + bw * pct / 100, by + bh};
        HBRUSH fillBrush = CreateSolidBrush(RGB(77, 163, 255));
        FillRect(hdc, &fill, fillBrush);
        DeleteObject(fillBrush);
    }

    wchar_t pctBuf[32];
    swprintf_s(pctBuf, L"%d%%", pct);
    RECT pctRect = {bx, by + bh + 8, bx + bw, by + bh + 34};
    SetTextColor(hdc, RGB(140, 160, 185));
    DrawTextW(hdc, pctBuf, -1, &pctRect, DT_CENTER | DT_SINGLELINE);

    SelectObject(hdc, old);
    DeleteObject(font);
}

bool App::syncRuntime(bool blocking, std::string& err) {
    if (!blocking) {
        {
            std::lock_guard<std::mutex> lock(ui_.m);
            if (ui_.busy) return true;
            ui_.busy = true;
        }
        opMode_ = 6;
        pushUiEvent(4, "", 0, true, 0, false, 6);
        pushUiEvent(0, "正在更新启动器资源…");
    }
    cancel_.store(false);

    HttpResult resp = HttpClient().get(httpUrlJoin(cfg_.serverUrl, "api/launcher/runtime"), 15);
    if (!resp.ok()) {
        err = resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error;
        if (!blocking) endBusySilent();
        return false;
    }
    std::string jerr;
    Json root = Json::parse(resp.body, &jerr);
    if (root.type != Json::Type::Object) {
        err = "运行时清单解析失败";
        if (!blocking) endBusySilent();
        return false;
    }
    const std::string remoteVersion = root.getString("version");
    const Json& files = root.get("files");
    if (remoteVersion.empty() || files.type != Json::Type::Array || files.arr.empty()) {
        if (!blocking) endBusySilent();
        return true;  // 服务器未发布运行时资源，跳过
    }
    const std::string localVersion = readLocalRuntimeVersion();
    if (!localVersion.empty() && localVersion == remoteVersion) {
        if (!blocking) endBusySilent();
        return true;
    }

    bool downloaded = false;
    createDirectories(runtimePath(""));
    int64_t totalBytes = 0;
    for (const Json& f : files.arr) totalBytes += static_cast<int64_t>(f.getNumber("size"));
    int64_t doneBytes = 0;
    bool failed = false;
    for (const Json& f : files.arr) {
        if (cancel_.load()) { failed = true; break; }
        const std::string rel = f.getString("path");
        if (rel.empty() || rel.find("..") != std::string::npos) continue;
        std::string fileUrl = f.getString("url");
        if (fileUrl.rfind("http", 0) != 0) fileUrl = httpUrlJoin(cfg_.serverUrl, fileUrl);

        bootstrapText_ = "正在下载 " + rel + " …";
        const std::string dest = runtimePath(rel);
        std::string dir = dest;
        auto pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos);
        createDirectories(dir);

        const std::string tmp = dest + ".download";
        const int64_t fileSize = static_cast<int64_t>(f.getNumber("size"));
        bool ok = HttpClient().download(
            fileUrl, tmp, cfg_.speedLimitKBps, 60,
            [&](int64_t done, int64_t) {
                if (blocking) {
                    bootstrapProgress_ = totalBytes > 0
                        ? static_cast<int>((doneBytes + done) * 100 / totalBytes)
                        : 0;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    pumpNativeMessages();
                } else {
                    reportProgress("正在更新启动器资源…", doneBytes + done, totalBytes, rel, false);
                }
            },
            cancel_, err);
        if (!ok) { failed = true; break; }

        const std::string hash = f.getString("hash");
        if (!hash.empty()) {
            std::string md5 = fileMD5(tmp, nullptr);
            if (md5.empty() || _stricmp(md5.c_str(), hash.c_str()) != 0) {
                failed = true;
                break;
            }
        }
        DeleteFileA(dest.c_str());
        MoveFileExA(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        downloaded = true;
        doneBytes += fileSize;
    }

    if (failed) {
        if (!blocking) {
            applyOperationResult(false, cancel_.load() ? "操作已取消" : "启动器资源更新失败");
        }
        return false;
    }

    saveLocalRuntimeVersion(remoteVersion);
    // 服务器提供了配置文件时重新加载；本地根目录已有配置则本地优先
    const std::string appCfg = runtimePath("config\\launcher_config.json");
    const std::string rootCfg = joinPath(exeDirectory(), "launcher_config.json");
    if (GetFileAttributesA(appCfg.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA(rootCfg.c_str()) == INVALID_FILE_ATTRIBUTES) {
        cfg_ = ConfigStore::load(appCfg);
        updater_ = std::make_unique<UpdateManager>(cfg_);
    }
    if (!blocking) {
        applyOperationResult(true, "启动器资源已更新");
        if (downloaded) pushUiEvent(8, "", 0, false);  // 重载页面应用新版前端
    }
    return true;
}

bool App::bootstrapRuntime() {
    // app/ui 已存在则无需初始化
    if (GetFileAttributesA(runtimePath("ui\\index.html").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    bootstrapActive_ = true;
    bootstrapProgress_ = 0;
    bootstrapText_ = "首次运行：正在连接服务器初始化启动器…";
    InvalidateRect(hwnd_, nullptr, TRUE);
    pumpNativeMessages();

    std::string err;
    bool ok = syncRuntime(true, err);
    bootstrapActive_ = false;
    InvalidateRect(hwnd_, nullptr, TRUE);
    pumpNativeMessages();
    if (!ok) {
        MessageBoxW(hwnd_,
                    u8w("首次运行需要从服务器下载界面资源（app/ui）。\n请确认管理服务器已启动且 serverUrl 正确。\n\n" + err).c_str(),
                    L"CloudUpdate 启动器", MB_ICONERROR);
        return false;
    }
    return true;
}

void App::checkBackgroundUpdate() {
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        if (ui_.busy) return;
        ui_.busy = true;
    }
    opMode_ = 5;
    pushUiEvent(4, "", 0, true, 0, false, 5);
    pushUiEvent(0, "正在检查背景资源…");
    cancel_.store(false);

    HttpResult resp = HttpClient().get(httpUrlJoin(cfg_.serverUrl, "api/launcher/background"), 15);
    if (!resp.ok()) {
        endBusySilent();  // 服务器未配置背景时静默跳过
        return;
    }
    std::string jerr;
    Json root = Json::parse(resp.body, &jerr);
    if (root.type != Json::Type::Object) {
        endBusySilent();
        return;
    }
    const std::string remoteVersion = root.getString("version");
    const Json& files = root.get("files");
    if (remoteVersion.empty() || files.type != Json::Type::Array || files.arr.empty()) {
        endBusySilent();
        return;
    }
    std::set<std::string> keepNames;
    for (const Json& f : files.arr) {
        std::string name = f.getString("name");
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (!low.empty()) keepNames.insert(low);
    }
    const std::string frameDir = runtimePath(cfg_.frameDir);

    const std::string localVersion = readLocalBackgroundVersion();
    // 版本不一致即更新（切换背景文件夹后版本可能回退，不能只按“更新”比较）
    if (!localVersion.empty() && remoteVersion == localVersion) {
        cleanupStaleBackgroundFrames(frameDir, keepNames);
        endBusySilent();
        return;
    }

    int64_t totalBytes = 0;
    for (const Json& f : files.arr) {
        totalBytes += static_cast<int64_t>(f.getNumber("size"));
    }
    int64_t doneBytes = 0;
    bool failed = false;
    for (const Json& f : files.arr) {
        if (cancel_.load()) { failed = true; break; }
        std::string name = f.getString("name");
        if (name.empty()) continue;
        std::string fileUrl = f.getString("url");
        if (fileUrl.rfind("http", 0) != 0) {
            fileUrl = httpUrlJoin(cfg_.serverUrl, fileUrl);
        }
        std::string dest = joinPath(frameDir, name);
        std::string dir = dest;
        auto pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos);
        createDirectories(dir);

        const std::string tmp = dest + ".download";
        std::string err;
        const int64_t fileSize = static_cast<int64_t>(f.getNumber("size"));
        bool ok = HttpClient().download(
            fileUrl, tmp, cfg_.speedLimitKBps, 60,
            [&](int64_t done, int64_t) {
                reportProgress("正在更新背景资源…", doneBytes + done, totalBytes, name, false);
            },
            cancel_, err);
        if (!ok) { failed = true; break; }

        const std::string hash = f.getString("hash");
        if (!hash.empty()) {
            std::string md5 = fileMD5(tmp, nullptr);
            if (md5.empty() || _stricmp(md5.c_str(), hash.c_str()) != 0) {
                failed = true;
                break;
            }
        }
        DeleteFileA(dest.c_str());
        MoveFileExA(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        doneBytes += fileSize;
    }

    if (failed) {
        applyOperationResult(false, cancel_.load() ? "操作已取消" : "背景资源更新失败");
        return;
    }
    cleanupStaleBackgroundFrames(frameDir, keepNames);

    double fps = root.getNumber("frameFps", 0);
    if (fps > 0 && cfg_.frameFps != fps) {
        cfg_.frameFps = fps;
        ConfigStore::save(configPath(), cfg_);
    }
    saveLocalBackgroundVersion(remoteVersion);
    loadFrames();
    applyOperationResult(true, "背景资源已更新");
    pushUiEvent(6, "", 0, false);  // 回 UI 线程重新映射并刷新前端
}

/* ---------- 设置与目录选择 ---------- */

void App::saveSettings(const Json& root) {
    cfg_.gamePath = root.getString("gamePath", cfg_.gamePath);
    cfg_.serverUrl = root.getString("serverUrl", cfg_.serverUrl);
    cfg_.speedLimitKBps = static_cast<long>(root.getNumber("speedLimitKBps", cfg_.speedLimitKBps));
    cfg_.frameDir = root.getString("frameDir", cfg_.frameDir);
    cfg_.frameFps = root.getNumber("frameFps", cfg_.frameFps);
    if (cfg_.frameFps <= 0) cfg_.frameFps = 12;
    cfg_.autoCheckOnStart = root.getBool("autoCheckOnStart", cfg_.autoCheckOnStart);
    cfg_.autoRepairOnStart = root.getBool("autoRepairOnStart", cfg_.autoRepairOnStart);

    ConfigStore::save(configPath(), cfg_);
    updater_ = std::make_unique<UpdateManager>(cfg_);
    loadFrames();
    setVirtualHostMappings();

    Json ack = Json::object();
    ack.obj["type"] = Json::string("settings_saved");
    sendJson(ack.dump(0));
    sendInit();
}

void App::browseGamePath() {
    std::string path = browseForFolder(L"选择游戏安装目录");
    if (!path.empty()) {
        pushUiEvent(5, path);
    }
}

std::string App::browseForFolder(const wchar_t* title) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::string result;
    if (pidl) {
        wchar_t path[MAX_PATH] = {0};
        if (SHGetPathFromIDListW(pidl, path)) {
            result = wideToUtf8(path);
        }
        CoTaskMemFree(pidl);
    }
    return result;
}

bool App::gameInstalled() const {
    if (!updater_) return false;
    if (GetFileAttributesA(updater_->gameExePath().c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    return !updater_->localVersion().empty();
}

void App::startDownloadGame() {
    // 已设置过游戏目录时直接使用，不再重复选择
    if (cfg_.gamePath.empty()) {
        std::string path = browseForFolder(L"选择游戏安装目录（下载位置）");
        if (path.empty()) return;  // 用户取消
        cfg_.gamePath = path;
        ConfigStore::save(configPath(), cfg_);
        updater_ = std::make_unique<UpdateManager>(cfg_);
    }
    sendInit();

    bool hasPending = false;
    bool busy = false;
    {
        std::lock_guard<std::mutex> lock(ui_.m);
        hasPending = !ui_.pending.empty();
        busy = ui_.busy;
    }
    if (hasPending) {
        startApplyAll();
        return;
    }
    // 检查可能还没跑完：等检查完成后自动开始下载
    applyAfterCheck_.store(true);
    if (!busy) {
        startCheck();
    } else {
        pushUiEvent(0, "正在等待更新检查完成…");
    }
}

std::string App::configPath() const {
    return joinPath(exeDirectory(), "launcher_config.json");
}
