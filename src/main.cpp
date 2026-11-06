#include "pch.h"
#include "core/device.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/str.h"
#include "ui/main_window.h"
#include "ui/theme.h"

#include <commctrl.h>

namespace {

// The embedded manifest asks for elevation. Getting here without admin means it was stripped
// or the user declined, so say what still works instead of failing silently later.
bool prepareProcess(HINSTANCE inst) {
    if (core::processIsAdmin()) return true;
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(inst, path, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_NORMAL;
    if (ShellExecuteExW(&sei)) return false;  // a new elevated instance is starting
    MessageBoxW(nullptr,
                L"Bu arac fiziksel disklere yazmak icin yonetici hakki ister. Yonetici olmadan "
                L"yalnizca okuma ve raporlama calisir; silme ve donanim komutlari reddedilir.",
                L"Yonetici hakki gerekiyor", MB_ICONWARNING | MB_OK);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    // Both are primary in the manifest; these calls only cover a build that lost the resource
    // and harmlessly fail when the manifest already applied.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!prepareProcess(inst)) return 0;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    ui::GdiplusSession gdiplus;
    if (!gdiplus.ok()) core::LogWarn(L"[ui] GDI+ baslatilamadi");

    core::LogInfo(L"[baslangic] DiskUnlock Studio | yonetici: " +
                  std::wstring(core::processIsAdmin() ? L"evet" : L"HAYIR - salt okunur mod"));

    HWND hwnd = ui::CreateMainWindow(inst, showCmd);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Ana pencere olusturulamadi.", L"DiskUnlock", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Re-read each pass: the window destroys the table on WM_DESTROY, so a cached handle
        // would dangle during the final messages.
        const HACCEL accel = ui::MainWindowAccel();
        // Accelerators run before the dialog manager so F5 / Ctrl+shortcuts win even while an
        // edit control holds the caret.
        if (!(accel && TranslateAcceleratorW(hwnd, accel, &msg))) {
            if (!IsDialogMessageW(hwnd, &msg)) {  // tab/enter navigation without a dialog class
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    return (int)msg.wParam;
}
