#include "SettingsView.h"
#include "Lang.h"
#include "DarkMode.h"
#include "Settings.h"
#include "DWriteText.h"

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Settings";
// 96-DPI units. SettingsPage.xaml: SidebarView OpenPaneLength 240, header TextBlock Margin 16,12,0,4 (Subtitle 20px),
// items 32px rows; content ScrollViewer Padding 24,12,24,24, page title with 12px below, SettingsCards 4px apart
constexpr int kNavWidth = 240, kNavTop = 12 + 28 + 4 + 8, kNavItem = 32, kNavGap = 0, kPadX = 24, kHeader = 12 + 28 + 12, kCardGap = 4;
constexpr wchar_t kClearGlyph[] = L"\xE711"; // Cancel
// WinUI ToggleSwitch knob ~ ControlFasterAnimationDuration, page EntranceThemeTransition, NavigationView indicator
constexpr int kToggleMs = 167, kPageMs = 250, kNavMs = 200, kPageRise = 24;
constexpr UINT_PTR kAnimTimer = 1;

HFONT Derive(HFONT base, int px, int weight)
{
    LOGFONTW lf{};
    GetObjectW(base, sizeof(lf), &lf);
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

// Keys every Windows app keeps (clipboard, undo, window menu, system shortcuts)
bool Reserved(uint32_t code)
{
    static const wchar_t* const keys[] = { L"Ctrl+C", L"Ctrl+X", L"Ctrl+V", L"Ctrl+A", L"Ctrl+Z", L"Ctrl+Y", L"Ctrl+Insert", L"Shift+Insert",
        L"Shift+Delete", L"Alt+F4", L"Alt+Space", L"Alt+Tab", L"Alt+Enter", L"Ctrl+Esc", L"Shift+F10", L"Ctrl+Alt+Delete", L"F1", L"F2", L"F10" };
    for (const wchar_t* k : keys)
        if (ParseShortcut(k) == code)
            return true;
    return false;
}

int TextHeight(HDC hdc, HFONT font, const std::wstring& text, int width)
{
    if (DWriteText::Available())
        return DWriteText::WrappedHeight(hdc, font, text, width);
    HGDIOBJ old = SelectObject(hdc, font);
    RECT r{ 0, 0, width, 0 };
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
    return r.bottom;
}

void DrawWrapped(HDC hdc, HFONT font, const std::wstring& text, RECT r, COLORREF color)
{
    if (DWriteText::Available())
    {
        DWriteText::DrawWrapped(hdc, font, text, r, color, DT_LEFT);
        return;
    }
    HGDIOBJ old = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
}

// WinUI Button / ComboBox face on a SettingsCard: ControlFillColorDefault / Secondary / Tertiary over the card,
// ControlElevationBorder (ControlStrokeColorDefault, the bottom edge ControlStrokeColorSecondary in light mode)
void ControlFace(HDC hdc, const Theme& t, const RECT& rc, bool hot, bool down)
{
    const COLORREF white = t.fillWhite, black = t.fillBlack;
    COLORREF fill;
    if (t.dark)
        fill = Mix(t.card, white, down ? 3 : hot ? 8 : 6);    // #08 / #15 / #0F FFFFFF
    else
        fill = Mix(t.card, t.altFill, down ? 30 : hot ? 50 : 70); // #4DF9F9F9 / #80F9F9F9 / #B3FFFFFF
    const int radius = t.S(4);
    FillRound(hdc, rc, radius, fill);
    FrameRound(hdc, rc, radius, t.dark ? Mix(t.card, white, 7) : Mix(t.card, black, 6)); // #12FFFFFF / #0F000000
    if (!t.dark && !down)
    {
        const RECT bottom{ rc.left + radius, rc.bottom - 1, rc.right - radius, rc.bottom };
        FillSolid(hdc, bottom, Mix(t.card, black, 16)); // #29000000
    }
}

// Files' color button content: a 24px checkerboard under the color
void DrawColorSwatch(HDC hdc, const Theme& t, const RECT& rc, uint32_t argb)
{
    // LightGray squares over Gray, the color (with its alpha) on top
    const int w = rc.right - rc.left, cell = std::max(1, w / 4);
    const int a = static_cast<int>(argb >> 24) * 100 / 255;
    const COLORREF c = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            const RECT r{ rc.left + x * cell, rc.top + y * cell, x == 3 ? rc.right : rc.left + (x + 1) * cell, y == 3 ? rc.bottom : rc.top + (y + 1) * cell };
            FillSolid(hdc, r, Mix((x + y) % 2 ? t.checkerLight : t.checkerDark, c, a));
        }
}

// SettingsCard content starts after the HeaderIcon (20px + 20px margin)
int LabelLeft(const Theme& t, const SettingsRow& row, int left)
{
    return left + t.S(row.icon ? 16 + 20 + 20 : row.swatch ? 40 : 16);
}

// WinUI ComboBox: items 36px apart (ComboBoxItem MinHeight 32 + 2 margins), 4px popup padding, MaxDropDownHeight 504
constexpr int kDropItem = 36, kDropPad = 4, kDropMax = 504;
// GridViewItem cards of Files' AppThemeResourcesItemTemplate: 120 wide, 16 + 50 + text rows, 4px apart, 8px padding
constexpr int kTileW = 120, kTileH = 16 + 50 + 28, kTileGap = 4, kTilePad = 8, kExpanderHeader = 68;
}

SettingsView::~SettingsView()
{
    m_smooth.Reset(m_scroll); // drops it from the frame clock before the window goes
    if (m_hwnd)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
    }
    for (HFONT f : { m_titleFont, m_headerFont, m_cardIcons })
        if (f)
            DeleteObject(f);
}

bool SettingsView::Create(HWND parent, const Theme* theme, int page)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEW(4));
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered)
        return false;
    m_theme = theme;
    m_pages = m_host->BuildSettingsPages();
    m_page = std::clamp(page, 0, std::max(0, static_cast<int>(m_pages.size()) - 1));
    if (!CreateWindowExW(0, kClass, Tr(L"设置"), WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr,
            GetModuleHandleW(nullptr), this))
        return false;
    Relayout();
    return true;
}

