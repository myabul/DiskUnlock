#pragma once
#include "../pch.h"
#include <gdiplus.h>
#include <uxtheme.h>

namespace ui {

// Layout is authored in 96-DPI units and multiplied by the window's scale, so a 4K laptop does
// not get a 300x200 window with unreadable text.
class Dpi {
public:
    static void setWindow(HWND hwnd);
    // System-wide DPI, readable before any window exists (GetDpiForWindow needs one).
    static void setSystem();
    int scale(int base) const { return MulDiv(base, percent_, 100); }
    UINT percent() const { return percent_; }
    static const Dpi& current() { return instance(); }

private:
    static Dpi& instance();
    static void applyDpi(UINT dpi);
    UINT percent_ = 100;
};

// Light "Fluent" palette. There is deliberately no brand accent colour: grey carries the
// structure and the only saturated colours left are the four status meanings, so a red cell
// always means a real problem instead of being decoration.
namespace color {
constexpr COLORREF window = RGB(250, 250, 250);      // client background
constexpr COLORREF headerBand = RGB(240, 240, 240);  // title strip
constexpr COLORREF card = RGB(255, 255, 255);
constexpr COLORREF rowAlt = RGB(249, 250, 251);
constexpr COLORREF cardHead = RGB(247, 247, 247);
constexpr COLORREF border = RGB(227, 227, 227);
constexpr COLORREF borderSoft = RGB(238, 238, 238);
constexpr COLORREF text = RGB(26, 26, 26);
constexpr COLORREF textMuted = RGB(106, 106, 106);
constexpr COLORREF textFaint = RGB(160, 160, 160);
constexpr COLORREF ink = RGB(43, 43, 43);            // the one "filled" control colour
constexpr COLORREF selRow = RGB(236, 236, 236);
constexpr COLORREF noteBg = RGB(247, 247, 247);
constexpr COLORREF noteEdge = RGB(201, 201, 201);
constexpr COLORREF ok = RGB(15, 123, 15);
constexpr COLORREF okBg = RGB(231, 244, 231);
constexpr COLORREF warn = RGB(138, 90, 0);
constexpr COLORREF warnBg = RGB(251, 242, 222);
constexpr COLORREF warnEdge = RGB(184, 134, 11);
constexpr COLORREF fail = RGB(196, 43, 28);
constexpr COLORREF failBg = RGB(251, 234, 232);
constexpr COLORREF failEdge = RGB(234, 184, 178);
constexpr COLORREF logText = RGB(51, 51, 51);
}  // namespace color

// GDI+ has to outlive every window that paints with it.
class GdiplusSession {
public:
    GdiplusSession();
    ~GdiplusSession();
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
    bool ok() const { return token_ != 0; }

private:
    ULONG_PTR token_ = 0;
};

HFONT MakeFont(HWND reference, const wchar_t* face, int baseHeightPx, int weight);

void FillRectColor(HDC hdc, const RECT& rc, COLORREF fill);
void FrameRectColor(HDC hdc, const RECT& rc, COLORREF edge);
void HLine(HDC hdc, int x1, int x2, int y, COLORREF c);
void VLine(HDC hdc, int x, int y1, int y2, COLORREF c);

// Double-buffered WM_PAINT helper: the caller draws into a memory DC and this blits it.
bool BeginBufferedPaint(HWND hwnd, HDC target, RECT& rc, HDC& memDcOut, HBITMAP& bmpOut,
                        HBITMAP& oldOut);
void EndBufferedPaint(HDC target, const RECT& rc, HDC memDc, HBITMAP bmp, HBITMAP oldBmp);

// The "Explorer" visual style replaces the 3D sunken edges of a plain Win32 control with the
// flat 1px borders the light theme assumes. Cheaper than owner-drawing every edit.
void ThemeFlat(HWND hwnd);

// Standard report-mode list view, flat themed header, no dark-colour overrides.
HWND MakeListView(HWND parent, HINSTANCE inst, int id, DWORD extraStyle = 0);
void AddListColumn(HWND lv, int index, const wchar_t* title, int baseWidth);

}  // namespace ui
