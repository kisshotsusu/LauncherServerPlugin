#include "App.h"

#include <windows.h>

namespace {

// 单实例互斥量（全局命名空间，确保整台机器同时只有一个启动器实例）
constexpr const wchar_t* kSingleInstanceMutexName =
    L"Global\\CloudUpdate.Launcher.SingleInstance.{9B2E4F1A-7C3D-4A8E-9F2B-1D6E5C8A0B4F}";

// 已存在实例时，定位并把旧窗口还原到前台
HWND FindExistingLauncherWindow() {
    return FindWindowW(L"CloudLauncherWebViewWnd", L"CloudUpdate 启动器");
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // ---- 单实例检查：防止重复启动多个启动器 ----
    // 创建具名互斥量；若已存在，说明启动器已在运行。
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在运行：直接把已存在的窗口还原并置顶，然后退出（不再弹窗）
        CloseHandle(hMutex);
        HWND hwndExisting = FindExistingLauncherWindow();
        if (hwndExisting) {
            if (IsIconic(hwndExisting)) ShowWindow(hwndExisting, SW_RESTORE);
            SetForegroundWindow(hwndExisting);
        }
        return 0;
    }
    // 本进程持有互斥量；保持句柄打开直至进程退出时由系统自动释放（见下方 return 前显式关闭）。

    // 高 DPI 感知，保证 WebView2 与自绘 UI 清晰
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    App app;
    if (!app.init(hInstance)) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    int rc = app.run();
    if (hMutex) CloseHandle(hMutex);
    return rc;
}