void SettingsView::Relayout()
{
    CloseDropdown();
    for (HFONT* f : { &m_titleFont, &m_headerFont, &m_cardIcons })
        if (*f)
            DeleteObject(*f);
    m_titleFont = Derive(m_theme->ui, m_theme->S(20), FW_SEMIBOLD);  // App.Theme.SubtitleTextBlockStyle
    m_headerFont = Derive(m_theme->ui, m_theme->S(14), FW_SEMIBOLD); // BodyStrong (group headers)
    m_cardIcons = Derive(m_theme->icons, m_theme->S(20), FW_NORMAL); // SettingsCard HeaderIcon size
    Layout();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsView::ShowPage(int page)
{
    if (page < 0 || page >= static_cast<int>(m_pages.size()))
        return;
    EndCapture();
    CloseDropdown();
    m_bubble = -1;
    if (page != m_page && m_hwnd && IsWindowVisible(m_hwnd) && AnimationsEnabled())
    {
        m_navFrom = m_page;
        m_navStart = m_pageStart = GetTickCount64();
        Animate();
    }
    m_toggleRow = -1;
    m_page = page;
    m_scroll = 0;
    m_focus = -1;
    m_message.clear();
    Layout();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsView::Rebuild()
{
    CloseDropdown();
    m_pages = m_host->BuildSettingsPages();
    m_page = std::clamp(m_page, 0, std::max(0, static_cast<int>(m_pages.size()) - 1));
    m_bubble = -1;
    Layout();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

RECT SettingsView::ContentClient() const
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    return { m_theme->S(kNavWidth), m_theme->S(kHeader), rc.right, rc.bottom };
}

RECT SettingsView::NavRect(int page) const
{
    const Theme& t = *m_theme;
    const int top = t.S(kNavTop) + page * (t.S(kNavItem) + t.S(kNavGap));
    return { t.S(8), top, t.S(kNavWidth) - t.S(8), top + t.S(kNavItem) }; // MenuItemsHost 4 + RootPanel 4 each side
}

void SettingsView::Layout()
{
    m_cells.clear();
    if (!m_hwnd || m_pages.empty())
        return;
    const Theme& t = *m_theme;
    const RECT client = ContentClient();
    const int left = client.left + t.S(kPadX), right = std::max<int>(left + t.S(320), client.right - t.S(kPadX));
    HDC hdc = GetDC(m_hwnd);
    int y = 0;
    const auto& rows = m_pages[m_page].rows;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const SettingsRow& row = rows[i];
        Cell cell;
        cell.row = i;
        int h = 0;
        switch (row.kind)
        {
        case SettingsRow::Kind::Header:
            if (y > 0)
                y += t.S(12);
            h = t.S(32);
            break;
        case SettingsRow::Kind::Note:
            h = TextHeight(hdc, t.caption, row.label, right - left - t.S(4)) + t.S(10);
            break;
        case SettingsRow::Kind::Shortcut:
            h = t.S(44);
            break;
        case SettingsRow::Kind::Colors:
        {
            // Expander header, then the GridView (Padding 8) of cards wrapping to the card width
            h = t.S(kExpanderHeader);
            const int count = static_cast<int>(row.options.size());
            if (!m_colorsCollapsed && count > 0)
            {
                const int inner = right - left - 2 * t.S(kTilePad);
                const int perRow = std::max(1, (inner + t.S(kTileGap)) / (t.S(kTileW) + t.S(kTileGap)));
                const int rowsN = (count + perRow - 1) / perRow;
                const int top = y + h + 1 + t.S(kTilePad);
                for (int k = 0; k < count; ++k)
                {
                    const int x = left + t.S(kTilePad) + (k % perRow) * (t.S(kTileW) + t.S(kTileGap));
                    const int ty = top + (k / perRow) * (t.S(kTileH) + t.S(kTileGap));
                    cell.tiles.push_back({ x, ty, x + t.S(kTileW), ty + t.S(kTileH) });
                }
                h += 1 + 2 * t.S(kTilePad) + rowsN * t.S(kTileH) + (rowsN - 1) * t.S(kTileGap);
            }
            break;
        }
        default:
            h = t.S(68); // SettingsCardMinHeight
            break;
        }
        cell.rc = { left, y, right, y + h };
        const int mid = y + (row.kind == SettingsRow::Kind::Colors ? t.S(kExpanderHeader) : h) / 2;
        switch (row.kind)
        {
        case SettingsRow::Kind::Toggle:
            cell.control = { right - t.S(16) - t.S(40), mid - t.S(10), right - t.S(16), mid + t.S(10) };
            break;
        case SettingsRow::Kind::Choice:
        {
            // ComboBox sizes to its widest item: 12 padding, 30 for the chevron
            int widest = 0;
            for (const auto& o : row.options)
                widest = std::max(widest, TextWidth(hdc, t.ui, o));
            const int w = std::clamp(widest + t.S(12 + 30), t.S(120), t.S(300));
            cell.control = { right - t.S(16) - w, mid - t.S(16), right - t.S(16), mid + t.S(16) };
            break;
        }
        case SettingsRow::Kind::Colors:
        {
            // Color button (4 + 24 + 4, then the 12px chevron with 8,4,10,4 margins) left of the expander chevron
            const int w = t.S(32 + 8 + 12 + 10), x = right - t.S(16 + 32 + 8);
            cell.control = { x - w, mid - t.S(16), x, mid + t.S(16) };
            break;
        }
        case SettingsRow::Kind::Button:
        case SettingsRow::Kind::Value:
            if (!row.button.empty())
            {
                const int w = std::max(t.S(96), TextWidth(hdc, t.ui, row.button) + t.S(32));
                cell.control = { right - t.S(16) - w, mid - t.S(16), right - t.S(16), mid + t.S(16) };
            }
            break;
        case SettingsRow::Kind::Shortcut:
            cell.clear = { right - t.S(8) - t.S(28), mid - t.S(14), right - t.S(8), mid + t.S(14) };
            cell.reset = { cell.clear.left - t.S(4) - t.S(76), mid - t.S(14), cell.clear.left - t.S(4), mid + t.S(14) };
            cell.control = { cell.reset.left - t.S(8) - t.S(300), mid - t.S(14), cell.reset.left - t.S(8), mid + t.S(14) };
            break;
        default:
            break;
        }
        // (i) right after the label text
        if (!row.detail.empty())
        {
            const bool header = row.kind == SettingsRow::Kind::Header;
            const int labelX = header ? left + t.S(2) : LabelLeft(t, row, left);
            const int textMid = header ? y + t.S(8) + (h - t.S(8)) / 2 : mid;
            const int x = labelX + TextWidth(hdc, header ? m_headerFont : t.ui, row.label) + t.S(4);
            cell.info = { x, textMid - t.S(12), x + t.S(24), textMid + t.S(12) };
        }
        m_cells.push_back(cell);
        y += h + (row.kind == SettingsRow::Kind::Header || row.kind == SettingsRow::Kind::Note ? 0 : t.S(kCardGap));
    }
    ReleaseDC(m_hwnd, hdc);
    m_content = y + t.S(24);
    if (m_focus >= static_cast<int>(m_cells.size()))
        m_focus = -1;
    if (m_hot >= static_cast<int>(m_cells.size()))
        m_hot = -1;
    m_smooth.ClampTarget(std::max(0, m_content - static_cast<int>(client.bottom - client.top)));
    ScrollTo(m_scroll);
}

bool SettingsView::WantsKey(UINT vk) const
{
    if (m_drop.cell >= 0)
        return true;
    return vk == VK_F4 && Focusable(m_focus) && m_pages[m_page].rows[m_cells[m_focus].row].kind == SettingsRow::Kind::Choice;
}

bool SettingsView::Focusable(int cell) const
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()))
        return false;
    const auto kind = m_pages[m_page].rows[m_cells[cell].row].kind;
    const auto& row = m_pages[m_page].rows[m_cells[cell].row];
    return kind != SettingsRow::Kind::Header && kind != SettingsRow::Kind::Note && !(kind == SettingsRow::Kind::Value && row.button.empty());
}

int SettingsView::NavAt(POINT pt) const
{
    for (int i = 0; i < static_cast<int>(m_pages.size()); ++i)
    {
        const RECT r = NavRect(i);
        if (PtInRect(&r, pt))
            return i;
    }
    return -1;
}

int SettingsView::CellAt(POINT pt, Part* part, int* tile) const
{
    *part = Part::None;
    if (tile)
        *tile = -1;
    const RECT client = ContentClient();
    if (!PtInRect(&client, pt))
        return -1;
    const POINT c{ pt.x, pt.y - client.top + m_scroll };
    for (int i = 0; i < static_cast<int>(m_cells.size()); ++i)
    {
        const Cell& cell = m_cells[i];
        if (!PtInRect(&cell.rc, c))
            continue;
        if (PtInRect(&cell.info, c))
        {
            *part = Part::Info;
            return i;
        }
        if (!Focusable(i))
            continue;
        const auto& row = m_pages[m_page].rows[cell.row];
        for (int k = 0; k < static_cast<int>(cell.tiles.size()); ++k)
            if (PtInRect(&cell.tiles[k], c))
            {
                *part = Part::Tile;
                if (tile)
                    *tile = k;
                return i;
            }
        if (row.kind == SettingsRow::Kind::Colors && c.y >= cell.rc.top + m_theme->S(kExpanderHeader))
            return -1; // the GridView's padding between the cards
        if (row.kind == SettingsRow::Kind::Shortcut && PtInRect(&cell.clear, c))
            *part = Part::Clear;
        else if (row.kind == SettingsRow::Kind::Shortcut && PtInRect(&cell.reset, c) && row.overridden && row.overridden())
            *part = Part::Reset;
        else if (PtInRect(&cell.control, c))
            *part = Part::Control;
        else
            *part = Part::Row;
        return i;
    }
    return -1;
}

