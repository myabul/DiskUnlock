#include "main_window.h"
#include "theme.h"
#include "../app/state.h"
#include "../app/workers.h"
#include "../core/device.h"
#include "../core/log.h"
#include "../core/jobs.h"
#include "../core/str.h"

#include <algorithm>
#include <vector>

namespace ui {

namespace workers = app::workers;

namespace {

enum Ids : int {
    ID_BTN_REFRESH = 100, ID_BTN_LBA0, ID_BTN_REPORT, ID_BTN_CLEARLOG,
    ID_LV_DISKS = 110, ID_LV_DETAIL, ID_LV_SMART,
    // The page order is the order an operator works in: look, then health detail, then the
    // drive's own logs, then measurement, then layout, then security, then destruction.
    ID_TAB_GENEL = 120, ID_TAB_SMART, ID_TAB_LOGS, ID_TAB_SURFACE, ID_TAB_PARTS, ID_TAB_SEC,
    ID_TAB_ERASE,
    ID_LV_PARTS = 132, ID_BTN_PROBE_LAYOUT, ID_BTN_RO_ON, ID_BTN_RO_OFF, ID_BTN_OFF_ON,
    ID_BTN_OFF_OFF, ID_LBL_PARTS,
    ID_EDIT_PASS = 140, ID_CHK_MASTER, ID_BTN_SET_PASS, ID_BTN_UNLOCK, ID_BTN_DISABLE,
    ID_BTN_FREEZE, ID_LBL_ATA, ID_LBL_PASS,
    ID_EDIT_PSID = 150, ID_BTN_TCG_DISCO, ID_BTN_PSID, ID_LBL_TCG, ID_LBL_PSID,
    ID_COMBO_PROFILE = 160, ID_CHK_ENHANCED, ID_BTN_WIPE_TABLES, ID_BTN_OVERWRITE,
    ID_BTN_PURGE, ID_BTN_CANCEL, ID_PBAR_WIPE, ID_LBL_WIPE, ID_LBL_PROFILE,
    ID_EDIT_CONFIRM = 168, ID_LBL_CONFIRM_REQ, ID_LBL_CONFIRM_INFO, ID_BTN_CONFIRM_YES,
    ID_BTN_CONFIRM_NO,
    ID_PBAR_SCAN = 180, ID_LBL_SCAN, ID_BTN_DEEP, ID_BTN_SCAN, ID_EDIT_LOG,
    ID_LV_SMARTLOG = 190, ID_LBL_SMART, ID_LBL_SMARTLOG, ID_LBL_SMARTINFO,
    ID_BTN_SMART_LOGS, ID_BTN_SMART_ON, ID_BTN_SMART_OFF, ID_BTN_AUTOSAVE_ON,
    ID_BTN_AUTOSAVE_OFF, ID_BTN_DST_SHORT, ID_BTN_DST_LONG, ID_BTN_DST_ABORT,
    ID_BTN_SMART_CLEARLOG, ID_BTN_BASELINE_TAKE, ID_BTN_BASELINE_CLEAR,
    ID_LV_SURFACE = 206, ID_LBL_GENEL, ID_LBL_LOGINFO,
    ID_TIMER = 1,
    ID_TIMER_DEVICE = 2,
};

constexpr UINT kTimerMs = 200;
constexpr int kTabCount = 7;

// Geometry is authored once per resize in LayoutMain and stored here, so WM_PAINT draws the
// very same card frames the controls were placed inside. Two independent geometry
// calculations is how a layout ends up with borders that do not touch their contents.
struct Card {
    RECT rc{};
    const wchar_t* title = L"";
};

struct FontSlot {
    HWND h = nullptr;
    int kind = 0;  // 0 ui, 1 bold, 2 label, 3 mono, 4 gauge
};

// One tile of the summary strip. LayoutMain only writes rc; UpdateTiles writes the text, so a
// resize never repaints stale numbers and a refresh never moves the boxes.
struct Tile {
    RECT rc{};
    std::wstring label, value, sub;
    int pct = -1;  // -1 draws no bar, which is what "unknown" looks like
};

// One segment of the status strip. The strip is painted instead of being a real status bar
// because the panes carry per-segment colours (locked / frozen / blocked) that a common
// control status bar cannot express without owner-draw anyway.
struct Pane {
    std::wstring text;
    COLORREF fg = color::textMuted;
    int dot = -1;  // -1 = no marker, otherwise color::ok/warn/fail
};

// Every control is a direct child of the main window. Cards are painted, not created: a real
// STATIC panel parent swallows WM_DRAWITEM and WM_CTLCOLOR* from its children, which is what
// made the earlier panel buttons render as blank rectangles.
struct Ctrls {
    HWND main = nullptr;
    HWND disks = nullptr, detail = nullptr, smart = nullptr, parts = nullptr, surface = nullptr;
    HWND tabBtn[kTabCount] = {};
    std::vector<HWND> tabKids[kTabCount];
    HWND pass = nullptr, master = nullptr, ataInfo = nullptr, lblPass = nullptr;
    HWND psid = nullptr, tcgInfo = nullptr, lblPsid = nullptr;
    HWND profile = nullptr, enhanced = nullptr, lblProfile = nullptr;
    HWND wipeBar = nullptr, wipeInfo = nullptr;
    HWND btnWipeTables = nullptr, btnOverwrite = nullptr, btnPurge = nullptr, btnCancel = nullptr;
    HWND btnDeep = nullptr, btnScan = nullptr, scanBar = nullptr, scanInfo = nullptr;
    // SMART pages: attribute grid, the device's own error/self-test log grid, and the notes
    // that carry the reason when a read came back empty.
    HWND smartLog = nullptr, smartInfo = nullptr, lblSmart = nullptr, lblSmartLog = nullptr;
    HWND noteGenel = nullptr, noteLogs = nullptr;
    HWND btnSmartLogs = nullptr, btnSmartOn = nullptr, btnSmartOff = nullptr;
    HWND btnAutosaveOn = nullptr, btnAutosaveOff = nullptr;
    HWND btnDstShort = nullptr, btnDstLong = nullptr, btnDstAbort = nullptr;
    HWND btnSmartClearLog = nullptr, btnBaselineTake = nullptr, btnBaselineClear = nullptr;
    HWND btnProbeLayout = nullptr, btnRoOn = nullptr, btnRoOff = nullptr, btnOffOn = nullptr,
         btnOffOff = nullptr, partsInfo = nullptr;
    HWND btnSetPass = nullptr, btnUnlock = nullptr, btnDisable = nullptr, btnFreeze = nullptr;
    HWND btnTcgDisco = nullptr, btnPsid = nullptr;
    HWND confirmEdit = nullptr, confirmReq = nullptr, confirmInfo = nullptr, confirmYes = nullptr,
         confirmNo = nullptr;
    HWND btnRefresh = nullptr, btnLba0 = nullptr, btnReport = nullptr, btnClearLog = nullptr;
    HWND log = nullptr;
    HFONT fontUi = nullptr, fontBold = nullptr, fontLabel = nullptr, fontMono = nullptr,
          fontGauge = nullptr;
    std::vector<FontSlot> fonts;
    std::vector<Card> cards;
    std::vector<Tile> tiles;
    // Where LayoutMain wants the five tiles; UpdateTiles copies them in when it rebuilds the
    // text, so whichever of the two runs last still lands the tiles on the right rectangles.
    std::vector<RECT> tileSlots;
    RECT band{};
    HBRUSH brushWindow = nullptr, brushCard = nullptr, brushWarn = nullptr;
    int activeTab = 0;
    bool wasBusy = false;
    // Timer ticks, for the jobs that only need a table rebuilt a few times a second.
    int tick = 0;
    uint64_t lastRevision = 0;
    std::function<void()> pendingAction;

    // Status strip geometry + the last rendered signature, so a repaint only happens when the
    // content actually changed instead of every 200 ms.
    RECT statusRc{};
    std::vector<Pane> panes;
    std::wstring rightPane;
    std::wstring paneSignature;
    SYSTEMTIME lastScan{};
    bool lastScanValid = false;

    // Bit 0 = disk grid, 1 = detail grid, 2 = SMART grid, 3 = partition grid, 4 = SMART log
    // grid, 5 = surface grid: set once the user drags a divider, after which only the filler
    // column keeps tracking the window width.
    int gridManual = 0;
    // Per-row colour for the Durum column, rebuilt by fillDiskList.
    std::vector<COLORREF> diskStatusColor;
    RECT tabRule{};
    RECT logCard{};
};
Ctrls c;

HACCEL g_accel = nullptr;
HDEVNOTIFY g_diskNotifier = nullptr;

// {53f56307-b6be-11d0-94f2-00a0c91efbfc} - the disk device interface class. Spelled as a
// literal because pulling initguid.h into a header-heavy translation unit would instantiate
// every GUID in the SDK.
const GUID kGuidDevInterfaceDisk = {0x53f56307, 0xb6be, 0x11d0,
                                    {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0xc6}};

int S(int base) { return Dpi::current().scale(base); }

void LayoutMain();
LRESULT OnNotify(HWND hwnd, NMHDR* nh);
LRESULT OnCustomDraw(NMHDR* nh);
void fillDiskList();
void fillDetail();
void fillSmartTable();
void fillSmartLogs();
void fillSurfaceStats();
void UpdateTiles();
void fillParts();
void refreshWipeLine();
void showTab(int idx);
void cancelDestructive();
void AutoFitGrid(HWND lv, int manualBit, int fillerCol);
void BuildStatusPanes();
void InvalidateStatusStrip();

HFONT fontFor(int kind) {
    switch (kind) {
    case 1: return c.fontBold;
    case 2: return c.fontLabel;
    case 3: return c.fontMono;
    case 4: return c.fontGauge;
    default: return c.fontUi;
    }
}

void TrackFont(HWND h, int kind) {
    if (h) c.fonts.push_back(FontSlot{h, kind});
}

void ApplyFonts() {
    for (const FontSlot& f : c.fonts)
        SendMessageW(f.h, WM_SETFONT, (WPARAM)fontFor(f.kind), TRUE);
}

HWND MkBtn(HWND parent, int id, const wchar_t* text, HINSTANCE inst) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
}

HWND MkEdit(HWND parent, int id, HINSTANCE inst, bool password = false) {
    // WS_EX_CLIENTEDGE + the Explorer theme is the only combination that yields a flat 1px
    // border; without it a white edit on a white card simply disappears.
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                 (password ? ES_PASSWORD : 0),
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
    ThemeFlat(h);
    return h;
}

HWND MkCheck(HWND parent, int id, const wchar_t* text, HINSTANCE inst) {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, inst, nullptr);
    ThemeFlat(h);
    return h;
}

HWND MkLabel(HWND parent, int id, const wchar_t* text, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10,
                           10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
}

// The status/note strip is owner-draw so it can carry its own background and left rule; a
// plain STATIC can paint neither without a WM_CTLCOLOR special case per surface.
HWND MkNote(HWND parent, int id, const wchar_t* text, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_OWNERDRAW, 0, 0, 10, 10,
                           parent, (HMENU)(INT_PTR)id, inst, nullptr);
}

HWND MkBar(HWND parent, int id, HINSTANCE inst) {
    HWND h = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10,
                             10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
    ThemeFlat(h);
    SendMessageW(h, PBM_SETRANGE32, 0, 100);
    // Neutral grey chunk: green would read as "healthy", which is not what a progress bar means.
    SendMessageW(h, PBM_SETBARCOLOR, 0, color::ink);
    return h;
}

// Registering a control with its tab is what makes switching cheap: one ShowWindow per
// control of the outgoing/incoming page, no per-widget bookkeeping anywhere else.
HWND forTab(int t, HWND h) {
    if (h) {
        if (t >= 0) c.tabKids[t].push_back(h);
        if (t != c.activeTab && t >= 0) ShowWindow(h, SW_HIDE);
    }
    return h;
}

std::wstring EditText(HWND h) {
    if (!h) return L"";
    const int n = GetWindowTextLength(h);
    std::wstring s((size_t)n + 1, L'\0');
    const int got = GetWindowTextW(h, s.data(), n + 1);
    s.resize(got > 0 ? (size_t)got : 0);
    return s;
}

void SetText(HWND h, const std::wstring& s) {
    if (h) SetWindowTextW(h, s.c_str());
}

bool checked(HWND h) { return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// Drives report their identity as fixed-width ASCII fields and the padding is frequently
// garbage, so a control byte reaches the window and shows up as a missing-glyph box. The data
// stays visible - unprintable positions become '.' - instead of being silently dropped.
std::wstring Printable(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch == 0) break;
        // ASCII printables stay; above 0xA0 only real letters/digits survive. Everything else
        // that leaks out of descriptor padding is a control, private-use or unassigned code
        // point, and the font renders those as an empty box.
        const bool keep = (ch >= 0x20 && ch <= 0x7E) || (ch >= 0xA0 && IsCharAlphaNumericW(ch));
        out.push_back(keep ? ch : L'.');
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out;
}

void AddRow(HWND lv, int idx, const wchar_t* field, const std::wstring& value) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = idx;
    it.pszText = (LPWSTR)field;
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, idx, 1, (LPWSTR)value.c_str());
}

// Register and opcode values only mean anything in hex, and a decimal 242 where the manual
// says F2h is how a log line stops being useful.
std::wstring hex8(uint8_t v) {
    wchar_t b[8];
    swprintf(b, 8, L"0x%02X", (unsigned)v);
    return b;
}

