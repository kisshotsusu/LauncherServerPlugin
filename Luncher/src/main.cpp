#include "App.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // 高 DPI 感知，保证 WebView2 与自绘 UI 清晰
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    App app;
    if (!app.init(hInstance)) return 1;
    return app.run();
}