void SettingsView::ScrollTo(int offset)
{
    const RECT client = ContentClient();
    const int max = std::max(0, m_content - static_cast<int>(client.bottom - client.top));
    offset = std::clamp(offset, 0, max);
    if (offset != m_scroll)
    {
        m_scroll = offset;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SettingsView::EnsureVisible(int cell)
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()))
        return;
    const RECT client = ContentClient();
    const RECT& r = m_cells[cell].rc;
    const int view = client.bottom - client.top;
    int y = m_scroll;
    if (r.top < m_scroll)
        y = r.top - m_theme->S(8);
    else if (r.bottom > m_scroll + view)
        y = r.bottom - view + m_theme->S(8);
    if (y == m_scroll)
        return;
    m_smooth.Reset(y); // a key / selection jump, not the wheel: goes straight there
    ScrollTo(y);
}

bool SettingsView::ThumbRect(RECT& thumb) const
{
    const RECT client = ContentClient();
    const int view = client.bottom - client.top;
    if (m_content <= view || view <= 0)
        return false;
    const int track = view - m_theme->S(4);
    const int h = std::max(m_theme->S(32), MulDiv(track, view, m_content));
    const int top = client.top + m_theme->S(2) + MulDiv(track - h, m_scroll, m_content - view);
    thumb = { client.right - m_theme->S(6), top, client.right - m_theme->S(2), top + h };
    return true;
}

void SettingsView::MoveFocus(int delta)
{
    const int n = static_cast<int>(m_cells.size());
    int i = m_focus;
    for (int step = 0; step < n; ++step)
    {
        i = i < 0 ? (delta > 0 ? 0 : n - 1) : i + delta;
        if (i < 0 || i >= n)
            return;
        if (Focusable(i))
        {
            m_focus = i;
            m_keyboard = true;
            EnsureVisible(i);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }
}
// --- Painting ---

void SettingsView::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    const int navW = t.S(kNavWidth);
    // Navigation pane: the same SidebarView as the main sidebar (flat list, no chevrons: SupportsExpansion=false)
    FillSolid(hdc, { 0, 0, navW, rc.bottom }, t.content); // inside the file area card, no divider (Files)
    DrawLabel(hdc, m_titleFont, Tr(L"设置"), { t.S(16), t.S(12), navW - t.S(8), t.S(12 + 28) }, t.ink);
    for (int i = 0; i < static_cast<int>(m_pages.size()); ++i)
    {
        const RECT row = NavRect(i);
        const RECT box{ row.left, row.top + t.S(2), row.right, row.bottom - t.S(2) };
        const bool selected = i == m_page, hot = i == m_navHot;
        if (selected)
        {
            FillRound(hdc, box, t.S(4), hot ? t.controlFillHover : t.controlFill);
            FrameRound(hdc, box, t.S(4), t.controlStroke);
        }
        else if (hot)
            FillRound(hdc, box, t.S(4), t.subtleHover);
        // [3 indicator][icon margin 8][16 icon][text margin 12]
        const int iconX = box.left + t.S(1 + 3 + 8);
        if (m_pages[i].glyph)
            DrawGlyph(hdc, t.icons, m_pages[i].glyph, { iconX, box.top, iconX + t.S(16), box.bottom }, t.ink);
        DrawLabel(hdc, t.ui, m_pages[i].name, { iconX + t.S(16 + 12), box.top, box.right - t.S(12), box.bottom - t.S(2) }, t.ink);
    }
    // Selection indicator: slides from the previous page's item to the new one
    if (m_page < static_cast<int>(m_pages.size()))
    {
        auto centerOf = [&](int page) { const RECT r = NavRect(page); return (r.top + r.bottom) / 2; };
        int cy = centerOf(m_page);
        if (m_navStart && m_navFrom >= 0 && m_navFrom < static_cast<int>(m_pages.size()))
        {
            const int from = centerOf(m_navFrom);
            cy = from + static_cast<int>((cy - from) * AnimProgress(m_navStart, kNavMs) + 0.5);
        }
        const int left = NavRect(m_page).left;
        FillRound(hdc, { left + t.S(1), cy - t.S(8), left + t.S(4), cy + t.S(8) }, t.S(2), t.accent);
    }
    // Page
    FillSolid(hdc, { navW, 0, rc.right, rc.bottom }, t.content);
    if (m_pages.empty())
        return;
    const RECT client = ContentClient();
    auto paintPage = [&](HDC dc)
    {
        DrawLabel(dc, m_titleFont, m_pages[m_page].name, { client.left + t.S(kPadX), t.S(12), client.right - t.S(kPadX), t.S(12 + 28) }, t.ink);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
        const int offset = client.top - m_scroll;
        for (const Cell& cell : m_cells)
            if (cell.rc.bottom + offset >= client.top && cell.rc.top + offset <= client.bottom)
                PaintRow(dc, cell, offset);
        PaintBubble(dc, offset);
        RestoreDC(dc, saved);
    };
    const double enter = m_pageStart ? AnimProgress(m_pageStart, kPageMs) : 1.0;
    HDC mem = enter < 1.0 ? CreateCompatibleDC(hdc) : nullptr;
    const int w = client.right - navW, h = client.bottom;
    HBITMAP bmp = mem && w > 0 && h > 0 ? CreateCompatibleBitmap(hdc, w, h) : nullptr;
    if (bmp)
    {
        // Entrance: the page rises into place while it fades in (WinUI EntranceThemeTransition)
        HGDIOBJ old = SelectObject(mem, bmp);
        SetViewportOrgEx(mem, -navW, 0, nullptr);
        FillSolid(mem, { navW, 0, client.right, h }, t.content);
        paintPage(mem);
        SetViewportOrgEx(mem, 0, 0, nullptr);
        const int dy = static_cast<int>(t.S(kPageRise) * (1.0 - enter) + 0.5);
        const BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(255 * enter), 0 };
        GdiAlphaBlend(hdc, navW, dy, w, h - dy, mem, 0, 0, w, h - dy, blend);
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    else
        paintPage(hdc);
    if (mem)
        DeleteDC(mem);
    RECT thumb;
    if ((m_tracking || m_thumbDrag) && ThumbRect(thumb))
        FillRound(hdc, thumb, t.S(2), Mix(t.muted, t.content, 30));
    PaintDropdown(hdc);
}