int TextW(HDC dc, const wchar_t* s) {
    SIZE sz{};
    if (!dc || !s || !*s) return 0;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

void AutoFitGrid(HWND lv, int manualBit, int fillerCol) {
    if (!lv) return;
    const int colCount = Header_GetItemCount(ListView_GetHeader(lv));
    if (colCount <= 0) return;
    const bool manual = (c.gridManual & manualBit) != 0;
    const int minColW = S(46), maxColW = S(430);

    RECT client{};
    GetClientRect(lv, &client);
    if (HWND hdr = ListView_GetHeader(lv)) {
        RECT hr{};
        GetWindowRect(hdr, &hr);
        client.top += (hr.bottom - hr.top) + 2;  // the header is not usable width-free space
    }
    int avail = client.right - client.left;
    // The vertical scrollbar steals width as soon as the rows overflow, and ignoring it is
    // what makes a horizontal scrollbar appear next to a table that fits perfectly.
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    if (GetScrollInfo(lv, SB_VERT, &si) && (si.nMax + 1) > (int)si.nPage)
        avail -= GetSystemMetrics(SM_CXVSCROLL);

    std::vector<int> w((size_t)colCount, 0);
    HDC dc = GetDC(lv);
    HFONT gridFont = (HFONT)SendMessageW(lv, WM_GETFONT, 0, 0);
    HFONT of = gridFont ? (HFONT)SelectObject(dc, gridFont) : nullptr;

    if (!manual) {
        wchar_t buf[512];
        for (int col = 0; col < colCount; ++col) {
            // Header text sets the floor: a column narrower than its own title is never right.
            LVCOLUMNW hc{};
            hc.mask = LVCF_TEXT;
            hc.pszText = buf;
            hc.cchTextMax = 512;
            int best = S(16);
            if (ListView_GetColumn(lv, col, &hc)) best = TextW(dc, buf) + S(20);
            const int rows = ListView_GetItemCount(lv);
            for (int row = 0; row < rows; ++row) {
                buf[0] = 0;
                ListView_GetItemText(lv, row, col, buf, 512);
                if (buf[0]) best = (std::max)(best, TextW(dc, buf) + S(18));
            }
            w[(size_t)col] = (std::max)(minColW, (std::min)(best, maxColW));
        }
    } else {
        for (int col = 0; col < colCount; ++col) w[(size_t)col] = ListView_GetColumnWidth(lv, col);
    }
    if (of) SelectObject(dc, of);
    ReleaseDC(lv, dc);

    // Fitting is two-directional: leftover width goes to the filler column, but when the
    // measured content is wider than the grid the columns shrink back proportionally to their
    // own slack. Only handing out the surplus is what let a long "Rol" value crush Model down
    // to its minimum instead of trimming every column a little.
    int total = 0;
    for (int col = 0; col < colCount; ++col) total += w[(size_t)col];
    if (!manual && total > avail) {
        int shrinkable = 0;
        for (int col = 0; col < colCount; ++col)
            shrinkable += (std::max)(0, w[(size_t)col] - minColW);
        const int cut = (std::min)(total - avail, shrinkable);
        if (cut > 0 && shrinkable > 0) {
            int applied = 0;
            for (int col = 0; col < colCount && applied < cut; ++col) {
                const int room = (std::max)(0, w[(size_t)col] - minColW);
                const int share = (int)((long long)cut * room / shrinkable);
                w[(size_t)col] -= share;
                applied += share;
            }
            // Integer division leaves a residue - and three leftover pixels are exactly what
            // puts a horizontal scrollbar under a table whose columns all fit. Hand the residue
            // out one pixel at a time instead of accepting an overflow that measures as nothing.
            for (int pass = 0; applied < cut && pass < colCount; ++pass) {
                for (int col = 0; col < colCount && applied < cut; ++col) {
                    if (w[(size_t)col] <= minColW) continue;
                    w[(size_t)col]--;
                    applied++;
                }
            }
        }
    } else if (fillerCol >= 0 && fillerCol < colCount) {
        w[(size_t)fillerCol] += avail - total - S(2);
    }
    for (int col = 0; col < colCount; ++col)
        w[(size_t)col] = (std::max)(minColW, w[(size_t)col]);
    for (int col = 0; col < colCount; ++col)
        if (ListView_GetColumnWidth(lv, col) != w[(size_t)col])
            ListView_SetColumnWidth(lv, col, w[(size_t)col]);
}
void fillDiskList() {
    if (!c.disks) return;
    const int keep = ListView_GetNextItem(c.disks, -1, LVNI_SELECTED);
    const auto disks = app::snapshotDisks();
    ListView_DeleteAllItems(c.disks);
    c.diskStatusColor.clear();
    for (size_t i = 0; i < disks.size(); ++i) {
        const auto& d = disks[i];
        wchar_t num[24];
        swprintf(num, 24, L"%d", d.number);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = num;
        ListView_InsertItem(c.disks, &it);
        ListView_SetItemText(c.disks, (int)i, 1, (LPWSTR)Printable(d.model).c_str());
        ListView_SetItemText(c.disks, (int)i, 2, (LPWSTR)d.sizeText().c_str());
        // The list is a picker, not the report: the same facts in their shortest honest form,
        // so a long label never takes the width a model name needs. The full words stay on the
        // drive's own pages.
        const wchar_t* role = L"-";
        switch (d.role) {
        case core::DiskRole::SystemReserved: role = L"SISTEM"; break;
        case core::DiskRole::OsVolume:       role = L"OS"; break;
        case core::DiskRole::PageFile:       role = L"PAGEFILE"; break;
        case core::DiskRole::Recovery:       role = L"KURTARMA"; break;
        case core::DiskRole::DataOnly:       role = L"Veri"; break;
        default:                             role = L"Bilinmiyor"; break;
        }
        ListView_SetItemText(c.disks, (int)i, 3, (LPWSTR)role);
        std::wstring flags;
        // The colour states the same fact as the text, so the two must never disagree: red is
        // reserved for a drive that is locked or blocked, amber for frozen / TCG-managed.
        COLORREF col = color::textMuted;
        if (d.ataLocked) {
            flags += L"kilitli ";
            col = color::fail;
        }
        if (d.ataFrozen) {
            flags += L"frozen ";
            if (col == color::textMuted) col = color::warn;
        }
        if (d.tcgProbed && (d.tcg.hasOpal || d.tcg.locked)) {
            flags += L"TCG ";
            col = d.tcg.locked ? color::fail : (col == color::textMuted ? color::warn : col);
        }
        if (!d.blockedBecause.empty()) {
            flags += L"ENGEL";
            col = color::fail;
        }
        if (flags.empty()) {
            flags = d.ataSecuritySupported ? std::wstring(L"acik")
                                           : std::wstring(L"guvenlik yok");
        }
        ListView_SetItemText(c.disks, (int)i, 4, (LPWSTR)flags.c_str());
        // The same number the summary tile shows, so a drive that cannot report health is
        // visibly different in the list from one that reports a clean 100. A "?" is the short
        // form of the tile's "bilinmiyor" - the list is too narrow for the sentence.
        const app::Score hs = d.healthScore();
        const std::wstring healthText = hs.known ? hs.valueText() : std::wstring(L"?");
        ListView_SetItemText(c.disks, (int)i, 5, (LPWSTR)healthText.c_str());
        c.diskStatusColor.push_back(col);
    }
    int sel = app::The().selectedIndex();
    if (sel < 0) sel = keep;
    if (sel >= 0 && sel < (int)disks.size()) {
        ListView_SetItemState(c.disks, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(c.disks, sel, FALSE);
    }
    AutoFitGrid(c.disks, 1, 1);
}

void fillDetail() {
    if (!c.detail) return;
    ListView_DeleteAllItems(c.detail);
    const app::DiskRecord* r = app::selectedDisk();
    if (!r) {
        AddRow(c.detail, 0, L"Durum",
               L"Disk secilmedi - soldaki listeden bir surucu isaretleyin");
        SetText(c.ataInfo, L"ATA guvenlik durumu: disk secilmedi");
        SetText(c.tcgInfo, L"TCG: discovery calistirilmadi");
        refreshWipeLine();
        AutoFitGrid(c.detail, 2, 1);
        // The gauges have something honest to say before a drive is picked, so they are built
        // here too - an empty summary strip reads as a broken panel, not as "nothing chosen".
        UpdateTiles();
        return;
    }
    int i = 0;
    wchar_t path[64];
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", r->number);
    AddRow(c.detail, i++, L"Aygit", path);
    AddRow(c.detail, i++, L"Model", Printable(r->model));
    AddRow(c.detail, i++, L"Seri",
           r->serial.empty() ? std::wstring(L"(bildirilmedi)") : Printable(r->serial));
    AddRow(c.detail, i++, L"Firmware",
           r->firmware.empty() ? std::wstring(L"(bildirilmedi)") : Printable(r->firmware));
    AddRow(c.detail, i++, L"Kimlik kaynagi",
           r->ataIdentifyOk ? L"IDENTIFY DEVICE (surucunun kendisi)"
                            : (r->identityFromDescriptor ? L"STORAGE_DESCRIPTOR / INQUIRY"
                                                         : L"surucu bildirmiyor"));
    // Only when the SAT command was actually refused: this is the receipt for every "yok" the
    // rest of the window shows.
    if (!r->ataIdentifyOk && !r->ataIdentifyFail.empty())
        AddRow(c.detail, i++, L"IDENTIFY ret kaniti", r->ataIdentifyFail);
    AddRow(c.detail, i++, L"Veriyolu", r->busLabel);
    AddRow(c.detail, i++, L"Medya", r->mediaLabel);
    AddRow(c.detail, i++, L"Rol", r->roleLabel);
    AddRow(c.detail, i++, L"Kapasite", r->sizeText());
    AddRow(c.detail, i++, L"Sektor sayisi",
           r->geometryKnown ? core::formatU64(r->totalSectors) : L"bilinmiyor");
    AddRow(c.detail, i++, L"Sektor boyutu", r->sectorText());
    AddRow(c.detail, i++, L"ATA guvenlik", r->securityText());
    AddRow(c.detail, i++, L"Saglik", r->healthText());
    AddRow(c.detail, i++, L"Sicaklik",
           r->temperatureC >= 0
               ? core::formatU64((uint64_t)r->temperatureC) + L" C (" + r->temperatureSource + L")"
               : L"SURUCU SICAKLIK RAPORLAMIYOR");
    AddRow(c.detail, i++, L"Asinma",
           r->wearPercent >= 0
               ? core::formatU64((uint64_t)r->wearPercent) + L"% (" + r->wearSource + L")"
               : L"raporlanmadi");
    AddRow(c.detail, i++, L"Yeniden ayirma",
           r->reallocatedSectors >= 0 ? core::formatU64((uint64_t)r->reallocatedSectors)
                                      : L"bilinmiyor");
    AddRow(c.detail, i++, L"Acik saat",
           r->powerOnHours >= 0
               ? core::formatU64((uint64_t)r->powerOnHours) +
                     L" (SMART id 9 ham degeri; bazi ureticiler dakika sayar)"
               : L"bilinmiyor");
    AddRow(c.detail, i++, L"Donanim purge",
           r->planBuilt ? std::wstring(hw::erase::purgeMethodLabel(r->purge)) : L"planlanmadi");
    AddRow(c.detail, i++, L"Purge notu", r->purgeDetail);
    if (r->ataEraseTimeValid)
        AddRow(c.detail, i++, L"Erase tahmini",
               core::formatU64(r->ataEraseMinutesNormal) + L" dk normal / " +
                   core::formatU64(r->ataEraseMinutesEnhanced) +
                   L" dk enhanced (surucunun kelime 89/90 tahmini)");
    if (!r->blockedBecause.empty()) AddRow(c.detail, i++, L"ENGEL", r->blockedBecause);

    SetText(c.ataInfo, L"ATA guvenlik durumu: " + r->securityText());
    SetText(c.tcgInfo,
            L"TCG: " + (r->tcgProbed ? r->tcg.summary()
                                      : L"daha Level 0 Discovery calistirilmadi"));
    refreshWipeLine();
    AutoFitGrid(c.detail, 2, 1);
    UpdateTiles();
}

// The rectangle LayoutMain reserved for tile `i`, or an empty one before the first layout.
RECT tileSlot(int i) {
    if (i < 0 || i >= (int)c.tileSlots.size()) return RECT{};
    return c.tileSlots[(size_t)i];
}

// The summary tiles. LayoutMain owns the rectangles, this owns the text, and both the timer and
// a resize call it - which is why a tile can never show a value the current record does not
// have. An unknown number stays the word "bilinmiyor" with the reason underneath it.
void UpdateTiles() {
    app::State& st = app::The();
    const app::DiskRecord* r = app::selectedDisk();
    app::SurfaceStats stats;
    double speed = 0.0;
    bool scanLive = false;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        stats = st.surface;
    }
    speed = st.scanSpeedMiBs.load();
    scanLive = st.scanRunning.load();

    c.tiles.clear();
    if (!r) {
        const wchar_t* names[] = {L"SAGLIK", L"PERFORMANS", L"SICAKLIK", L"ACIK SAAT", L"ASINMA"};
        for (int k = 0; k < 5; ++k)
            c.tiles.push_back(Tile{tileSlot(k), names[k], L"-", L"surucu secilmedi", -1});
        SetText(c.noteGenel, L"Ozet: soldan bir surucu secin - hicbir deger uydurulmadi.");
        if (c.main) InvalidateRect(c.main, nullptr, FALSE);
        return;
    }
    const app::Score health = r->healthScore();
    const app::Score perf = r->performanceScore(stats);
    c.tiles.push_back(
        Tile{tileSlot(0), L"SAGLIK", health.valueText(),
             health.known ? health.basis : health.whyUnknown,
             health.known ? health.percent : -1});
    std::wstring perfSub = perf.known ? perf.basis : perf.whyUnknown;
    if (scanLive)
        perfSub = L"tarama suruyor: %" + core::formatU64(st.scanPercent.load()) +
                  L"  |  " + core::formatU64(stats.blocksScanned) + L" blok olculdu";
    c.tiles.push_back(
        Tile{tileSlot(1), L"PERFORMANS", perf.valueText(), perfSub, perf.known ? perf.percent : -1});
    c.tiles.push_back(Tile{
        tileSlot(2), L"SICAKLIK",
        r->temperatureC >= 0 ? core::formatU64((uint64_t)r->temperatureC) + L" C" : L"bilinmiyor",
        r->temperatureC >= 0
            ? r->temperatureSource
            : (r->geometryKnown ? L"surucu sicaklik raporu vermedi (kaynak: " +
                                      (r->smartRead ? std::wstring(L"SMART/id 194-197-231")
                                                    : std::wstring(L"SMART okunmadi")) +
                                      L")"
                                  : L"geometri bilinmiyor"),
        -1});
    c.tiles.push_back(
        Tile{tileSlot(3), L"ACIK SAAT",
             r->powerOnHours >= 0 ? core::formatU64((uint64_t)r->powerOnHours) + L" sa"
                                  : L"bilinmiyor",
             r->powerOnHours >= 0
                 ? L"SMART id 9 ham degeri (bazilari dakika sayar) / NVMe power_on_hours"
                 : L"ne SMART id 9 ne NVMe logu deger verdi",
             -1});
    c.tiles.push_back(Tile{tileSlot(4), L"ASINMA",
                           r->wearPercent >= 0
                               ? core::formatU64((uint64_t)r->wearPercent) + L" %"
                               : L"raporlanmadi",
                           r->wearPercent >= 0 ? r->wearSource
                                               : L"ureticinin asinma alani yok - yuzde uydurulmaz",
                           r->wearPercent >= 0 ? 100 - r->wearPercent : -1});

    std::wstring note = L"Saglik: " + (health.known ? health.basis : health.whyUnknown) +
                        L"  ||  Performans: " + (perf.known ? perf.basis : perf.whyUnknown);
    if (perf.known && speed > 0)
        note += L"  ||  olculen okuma hizi " + [&] {
                    wchar_t b[32];
                    swprintf(b, 32, L"%.1f MB/s", speed);
                    return std::wstring(b);
                }();
    SetText(c.noteGenel, note);
    if (c.main) InvalidateRect(c.main, nullptr, FALSE);
}

// The surface pass, bucketed by the latency the reads actually took. Every row is a counted
// fact from the last scan of this disk; a scan of another disk says so instead of borrowing
// someone else's numbers.
void fillSurfaceStats() {
    if (!c.surface) return;
    ListView_DeleteAllItems(c.surface);
    app::State& st = app::The();
    app::SurfaceStats s;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        s = st.surface;
    }
    const app::DiskRecord* r = app::selectedDisk();
    int i = 0;
    auto row = [&](const wchar_t* k, const std::wstring& v) {
        AddRow(c.surface, i++, k, v);
    };
    if (s.scannedDisk < 0) {
        row(L"Durum", L"yuzey taramasi henuz calistirilmadi - hicbir satir uydurulmadi");
        AutoFitGrid(c.surface, 32, 1);
        return;
    }
    if (r && s.scannedDisk != r->number) {
        row(L"Durum", L"son tarama PhysicalDrive" + core::formatU64((uint64_t)s.scannedDisk) +
                          L" uzerinde yapildi; bu surucu icin olcum yok");
        AutoFitGrid(c.surface, 32, 1);
        return;
    }
    row(L"Taranan blok", core::formatU64(s.blocksScanned) + L" blok");
    row(L"Hizli (<5 ms)", core::formatU64(s.fast));
    row(L"Normal (<20 ms)", core::formatU64(s.normal));
    row(L"Orta (<60 ms)", core::formatU64(s.mid));
    row(L"Yavas (<150 ms)", core::formatU64(s.slow));
    row(L"Hatali / okunamayan", core::formatU64(s.bad));
    row(L"Maks sure", core::formatU64(s.maxLatencyMs) + L" ms");
    wchar_t b[32];
    swprintf(b, 32, L"%.1f MB/s", st.scanSpeedMiBs.load());
    row(L"Anlik hiz", st.scanRunning.load() ? std::wstring(b) : std::wstring(L"tarama yok"));
    const app::Score p = r ? r->performanceScore(s) : app::Score{};
    row(L"Performans yuzdesi", p.valueText());
    if (!p.basis.empty()) row(L"Nasil", p.basis);
    AutoFitGrid(c.surface, 32, 1);
}

