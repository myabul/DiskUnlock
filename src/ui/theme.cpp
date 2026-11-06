#include "theme.h"

namespace ui {

Dpi& Dpi::instance() {
    static Dpi d;
    return d;
}

void Dpi::applyDpi(UINT dpi) { instance().percent_ = MulDiv(dpi, 100, 96); }

void Dpi::setSystem() {
    // Called before the window exists, so the very first CreateWindowExW size is already
    // authored for this monitor instead of 96 DPI.
    UINT dpi = 96;
#if (_WIN32_WINNT >= 0x0A00)
    const UINT s = GetDpiForSystem();
    if (s) dpi = s;
#endif
    if (dpi == 96) {
        HDC dc = GetDC(nullptr);
        const int lf = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (lf) dpi = (UINT)lf;
    }
    applyDpi(dpi);
}

void Dpi::setWindow(HWND hwnd) {
    UINT dpi = 96;
#if defined(GET_DPI_FOR_WINDOW) || (_WIN32_WINNT >= 0x0A00)
    const UINT w = GetDpiForWindow(hwnd);
    if (w) dpi = w;
#endif
    if (dpi == 96) {
        HDC dc = GetDC(hwnd);
        const int lf = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd, dc);
        if (lf) dpi = (UINT)lf;
    }
    applyDpi(dpi);
}

GdiplusSession::GdiplusSession() {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok) token_ = token;
}

GdiplusSession::~GdiplusSession() {
    if (token_) Gdiplus::GdiplusShutdown(token_);
}

HFONT MakeFont(HWND reference, const wchar_t* face, int baseHeightPx, int weight) {
    const int h = -Dpi::current().scale(baseHeightPx);
    HDC dc = GetDC(reference);
    HFONT f = CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, face);
    ReleaseDC(reference, dc);
    return f;
}

void FillRectColor(HDC hdc, const RECT& rc, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(hdc, &rc, b);
    DeleteObject(b);
}

// FrameRect, never Rectangle: Rectangle fills the interior with whatever brush the DC still
// has selected, which silently repaints the area the caller just drew.
void FrameRectColor(HDC hdc, const RECT& rc, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(edge);
    FrameRect(hdc, &rc, b);
    DeleteObject(b);
}

void HLine(HDC hdc, int x1, int x2, int y, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y, nullptr);
    LineTo(hdc, x2, y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

void VLine(HDC hdc, int x, int y1, int y2, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, x, y1, nullptr);
    LineTo(hdc, x, y2);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

bool BeginBufferedPaint(HWND hwnd, HDC target, RECT& rc, HDC& memDcOut, HBITMAP& bmpOut,
                        HBITMAP& oldOut) {
    GetClientRect(hwnd, &rc);
    memDcOut = CreateCompatibleDC(target);
    if (!memDcOut) return false;
    bmpOut = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    if (!bmpOut) {
        DeleteDC(memDcOut);
        memDcOut = nullptr;
        return false;
    }
    oldOut = (HBITMAP)SelectObject(memDcOut, bmpOut);
    return true;
}

void EndBufferedPaint(HDC target, const RECT& rc, HDC memDc, HBITMAP bmp, HBITMAP oldBmp) {
    // The copy has to happen while the painted bitmap is still selected; restoring first
    // silently blits the original empty surface and leaves the window unpainted.
    BitBlt(target, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDc);
}

void ThemeFlat(HWND hwnd) { SetWindowTheme(hwnd, L"Explorer", nullptr); }

HWND MakeListView(HWND parent, HINSTANCE inst, int id, DWORD extraStyle) {
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS |
                                  extraStyle,
                              0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                           LVS_EX_DOUBLEBUFFER);
    // The Explorer visual style is what turns the raised 9x gradient header into the flat
    // light one the rest of the window is drawn with.
    ThemeFlat(lv);
    return lv;
}

void AddListColumn(HWND lv, int index, const wchar_t* title, int baseWidth) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.iSubItem = index;
    col.pszText = (LPWSTR)title;
    col.cx = Dpi::current().scale(baseWidth);
    ListView_InsertColumn(lv, index, &col);
}

}  // namespace ui