void SettingsView::PaintRow(HDC hdc, const Cell& cell, int offsetY)
{
    const Theme& t = *m_theme;
    const SettingsRow& row = m_pages[m_page].rows[cell.row];
    auto at = [&](RECT r) { OffsetRect(&r, 0, offsetY); return r; };
    const RECT r = at(cell.rc);
    const int index = static_cast<int>(&cell - m_cells.data());
    const bool hot = index == m_hot, down = index == m_pressed;

    if (row.kind == SettingsRow::Kind::Header)
    {
        DrawLabel(hdc, m_headerFont, row.label, { r.left + t.S(2), r.top + t.S(8), r.right, r.bottom }, t.ink);
        PaintInfo(hdc, cell, offsetY);
        return;
    }
    if (row.kind == SettingsRow::Kind::Note)
    {
        DrawWrapped(hdc, t.caption, row.label, { r.left + t.S(2), r.top + t.S(2), r.right - t.S(2), r.bottom }, t.muted);
        return;
    }
    // SettingsCard: ControlCornerRadius 4, CardBackgroundFillColorDefault, CardStrokeColorDefault
    const bool clickable = row.kind != SettingsRow::Kind::Value || !row.button.empty();
    const bool colors = row.kind == SettingsRow::Kind::Colors;
    const COLORREF face = clickable && down && m_pressedPart == Part::Row ? t.cardPressed : clickable && hot && m_hotPart != Part::Tile ? t.cardHover : t.card;
    // Expanders: only the header reacts; the items area keeps the card color
    const RECT head = colors ? RECT{ r.left, r.top, r.right, r.top + t.S(kExpanderHeader) } : r;
    if (colors && !cell.tiles.empty())
    {
        FillRound(hdc, r, t.S(4), t.card);
        FillTopRound(hdc, head, t.S(4), face);
        PaintColors(hdc, cell, offsetY);
    }
    else
        FillRound(hdc, r, t.S(4), face);
    FrameRound(hdc, r, t.S(4), t.cardStroke);
    if (index == m_focus && m_keyboard && GetFocus() == m_hwnd)
        FrameRound(hdc, { r.left - t.S(2), r.top - t.S(2), r.right + t.S(2), r.bottom + t.S(2) }, t.S(6), t.ink);

    // Label and detail (or this row's message); swatch rows (tags) start with their color, cards with an icon
    const RECT control = at(cell.control);
    const int labelX = LabelLeft(t, row, r.left);
    if (row.icon)
        DrawGlyph(hdc, m_cardIcons, row.icon, { r.left + t.S(16), head.top, r.left + t.S(36), head.bottom }, t.ink);
    if (row.swatch)
    {
        const int d = t.S(12), cx = r.left + t.S(22), cy = (r.top + r.bottom) / 2;
        FillRound(hdc, { cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d }, d / 2, row.swatchColor);
    }
    const int textRight = (IsRectEmpty(&cell.control) ? r.right - t.S(16) : control.left - t.S(16));
    // Only a rejected shortcut shows a second line (its error); explanations live behind the (i)
    const std::wstring message = m_messageCommand == row.command && !row.command.empty() ? m_message : std::wstring();
    if (row.kind == SettingsRow::Kind::Value && row.value)
    {
        // Value rows: label left (after the HeaderIcon, like every card), value right (before the button)
        const std::wstring value = row.value();
        DrawLabel(hdc, t.ui, row.label, { labelX, r.top, labelX + t.S(200), r.bottom }, t.ink);
        DrawLabel(hdc, t.ui, value, { labelX + t.S(210), r.top, textRight, r.bottom }, t.muted, DT_RIGHT | DT_PATH_ELLIPSIS);
    }
    else if (message.empty())
    {
        DrawLabel(hdc, t.ui, row.label, { labelX, head.top, IsRectEmpty(&cell.info) ? textRight : std::min<int>(textRight, at(cell.info).left + t.S(4)), head.bottom }, t.ink);
    }
    else
    {
        const int mid = (r.top + r.bottom) / 2;
        DrawLabel(hdc, t.ui, row.label, { labelX, mid - t.S(18), textRight, mid }, t.ink);
        DrawLabel(hdc, t.caption, message, { labelX, mid, textRight, mid + t.S(18) }, t.criticalText);
    }
    PaintInfo(hdc, cell, offsetY);

    const bool controlHot = hot && m_hotPart == Part::Control;
    const bool controlDown = down && m_pressedPart == Part::Control;
    switch (row.kind)
    {
    case SettingsRow::Kind::Toggle:
    {
        const bool on = row.isOn && row.isOn();
        // pos: 0 = off, 1 = on; mid-slide right after a click
        const double p = cell.row == m_toggleRow && m_toggleStart ? AnimProgress(m_toggleStart, kToggleMs) : 1.0;
        const double pos = on ? p : 1.0 - p;
        const int pct = static_cast<int>(pos * 100 + 0.5);
        const COLORREF onTrack = controlHot ? Mix(t.accent, t.ink, 12) : t.accent;
        FillRound(hdc, control, t.S(10), Mix(t.content, onTrack, pct));
        if (pct < 100)
            FrameRound(hdc, control, t.S(10), Mix(controlHot ? t.ink : t.muted, onTrack, pct));
        const int knob = t.S(10) + static_cast<int>(t.S(2) * pos + 0.5);
        const int cx = control.left + t.S(10) + static_cast<int>((control.right - control.left - t.S(20)) * pos + 0.5);
        const int cy = (control.top + control.bottom) / 2;
        FillRound(hdc, { cx - knob / 2, cy - knob / 2, cx + knob / 2, cy + knob / 2 }, knob / 2,
            Mix(t.muted, t.dark ? t.content : t.fillWhite, pct));
        DrawLabel(hdc, t.ui, on ? Tr(L"开") : Tr(L"关"), { control.left - t.S(40), r.top, control.left - t.S(10), r.bottom }, t.ink, DT_RIGHT);
        break;
    }
    case SettingsRow::Kind::Choice:
    {
        // ComboBox: Padding 12,5,0,7, the 12px chevron in a 30px column (TextFillColorSecondary)
        const int sel = row.selected ? row.selected() : -1;
        ControlFace(hdc, t, control, controlHot || m_drop.cell == index, controlDown);
        const std::wstring text = sel >= 0 && sel < static_cast<int>(row.options.size()) ? row.options[sel] : L"";
        DrawLabel(hdc, t.ui, text, { control.left + t.S(12), control.top, control.right - t.S(30), control.bottom }, t.ink);
        DrawGlyph(hdc, t.iconsMid, Glyph::ChevronDown, { control.right - t.S(30), control.top, control.right - t.S(8), control.bottom }, t.muted);
        break;
    }
    case SettingsRow::Kind::Button:
    case SettingsRow::Kind::Value:
        if (!row.button.empty())
        {
            ControlFace(hdc, t, control, controlHot, controlDown);
            DrawLabel(hdc, t.ui, row.button, control, t.ink, DT_CENTER);
        }
        break;
    case SettingsRow::Kind::Colors:
    {
        // Color button: the 24px swatch (margin 4) and a 12px chevron (margins 8,4,10,4)
        ControlFace(hdc, t, control, controlHot, controlDown);
        const int sy = (control.top + control.bottom) / 2 - t.S(12);
        DrawColorSwatch(hdc, t, { control.left + t.S(4), sy, control.left + t.S(28), sy + t.S(24) }, row.color ? row.color() : 0);
        DrawGlyph(hdc, t.iconsMid, Glyph::ChevronDown, { control.left + t.S(36), control.top, control.left + t.S(48), control.bottom }, t.ink);
        // The expander's own chevron (32px button at the right)
        const int ex = r.right - t.S(16 + 32);
        if (hot && m_hotPart == Part::Row)
            FillRound(hdc, { ex, (head.top + head.bottom) / 2 - t.S(16), ex + t.S(32), (head.top + head.bottom) / 2 + t.S(16) }, t.S(4), t.subtleHover);
        DrawGlyph(hdc, t.iconsMid, m_colorsCollapsed ? Glyph::ChevronDown : Glyph::ChevronUp, { ex, head.top, ex + t.S(32), head.bottom }, t.ink);
        break;
    }
    case SettingsRow::Kind::Shortcut:
    {
        if (index == m_capture)
        {
            FillRound(hdc, control, t.S(5), t.content);
            FrameRound(hdc, control, t.S(5), t.accent);
            DrawLabel(hdc, t.caption, Tr(L"按下快捷键"), control, t.accent, DT_CENTER);
        }
        else
        {
            const auto chords = row.chords ? row.chords() : std::vector<uint32_t>();
            if (controlHot || controlDown)
                FillRound(hdc, control, t.S(5), controlDown ? t.pressed : t.hover);
            int x = control.right - t.S(6);
            if (chords.empty())
                DrawLabel(hdc, t.caption, Tr(L"未设置"), { control.left, control.top, x, control.bottom }, t.muted, DT_RIGHT);
            for (auto it = chords.rbegin(); it != chords.rend(); ++it)
            {
                const std::wstring text = FormatShortcut(*it);
                const int w = TextWidth(hdc, t.caption, text) + t.S(14);
                if (x - w < control.left)
                    break;
                const RECT chip{ x - w, control.top + t.S(3), x, control.bottom - t.S(3) };
                FillRound(hdc, chip, t.S(4), t.content);
                FrameRound(hdc, chip, t.S(4), t.line);
                DrawLabel(hdc, t.caption, text, chip, t.ink, DT_CENTER);
                x = chip.left - t.S(6);
            }
        }
        if (row.overridden && row.overridden())
        {
            const RECT reset = at(cell.reset);
            const bool resetHot = hot && m_hotPart == Part::Reset;
            if (resetHot)
                FillRound(hdc, reset, t.S(5), t.hover);
            DrawLabel(hdc, t.caption, Tr(L"恢复默认"), reset, t.accent, DT_CENTER);
        }
        const RECT clear = at(cell.clear);
        if (hot && m_hotPart == Part::Clear)
            FillRound(hdc, clear, t.S(5), t.hover);
        DrawGlyph(hdc, t.iconsSmall, kClearGlyph, clear, t.muted);
        break;
    }
    default:
        break;
    }
}