void fillSmartTable() {
    if (!c.smart) return;
    ListView_DeleteAllItems(c.smart);
    const app::DiskRecord* r = app::selectedDisk();
    if (!r) {
        SetText(c.smartInfo, L"SMART: soldan bir surucu secin.");
        fillSmartLogs();
        return;
    }
    int i = 0;
    // Drive counters are cumulative and no ATA or NVMe command resets them, so the only change
    // this column can report honestly is the difference against a locally stored snapshot.
    auto diff = [](bool anchored, uint64_t base, uint64_t now) {
        if (!anchored) return std::wstring(L"baseline yok");
        if (now == base) return std::wstring(L"ayni");
        return (now > base ? L"+" : L"-") + core::formatU64(now > base ? now - base : base - now);
    };
    auto deltaOf = [&](uint8_t id, uint64_t raw) {
        uint64_t base = 0;
        if (!r->baseline.valid) return std::wstring(L"baseline yok");
        if (!r->baseline.ataRawOf(id, base)) return std::wstring(L"id kaydedilmemisti");
        return diff(true, base, raw);
    };
    if (r->smartRead) {
        for (const auto& a : r->smart.attributes) {
            wchar_t id[16], val[24], worst[24], raw[32];
            swprintf(id, 16, L"%u", (unsigned)a.id);
            swprintf(val, 24, L"%u", (unsigned)a.value);
            swprintf(worst, 24, L"%u", (unsigned)a.worst);
            swprintf(raw, 32, L"0x%012llX", (unsigned long long)a.raw);
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = id;
            ListView_InsertItem(c.smart, &it);
            ListView_SetItemText(c.smart, i, 1, (LPWSTR)a.label().c_str());
            ListView_SetItemText(c.smart, i, 2, val);
            ListView_SetItemText(c.smart, i, 3, worst);
            ListView_SetItemText(c.smart, i, 4, raw);
            std::wstring flags;
            if (a.preFailure()) flags += L"PreFail ";
            if (a.onlineUpdated()) flags += L"Online ";
            if (a.thresholdValid) flags += L"esik=" + core::formatU64(a.threshold);
            ListView_SetItemText(c.smart, i, 5, (LPWSTR)flags.c_str());
            std::wstring d = deltaOf(a.id, a.raw);
            ListView_SetItemText(c.smart, i, 6, (LPWSTR)d.c_str());
            ++i;
        }
    }
    if (r->nvmeHealthRead && r->nvmeHealth.valid) {
        const auto& h = r->nvmeHealth;
        // NVMe log fields are not SMART attributes, so they get their own rows: the narrow ID
        // column stays empty and the value lands in the wide last column.
        auto nvRow = [&](const std::wstring& k, const std::wstring& v, const std::wstring& d) {
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = (LPWSTR)L"-";
            ListView_InsertItem(c.smart, &it);
            ListView_SetItemText(c.smart, i, 1, (LPWSTR)k.c_str());
            ListView_SetItemText(c.smart, i, 2, (LPWSTR)L"-");
            ListView_SetItemText(c.smart, i, 3, (LPWSTR)L"-");
            ListView_SetItemText(c.smart, i, 4, (LPWSTR)L"-");
            ListView_SetItemText(c.smart, i, 5, (LPWSTR)v.c_str());
            ListView_SetItemText(c.smart, i, 6, (LPWSTR)d.c_str());
            ++i;
        };
        const bool bn = r->baseline.valid && r->baseline.nvmePercentageUsed >= 0;
        nvRow(L"NVMe uyari", h.criticalWarningText(), L"");
        nvRow(L"NVMe acik saat", core::formatU64(h.powerOnHours.value),
              diff(r->baseline.valid, r->baseline.nvmePowerOnHours, h.powerOnHours.value));
        nvRow(L"NVMe media errors", core::formatU64(h.mediaErrors.value),
              diff(r->baseline.valid, r->baseline.nvmeMediaErrors, h.mediaErrors.value));
        nvRow(L"NVMe yazilan", core::formatU64(h.dataUnitsWritten.value) + L" x 1000 blok", L"");
        nvRow(L"NVMe unsafe shutdown", core::formatU64(h.unsafeShutdowns.value), L"");
        nvRow(L"NVMe kullanim", core::formatU64(h.percentageUsed) + L"% (percentage_used)",
              diff(bn, uint64_t(r->baseline.nvmePercentageUsed), h.percentageUsed));
    }
    // The empty grid used to say "run a deep inspect", which is not an answer: the reason a
    // drive reports nothing (no ATA transport, SMART unsupported, SMART switched off, READ DATA
    // refused) is stated here instead of left for the user to guess.
    if (i == 0) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = 0;
        it.pszText = (LPWSTR)L"-";
        ListView_InsertItem(c.smart, &it);
        ListView_SetItemText(c.smart, 0, 1, (LPWSTR)L"satir yok");
        const std::wstring why = r->smartWhyText();
        ListView_SetItemText(c.smart, 0, 5,
                             (LPWSTR)(why.empty() ? L"surucu hicbir attribute raporlamadi"
                                                  : why.c_str()));
    }
    std::wstring note;
    const std::wstring why = r->smartWhyText();
    if (!why.empty()) note += why + L"\n";
    note += L"self-test: " + r->selfTestActivityText();
    note += L"\nloglar: " + r->smartLogsSummary();
    note += L"\n" + r->baselineSummary();
    SetText(c.smartInfo, note);
    AutoFitGrid(c.smart, 4, 6);
    fillSmartLogs();
}

// The drive's own error and self-test logs, read straight out of log address 01h / 06h (ATA)
// or the NVMe error and self-test pages. Rows are only what the device stored: a refused or
// unsupported read produces one explanatory line instead of an invented history.
void fillSmartLogs() {
    if (!c.smartLog) return;
    ListView_DeleteAllItems(c.smartLog);
    const app::DiskRecord* r = app::selectedDisk();
    int i = 0;
    auto addRow = [&](const std::vector<std::wstring>& cols) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)(cols.empty() ? L"-" : cols[0].c_str());
        ListView_InsertItem(c.smartLog, &it);
        for (size_t k = 1; k < cols.size() && k < 5; ++k)
            ListView_SetItemText(c.smartLog, i, (int)k, (LPWSTR)cols[k].c_str());
        ++i;
    };
    auto whyRow = [&](const std::wstring& text) {
        addRow({L"-", L"log sutunlari bos kalir", L"-", L"-", text});
    };

    if (!r) {
        whyRow(L"diger sutunlar bos kalir - soldan bir surucu secin");
        AutoFitGrid(c.smartLog, 16, 4);
        return;
    }
    if (r->bus == core::BusKind::Nvme) {
        if (!r->nvmeSelfTestRead && !r->nvmeErrorsRead)
            whyRow(L"NVMe loglari henuz okunmadi - 'Derin teshis' veya 'Loglari oku' calistirin");
        if (r->nvmeSelfTest.running())
            addRow({L"devam", r->nvmeSelfTest.statusText(), L"-", L"-",
                    L"test suruyor: log tamamlaninca yeniden okuyun"});
        if (r->nvmeSelfTestRead) {
            uint32_t n = 1;
            for (const auto& t : r->nvmeSelfTest.results) {
                addRow({L"test#" + core::formatU64(n++),
                        t.opText() + L" / " + t.resultText(),
                        core::formatU64(t.powerOnHours),
                        (t.validBits & 0x02) ? core::formatU64(t.firstErrorLba) : L"bildirilmedi",
                        L"nsid=" + core::formatU64(t.nsid) + L" SCT/SC=" + hex8(t.statusCodeType) +
                            L"/" + hex8(t.statusCode) +
                            (t.resultCode == 7 ? L" segment=" + core::formatU64(t.segment)
                                               : L"")});
            }
        } else if (!r->nvmeSelfTest.note.empty()) {
            whyRow(r->nvmeSelfTest.note);
        }
        if (r->nvmeErrorsRead) {
            for (const auto& e : r->nvmeErrors)
                addRow({L"hata#" + core::formatU64(e.errorCount), L"NVMe komut hatasi", L"-",
                        core::formatU64(e.lba), e.describe()});
        } else {
            whyRow(L"NVMe hata logu (page 01h) okunamadi - ret nedeni is gunlugunda");
        }
    } else if (!r->smartLogsRead) {
        whyRow(L"SMART loglari henuz okunmadi - 'Loglari oku' veya 'Derin teshis' calistirin");
    } else {
        if (r->smartLogs.errorRead) {
            for (const auto& e : r->smartLogs.errors)
                addRow({L"hata#" + core::formatU64(e.index), e.stateText(),
                        core::formatU64(e.powerOnHours), core::formatU64(e.lba),
                        L"komut=" + hex8(e.commandRegister) + L" err=" + hex8(e.errorRegister) +
                            L" status=" + hex8(e.statusRegister) +
                            L" sektor=" + hex8(e.sectorCount) +
                            L" | surucunun toplam hata sayaci=" +
                            core::formatU64(r->smartLogs.ataErrorCount)});
        } else if (!r->smartLogs.errorNote.empty()) {
            whyRow(r->smartLogs.errorNote);
        }
        if (r->smartLogs.selfTestRead) {
            for (const auto& t : r->smartLogs.selfTests)
                addRow({L"test#" + core::formatU64(t.index),
                        hw::ata::selfTestCodeLabel(t.typeCode) + L" / " + t.statusText(),
                        core::formatU64(t.powerOnHours),
                        t.firstErrorLba == 0xFFFFFFFFu
                            ? L"bildirilmedi"
                            : core::formatU64(t.firstErrorLba),
                        L"kalan %" + core::formatU64(t.percentRemaining()) +
                            (t.failed() ? L" | BASARISIZ" : L"")});
        } else if (!r->smartLogs.selfTestNote.empty()) {
            whyRow(r->smartLogs.selfTestNote);
        }
    }
    if (i == 0) whyRow(L"surucu hicbir log kaydi tutmuyor (loglar okundu, bos dondu)");
    AutoFitGrid(c.smartLog, 16, 4);
}

