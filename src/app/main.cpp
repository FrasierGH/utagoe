// 歌声りっぷ (Utagoe Rip) 3.0 — open-source rebuild of TODAKEN's vocal extractor.
//
//   utagoe.exe [--lang ja|en] [--ini path]
#include <windows.h>
#include <shellapi.h>

#include "app.hpp"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    ui::hinst = inst;
    ui::init();
    std::wstring ini, language;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--ini")) ini = argv[++i];
        else if (!wcscmp(argv[i], L"--lang")) language = argv[++i];
    }
    if (argv) LocalFree(argv);

    utagoe::MainForm main(ini, language);
    if (!main.create_window()) return 1;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) ui::message_loop_step(m);
    return (int)m.wParam;
}