void SettingsView::PaintInfo(HDC hdc, const Cell& cell, int offsetY)
{
    if (IsRectEmpty(&cell.info))
        return;
    const Theme& t = *m_theme;
    RECT info = cell.info;
    OffsetRect(&info, 0, offsetY);
    const int index = static_cast<int>(&cell - m_cells.data());
    const bool open = index == m_bubble, hot = index == m_hot && m_hotPart == Part::Info;
    if (open || hot)
        FillRound(hdc, info, t.S(4), open ? t.subtlePressed : t.subtleHover);
    DrawGlyph(hdc, t.iconsSmall, Glyph::Info, info, open || hot ? t.accent : t.muted);
}

// The explanation bubble: a rounded flyout under the (i), drawn over the rows
void SettingsView::PaintBubble(HDC hdc, int offsetY)
{
    if (m_bubble < 0 || m_bubble >= static_cast<int>(m_cells.size()))
        return;
    const Theme& t = *m_theme;
    const Cell& cell = m_cells[m_bubble];
    const std::wstring& text = m_pages[m_page].rows[cell.row].detail;
    const RECT client = ContentClient();
    RECT info = cell.info;
    OffsetRect(&info, 0, offsetY);
    const int pad = t.S(12), maxW = std::min<int>(t.S(360), client.right - client.left - t.S(32));
    const int textW = std::min(maxW - 2 * pad, TextWidth(hdc, t.ui, text) + t.S(2));
    const int textH = TextHeight(hdc, t.ui, text, textW);
    const int w = textW + 2 * pad, h = textH + 2 * pad;
    int x = std::clamp<int>((info.left + info.right) / 2 - t.S(24), client.left + t.S(8), client.right - t.S(8) - w);
    int y = info.bottom + t.S(4);
    if (y + h > client.bottom - t.S(4) && info.top - t.S(4) - h >= client.top)
        y = info.top - t.S(4) - h; // no room below: open upward
    const RECT box{ x, y, x + w, y + h };
    // Flyout: AcrylicInAppFillColorDefault-like solid, 1px stroke, 8 radius, soft shadow
    for (int i = 3; i >= 1; --i)
        FillRound(hdc, { box.left - i + t.S(1), box.top - i + t.S(3), box.right + i - t.S(1), box.bottom + i + t.S(3) }, t.S(8) + i,
            Mix(t.content, t.fillBlack, t.dark ? 10 + 6 * (3 - i) : 2 + 2 * (3 - i)));
    FillRound(hdc, box, t.S(8), t.dark ? Mix(t.card, t.fillWhite, 4) : t.card);
    FrameRound(hdc, box, t.S(8), t.cardStroke);
    DrawWrapped(hdc, t.ui, text, { box.left + pad, box.top + pad, box.right - pad, box.bottom - pad }, t.ink);
}

void SettingsView::SetBubble(int cell)
{
    if (cell == m_bubble)
        return;
    m_bubble = cell;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool SettingsView::Animating() const
{
    const ULONGLONG now = GetTickCount64();
    return (m_toggleStart && now - m_toggleStart < kToggleMs) || (m_pageStart && now - m_pageStart < kPageMs) ||
        (m_navStart && now - m_navStart < kNavMs);
}

void SettingsView::Animate()
{
    SetTimer(m_hwnd, kAnimTimer, 10, nullptr); // ~one frame; WM_TIMER stops it when everything has settled
}

// --- Input ---

void SettingsView::Activate(int cell, Part part, int tile)
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()))
        return;
    // Copies: the callbacks rebuild m_pages
    const SettingsRow row = m_pages[m_page].rows[m_cells[cell].row];
    switch (row.kind)
    {
    case SettingsRow::Kind::Colors:
        if (part == Part::Tile)
        {
            if (row.select && tile >= 0 && (!row.selected || row.selected() != tile))
                row.select(tile);
        }
        else if (part == Part::Control)
        {
            if (row.press)
                row.press();
        }
        else
        {
            m_colorsCollapsed = !m_colorsCollapsed; // the header opens / closes the expander
            Layout();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        break;
    case SettingsRow::Kind::Toggle:
        if (row.setOn && row.isOn)
        {
            if (AnimationsEnabled())
            {
                m_toggleRow = m_cells[cell].row;
                m_toggleStart = GetTickCount64();
            }
            row.setOn(!row.isOn());
            if (m_hwnd && m_toggleRow >= 0)
                Animate();
        }
        break;
    case SettingsRow::Kind::Choice:
        OpenDropdown(cell);
        return;
    case SettingsRow::Kind::Button:
    case SettingsRow::Kind::Value:
        if (row.press && (part == Part::Control || part == Part::Row))
            row.press();
        break;
    case SettingsRow::Kind::Shortcut:
        m_message.clear();
        if (part == Part::Clear)
            m_host->ClearShortcut(row.command);
        else if (part == Part::Reset)
            m_host->ResetShortcut(row.command);
        else
        {
            BeginCapture(cell);
            return;
        }
        break;
    default:
        return;
    }
    if (m_hwnd)
        Rebuild();
}