// Partition table and volumes of the selected disk. Empty table slots are listed too (an MBR
// disk always reports four), because "four rows, two used" is itself a fact about what a wipe
// would destroy, and the GPT entries that carry no drive letter are exactly the ones a letter
// based listing hides.
void fillParts() {
    if (!c.parts) return;
    ListView_DeleteAllItems(c.parts);
    const app::DiskRecord* r = app::selectedDisk();
    int i = 0;
    auto addRow = [&](const std::vector<std::wstring>& cols) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)(cols.empty() ? L"-" : cols[0].c_str());
        ListView_InsertItem(c.parts, &it);
        for (size_t k = 1; k < cols.size() && k < 8; ++k)
            ListView_SetItemText(c.parts, i, (int)k, (LPWSTR)cols[k].c_str());
        ++i;
    };

    if (!r) {
        SetText(c.partsInfo, L"Bolum haritasi: disk secilmedi");
        addRow({L"-", L"Diger sütunlar bos kalir - soldan bir surucu secin"});
        AutoFitGrid(c.parts, 8, 7);
        return;
    }

    if (r->layout.read) {
        for (const auto& p : r->layout.parts) {
            std::wstring flags;
            if (p.unused) flags += L"bos slot";
            if (p.bootable) flags += L"boot ";
            if (p.hidden) flags += L"gizli ";
            if (p.noLetter) flags += L"harf yok ";
            if (p.platformRequired) flags += L"platform-required ";
            addRow({L"Bolum " + core::formatU64(p.index), Printable(p.typeText),
                    core::formatU64(p.startLba), core::formatU64(p.sectorCount),
                    p.sizeBytes ? core::formatBytes(p.sizeBytes) : L"-",
                    p.letters.empty() ? L"-" : p.letters + L":",
                    flags.empty() ? L"-" : flags,
                    p.name.empty() ? (p.offsetBytes ? L"ofset " + core::formatU64(p.offsetBytes) +
                                                         L" B"
                                                     : L"-")
                                   : Printable(p.name)});
        }
    } else {
        addRow({L"-", L"bolum tablosu okunamadi: " +
                           (r->layout.errorText.empty() ? L"neden bildirilmedi"
                                                        : r->layout.errorText)});
    }

    for (const auto& v : r->volumes) {
        std::wstring flags;
        if (v.bootForCurrentOs) flags += L"BOOT (SystemRoot) ";
        if (v.pagefileCandidate) flags += L"pagefile ";
        if (v.capacityBytes)
            flags += L"bos alan: " + core::formatBytes(v.freeBytes) + L" / " +
                     core::formatBytes(v.capacityBytes);
        addRow({L"Volume", v.fileSystem.empty() ? L"(fs bildirilmedi)" : v.fileSystem, L"-", L"-",
                v.capacityBytes ? core::formatBytes(v.capacityBytes) : L"-",
                v.mountPoint.empty() ? L"-" : v.mountPoint,
                flags.empty() ? L"-" : flags, Printable(v.volumeGuidPath)});
    }
    if (i == 0) addRow({L"-", L"tablo bos: bu diskte bolum kaydi yok"});

    SetText(c.partsInfo, L"Mount: " + r->mountStateText() + L" | " + r->layoutSummary() +
                             L" | " + core::formatU64((uint64_t)r->volumes.size()) +
                             L" volume | rol: " + r->roleLabel);
    AutoFitGrid(c.parts, 8, 7);
}

// The Erase tab has a single status row, so it shows the target while idle and the live step
// plus percent only while a wipe is actually running. Two writers on one label otherwise
// fought over it every timer tick.
void refreshWipeLine() {
    if (!c.wipeInfo) return;
    app::State& st = app::The();
    const app::DiskRecord* r = app::selectedDisk();
    std::wstring head;
    if (r) {
        wchar_t path[64];
        swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", r->number);
        head = std::wstring(L"Hedef: ") + path + L" (" + r->sizeText() + L") | purge: " +
               std::wstring(hw::erase::purgeMethodLabel(r->purge));
        if (!r->blockedBecause.empty()) head += L" | ENGEL: " + r->blockedBecause;
    } else {
        head = L"Ezme: hedef secilmedi";
    }
    if (!st.wipeRunning.load()) {
        SetText(c.wipeInfo, head);
        return;
    }
    std::wstring progress;
    {
        std::lock_guard<std::mutex> lk(st.progressMutex_);
        progress = L"  |  adim " + core::formatU64(st.wipeStep.load() + 1) + L"/" +
                   core::formatU64(st.wipeStepCount.load()) + L": " +
                   (st.wipeStepLabel.empty() ? std::wstring(L"-") : st.wipeStepLabel) + L"  |  %" +
                   core::formatU64(st.wipePercent.load()) + L"  |  " +
                   [&] {
                       wchar_t b[32];
                       swprintf(b, 32, L"%.1f MB/s", st.wipeSpeedMiBs.load());
                       return std::wstring(b);
                   }();
    }
    SetText(c.wipeInfo, head + progress);
}

// The status strip is the densest thing on the window: what the engine thinks, which drive is
// in scope, how many drives are locked/blocked, what the queue is doing and whether this
// process can write at all. Every number here comes from the same records the tables show.
void BuildStatusPanes() {
    app::State& st = app::The();
    const auto disks = app::snapshotDisks();
    int locked = 0, frozen = 0, blocked = 0, opal = 0, system = 0;
    for (const auto& d : disks) {
        if (d.ataLocked) ++locked;
        if (d.ataFrozen) ++frozen;
        if (!d.blockedBecause.empty()) ++blocked;
        if (d.tcgProbed && d.tcg.hasOpal) ++opal;
        if (d.isSystemCritical()) ++system;
    }
    const bool busy = workers::busy();

    const std::wstring state = st.status().empty() ? std::wstring(L"hazir") : st.status();
    c.panes.clear();
    c.panes.push_back(Pane{state, color::text, busy ? 1 : 0});

    const app::DiskRecord* r = app::selectedDisk();
    if (r) {
        std::wstring s = L"PhysicalDrive" + core::formatU64((uint64_t)r->number);
        s += L"  |  " + Printable(r->model);
        s += L"  |  " + r->sizeText();
        s += L"  |  " + r->busLabel;
        if (r->temperatureC >= 0)
            s += L"  |  " + core::formatU64((uint64_t)r->temperatureC) + L" C";
        if (r->wearPercent >= 0)
            s += L"  |  asinma %" + core::formatU64((uint64_t)r->wearPercent);
        c.panes.push_back(Pane{s, color::text, r->isSystemCritical() ? 1 : -1});
    } else {
        c.panes.push_back(Pane{L"surucu secilmedi", color::textFaint, -1});
    }

    wchar_t cnt[192];
    swprintf(cnt, 192, L"surucu %" PRIu64 L"  |  kilitli %" PRIu64 L"  |  frozen %" PRIu64
                       L"  |  opal %" PRIu64 L"  |  engel %" PRIu64 L"  |  sistem %" PRIu64,
             (uint64_t)disks.size(), (uint64_t)locked, (uint64_t)frozen, (uint64_t)opal,
             (uint64_t)blocked, (uint64_t)system);
    c.panes.push_back(Pane{cnt, blocked || locked ? color::fail : color::textMuted, -1});

    // The queue is strictly serial, so "not busy" genuinely means "nothing waiting".
    std::wstring q = L"kuyruk bos";
    int qdot = -1;
    if (busy) {
        q = L"calisiyor: " + workers::currentJobName();
        qdot = 1;
        if (st.wipeRunning.load())
            q += L"  |  %" + core::formatU64(st.wipePercent.load());
        else if (st.scanRunning.load())
            q += L"  |  %" + core::formatU64(st.scanPercent.load());
    }
    c.panes.push_back(Pane{q, qdot == 1 ? color::warn : color::textMuted, qdot});

    wchar_t right[128];
    // %ls: a bare %s here is read as a narrow argument by this toolchain and "evet" prints "e".
    if (c.lastScanValid)
        swprintf(right, 128, L"yonetici: %ls  |  dpi %lu%%  |  son guncelleme %02d:%02d:%02d",
                 core::processIsAdmin() ? L"evet" : L"HAYIR", (unsigned long)Dpi::current().percent(),
                 c.lastScan.wHour, c.lastScan.wMinute, c.lastScan.wSecond);
    else
        swprintf(right, 128, L"yonetici: %ls  |  dpi %lu%%  |  henuz tarama yok",
                 core::processIsAdmin() ? L"evet" : L"HAYIR", (unsigned long)Dpi::current().percent());
    c.rightPane = right;

    std::wstring sig = state;
    for (const Pane& p : c.panes) sig += L"\n" + p.text;
    sig += L"\n" + c.rightPane;
    if (sig != c.paneSignature) {
        c.paneSignature = sig;
        InvalidateStatusStrip();
    }
}

void InvalidateStatusStrip() {
    if (c.main && c.statusRc.right > c.statusRc.left)
        InvalidateRect(c.main, &c.statusRc, TRUE);
}

