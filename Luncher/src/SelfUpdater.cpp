// SelfUpdater.exe：等待启动器退出后替换 exe 并可选择重新启动
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) {
        if (argv) LocalFree(argv);
        return 1;
    }
    const wchar_t* currentExe = argv[1];
    const wchar_t* newExe = argv[2];
    bool relaunch = argc >= 4 && _wcsicmp(argv[3], L"relaunch") == 0;

    // 等待旧进程退出（最多 15 秒）
    for (int i = 0; i < 150; ++i) {
        HANDLE h = CreateFileW(currentExe, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            Sleep(100);
        } else {
            break;
        }
    }
    Sleep(300);

    bool replaced = false;
    for (int i = 0; i < 40; ++i) {
        if (MoveFileExW(newExe, currentExe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            replaced = true;
            break;
        }
        Sleep(500);
    }

    if (relaunch && replaced) {
        ShellExecuteW(nullptr, L"open", currentExe, nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (argv) LocalFree(argv);
    return replaced ? 0 : 2;
}