void SettingsView::OpenDropdown(int cell)
{
    const Theme& t = *m_theme;
    const SettingsRow& row = m_pages[m_page].rows[m_cells[cell].row];
    const int n = static_cast<int>(row.options.size());
    if (n == 0)
        return;
    SetBubble(-1);
    RECT control = m_cells[cell].control;
    const RECT client = ContentClient();
    OffsetRect(&control, 0, client.top - m_scroll);
    RECT view{};
    GetClientRect(m_hwnd, &view);
    // WinUI ComboBox popup: at least the box's width, the selected item laid over the box, kept on screen
    HDC hdc = GetDC(m_hwnd);
    int widest = 0;
    for (const auto& o : row.options)
        widest = std::max(widest, TextWidth(hdc, t.ui, o));
    ReleaseDC(m_hwnd, hdc);
    Dropdown d;
    d.cell = cell;
    d.itemH = t.S(kDropItem);
    d.current = row.selected ? std::clamp(row.selected(), -1, n - 1) : -1;
    const int pad = t.S(kDropPad);
    const int room = std::max<int>(d.itemH, view.bottom - view.top - 2 * t.S(8) - 2 * pad);
    d.visible = std::max(1, std::min({ n, (t.S(kDropMax) - 2 * pad) / d.itemH, room / d.itemH }));
    const int h = d.visible * d.itemH + 2 * pad;
    const int w = std::max<int>(control.right - control.left + t.S(8), widest + t.S(5 + 11 + 12 + 24));
    const int sel = std::max(0, d.current);
    d.scroll = std::clamp((sel - d.visible / 2) * d.itemH, 0, (n - d.visible) * d.itemH);
    const int selTop = pad + sel * d.itemH - d.scroll;
    int top = (control.top + control.bottom) / 2 - selTop - d.itemH / 2;
    top = std::clamp<int>(top, view.top + t.S(8), std::max<int>(view.top + t.S(8), view.bottom - t.S(8) - h));
    int left = control.left - t.S(4);
    left = std::clamp<int>(left, view.left + t.S(4), std::max<int>(view.left + t.S(4), view.right - t.S(4) - w));
    d.box = { left, top, left + w, top + h };
    d.hot = d.current;
    m_drop = d;
    SetCapture(m_hwnd); // light dismiss: the next click anywhere closes it
    PublishDropdown();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsView::CloseDropdown()
{
    if (m_drop.cell < 0)
        return;
    m_drop = Dropdown{};
    if (m_hwnd && GetCapture() == m_hwnd)
        ReleaseCapture();
    PublishDropdown();
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsView::CommitDropdown(int item)
{
    const int cell = m_drop.cell, current = m_drop.current;
    CloseDropdown();
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()))
        return;
    const SettingsRow row = m_pages[m_page].rows[m_cells[cell].row]; // copy: select rebuilds m_pages
    if (item >= 0 && item < static_cast<int>(row.options.size()) && item != current && row.select)
        row.select(item);
    if (m_hwnd)
        Rebuild();
}

int SettingsView::DropdownItemAt(POINT pt) const
{
    if (m_drop.cell < 0 || !PtInRect(&m_drop.box, pt))
        return -2; // outside
    const int n = static_cast<int>(m_pages[m_page].rows[m_cells[m_drop.cell].row].options.size());
    const int y = pt.y - m_drop.box.top - m_theme->S(kDropPad) + m_drop.scroll;
    const int i = y >= 0 ? y / m_drop.itemH : -1;
    return i >= 0 && i < n ? i : -1;
}