// Left panes are laid out by measured text width and clipped against the right pane, so a
// long model string never walks over the admin/dpi readout.
void DrawStatusStrip(HDC dc) {
    const RECT r = c.statusRc;
    if (r.right <= r.left || r.bottom <= r.top) return;
    FillRectColor(dc, r, color::headerBand);
    HLine(dc, r.left, r.right, r.top, color::border);
    SetBkMode(dc, TRANSPARENT);
    HFONT of = (HFONT)SelectObject(dc, c.fontUi);

    const int padX = S(10), dot = S(8), gapAfterDot = S(7);
    RECT rightRc{};
    int rightW = 0;
    if (!c.rightPane.empty()) {
        rightW = TextW(dc, c.rightPane.c_str()) + padX * 2;
        rightRc = {r.right - rightW, r.top, r.right - padX, r.bottom};
        SetTextColor(dc, color::textMuted);
        DrawTextW(dc, c.rightPane.c_str(), -1, &rightRc,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (rightW) VLine(dc, r.right - rightW - S(2), r.top + S(6), r.bottom - S(6),
                          color::border);
    }

    int x = r.left;
    for (const Pane& p : c.panes) {
        if (x >= r.right - rightW - S(8)) break;
        RECT cell = {x, r.top, r.right - rightW - S(8), r.bottom};
        int textX = cell.left + padX;
        if (p.dot >= 0) {
            const COLORREF dc2 = p.dot == 1 ? color::warn : (p.dot == 2 ? color::fail : color::ok);
            RECT d = {cell.left + padX, (cell.top + cell.bottom - dot) / 2,
                      cell.left + padX + dot, (cell.top + cell.bottom + dot) / 2};
            FillRectColor(dc, d, dc2);
            textX += dot + gapAfterDot;
        }
        RECT text = {textX, cell.top, cell.right, cell.bottom};
        const int w = TextW(dc, p.text.c_str());
        SetTextColor(dc, p.fg);
        DrawTextW(dc, p.text.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        const int used = (textX - cell.left) + w + padX;
        const int next = cell.left + used;
        if (next < r.right - rightW - S(8)) {
            VLine(dc, next, cell.top + S(6), cell.bottom - S(6), color::border);
            x = next + 1;
        } else {
            x = next;
        }
    }
    SelectObject(dc, of);
}

void showTab(int idx) {
    if (idx < 0 || idx >= kTabCount) return;
    c.activeTab = idx;
    for (int t = 0; t < kTabCount; ++t) {
        const int cmd = t == idx ? SW_SHOW : SW_HIDE;
        for (HWND h : c.tabKids[t]) ShowWindow(h, cmd);
        if (c.tabBtn[t]) InvalidateRect(c.tabBtn[t], nullptr, TRUE);
    }
    LayoutMain();
    if (c.main) InvalidateRect(c.main, nullptr, FALSE);
}

void requestDestructive(const std::wstring& what, std::function<void()> action) {
    const app::DiskRecord* r = app::selectedDisk();
    if (!r) {
        core::LogWarn(L"[islem] " + what + L" icin once bir disk secin");
        return;
    }
    if (r->blockedBecause.empty() && !r->isSystemCritical()) {
        // Still records the phrase: the worker gate re-checks it after dismounting.
        app::The().destructiveOverride = workers::confirmationPhrase(r->number);
        action();
        return;
    }
    c.pendingAction = std::move(action);
    SetText(c.confirmInfo, L"GERI ALINAMAZ: " + what + L"   |   PhysicalDrive" +
                               core::formatU64((uint64_t)r->number) + L" (" + r->model + L", " +
                               r->roleLabel + L")");
    SetText(c.confirmReq, L"Onay deyimi birebir yazilmali: " + workers::confirmationPhrase(r->number));
    SetText(c.confirmEdit, L"");
    for (HWND h : {c.confirmEdit, c.confirmReq, c.confirmInfo, c.confirmYes, c.confirmNo})
        ShowWindow(h, SW_SHOW);
    LayoutMain();
    InvalidateRect(c.main, nullptr, TRUE);
    SetFocus(c.confirmEdit);
}

void cancelDestructive() {
    c.pendingAction = nullptr;
    app::The().destructiveOverride.clear();
    for (HWND h : {c.confirmEdit, c.confirmReq, c.confirmInfo, c.confirmYes, c.confirmNo})
        ShowWindow(h, SW_HIDE);
    LayoutMain();
    InvalidateRect(c.main, nullptr, TRUE);
}

void exportReport(HWND hwnd) {
    wchar_t file[MAX_PATH] = L"DiskUnlock_Rapor.txt";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Metin raporu (*.txt)\0*.txt\0Tum dosyalar\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::wstring why;
    if (workers::writeReport(file, why)) core::LogOk(L"[rapor] yazildi: " + std::wstring(file));
    else core::LogFail(L"[rapor] yazilamadi: " + why);
}

void createControls(HWND hwnd, HINSTANCE inst) {
    c.main = hwnd;
    c.fontUi = MakeFont(hwnd, L"Segoe UI", 13, FW_NORMAL);
    c.fontBold = MakeFont(hwnd, L"Segoe UI", 13, FW_BOLD);
    c.fontLabel = MakeFont(hwnd, L"Segoe UI", 11, FW_SEMIBOLD);
    c.fontMono = MakeFont(hwnd, L"Consolas", 12, FW_NORMAL);
    c.fontGauge = MakeFont(hwnd, L"Segoe UI", 26, FW_BOLD);
    c.brushWindow = CreateSolidBrush(color::window);
    c.brushCard = CreateSolidBrush(color::card);
    c.brushWarn = CreateSolidBrush(color::warnBg);

    const wchar_t* tabNames[] = {L"Genel",      L"S.M.A.R.T.", L"Hata logu",  L"Yuzey testi",
                                 L"Bolumler",   L"Guvenlik",   L"Silme / Sanitize"};
    for (int t = 0; t < kTabCount; ++t)
        c.tabBtn[t] = MkBtn(hwnd, ID_TAB_GENEL + t, tabNames[t], inst);

    // Column widths are chosen so the set fits the list; an overflowing column set reads as
    // broken even when every value is correct. The bus is deliberately not here - it is in the
    // status strip and on the drive's own page, and the list needs that width for the model.
    c.disks = MakeListView(hwnd, inst, ID_LV_DISKS);
    AddListColumn(c.disks, 0, L"#", 30);
    AddListColumn(c.disks, 1, L"Model", 190);
    AddListColumn(c.disks, 2, L"Kapasite", 78);
    AddListColumn(c.disks, 3, L"Rol", 96);
    AddListColumn(c.disks, 4, L"Durum", 84);
    AddListColumn(c.disks, 5, L"Saglik", 54);

    // --- page 0: Genel ---------------------------------------------------------------
    c.detail = forTab(0, MakeListView(hwnd, inst, ID_LV_DETAIL));
    AddListColumn(c.detail, 0, L"Alan", 158);
    AddListColumn(c.detail, 1, L"Deger (surucunun raporladigi)", 420);
    c.noteGenel = forTab(0, MkNote(hwnd, ID_LBL_GENEL, L"Ozet: disk secilmedi", inst));

    // --- page 5: Guvenlik (ATA password + TCG/OPAL) ----------------------------------
    c.lblPass = forTab(5, MkLabel(hwnd, ID_LBL_PASS, L"SURUCU PAROLASI", inst));
    c.pass = forTab(5, MkEdit(hwnd, ID_EDIT_PASS, inst, true));
    SendMessageW(c.pass, EM_SETCUEBANNER, TRUE, (LPARAM)L"ATA security parola");
    c.master = forTab(5, MkCheck(hwnd, ID_CHK_MASTER, L"Master parola kullan", inst));
    c.btnSetPass = forTab(5, MkBtn(hwnd, ID_BTN_SET_PASS, L"Parola Ayarla (0xF1)", inst));
    c.btnUnlock = forTab(5, MkBtn(hwnd, ID_BTN_UNLOCK, L"Kilit Ac (0xF2)", inst));
    c.btnDisable = forTab(5, MkBtn(hwnd, ID_BTN_DISABLE, L"Parolayi Kaldir (0xF6)", inst));
    c.btnFreeze = forTab(5, MkBtn(hwnd, ID_BTN_FREEZE, L"Freeze Lock (0xF5)", inst));
    c.ataInfo = forTab(5, MkNote(hwnd, ID_LBL_ATA, L"ATA guvenlik durumu: disk secilmedi", inst));

    c.lblPsid = forTab(5, MkLabel(hwnd, ID_LBL_PSID, L"PSID (SURUCU ETIKETINDEKI)", inst));
    c.psid = forTab(5, MkEdit(hwnd, ID_EDIT_PSID, inst));
    SendMessageW(c.psid, EM_SETCUEBANNER, TRUE, (LPARAM)L"16 haneli PSID");
    c.btnTcgDisco = forTab(5, MkBtn(hwnd, ID_BTN_TCG_DISCO, L"Level 0 Discovery", inst));
    c.btnPsid = forTab(5, MkBtn(hwnd, ID_BTN_PSID, L"PSID Revert (geri alinamaz)", inst));
    c.tcgInfo = forTab(5, MkNote(hwnd, ID_LBL_TCG, L"TCG: discovery calistirilmadi", inst));

    // --- page 6: Silme / Sanitize ----------------------------------------------------
    c.lblProfile = forTab(6, MkLabel(hwnd, ID_LBL_PROFILE, L"ERUTME PROFILI", inst));
    c.profile = forTab(6, CreateWindowExW(0, L"COMBOBOX", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                              CBS_HASSTRINGS,
                                          0, 0, 10, 300, hwnd, (HMENU)(INT_PTR)ID_COMBO_PROFILE,
                                          inst, nullptr));
    ThemeFlat(c.profile);
    SendMessageW(c.profile, CB_ADDSTRING, 0, (LPARAM)L"NIST 800-88r1 Clear - tek gecis 0x00");
    SendMessageW(c.profile, CB_ADDSTRING, 0, (LPARAM)L"Tek gecis pseudorandom");
    SendMessageW(c.profile, CB_ADDSTRING, 0, (LPARAM)L"DoD deseni - 0x00 / 0xFF / random + dogrulama");
    SendMessageW(c.profile, CB_SETCURSEL, 0, 0);
    c.enhanced = forTab(6, MkCheck(hwnd, ID_CHK_ENHANCED, L"Enhanced erase kullan", inst));
    c.btnWipeTables = forTab(6, MkBtn(hwnd, ID_BTN_WIPE_TABLES, L"Bolum tablosunu sil", inst));
    c.btnOverwrite = forTab(6, MkBtn(hwnd, ID_BTN_OVERWRITE, L"Diski ez (secili profil)", inst));
    c.btnPurge = forTab(6, MkBtn(hwnd, ID_BTN_PURGE, L"Donanim purge", inst));
    c.btnCancel = forTab(6, MkBtn(hwnd, ID_BTN_CANCEL, L"Calisan isi durdur", inst));
    c.wipeInfo = forTab(6, MkNote(hwnd, ID_LBL_WIPE, L"Ezme: hedef secilmedi", inst));
    c.wipeBar = forTab(6, MkBar(hwnd, ID_PBAR_WIPE, inst));

    // --- page 1: SMART ---------------------------------------------------------------
    // The reads are harmless; the device commands (enable, disable, autosave, WRITE LOG reset)
    // need raw write access and the worker says so when Windows refuses them on a mounted disk.
    c.btnDeep = forTab(1, MkBtn(hwnd, ID_BTN_DEEP, L"Derin teshis (SMART + NVMe + TCG)", inst));
    c.btnDstShort = forTab(1, MkBtn(hwnd, ID_BTN_DST_SHORT, L"Kisa test (D4h/01h)", inst));
    c.btnDstLong = forTab(1, MkBtn(hwnd, ID_BTN_DST_LONG, L"Uzun test (D4h/02h)", inst));
    c.btnDstAbort = forTab(1, MkBtn(hwnd, ID_BTN_DST_ABORT, L"Testi durdur (D4h/7Fh)", inst));
    c.btnSmartOn = forTab(1, MkBtn(hwnd, ID_BTN_SMART_ON, L"SMART ac (D8h)", inst));
    c.btnSmartOff = forTab(1, MkBtn(hwnd, ID_BTN_SMART_OFF, L"SMART kapat (D9h)", inst));
    c.btnAutosaveOn = forTab(1, MkBtn(hwnd, ID_BTN_AUTOSAVE_ON, L"Autosave ac (D2h/F1h)", inst));
    c.btnAutosaveOff =
        forTab(1, MkBtn(hwnd, ID_BTN_AUTOSAVE_OFF, L"Autosave kapa (D2h/F2h)", inst));
    c.btnBaselineTake = forTab(1, MkBtn(hwnd, ID_BTN_BASELINE_TAKE, L"Baseline al", inst));
    c.btnBaselineClear = forTab(1, MkBtn(hwnd, ID_BTN_BASELINE_CLEAR, L"Baseline sil", inst));
    c.lblSmart = forTab(1, MkLabel(hwnd, ID_LBL_SMART,
                                   L"SMART ATTRIBUTELERI (yalnizca surucunun raporladigi)", inst));
    c.smart = forTab(1, MakeListView(hwnd, inst, ID_LV_SMART));
    AddListColumn(c.smart, 0, L"ID", 40);
    AddListColumn(c.smart, 1, L"Attribut", 190);
    AddListColumn(c.smart, 2, L"Current", 58);
    AddListColumn(c.smart, 3, L"Worst", 52);
    AddListColumn(c.smart, 4, L"Raw", 116);
    AddListColumn(c.smart, 5, L"Deger / Bayraklar", 150);
    AddListColumn(c.smart, 6, L"Baseline farki", 104);
    c.smartInfo = forTab(1, MkNote(hwnd, ID_LBL_SMARTINFO, L"SMART: disk secilmedi", inst));

    // --- page 2: the drive's own logs --------------------------------------------------
    c.btnSmartLogs = forTab(2, MkBtn(hwnd, ID_BTN_SMART_LOGS, L"Loglari oku", inst));
    c.btnSmartClearLog =
        forTab(2, MkBtn(hwnd, ID_BTN_SMART_CLEARLOG, L"Hata logunu sifirla (D6h/01h)", inst));
    c.lblSmartLog =
        forTab(2, MkLabel(hwnd, ID_LBL_SMARTLOG, L"HATA VE SELF-TEST LOGLARI (cihazin kaydi)",
                          inst));
    c.smartLog = forTab(2, MakeListView(hwnd, inst, ID_LV_SMARTLOG));
    AddListColumn(c.smartLog, 0, L"Kayit", 62);
    AddListColumn(c.smartLog, 1, L"Durum / Tur", 170);
    AddListColumn(c.smartLog, 2, L"Acik saat", 62);
    AddListColumn(c.smartLog, 3, L"LBA", 84);
    AddListColumn(c.smartLog, 4, L"Kayit detayi (byte byte)", 220);
    c.noteLogs = forTab(2, MkNote(hwnd, ID_LBL_LOGINFO, L"Loglar: disk secilmedi", inst));

    // --- page 3: surface ---------------------------------------------------------------
    c.btnScan = forTab(3, MkBtn(hwnd, ID_BTN_SCAN, L"Yuzey taramasi", inst));
    c.scanBar = forTab(3, MkBar(hwnd, ID_PBAR_SCAN, inst));
    c.surface = forTab(3, MakeListView(hwnd, inst, ID_LV_SURFACE));
    AddListColumn(c.surface, 0, L"Olcum", 190);
    AddListColumn(c.surface, 1, L"Deger", 260);
    c.scanInfo =
        forTab(3, MkNote(hwnd, ID_LBL_SCAN, L"Yuzey taramasi: henuz calistirilmadi", inst));

    // --- page 4: partitions / mount ----------------------------------------------------
    c.btnProbeLayout =
        forTab(4, MkBtn(hwnd, ID_BTN_PROBE_LAYOUT, L"Bolumleri yeniden oku", inst));
    c.btnRoOn = forTab(4, MkBtn(hwnd, ID_BTN_RO_ON, L"Yazma korumayi AC", inst));
    c.btnRoOff = forTab(4, MkBtn(hwnd, ID_BTN_RO_OFF, L"Yazma korumayi KAPA", inst));
    c.btnOffOn = forTab(4, MkBtn(hwnd, ID_BTN_OFF_ON, L"Diski cevrimdisi birak", inst));
    c.btnOffOff = forTab(4, MkBtn(hwnd, ID_BTN_OFF_OFF, L"Diski cevrimici al", inst));
    c.parts = forTab(4, MakeListView(hwnd, inst, ID_LV_PARTS));
    AddListColumn(c.parts, 0, L"Kayit", 74);
    AddListColumn(c.parts, 1, L"Tip / Dosya sistemi", 170);
    AddListColumn(c.parts, 2, L"Baslangic LBA", 84);
    AddListColumn(c.parts, 3, L"Sektor", 78);
    AddListColumn(c.parts, 4, L"Boyut", 70);
    AddListColumn(c.parts, 5, L"Harf", 46);
    AddListColumn(c.parts, 6, L"Bayraklar", 130);
    AddListColumn(c.parts, 7, L"Ad / Aygit yolu", 200);
    c.partsInfo =
        forTab(4, MkNote(hwnd, ID_LBL_PARTS, L"Bolum haritasi: disk secilmedi", inst));

    c.confirmInfo = MkLabel(hwnd, ID_LBL_CONFIRM_INFO, L"", inst);
    c.confirmReq = MkLabel(hwnd, ID_LBL_CONFIRM_REQ, L"", inst);
    c.confirmEdit = MkEdit(hwnd, ID_EDIT_CONFIRM, inst);
    c.confirmYes = MkBtn(hwnd, ID_BTN_CONFIRM_YES, L"Onayla ve calistir", inst);
    c.confirmNo = MkBtn(hwnd, ID_BTN_CONFIRM_NO, L"Vazge", inst);
    // The band only exists while a destructive action waits for the typed phrase.
    for (HWND h : {c.confirmEdit, c.confirmReq, c.confirmInfo, c.confirmYes, c.confirmNo})
        ShowWindow(h, SW_HIDE);

    c.log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | WS_VSCROLL,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_EDIT_LOG, inst, nullptr);
    ThemeFlat(c.log);
    c.btnRefresh = MkBtn(hwnd, ID_BTN_REFRESH, L"Tumunu tara", inst);
    c.btnLba0 = MkBtn(hwnd, ID_BTN_LBA0, L"LBA 0 hex", inst);
    c.btnReport = MkBtn(hwnd, ID_BTN_REPORT, L"Rapor kaydet", inst);
    c.btnClearLog = MkBtn(hwnd, ID_BTN_CLEARLOG, L"Log temizle", inst);

    for (HWND h : {c.disks, c.detail, c.smart, c.parts, c.pass, c.psid, c.confirmEdit, c.profile,
                   c.master, c.enhanced, c.lblPass, c.lblPsid, c.lblProfile, c.btnRefresh,
                   c.btnLba0, c.btnReport, c.btnClearLog, c.btnSetPass, c.btnUnlock, c.btnDisable,
                   c.btnFreeze, c.btnTcgDisco, c.btnPsid, c.btnWipeTables, c.btnOverwrite,
                   c.btnPurge, c.btnCancel, c.btnDeep, c.btnScan, c.confirmYes, c.confirmNo,
                   c.btnProbeLayout, c.btnRoOn, c.btnRoOff, c.btnOffOn, c.btnOffOff, c.ataInfo,
                   c.tcgInfo, c.wipeInfo, c.scanInfo, c.partsInfo, c.confirmInfo, c.confirmReq,
                   c.smartInfo, c.lblSmart, c.lblSmartLog, c.btnSmartLogs, c.btnSmartOn,
                   c.btnSmartOff, c.btnAutosaveOn, c.btnAutosaveOff, c.btnDstShort, c.btnDstLong,
                   c.btnDstAbort, c.btnSmartClearLog, c.btnBaselineTake, c.btnBaselineClear,
                   c.smartLog, c.surface, c.noteGenel, c.noteLogs})
        TrackFont(h, 0);
    for (int t = 0; t < kTabCount; ++t) TrackFont(c.tabBtn[t], 1);
    TrackFont(c.log, 3);
    ApplyFonts();

    showTab(0);
    LayoutMain();
    fillDetail();
    fillSurfaceStats();
}

// The window has to exist before GetDpiForWindow can answer, so CreateMainWindow sizes it
// with the system DPI. WM_CREATE re-checks against the window's own monitor and clamps the
// result to the work area; without that a 125% monitor gets a window whose right column
// falls off the edge of the screen.
void sizeToMonitor(HWND hwnd) {
    RECT want = {0, 0, S(1420), S(880)};
    const DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    const DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&want, style, FALSE, exStyle);

    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    int fw = want.right - want.left, fh = want.bottom - want.top;
    int left = CW_USEDEFAULT, top = CW_USEDEFAULT;
    if (GetMonitorInfoW(mon, &mi)) {
        fw = (std::min)(fw, (int)(mi.rcWork.right - mi.rcWork.left) - S(24));
        fh = (std::min)(fh, (int)(mi.rcWork.bottom - mi.rcWork.top) - S(24));
        left = mi.rcWork.left + S(20);
        top = mi.rcWork.top + S(16);
    }
    SetWindowPos(hwnd, nullptr, left, top, (std::max)(S(900), fw), (std::max)(S(560), fh),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        Dpi::setWindow(hwnd);
        sizeToMonitor(hwnd);
        core::LogInfo(L"[arayuz] dpi olcegi %" + core::formatU64((uint64_t)Dpi::current().percent()) +
                      L"% - duzen 96 dpi birimlerle tasarlandi");
        createControls(hwnd, ((LPCREATESTRUCT)lp)->hInstance);
        {
            // Volume broadcasts reach a top-level window by themselves; a physical disk being
            // added or removed needs this explicit registration. If the OS refuses it, the
            // volume path still works, so the refusal is logged instead of swallowed.
            DEV_BROADCAST_DEVICEINTERFACE_W dbi{};
            dbi.dbcc_size = sizeof(dbi);
            dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            dbi.dbcc_classguid = kGuidDevInterfaceDisk;
            g_diskNotifier = RegisterDeviceNotificationW(hwnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (g_diskNotifier)
                core::LogInfo(L"[arayuz] disk arayuzu bildirimi kayitli: canli ekleme/cikarma");
            else
                core::LogWarn(L"[arayuz] disk arayuzu bildirimi kaydolamadi (" +
                              core::lastErrorText() +
                              L") - yalnızca volume Yayınları yakalanacak");
        }
        {
            // Every entry fires an id the WM_COMMAND switch already knows, so the keyboard
            // path and the mouse path can never diverge into two different behaviours.
            ACCEL a[] = {
                {FVIRTKEY, VK_F5, (WORD)ID_BTN_REFRESH},
                {FVIRTKEY | FCONTROL, 'L', (WORD)ID_BTN_CLEARLOG},
                {FVIRTKEY | FCONTROL, 'E', (WORD)ID_BTN_REPORT},
                {FVIRTKEY | FCONTROL, 'D', (WORD)ID_BTN_DEEP},
                {FVIRTKEY | FCONTROL, 'K', (WORD)ID_BTN_SCAN},
                {FVIRTKEY | FCONTROL | FSHIFT, 'L', (WORD)ID_BTN_SMART_LOGS},
                {FVIRTKEY | FCONTROL | FSHIFT, 'T', (WORD)ID_BTN_DST_SHORT},
                {FVIRTKEY | FCONTROL | FSHIFT, 'E', (WORD)ID_BTN_DST_LONG},
                {FVIRTKEY | FCONTROL | FSHIFT, 'X', (WORD)ID_BTN_DST_ABORT},
                {FVIRTKEY, VK_ESCAPE, (WORD)ID_BTN_CONFIRM_NO},
                {FVIRTKEY, VK_RETURN, (WORD)ID_BTN_CONFIRM_YES},
                {FVIRTKEY | FCONTROL, '1', (WORD)ID_TAB_GENEL},
                {FVIRTKEY | FCONTROL, '2', (WORD)ID_TAB_SMART},
                {FVIRTKEY | FCONTROL, '3', (WORD)ID_TAB_LOGS},
                {FVIRTKEY | FCONTROL, '4', (WORD)ID_TAB_SURFACE},
                {FVIRTKEY | FCONTROL, '5', (WORD)ID_TAB_PARTS},
                {FVIRTKEY | FCONTROL, '6', (WORD)ID_TAB_SEC},
                {FVIRTKEY | FCONTROL, '7', (WORD)ID_TAB_ERASE},
            };
            g_accel = CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
        }
        SetTimer(hwnd, ID_TIMER, kTimerMs, nullptr);
        workers::postRefresh();
        return 0;

    case WM_GETMINMAXINFO: {
        // Below this the two-column layout genuinely stops fitting; refusing to shrink is
        // kinder than letting the right column collapse onto the left one.
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(900);
        mmi->ptMinTrackSize.y = S(560);
        return 0;
    }

    case WM_SIZE:
        LayoutMain();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DPICHANGED: {
        Dpi::setWindow(hwnd);
        LPRECT suggested = (LPRECT)lp;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        const HFONT oldUi = c.fontUi, oldBold = c.fontBold, oldLabel = c.fontLabel,
                    oldMono = c.fontMono, oldGauge = c.fontGauge;
        c.fontUi = MakeFont(hwnd, L"Segoe UI", 13, FW_NORMAL);
        c.fontBold = MakeFont(hwnd, L"Segoe UI", 13, FW_BOLD);
        c.fontLabel = MakeFont(hwnd, L"Segoe UI", 11, FW_SEMIBOLD);
        c.fontMono = MakeFont(hwnd, L"Consolas", 12, FW_NORMAL);
        c.fontGauge = MakeFont(hwnd, L"Segoe UI", 26, FW_BOLD);
        // Re-point every control at the new handles before releasing the old ones.
        ApplyFonts();
        if (oldUi) DeleteObject(oldUi);
        if (oldBold) DeleteObject(oldBold);
        if (oldLabel) DeleteObject(oldLabel);
        if (oldMono) DeleteObject(oldMono);
        if (oldGauge) DeleteObject(oldGauge);
        LayoutMain();
        UpdateTiles();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    // A disk being plugged in or yanked out is the one thing the user should not have to press a
    // button for. Windows broadcasts several times per device (interface, class, volume), so an
    // event only arms a short debounce and a single rescan swallows the burst.
    case WM_DEVICECHANGE: {
        if (wp != DBT_DEVICEARRIVAL && wp != DBT_DEVICEREMOVECOMPLETE) return TRUE;
        auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lp);
        if (!hdr) return TRUE;
        std::wstring what;
        if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
            auto* vol = reinterpret_cast<DEV_BROADCAST_VOLUME*>(hdr);
            std::wstring letters;
            for (int i = 0; i < 26; ++i)
                if (vol->dbcv_unitmask & (1u << i)) letters += (wchar_t)(L'A' + i);
            what = L"volume harfleri: " + (letters.empty() ? L"(bildirilmedi)" : letters);
        } else if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            what = L"arayuz aygiti";
        } else if (hdr->dbch_devicetype == DBT_DEVTYP_HANDLE) {
            what = L"aygit handle'i";
        } else {
            return TRUE;
        }
        core::LogInfo(L"[cihaz] " +
                      std::wstring(wp == DBT_DEVICEARRIVAL ? L"ekleme" : L"cikarma") + L" | " +
                      what + L" | tarama 1200 ms ertelendi");
        SetTimer(hwnd, (UINT_PTR)ID_TIMER_DEVICE, 1200, nullptr);
        return TRUE;
    }

    case WM_TIMER: {
        if (wp == ID_TIMER_DEVICE) {
            KillTimer(hwnd, (UINT_PTR)ID_TIMER_DEVICE);
            core::LogInfo(L"[cihaz] canli yenileme: tum fiziksel diskler yeniden taraniyor");
            workers::postRefresh();
            return 0;
        }
        if (wp != ID_TIMER) return 0;
        // Workers only ever append to the ring buffer; the GUI drains it here, so no worker
        // thread ever sends a message to a window it does not own.
        auto records = core::Log().drain(200);
        if (!records.empty()) {
            std::wstring chunk;
            for (const auto& r : records) {
                wchar_t stamp[32];
                swprintf(stamp, 32, L"[%02d:%02d:%02d] ", r.when.wHour, r.when.wMinute,
                         r.when.wSecond);
                const wchar_t* tag = L"";
                switch (r.level) {
                case core::LogLevel::Info: tag = L""; break;
                case core::LogLevel::Command: tag = L">>> "; break;
                case core::LogLevel::Ok: tag = L"[OK] "; break;
                case core::LogLevel::Warn: tag = L"[UYARI] "; break;
                case core::LogLevel::Fail: tag = L"[HATA] "; break;
                }
                chunk += stamp;
                chunk += tag;
                chunk += r.message;
                chunk += L"\r\n";
            }
            const int len = GetWindowTextLength(c.log);
            SendMessageW(c.log, EM_SETSEL, len, len);
            SendMessageW(c.log, EM_REPLACESEL, FALSE, (LPARAM)chunk.c_str());
            SendMessageW(c.log, EM_SCROLLCARET, 0, 0);
            if (len > 200000) SetText(c.log, L"...(log kesildi)\r\n");
        }

        const bool busy = workers::busy();
        app::State& st = app::The();
        refreshWipeLine();
        SendMessageW(c.wipeBar, PBM_SETPOS, st.wipePercent.load(), 0);

        const uint32_t scanPct = st.scanPercent.load();
        app::SurfaceStats stats;
        {
            std::lock_guard<std::mutex> lk(st.mutex_);
            stats = st.surface;
        }
        // The buckets live in the page's table and the note carries only the live sentence: a
        // number shown in two places is two numbers the user has to reconcile.
        if (st.scanRunning.load()) {
            wchar_t sbuf[192];
            swprintf(sbuf, 192,
                     L"Tarama suruyor: %" PRIu32 L"%%   |   %.1f MB/s   |   simdiye kadar %" PRIu64
                     L" blok olculdu",
                     scanPct, st.scanSpeedMiBs.load(), (uint64_t)stats.blocksScanned);
            SetText(c.scanInfo, sbuf);
            // Every fourth tick: a 20 Hz rebuild of ten rows buys nothing a 5 Hz one does not.
            if ((++c.tick & 3) == 0) fillSurfaceStats();
        } else if (scanPct == 0 && stats.blocksScanned == 0) {
            SetText(c.scanInfo, L"Yuzey taramasi: henuz calistirilmadi");
        } else {
            wchar_t sbuf[192];
            swprintf(sbuf, 192,
                     L"Son tarama bitti: %" PRIu64 L" blok olculdu   |   %" PRIu32
                     L" hatali blok   |   maks %" PRIu32 L" ms",
                     (uint64_t)stats.blocksScanned, stats.bad, stats.maxLatencyMs);
            SetText(c.scanInfo, sbuf);
        }
        SendMessageW(c.scanBar, PBM_SETPOS, scanPct, 0);

        // A fast job can finish inside one timer tick, so the busy->idle edge alone is not
        // enough: the records carry a revision and the tables follow it.
        const uint64_t rev = st.disksRevision.load();
        if (rev != c.lastRevision || (c.wasBusy && !busy)) {
            c.lastRevision = rev;
            fillDiskList();
            fillDetail();
            fillSmartTable();
            fillParts();
            fillSurfaceStats();
            GetLocalTime(&c.lastScan);
            c.lastScanValid = true;
        }
        // The strip carries live queue/selection facts, so it rebuilds every tick; it only
        // invalidates its own rectangle when the rendered signature actually changed.
        BuildStatusPanes();
        c.wasBusy = busy;
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType == ODT_STATIC) {
            // Note strip: pale field with a left rule, so it reads as machine output and not
            // as something the user is meant to edit.
            HDC dc = dis->hDC;
            RECT r = dis->rcItem;
            FillRectColor(dc, r, color::noteBg);
            RECT bar = {r.left, r.top, r.left + S(3), r.bottom};
            FillRectColor(dc, bar, color::noteEdge);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, color::text);
            HFONT of = (HFONT)SelectObject(dc, c.fontUi);
            RECT inner = {r.left + S(12), r.top + S(5), r.right - S(8), r.bottom};
            wchar_t text[1024] = L"";
            GetWindowTextW(dis->hwndItem, text, 1024);
            DrawTextW(dc, text, -1, &inner, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, of);
            return TRUE;
        }
        if (dis->CtlType != ODT_BUTTON) return FALSE;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const int id = (int)dis->CtlID;
        const bool isTab = id >= ID_TAB_GENEL && id < ID_TAB_GENEL + kTabCount;
        const bool activeTab = isTab && (id - ID_TAB_GENEL) == c.activeTab;

        COLORREF bg = pressed ? RGB(232, 232, 232) : color::card;
        COLORREF fg = color::text;
        COLORREF edge = RGB(214, 214, 214);
        const bool risky = id == ID_BTN_OVERWRITE || id == ID_BTN_PURGE ||
                           id == ID_BTN_WIPE_TABLES || id == ID_BTN_PSID ||
                           id == ID_BTN_SET_PASS || id == ID_BTN_DISABLE || id == ID_BTN_FREEZE ||
                           id == ID_BTN_SMART_CLEARLOG;
        if (risky) {
            fg = color::fail;
            edge = color::failEdge;
            bg = pressed ? color::failBg : RGB(253, 250, 250);
        }
        if (id == ID_BTN_CONFIRM_YES) {
            bg = pressed ? RGB(28, 28, 28) : color::ink;
            fg = RGB(255, 255, 255);
            edge = color::ink;
        }
        if (isTab) {
            bg = color::card;
            fg = activeTab ? color::text : color::textMuted;
            edge = color::card;
        }
        if (dis->itemState & ODS_DISABLED) {
            bg = RGB(245, 245, 245);
            fg = color::textFaint;
            edge = RGB(235, 235, 235);
        }

        HDC dc = dis->hDC;
        RECT r = dis->rcItem;
        FillRectColor(dc, r, bg);
        if (!isTab) FrameRectColor(dc, r, edge);
        if (activeTab) {
            // The strip is vertical, so the active marker is the edge that faces the content
            // instead of the underline a horizontal tab row would use.
            RECT marker = {r.right - S(3), r.top + S(4), r.right, r.bottom - S(4)};
            FillRectColor(dc, marker, color::ink);
        }

        wchar_t text[128] = L"";
        GetWindowTextW(dis->hwndItem, text, 128);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        HFONT of = (HFONT)SelectObject(dc, activeTab ? c.fontBold : c.fontUi);
        RECT inner = r;
        InflateRect(&inner, -S(8), 0);
        DrawTextW(dc, text, -1, &inner,
                  (isTab ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
        SelectObject(dc, of);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        const HWND h = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (h == c.confirmInfo || h == c.confirmReq) {
            SetTextColor(dc, h == c.confirmReq ? color::warn : color::text);
            SetBkColor(dc, color::warnBg);
            return (LRESULT)c.brushWarn;
        }
        // Field labels: small, muted, uppercase - the value is what deserves the contrast.
        SetTextColor(dc, color::textMuted);
        SetBkColor(dc, color::card);
        return (LRESULT)c.brushCard;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HDC mem = nullptr;
        HBITMAP bmp = nullptr, oldBmp = nullptr;
        if (BeginBufferedPaint(hwnd, hdc, rc, mem, bmp, oldBmp)) {
            FillRectColor(mem, rc, color::window);

            const int headH = S(44);
            RECT band = {0, 0, rc.right, headH};
            FillRectColor(mem, band, color::headerBand);
            HLine(mem, 0, rc.right, headH - 1, color::border);
            SetBkMode(mem, TRANSPARENT);
            HFONT of = (HFONT)SelectObject(mem, c.fontBold);
            SetTextColor(mem, color::text);
            const wchar_t* appTitle = L"DiskUnlock Studio";
            const int titleW = TextW(mem, appTitle);
            RECT title = {S(16), 0, S(16) + titleW + S(8), headH};
            DrawTextW(mem, appTitle, -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, of);
            HFONT sf = (HFONT)SelectObject(mem, c.fontLabel);
            SetTextColor(mem, color::textFaint);
            RECT sub = {title.right + S(10), 0, rc.right - S(600), headH};
            DrawTextW(mem, L"ATA SECURITY  ·  TCG / OPAL  ·  NIST 800-88r1  ·  SCSI / SAT",
                      -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(mem, sf);

            for (const Card& card : c.cards) {
                RECT body = card.rc;
                if (body.right <= body.left || body.bottom <= body.top) continue;
                FillRectColor(mem, body, color::card);
                FrameRectColor(mem, body, color::border);
                if (!card.title[0]) continue;
                const int hh = S(30);
                RECT head = {body.left + 1, body.top + 1, body.right - 1, body.top + hh};
                FillRectColor(mem, head, color::cardHead);
                HLine(mem, head.left, head.right, head.bottom - 1, color::borderSoft);
                HFONT tf = (HFONT)SelectObject(mem, c.fontLabel);
                SetTextColor(mem, color::textMuted);
                RECT t = {head.left + S(12), head.top, head.right - S(8), head.bottom - 1};
                DrawTextW(mem, card.title, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(mem, tf);
            }

            if (c.tabRule.right > c.tabRule.left)
                VLine(mem, c.tabRule.left, c.tabRule.top, c.tabRule.bottom, color::borderSoft);

            // Summary tiles: label, the number in large type, an optional bar, and the one-line
            // reason underneath so a "bilinmiyor" is never mistaken for a zero.
            for (const Tile& t : c.tiles) {
                if (t.rc.right <= t.rc.left || t.rc.bottom <= t.rc.top) continue;
                SetBkMode(mem, TRANSPARENT);
                FrameRectColor(mem, t.rc, color::borderSoft);
                HFONT lf = (HFONT)SelectObject(mem, c.fontLabel);
                SetTextColor(mem, color::textMuted);
                RECT lab = {t.rc.left + S(10), t.rc.top + S(5), t.rc.right - S(8),
                            t.rc.top + S(21)};
                DrawTextW(mem, t.label.c_str(), -1, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                  DT_NOPREFIX);
                SelectObject(mem, lf);

                HFONT gf = (HFONT)SelectObject(mem, c.fontGauge);
                const COLORREF vfg = t.pct < 0   ? color::textFaint
                                   : t.pct < 50  ? color::fail
                                   : t.pct < 80  ? color::warn
                                                 : color::text;
                SetTextColor(mem, vfg);
                RECT val = {t.rc.left + S(10), t.rc.top + S(21), t.rc.right - S(8),
                            t.rc.top + S(53)};
                DrawTextW(mem, t.value.c_str(), -1, &val,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(mem, gf);

                int subTop = t.rc.top + S(56);
                if (t.pct >= 0) {
                    RECT track = {t.rc.left + S(10), t.rc.top + S(54), t.rc.right - S(10),
                                  t.rc.top + S(60)};
                    FillRectColor(mem, track, color::noteBg);
                    RECT fill = track;
                    fill.right = track.left +
                                 (int)((long long)(track.right - track.left) * t.pct / 100);
                    FillRectColor(mem, fill, color::ink);
                    subTop = t.rc.top + S(66);
                }
                HFONT sf = (HFONT)SelectObject(mem, c.fontLabel);
                SetTextColor(mem, color::textFaint);
                RECT sub = {t.rc.left + S(10), subTop, t.rc.right - S(8), t.rc.bottom - S(4)};
                DrawTextW(mem, t.sub.c_str(), -1, &sub,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(mem, sf);
            }

            if (c.pendingAction && c.band.right > c.band.left) {
                FillRectColor(mem, c.band, color::warnBg);
                FrameRectColor(mem, c.band, RGB(232, 211, 160));
                RECT rule = {c.band.left, c.band.top, c.band.left + S(3), c.band.bottom};
                FillRectColor(mem, rule, color::warnEdge);
            }
            DrawStatusStrip(mem);
            EndBufferedPaint(hdc, rc, mem, bmp, oldBmp);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_NOTIFY:
        return OnNotify(hwnd, (NMHDR*)lp);

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const std::wstring pass = EditText(c.pass);
        const bool master = checked(c.master);
        switch (id) {
        case ID_TAB_GENEL:
        case ID_TAB_SMART:
        case ID_TAB_LOGS:
        case ID_TAB_SURFACE:
        case ID_TAB_PARTS:
        case ID_TAB_SEC:
        case ID_TAB_ERASE:
            showTab(id - ID_TAB_GENEL);
            return 0;
        case ID_BTN_REFRESH: workers::postRefresh(); break;
        case ID_BTN_DEEP: workers::postDeepInspect(); break;
        case ID_BTN_LBA0: workers::postInspectLba0(); break;
        case ID_BTN_SCAN: workers::postSurfaceScan(); break;
        case ID_BTN_REPORT: exportReport(hwnd); break;
        case ID_BTN_CLEARLOG: SetText(c.log, L""); break;
        case ID_BTN_PROBE_LAYOUT: workers::postRelayout(); break;
        case ID_BTN_RO_ON: workers::postSetDiskReadOnly(true); break;
        case ID_BTN_RO_OFF: workers::postSetDiskReadOnly(false); break;
        case ID_BTN_OFF_ON: workers::postSetDiskOffline(true); break;
        case ID_BTN_OFF_OFF: workers::postSetDiskOffline(false); break;
        case ID_BTN_SET_PASS:
            requestDestructive(L"SECURITY SET PASSWORD mevcut parolanin ustune yazar",
                               [=] { workers::postAtaSetPassword(pass, master); });
            break;
        case ID_BTN_UNLOCK: workers::postAtaUnlock(pass, master); break;
        case ID_BTN_DISABLE:
            requestDestructive(L"SECURITY DISABLE PASSWORD surucunun korumasini kaldirir",
                               [=] { workers::postAtaDisablePassword(pass, master); });
            break;
        case ID_BTN_FREEZE:
            requestDestructive(L"FREEZE LOCK power cycle olmadan geri alinamaz",
                               [] { workers::postAtaFreeze(); });
            break;
        case ID_BTN_TCG_DISCO: workers::postTcgDiscover(); break;
        case ID_BTN_PSID: {
            const std::wstring psid = EditText(c.psid);
            requestDestructive(L"PSID REVERT tum kilitleri ve PBR'yi kaldirir",
                               [=] { workers::postPsidRevert(psid); });
            break;
        }
        case ID_BTN_WIPE_TABLES:
            requestDestructive(L"MBR/GPT sifirlama", [] { workers::postWipePartitionTables(); });
            break;
        case ID_BTN_OVERWRITE: {
            const int sel = (int)SendMessageW(c.profile, CB_GETCURSEL, 0, 0);
            const auto profile = sel == 1   ? hw::erase::Profile::SinglePassRandom
                                : sel == 2  ? hw::erase::Profile::DoDStyleThreePass
                                            : hw::erase::Profile::NistClearOnePass;
            requestDestructive(std::wstring(L"TUM DISK EZME (") +
                                   hw::erase::profileLabel(profile) + L")",
                               [=] { workers::postOverwrite(profile); });
            break;
        }
        case ID_BTN_PURGE: {
            const bool enh = checked(c.enhanced);
            requestDestructive(L"DONANIM PURGE (ATA SECURITY ERASE UNIT / NVMe)",
                               [=] { workers::postHardwarePurge(enh, pass, master); });
            break;
        }
        case ID_BTN_CANCEL: workers::postStopActiveJob(); break;
        case ID_BTN_SMART_LOGS: workers::postSmartLogs(); break;
        case ID_BTN_SMART_ON:
            workers::postSmartControl(workers::SmartControl::enableOperations);
            break;
        case ID_BTN_SMART_OFF:
            workers::postSmartControl(workers::SmartControl::disableOperations);
            break;
        case ID_BTN_AUTOSAVE_ON:
            workers::postSmartControl(workers::SmartControl::autosaveOn);
            break;
        case ID_BTN_AUTOSAVE_OFF:
            workers::postSmartControl(workers::SmartControl::autosaveOff);
            break;
        case ID_BTN_DST_SHORT:
            workers::postSmartControl(workers::SmartControl::selfTestShort);
            break;
        case ID_BTN_DST_LONG:
            workers::postSmartControl(workers::SmartControl::selfTestExtended);
            break;
        case ID_BTN_DST_ABORT:
            workers::postSmartControl(workers::SmartControl::selfTestAbort);
            break;
        case ID_BTN_SMART_CLEARLOG:
            requestDestructive(
                L"SMART hata logu sifirlama: surucunun tuttugu son 5 hatanin kaydi silinir",
                [] { workers::postSmartControl(workers::SmartControl::clearErrorLog); });
            break;
        case ID_BTN_BASELINE_TAKE: workers::postSmartBaseline(true); break;
        case ID_BTN_BASELINE_CLEAR: workers::postSmartBaseline(false); break;
        case ID_BTN_CONFIRM_YES: {
            // Also reachable from the Enter accelerator, so a stray Enter outside the confirm
            // band must stay silent instead of logging a failed confirmation.
            if (!c.pendingAction) break;
            const app::DiskRecord* r = app::selectedDisk();
            const std::wstring typed = EditText(c.confirmEdit);
            if (r && typed == workers::confirmationPhrase(r->number) && c.pendingAction) {
                app::The().destructiveOverride = typed;
                auto act = c.pendingAction;
                cancelDestructive();
                act();
                core::LogWarn(L"[onay] kullanici onay deyimiyle islemi baslatti");
            } else {
                core::LogWarn(L"[onay] deyimi eslesmedi - islem baslamadi");
                MessageBeep(MB_ICONWARNING);
            }
            break;
        }
        case ID_BTN_CONFIRM_NO:
            if (!c.pendingAction) break;
            cancelDestructive();
            break;
        default: break;
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        KillTimer(hwnd, (UINT_PTR)ID_TIMER_DEVICE);
        if (g_diskNotifier) {
            UnregisterDeviceNotification(g_diskNotifier);
            g_diskNotifier = nullptr;
        }
        if (g_accel) {
            DestroyAcceleratorTable(g_accel);
            g_accel = nullptr;
        }
        workers::postStopActiveJob();
        PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY:
        // Children are gone by this point, so no control can still be holding one of these
        // handles when the GDI objects are released.
        if (c.fontUi) DeleteObject(c.fontUi);
        if (c.fontBold) DeleteObject(c.fontBold);
        if (c.fontLabel) DeleteObject(c.fontLabel);
        if (c.fontMono) DeleteObject(c.fontMono);
        if (c.brushWindow) DeleteObject(c.brushWindow);
        if (c.brushCard) DeleteObject(c.brushCard);
        if (c.brushWarn) DeleteObject(c.brushWarn);
        c = Ctrls{};
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Only the rows need help here: the flat themed header already matches the light palette, and
// the system selection blue would be the loudest thing on the screen.
LRESULT OnCustomDraw(NMHDR* nh) {
    NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)nh;
    HWND lv = nullptr;
    for (HWND h : {c.disks, c.detail, c.smart}) {
        if (h && nh->hwndFrom == h) lv = h;
    }
    if (!lv) return CDRF_DODEFAULT;
    const int i = (int)cd->nmcd.dwItemSpec;

    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        const bool selected =
            (ListView_GetItemState(lv, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        // Alternating rows are what makes a 30-field inspector readable without a ruler.
        cd->clrTextBk = selected ? color::selRow : ((i & 1) ? color::rowAlt : color::card);
        cd->clrText = color::text;
        return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    }
    case (CDDS_PREPAINT | CDDS_SUBITEM): {
        const bool selected =
            (ListView_GetItemState(lv, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        cd->clrTextBk = selected ? color::selRow : ((i & 1) ? color::rowAlt : color::card);
        COLORREF fg = color::text;
        if (lv == c.disks && cd->iSubItem == 4 && i < (int)c.diskStatusColor.size()) {
            fg = c.diskStatusColor[(size_t)i];
        } else if (lv == c.detail && cd->iSubItem == 1) {
            wchar_t key[64] = L"";
            ListView_GetItemText(lv, i, 0, key, 64);
            if (wcscmp(key, L"ENGEL") == 0) fg = color::fail;
        } else if (lv == c.detail && cd->iSubItem == 0) {
            fg = color::textMuted;
        }
        cd->clrText = fg;
        return CDRF_DODEFAULT;
    }
    case CDDS_ITEMPOSTPAINT: {
        // The selection marker is a grey row plus a dark left rule - no accent colour.
        if (!ListView_GetItemState(lv, i, LVIS_SELECTED)) return CDRF_DODEFAULT;
        HDC dc = cd->nmcd.hdc;
        RECT r = cd->nmcd.rc;
        RECT bar = {r.left + 1, r.top + 1, r.left + 1 + S(3), r.bottom - 1};
        FillRectColor(dc, bar, color::ink);
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

LRESULT OnNotify(HWND hwnd, NMHDR* nh) {
    if (nh->code == NM_CUSTOMDRAW) return OnCustomDraw(nh);
    // A dragged header divider means the user wants these widths; from then on only the filler
    // column tracks the window, so a manual fix never gets stomped by the next refresh.
    // HDN_ENDTRACK is the only reliable signal - HDN_ITEMCHANGED also fires for the widths this
    // function sets itself, which would mark every grid manual on the first repaint.
    if (nh->code == HDN_ENDTRACK) {
        HWND owner = nh->hwndFrom;
        if (owner == ListView_GetHeader(c.disks)) c.gridManual |= 1;
        else if (owner == ListView_GetHeader(c.detail)) c.gridManual |= 2;
        else if (owner == ListView_GetHeader(c.smart)) c.gridManual |= 4;
        else if (owner == ListView_GetHeader(c.parts)) c.gridManual |= 8;
        else if (owner == ListView_GetHeader(c.smartLog)) c.gridManual |= 16;
        else if (owner == ListView_GetHeader(c.surface)) c.gridManual |= 32;
        return 0;
    }
    if (nh->idFrom == ID_LV_DISKS && nh->code == LVN_ITEMCHANGED) {
        NMLISTVIEW* n = (NMLISTVIEW*)nh;
        if ((n->uChanged & LVIF_STATE) && (n->uNewState & LVIS_SELECTED)) {
            app::The().selectIndex(n->iItem);
            fillDetail();
            fillSmartTable();
            fillParts();
            // The surface table and the gauges are per-drive: the scan belongs to whichever
            // drive ran it, so picking another drive must re-ask that question immediately.
            fillSurfaceStats();
            UpdateTiles();
            BuildStatusPanes();
            // Asking the drive for its own health facts is a read, so a selection is enough to
            // trigger it - the gauges should not wait for a button. The job drops out on its own
            // when this drive already answered or the SAT path is known to refuse it.
            workers::postSmartRead();
        }
    }
    (void)hwnd;
    return 0;
}

// All geometry is authored in 96-DPI units and scaled once, so a 150%/200% panel gets a
// usable window instead of the tiny fixed-pixel layout of the first version.
//
// The shell is the shape the old two-column layout kept growing out of: the drive list owns the
// left column, the selected drive's gauges own the strip above the page, and every page carries
// exactly one table plus one explanation line. Nothing is placed twice - the previous SMART page
// packed two grids and a four-line note beside thirteen buttons, which is what made it unread.
void LayoutMain() {
    if (!c.main) return;
    RECT rc;
    GetClientRect(c.main, &rc);
    const int W = rc.right, H = rc.bottom;

    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h) SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    const int pad = S(16);
    const int headH = S(44);
    const int gap = S(12);
    const int cardHead = S(30);
    const int statusH = S(26);
    const int logCardH = S(170);
    const int barH = S(10);
    const int fieldH = S(28);
    const int labelH = S(16);
    const int rowGap = S(8);
    const int bandH = (c.confirmEdit && IsWindowVisible(c.confirmEdit)) ? S(46) : 0;
    const bool bandVisible = bandH != 0;

    const int bodyTop = headH + gap;
    const int logTop = H - statusH - logCardH - S(6);
    const int contentBottom = logTop - gap - bandH - S(6);

    // Toolbar inside the title strip, right-aligned. Report/clear moved into the log card's
    // own header, because they act on that surface and not on the window.
    int bx = W - pad;
    struct Item { HWND h; int w; };
    for (Item it : {Item{c.btnLba0, 92}, Item{c.btnRefresh, 104}}) {
        const int w = S(it.w);
        bx -= w + S(6);
        place(it.h, bx, (headH - S(26)) / 2, w, S(26));
    }

    // The list carries seven short columns; below this width the model name is the first thing
    // to become unreadable, and a drive you cannot name is a drive you should not act on.
    int leftW = S(470);
    const int minRight = S(600);
    if (W - leftW - pad * 3 < minRight) leftW = (std::max)(S(200), W - minRight - pad * 3);

    const int rightX = pad + leftW + gap;
    const int rightW = W - rightX - pad;
    // label + 26pt value + bar + up to three wrapped reason lines.
    const int gaugeH = S(156);

    const Card disksCard{{pad, bodyTop, pad + leftW, contentBottom}, L"SURUCULER"};
    const Card gaugeCard{{rightX, bodyTop, rightX + rightW, bodyTop + gaugeH},
                         L"SECILI SURUCU - OZET"};
    const Card pageCard{{rightX, bodyTop + gaugeH + gap, rightX + rightW, contentBottom}, L""};
    const Card logCard{{pad, logTop, W - pad, logTop + logCardH}, L"IS GUNLUGU"};
    c.cards.clear();
    c.cards.push_back(disksCard);
    c.cards.push_back(gaugeCard);
    c.cards.push_back(pageCard);
    c.cards.push_back(logCard);

    // Content area of a card: below its title band, inside its 1px border.
    auto inner = [cardHead](const Card& card) {
        RECT r = card.rc;
        r.top += card.title[0] ? cardHead : 0;
        r.left += 1;
        r.right -= 1;
        r.bottom -= 1;
        return r;
    };

    RECT dl = inner(disksCard);
    place(c.disks, dl.left, dl.top, dl.right - dl.left, dl.bottom - dl.top);
    AutoFitGrid(c.disks, 1, 1);

    // Gauge tiles: the rectangles live here, the text lives in UpdateTiles, and neither of
    // them needs the other to run first - so a resize and a refresh cannot race for a tile.
    {
        const RECT gr = inner(gaugeCard);
        const int m = S(8), tileGap = S(10);
        const int tw = (std::max)(S(112), (((int)gr.right - (int)gr.left) - 2 * m - 4 * tileGap) / 5);
        c.tileSlots.clear();
        for (int i = 0; i < 5; ++i) {
            const int x0 = gr.left + m + i * (tw + tileGap);
            c.tileSlots.push_back(RECT{x0, gr.top + m, x0 + tw, gr.bottom - m});
        }
        for (size_t i = 0; i < c.tiles.size() && i < c.tileSlots.size(); ++i)
            c.tiles[i].rc = c.tileSlots[i];
    }

    // Page strip: the tabs run down the left edge of the page card so the seven pages read as
    // one navigation column instead of seven buttons competing with the content for width.
    const RECT pr = inner(pageCard);
    const int stripW = S(136);
    const int stripX = pr.left + S(8);
    const int tabStep = S(34);
    for (int t = 0; t < kTabCount; ++t)
        place(c.tabBtn[t], stripX, pr.top + S(8) + t * tabStep, stripW, tabStep - S(4));
    const int ruleX = stripX + stripW + S(8);
    c.tabRule = {ruleX, pr.top + S(6), ruleX + 1, pr.bottom - S(6)};

    const int cx = ruleX + S(14);
    const int cw = (std::max)(S(220), (int)pr.right - S(12) - cx);
    const int pageTop = pr.top + S(8);
    const int pageBottom = (int)pr.bottom - S(10);

    // Weighted slots: the same row helper places three command buttons and a
    // "field + checkbox + button" form row without either needing its own arithmetic.
    auto rowAt = [&](int y, std::initializer_list<std::pair<HWND, int>> items, int hh) {
        int total = 0;
        for (const auto& it : items) total += it.second;
        const int gutter = S(8) * ((int)items.size() - 1);
        int x = cx;
        for (const auto& it : items) {
            const int w = (cw - gutter) * it.second / total;
            place(it.first, x, y, w, hh);
            x += w + S(8);
        }
        return y + hh + rowGap;
    };
    auto labelAt = [&](int y, HWND lbl) { return rowAt(y, {{lbl, 1}}, labelH); };
    // The grid is the only flexible part of a page: a short window takes height from the table,
    // never from the buttons or from the line that explains them.
    auto gridThenNote = [&](int y, HWND lv, int manualBit, int filler, HWND note, int noteH) {
        const int gridH = (std::max)(S(56), (pageBottom - noteH) - y - rowGap);
        place(lv, cx, y, cw, gridH);
        AutoFitGrid(lv, manualBit, filler);
        place(note, cx, pageBottom - noteH, cw, noteH);
    };

    // --- page 0: Genel -------------------------------------------------------------
    // The inspector is the drive's own reported fields; the note repeats the two derivations
    // the gauges above are built from, in the words of the formulas.
    gridThenNote(pageTop, c.detail, 2, 1, c.noteGenel, S(72));

    // --- page 1: SMART -------------------------------------------------------------
    // Reads and anchors first, then the self-test commands, then the four SMART-operational
    // writes; only the last row needs raw write access, and it is visibly its own row.
    int y = rowAt(pageTop, {{c.btnDeep, 1}, {c.btnBaselineTake, 1}, {c.btnBaselineClear, 1}},
                  fieldH);
    y = rowAt(y, {{c.btnDstShort, 1}, {c.btnDstLong, 1}, {c.btnDstAbort, 1}}, fieldH);
    y = rowAt(y,
              {{c.btnSmartOn, 1}, {c.btnSmartOff, 1}, {c.btnAutosaveOn, 1}, {c.btnAutosaveOff, 1}},
              fieldH);
    y = labelAt(y, c.lblSmart);
    gridThenNote(y, c.smart, 4, 6, c.smartInfo, S(72));

    // --- page 2: the drive's own logs ----------------------------------------------
    // The reset sits next to the read on purpose: seeing the entry you are about to erase is
    // the only context that makes pressing it a decision instead of a guess.
    y = rowAt(pageTop, {{c.btnSmartLogs, 1}, {c.btnSmartClearLog, 1}, {(HWND)nullptr, 1}}, fieldH);
    y = labelAt(y, c.lblSmartLog);
    gridThenNote(y, c.smartLog, 16, 4, c.noteLogs, S(60));

    // --- page 3: surface -----------------------------------------------------------
    // Progress bar beside the button: while a scan runs, the only two numbers that matter are
    // the one that started it and how far it has got.
    y = rowAt(pageTop, {{c.btnScan, 1}, {c.scanBar, 2}}, fieldH - S(8));
    gridThenNote(y, c.surface, 32, 1, c.scanInfo, S(60));

    // --- page 4: partitions / mount --------------------------------------------------
    y = rowAt(pageTop, {{c.btnProbeLayout, 1}, {c.btnRoOn, 1}, {c.btnRoOff, 1}}, fieldH);
    y = rowAt(y, {{c.btnOffOn, 1}, {c.btnOffOff, 1}, {(HWND)nullptr, 1}}, fieldH);
    gridThenNote(y, c.parts, 8, 7, c.partsInfo, S(56));

    // --- page 5: Guvenlik ------------------------------------------------------------
    // ATA and TCG keep their own block and their own state line: a password is not a PSID and
    // the two refusals read completely differently.
    y = labelAt(pageTop, c.lblPass);
    y = rowAt(y, {{c.pass, 2}, {c.master, 1}, {c.btnSetPass, 1}}, fieldH);
    y = rowAt(y, {{c.btnUnlock, 1}, {c.btnDisable, 1}, {c.btnFreeze, 1}}, fieldH);
    place(c.ataInfo, cx, y, cw, S(56));
    y += S(56) + rowGap;
    y = labelAt(y, c.lblPsid);
    y = rowAt(y, {{c.psid, 2}, {c.btnTcgDisco, 1}, {c.btnPsid, 1}}, fieldH);
    place(c.tcgInfo, cx, pageBottom - S(48), cw, S(48));

    // --- page 6: Silme / Sanitize ----------------------------------------------------
    y = labelAt(pageTop, c.lblProfile);
    y = rowAt(y, {{c.profile, 3}, {c.enhanced, 1}}, fieldH);
    y = rowAt(y, {{c.btnWipeTables, 1}, {c.btnOverwrite, 1}, {c.btnPurge, 1}, {c.btnCancel, 1}},
              fieldH);
    place(c.wipeBar, cx, y, cw, barH);
    place(c.wipeInfo, cx, pageBottom - S(56), cw, S(56));

    if (bandVisible) {
        c.band = {pad, contentBottom + gap, W - pad, contentBottom + gap + bandH};
        int rx = (int)c.band.right - S(12);
        const int noW = S(90), yesW = S(170), editW = S(170);
        rx -= noW;
        place(c.confirmNo, rx, c.band.top + S(10), noW, S(26));
        rx -= yesW + S(6);
        place(c.confirmYes, rx, c.band.top + S(10), yesW, S(26));
        rx -= editW + S(12);
        place(c.confirmEdit, rx, c.band.top + S(9), editW, S(26));
        const int labelW = (std::max)(S(200), rx - (int)c.band.left - S(14));
        place(c.confirmInfo, c.band.left + S(14), c.band.top + S(7), labelW, S(18));
        place(c.confirmReq, c.band.left + S(14), c.band.top + S(26), labelW, S(16));
    } else {
        SetRect(&c.band, 0, 0, 0, 0);
    }

    RECT lg = inner(logCard);
    place(c.log, lg.left, lg.top, lg.right - lg.left, lg.bottom - lg.top);
    int lx = lg.right - S(6);
    for (Item it : {Item{c.btnClearLog, 96}, Item{c.btnReport, 112}}) {
        const int w = S(it.w);
        lx -= w + S(6);
        place(it.h, lx, (int)logCard.rc.top + 1, w, cardHead - S(5));
    }

    c.statusRc = {0, H - statusH, W, H};
    InvalidateStatusStrip();
}

}  // namespace

HWND CreateMainWindow(HINSTANCE inst, int showCmd) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1001));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return nullptr;

    // Authored size; WM_CREATE re-checks it against the window's monitor and the work area.
    Dpi::setSystem();
    const int w = Dpi::current().scale(1420), h = Dpi::current().scale(880);
    HWND hwnd = CreateWindowExW(0, kWindowClass,
                                L"DiskUnlock Studio - SCSI/SAT low-level motoru",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    // The tool is a two-column console: at 1366x768 the authored size already fills the work
    // area, and on anything larger the extra room goes to the tables instead of to margins.
    ShowWindow(hwnd, showCmd == SW_SHOWMINIMIZED ? showCmd : SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);
    return hwnd;
}

HACCEL MainWindowAccel() { return g_accel; }

}  // namespace ui