void SettingsView::DropdownScrollTo(int offset)
{
    const int n = static_cast<int>(m_pages[m_page].rows[m_cells[m_drop.cell].row].options.size());
    offset = std::clamp(offset, 0, std::max(0, (n - m_drop.visible) * m_drop.itemH));
    if (offset != m_drop.scroll)
    {
        m_drop.scroll = offset;
        PublishDropdown();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SettingsView::DropdownMoveHot(int item)
{
    const int n = static_cast<int>(m_pages[m_page].rows[m_cells[m_drop.cell].row].options.size());
    item = std::clamp(item, 0, n - 1);
    m_drop.hot = item;
    if (item * m_drop.itemH < m_drop.scroll)
        DropdownScrollTo(item * m_drop.itemH);
    else if ((item + 1) * m_drop.itemH > m_drop.scroll + m_drop.visible * m_drop.itemH)
        DropdownScrollTo((item + 1 - m_drop.visible) * m_drop.itemH);
    PublishDropdown();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool SettingsView::DropdownKey(UINT vk)
{
    const int hot = std::max(0, m_drop.hot);
    switch (vk)
    {
    case VK_ESCAPE: CloseDropdown(); return true;
    case VK_UP: DropdownMoveHot(m_drop.hot < 0 ? 0 : hot - 1); return true;
    case VK_DOWN: DropdownMoveHot(m_drop.hot < 0 ? 0 : hot + 1); return true;
    case VK_HOME: DropdownMoveHot(0); return true;
    case VK_END: DropdownMoveHot(INT_MAX / 2); return true;
    case VK_PRIOR: DropdownMoveHot(hot - std::max(1, m_drop.visible - 1)); return true;
    case VK_NEXT: DropdownMoveHot(hot + std::max(1, m_drop.visible - 1)); return true;
    case VK_RETURN: case VK_SPACE: case VK_TAB: CommitDropdown(m_drop.hot); return true;
    default: return true; // the open popup takes every key
    }
}

// Type-ahead: letters typed within a second jump to the first item starting with them
void SettingsView::DropdownChar(wchar_t ch)
{
    if (ch < L' ' || ch == L' ')
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - m_drop.typedAt > 1000)
        m_drop.typed.clear();
    m_drop.typedAt = now;
    m_drop.typed.push_back(ch);
    const auto& options = m_pages[m_page].rows[m_cells[m_drop.cell].row].options;
    const int n = static_cast<int>(options.size());
    // One repeated letter cycles through the items starting with it
    const bool cycle = m_drop.typed.size() > 1 && m_drop.typed.find_first_not_of(m_drop.typed[0]) == std::wstring::npos;
    const std::wstring prefix = cycle ? m_drop.typed.substr(0, 1) : m_drop.typed;
    const int start = cycle || m_drop.typed.size() == 1 ? m_drop.hot + 1 : std::max(0, m_drop.hot);
    for (int k = 0; k < n; ++k)
    {
        const int i = (start + k) % n;
        if (options[i].size() >= prefix.size() && _wcsnicmp(options[i].c_str(), prefix.c_str(), prefix.size()) == 0)
        {
            DropdownMoveHot(i);
            return;
        }
    }
}

void SettingsView::PublishDropdown()
{
    if (!m_hwnd)
        return;
    // "设置" normally; with a dropdown open, its state for tests: count, current, highlighted, first visible
    std::wstring text = Tr(L"设置");
    if (m_drop.cell >= 0)
    {
        const int n = static_cast<int>(m_pages[m_page].rows[m_cells[m_drop.cell].row].options.size());
        text += L"\tdropdown " + std::to_wstring(n) + L" " + std::to_wstring(m_drop.current) + L" " + std::to_wstring(m_drop.hot) + L" " +
            std::to_wstring(m_drop.scroll / std::max(1, m_drop.itemH)) + L" " + std::to_wstring(m_drop.visible);
    }
    SetWindowTextW(m_hwnd, text.c_str());
}

void SettingsView::PaintDropdown(HDC hdc)
{
    if (m_drop.cell < 0)
        return;
    const Theme& t = *m_theme;
    const auto& options = m_pages[m_page].rows[m_cells[m_drop.cell].row].options;
    const RECT box = m_drop.box;
    // ComboBoxDropDownBackground: AcrylicInAppFillColorDefault (its fallback #2C2C2C / #F9F9F9 without the blur),
    // SurfaceStrokeColorFlyout, OverlayCornerRadius 8, the flyout shadow
    const COLORREF fill = t.flyout;
    for (int i = 3; i >= 1; --i)
        FillRound(hdc, { box.left - i + t.S(1), box.top - i + t.S(3), box.right + i - t.S(1), box.bottom + i + t.S(3) }, t.S(8) + i,
            Mix(t.content, t.fillBlack, t.dark ? 10 + 6 * (3 - i) : 2 + 2 * (3 - i)));
    FillRound(hdc, box, t.S(8), fill);
    FrameRound(hdc, box, t.S(8), Mix(fill, t.fillBlack, t.dark ? 20 : 6));
    const int saved = SaveDC(hdc);
    const int pad = t.S(kDropPad);
    IntersectClipRect(hdc, box.left + 1, box.top + pad, box.right - 1, box.bottom - pad);
    const COLORREF white = t.fillWhite, black = t.fillBlack;
    const int first = m_drop.scroll / m_drop.itemH;
    for (int i = first; i < static_cast<int>(options.size()) && i <= first + m_drop.visible; ++i)
    {
        const int y = box.top + pad + i * m_drop.itemH - m_drop.scroll;
        // ComboBoxItem: Margin 5,2,5,2, ControlCornerRadius 4; selected / pointer-over SubtleFillColorSecondary,
        // pressed SubtleFillColorTertiary; the selected one has the 3x16 accent pill
        const RECT item{ box.left + t.S(5), y + t.S(2), box.right - t.S(5), y + m_drop.itemH - t.S(2) };
        const bool selected = i == m_drop.current, hot = i == m_drop.hot, down = i == m_drop.pressed;
        if (selected || hot || down)
        {
            // #0FFFFFFF / #0AFFFFFF dark, #09000000 / #06000000 light
            FillRound(hdc, item, t.S(4), t.dark ? Mix(fill, white, down ? 4 : 6) : Mix(fill, black, down ? 2 : 4));
        }
        if (selected)
        {
            const int cy = (item.top + item.bottom) / 2;
            FillRound(hdc, { item.left, cy - t.S(8), item.left + t.S(3), cy + t.S(8) }, t.S(2), t.accent);
        }
        DrawLabel(hdc, t.ui, options[i], { item.left + t.S(11), item.top, item.right - t.S(8), item.bottom }, t.ink);
    }
    RestoreDC(hdc, saved);
    // Scroll bar (thin while not hovered, like WinUI's)
    const int n = static_cast<int>(options.size());
    if (n > m_drop.visible)
    {
        const int track = box.bottom - box.top - 2 * pad;
        const int th = std::max(t.S(24), MulDiv(track, m_drop.visible, n));
        const int range = (n - m_drop.visible) * m_drop.itemH;
        const int ty = box.top + pad + (range > 0 ? MulDiv(track - th, m_drop.scroll, range) : 0);
        FillRound(hdc, { box.right - t.S(6), ty, box.right - t.S(4), ty + th }, t.S(1), Mix(t.muted, fill, 30));
    }
}

void SettingsView::PaintColors(HDC hdc, const Cell& cell, int offsetY)
{
    const Theme& t = *m_theme;
    const SettingsRow& row = m_pages[m_page].rows[cell.row];
    const int index = static_cast<int>(&cell - m_cells.data());
    const RECT r{ cell.rc.left, cell.rc.top + offsetY, cell.rc.right, cell.rc.bottom + offsetY };
    // Header / items divider
    FillSolid(hdc, { r.left + 1, r.top + t.S(kExpanderHeader), r.right - 1, r.top + t.S(kExpanderHeader) + 1 }, t.cardStroke);
    const int sel = row.selected ? row.selected() : -1;
    const COLORREF white = t.fillWhite, black = t.fillBlack;
    // ControlElevationBorder and LayerOnMicaBaseAltFillColorDefault (#733A3A3A / #B3FFFFFF) over the card
    const COLORREF edge = t.dark ? Mix(t.card, white, 9) : Mix(t.card, black, 10);
    for (int k = 0; k < static_cast<int>(cell.tiles.size()) && k < static_cast<int>(row.colors.size()); ++k)
    {
        RECT tile = cell.tiles[k];
        OffsetRect(&tile, 0, offsetY);
        const bool hot = index == m_hot && m_hotPart == Part::Tile && k == m_hotTile;
        const bool down = index == m_pressed && m_pressedPart == Part::Tile && k == m_pressedTile;
        if (hot || down)
            FillRound(hdc, tile, t.S(6), down ? t.subtlePressed : t.subtleHover);
        // The color under the mock tab row and file area (Border RowSpan 2, 66px, CornerRadius 4,4,0,0)
        const uint32_t argb = row.colors[k];
        const COLORREF c = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
        const COLORREF bg = Mix(t.card, c, static_cast<int>(argb >> 24) * 100 / 255);
        const RECT preview{ tile.left + 1, tile.top + 1, tile.right - 1, tile.top + 1 + t.S(66) };
        FillTopRound(hdc, preview, t.S(5), bg);
        const COLORREF layerFill = Mix(bg, t.layerFill, t.layerPct);
        // Tab bar: 4px line, a 32x12 tab, the rest of the line
        const int lineY = preview.top + t.S(16) - 1;
        FillSolid(hdc, { preview.left, lineY, preview.left + t.S(4), lineY + 1 }, edge);
        const RECT tab{ preview.left + t.S(4), preview.top + t.S(4), preview.left + t.S(36), lineY + 1 };
        FillTopRound(hdc, tab, t.S(4), layerFill);
        FillSolid(hdc, { tab.left, tab.top + t.S(2), tab.left + 1, tab.bottom }, edge);
        FillSolid(hdc, { tab.right - 1, tab.top + t.S(2), tab.right, tab.bottom }, edge);
        FillSolid(hdc, { tab.left + t.S(2), tab.top, tab.right - t.S(2), tab.top + 1 }, edge);
        FillSolid(hdc, { tab.right, lineY, preview.right, lineY + 1 }, edge);
        // File area (50px)
        FillSolid(hdc, { preview.left, lineY + 1, preview.right, preview.bottom }, layerFill);
        FillSolid(hdc, { preview.left, preview.bottom - 1, preview.right, preview.bottom }, edge);
        // Name (Padding 4, centered, ellipsis)
        DrawLabel(hdc, t.ui, k < static_cast<int>(row.options.size()) ? row.options[k] : L"",
            { tile.left + t.S(4), preview.bottom, tile.right - t.S(4), tile.bottom }, t.ink, DT_CENTER);
        FrameRound(hdc, tile, t.S(6), edge);
        // GridViewItem selection: the accent border
        if (k == sel)
        {
            FrameRound(hdc, tile, t.S(6), t.accent);
            FrameRound(hdc, { tile.left + 1, tile.top + 1, tile.right - 1, tile.bottom - 1 }, t.S(5), t.accent);
        }
    }
}

void SettingsView::BeginCapture(int cell)
{
    m_capture = cell;
    m_message.clear();
    SetFocus(m_hwnd);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsView::EndCapture()
{
    if (m_capture < 0)
        return;
    m_capture = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsView::OnCaptureKey(UINT vk)
{
    if (vk == VK_ESCAPE)
    {
        EndCapture();
        return;
    }
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
        (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_CAPITAL || vk == VK_NUMLOCK)
        return; // wait for the actual key
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000, alt = GetKeyState(VK_MENU) & 0x8000;
    const uint32_t code = ((ctrl ? kModCtrl : 0) | (shift ? kModShift : 0) | (alt ? kModAlt : 0)) << 16 | (vk & 0xFFFF);
    const std::wstring command = m_pages[m_page].rows[m_cells[m_capture].row].command;
    std::wstring error;
    if (FormatShortcut(code).empty())
        error = Tr(L"这个键不能用作快捷键");
    else if (!ShortcutBindable(code))
        error = Tr(L"快捷键需要包含 Ctrl 或 Alt");
    else if (Reserved(code))
        error = FormatShortcut(code) + Tr(L" 是系统保留的快捷键");
    else
        error = m_host->SetShortcut(command, code);
    m_capture = -1;
    m_message = error;
    m_messageCommand = command;
    if (m_hwnd)
        Rebuild();
}

LRESULT CALLBACK SettingsView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = static_cast<SettingsView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SettingsView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsView::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case WM_SIZE:
        CloseDropdown();
        Layout();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == kAnimTimer)
        {
            InvalidateRect(m_hwnd, nullptr, FALSE);
            if (!Animating())
            {
                KillTimer(m_hwnd, kAnimTimer);
                m_toggleStart = m_pageStart = m_navStart = 0;
                m_toggleRow = m_navFrom = -1;
            }
            return 0;
        }
        break;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        Paint(paint.dc(), paint.rect());
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        if (m_thumbDrag)
        {
            const RECT client = ContentClient();
            RECT thumb;
            if (ThumbRect(thumb))
            {
                const int view = client.bottom - client.top;
                const int track = view - m_theme->S(4) - (thumb.bottom - thumb.top);
                const int offset = track > 0 ? MulDiv(pt.y - client.top - m_theme->S(2) - m_thumbGrab, m_content - view, track) : 0;
                m_smooth.Reset(offset); // dragging the thumb itself: no wheel animation to fight it
                ScrollTo(offset);
            }
            return 0;
        }
        if (m_drop.cell >= 0)
        {
            const int item = DropdownItemAt(pt);
            if (item >= 0 && item != m_drop.hot)
            {
                m_drop.hot = item;
                PublishDropdown();
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;
        }
        Part part;
        int tile = -1;
        const int cell = CellAt(pt, &part, &tile);
        const int nav = NavAt(pt);
        if (cell != m_hot || part != m_hotPart || nav != m_navHot || tile != m_hotTile)
        {
            m_hot = cell;
            m_hotPart = part;
            m_hotTile = tile;
            m_navHot = nav;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        m_hot = m_navHot = -1;
        m_hotPart = Part::None;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (m_drop.cell >= 0)
        {
            // Inside: press the item; outside: light dismiss, and the click goes nowhere else
            const int item = msg == WM_LBUTTONDOWN ? DropdownItemAt(pt) : -2;
            if (item == -2)
                CloseDropdown();
            else
            {
                m_drop.pressed = item;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (msg == WM_RBUTTONDOWN)
            break;
    {
        SetFocus(m_hwnd);
        m_keyboard = false;
        RECT thumb;
        if (ThumbRect(thumb) && pt.x >= thumb.left - m_theme->S(6))
        {
            m_thumbDrag = true;
            m_thumbGrab = PtInRect(&thumb, pt) ? pt.y - thumb.top : (thumb.bottom - thumb.top) / 2;
            SetCapture(m_hwnd);
            return 0;
        }
        if (const int nav = NavAt(pt); nav >= 0)
        {
            ShowPage(nav);
            return 0;
        }
        EndCapture();
        m_pressed = CellAt(pt, &m_pressedPart, &m_pressedTile);
        if (m_pressedPart == Part::Info)
        {
            SetBubble(m_pressed == m_bubble ? -1 : m_pressed); // the (i) toggles its bubble
            m_pressed = -1;
            m_pressedPart = Part::None;
            return 0;
        }
        SetBubble(-1); // any other click dismisses it
        if (m_pressed >= 0)
        {
            m_focus = m_pressed;
            SetCapture(m_hwnd);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (m_drop.cell >= 0)
        {
            const int item = DropdownItemAt(pt);
            if (item >= 0 && item == m_drop.pressed)
                CommitDropdown(item);
            else
            {
                m_drop.pressed = -1;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;
        }
        const int pressed = m_pressed, pressedTile = m_pressedTile;
        const Part part = m_pressedPart;
        m_pressed = m_pressedTile = -1;
        m_pressedPart = Part::None;
        m_thumbDrag = false;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        Part now;
        int tile = -1;
        if (pressed >= 0 && CellAt(pt, &now, &tile) == pressed && now == part && tile == pressedTile)
            Activate(pressed, part, tile);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != m_hwnd)
        {
            m_pressed = -1;
            m_thumbDrag = false;
            CloseDropdown();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (m_drop.cell >= 0)
        {
            // The popup's own list (few rows, closes as soon as the pointer leaves it): jumps per notch like a
            // native ComboBox's dropdown, not the page body's animation.
            DropdownScrollTo(m_drop.scroll - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), 3 * m_drop.itemH, WHEEL_DELTA));
            return 0;
        }
        SetBubble(-1);
        {
            const RECT client = ContentClient();
            ScrollTo(m_smooth.OnWheel(m_hwnd, GET_WHEEL_DELTA_WPARAM(wp), m_theme->S(120), m_content - static_cast<int>(client.bottom - client.top)));
        }
        return 0;
    case SmoothFrame::kMsgTick:
        if (m_smooth.Animating())
            ScrollTo(m_smooth.Tick());
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    {
        const UINT vk = static_cast<UINT>(wp);
        if (m_capture >= 0)
        {
            OnCaptureKey(vk);
            return 0; // no Alt menu, no beep while a shortcut is being recorded
        }
        if (m_drop.cell >= 0)
        {
            // Alt+Up / F4 close it like Esc (keeping the selection); other Alt keys close it and go on
            if (vk == VK_F4 || (msg == WM_SYSKEYDOWN && vk == VK_UP))
            {
                CloseDropdown();
                return 0;
            }
            if (msg == WM_SYSKEYDOWN)
            {
                if (vk != VK_MENU)
                    CloseDropdown();
                break;
            }
            DropdownKey(vk);
            return 0;
        }
        // Alt+Down / F4 open a focused ComboBox
        if ((vk == VK_F4 || (msg == WM_SYSKEYDOWN && vk == VK_DOWN)) && Focusable(m_focus) &&
            m_pages[m_page].rows[m_cells[m_focus].row].kind == SettingsRow::Kind::Choice)
        {
            OpenDropdown(m_focus);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN)
            break;
        if (m_bubble >= 0)
        {
            SetBubble(-1); // the first key closes the bubble (Esc only closes)
            if (vk == VK_ESCAPE)
                return 0;
        }
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        // Left / Right pick the previous / next background color card
        if ((vk == VK_LEFT || vk == VK_RIGHT) && Focusable(m_focus))
        {
            const SettingsRow& row = m_pages[m_page].rows[m_cells[m_focus].row];
            if (row.kind == SettingsRow::Kind::Colors && !row.options.empty())
            {
                const int sel = row.selected ? row.selected() : -1;
                const int next = std::clamp(sel + (vk == VK_RIGHT ? 1 : -1), 0, static_cast<int>(row.options.size()) - 1);
                if (next != sel)
                    Activate(m_focus, Part::Tile, next);
                return 0;
            }
        }
        switch (vk)
        {
        case VK_UP: MoveFocus(-1); return 0;
        case VK_DOWN: MoveFocus(1); return 0;
        case VK_TAB: MoveFocus(shift ? -1 : 1); return 0;
        case VK_HOME: m_focus = -1; MoveFocus(1); ScrollTo(0); return 0;
        case VK_END: m_focus = -1; MoveFocus(-1); return 0;
        case VK_PRIOR: case VK_NEXT:
        {
            const RECT client = ContentClient();
            ScrollTo(m_scroll + (vk == VK_NEXT ? 1 : -1) * (client.bottom - client.top - m_theme->S(40)));
            return 0;
        }
        case VK_RETURN: case VK_SPACE:
            if (m_focus >= 0)
                Activate(m_focus, Part::Row);
            return 0;
        case VK_DELETE:
            if (m_focus >= 0 && m_pages[m_page].rows[m_cells[m_focus].row].kind == SettingsRow::Kind::Shortcut)
                Activate(m_focus, Part::Clear);
            return 0;
        }
        break;
    }
    case WM_SYSCHAR:
    case WM_CHAR:
        if (m_capture >= 0)
            return 0;
        if (m_drop.cell >= 0 && msg == WM_CHAR)
        {
            DropdownChar(static_cast<wchar_t>(wp));
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (msg == WM_KILLFOCUS)
        {
            EndCapture();
            CloseDropdown();
            m_bubble = -1;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
