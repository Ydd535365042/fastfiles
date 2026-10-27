#include "FileList.h"
#include "Tab.h"
#include "Format.h"
#include "Lang.h"
#include "DarkMode.h"

// Files' DetailsLayoutPage, measured in its XAML (96 DPI): ListView Padding 8,0,8,12; HeaderGrid 40 high, Margin -8,0
// Padding 24,0,0,0, 1px DividerStrokeColorDefault under it; DataGridHeader button Padding 12,12,12,0 (Margin -4,0; the
// name header 4,0,-4,0), 12px SemiBold TextFillColorTertiary, sort caret 8px; rows 36 high (DetailsViewSizeKind.Small),
// icon column 20 (16px icon, Margin 2), name 14px with Padding 12+6, other cells Padding 10 in 12px at Opacity 0.6;
// dimmed (hidden) items at 0.4; tag pills 24 high, Padding 8,0, CornerRadius 12, Spacing 8 (16px tag icon).
// Row background and selection pill: WinUI ListViewItem (corner 4, inset 4,2; pill 3x16 in the accent color).

void FileList::CacheColumns()
{
    for (int i = 0; i < kColumnCount; ++i)
    {
        RECT rc{};
        if (m_header && Header_GetItemRect(m_header, i, &rc))
            m_colLeft[i] = rc.left, m_colRight[i] = rc.right;
        else
            m_colLeft[i] = m_colRight[i] = 0;
    }
}

void FileList::Paint(HWND hwnd, bool header)
{
    const bool compose = Backdrop::Active();
    if (!header && compose)
    {
        // A scroll moved composed pixels (the material stays where the window is): paint all of it again
        const POINT now = m_scroll;
        if (now.x != m_scrolled.x || now.y != m_scrolled.y)
        {
            m_scrolled = now;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (m_header)
                InvalidateRect(m_header, nullptr, FALSE);
        }
    }
    PAINTSTRUCT ps;
    HDC target = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    {
        // The backdrop composes into its pixels; nothing to clear (FillSolid covers the client)
        PaintBuffer c(target, client.right, client.bottom, RECT{});
        if (c.bits)
        {
            FillSolid(c.dc, client, m_theme->content);
            if (header)
                DrawHeader(c.dc, client);
            else
                DrawItems(c.dc, ps.rcPaint);
            if (compose)
                Backdrop::Compose(hwnd, c.bits, c.width, ps.rcPaint, &m_layers);
            if (!header && m_marquee)
            {
                // Files' SelectionRectangle: SystemAccentColor at 0.5 with a SystemAccentColorLight1 stroke, over everything
                RECT band = m_marqueeRect;
                OffsetRect(&band, -m_scroll.x, -m_scroll.y);
                IntersectRect(&band, &band, &client);
                const COLORREF fill = m_theme->accent;
                for (int y = band.top; y < band.bottom; ++y)
                    for (int x = band.left; x < band.right; ++x)
                    {
                        uint32_t& px = c.bits[static_cast<size_t>(y) * c.width + x];
                        const int r = (((px >> 16) & 0xFF) + GetRValue(fill)) / 2, g = (((px >> 8) & 0xFF) + GetGValue(fill)) / 2,
                            b = ((px & 0xFF) + GetBValue(fill)) / 2;
                        px = static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b);
                    }
                if (band.right > band.left && band.bottom > band.top)
                {
                    HBRUSH stroke = CreateSolidBrush(m_theme->marqueeStroke);
                    FrameRect(c.dc, &band, stroke);
                    DeleteObject(stroke);
                }
            }
            BitBlt(target, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                c.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        }
    }
    EndPaint(hwnd, &ps);
}

void FileList::DrawRow(HDC hdc, int row, const RECT& rc)
{
    if (row < 0 || row >= Count())
        return;
    const Theme& t = *m_theme;
    const Item& item = m_items[m_order[row]];
    const bool selected = m_sel.Selected(row);
    const bool hot = row == m_hot || row == m_dropRow, pressed = row == m_pressed; // (the drop target shows as hovered)
    RECT client{};
    GetClientRect(m_list, &client);
    // WinUI ListViewItem: SubtleFillColorSecondary when selected or under the mouse, Tertiary pressed / selected and hovered
    COLORREF fill = t.content;
    if (pressed ? !selected : selected && hot)
        fill = m_pressedKey;
    else if (selected || hot || pressed)
        fill = m_hoverKey;
    const RECT bg{ rc.left + t.S(12), rc.top + t.S(2), std::max<LONG>(rc.right + t.S(8), client.right) - t.S(12), rc.bottom - t.S(2) };
    if (fill != t.content)
        FillRound(hdc, bg, t.S(4), fill);
    if (selected)
    {
        const int mid = (rc.top + rc.bottom) / 2;
        FillRound(hdc, { bg.left, mid - t.S(8), bg.left + t.S(3), mid + t.S(8) }, std::max(1, t.S(3) / 2), t.accent);
    }
    // WinUI's focus visual after keyboard navigation: a 2px frame around the item (corner 4)
    if (m_keyboardFocus && row == FocusedIndex() && GetFocus() == m_list)
    {
        FrameRound(hdc, bg, t.S(4), t.focusStroke);
        FrameRound(hdc, { bg.left + 1, bg.top + 1, bg.right - 1, bg.bottom - 1 }, t.S(4), t.focusStroke);
    }
    // Files' ShowCheckbox state: the box instead of the icon on a selected row (项目复选框 on) and under the mouse
    const ViewPrefs* prefs = m_events->ListPrefs();
    const bool checkHot = row == m_hotCheck, checkDown = row == m_pressedCheck;
    const bool showCheck = (prefs && prefs->checkboxes && selected) || checkHot || checkDown;
    if (showCheck)
    {
        const RECT hit = CheckRect(rc);
        const int mid = (hit.top + hit.bottom) / 2;
        DrawCheck(hdc, { hit.left, mid - t.S(10), hit.right, mid + t.S(10) }, selected ? Check::On : Check::Off, checkHot,
            checkDown && checkHot, fill);
    }
    // Icon (Files' IconBox: iconSize + 4, the icon centered), dimmed for hidden items, the shortcut arrow over it
    const bool dim = (item.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    if (!showCheck && IconSize() > 16)
    {
        const int icon = t.S(IconSize()), cx = rc.left + t.S(24) + t.S(IconSize() + 4) / 2, cy = (rc.top + rc.bottom) / 2;
        DrawPicture(hdc, row, { cx - icon / 2, cy - icon / 2, cx - icon / 2 + icon, cy - icon / 2 + icon }, dim);
    }
    else if (m_icons && item.icon >= 0 && !showCheck)
    {
        int cx = 16, cy = 16;
        ImageList_GetIconSize(m_icons, &cx, &cy);
        IMAGELISTDRAWPARAMS p{ sizeof(p) };
        p.himl = m_icons;
        p.i = item.icon;
        p.hdcDst = hdc;
        p.x = rc.left + t.S(24) + (t.S(20) - cx) / 2;
        p.y = (rc.top + rc.bottom - cy) / 2;
        p.rgbBk = p.rgbFg = CLR_NONE;
        p.fStyle = ILD_TRANSPARENT;
        p.fState = dim ? ILS_ALPHA : ILS_NORMAL;
        p.Frame = dim ? 102 : 0; // Files' DimItemOpacity 0.4
        ImageList_DrawIndirect(&p);
        if (item.link && m_linkOverlay >= 0)
        {
            p.i = m_linkOverlay;
            ImageList_DrawIndirect(&p);
        }
    }
    auto cell = [&](int i) { return RECT{ rc.left + m_colLeft[i], rc.top, rc.left + m_colRight[i], rc.bottom }; };
    const COLORREF ink = dim ? Mix(fill, t.ink, 40) : t.ink;
    const COLORREF sub = Mix(fill, t.ink, dim ? 24 : 60); // ColumnContentTextBlock: Opacity 0.6
    RECT name = cell(ColName);
    name.left += t.S(18);
    DrawLabel(hdc, t.ui, item.display, name, ink);
    // 文件标签: one pill per tag (tag icon in its color, the name)
    if (!item.tags.empty() && m_colRight[ColTag] > m_colLeft[ColTag])
    {
        const RECT c = cell(ColTag);
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, c.left, c.top, c.right, c.bottom);
        int x = c.left + t.S(12);
        const int mid = (rc.top + rc.bottom) / 2;
        for (const std::wstring& id : item.tags)
        {
            const FileTag* tag = Tags::Find(m_tags, id);
            if (!tag || x >= c.right)
                continue;
            const int tw = std::min(TextWidth(hdc, t.caption, tag->name), t.S(200));
            const RECT glyph{ x + t.S(8), mid - t.S(8), x + t.S(24), mid + t.S(8) };
            DrawGlyph(hdc, t.icons, Glyph::Tag, glyph, tag->color);
            DrawLabel(hdc, t.caption, tag->name, { glyph.right + t.S(8), rc.top, glyph.right + t.S(8) + tw, rc.bottom }, sub);
            x = glyph.right + t.S(8) + tw + t.S(8) + t.S(4);
        }
        RestoreDC(hdc, saved);
    }
    // The other columns: Padding 10 (a collapsed one is 0 wide and draws nothing)
    auto text = [&](int column, const std::wstring& s)
    {
        RECT r = cell(column);
        if (r.right - r.left <= t.S(10) || s.empty())
            return;
        r.left += t.S(10);
        DrawLabel(hdc, t.caption, s, r, sub);
    };
    text(ColModified, Format::DateShort(item.modified));
    text(ColCreated, Format::DateShort(item.created));
    text(ColType, item.type);
    if (!(item.attrs & FILE_ATTRIBUTE_DIRECTORY))
        text(ColSize, Format::Size(item.size));
}

float FileList::SortTurn() const
{
    const float to = m_sortDesc ? -180.0f : 0.0f;
    if (!m_sortTurnStart)
        return to;
    const ULONGLONG t = GetTickCount64() - m_sortTurnStart;
    if (t >= static_cast<ULONGLONG>(kSortTurnMs))
        return to;
    return m_sortTurnFrom + (to - m_sortTurnFrom) * static_cast<float>(t) / kSortTurnMs;
}

void FileList::DrawHeader(HDC hdc, const RECT& client)
{
    const Theme& t = *m_theme;
    CacheColumns();
    // Files' HeaderGrid: BorderThickness 0,0,0,1 in DividerStrokeColorDefault
    const int line = std::max(1, t.S(1));
    FillSolid(hdc, { client.left, client.bottom - line, client.right, client.bottom }, t.line);
    const COLORREF text = t.headerText;
    m_headerCheck = HeaderCheckState();
    DrawCheck(hdc, HeaderCheckRect(), m_headerCheck, m_headerCheckHot, m_headerCheckPressed && m_headerCheckHot, t.content);
    for (int i = ColName; i < kColumnCount; ++i)
    {
        const int l = m_colLeft[i], r = m_colRight[i];
        if (r <= l)
            continue;
        // The header button: Margin -4,0 (the name header's 4,0,-4,0), ending at the grid splitter (12) + 4
        const int bl = l + (i == 1 ? t.S(4) : -t.S(4)), br = r - t.S(8);
        if (i == m_headerHot)
            FillSolid(hdc, { bl, 0, br, client.bottom - line }, t.headerHover);
        const bool sorted = kColumns[i].key == static_cast<int>(m_sortKey);
        DrawLabel(hdc, m_headerFont, Tr(kColumns[i].title), { bl + t.S(12), t.S(12), br - t.S(12) - (sorted ? t.S(8) : 0), t.S(28) }, text);
        if (!sorted)
            continue;
        // Files' SortIcon (EDDB, 8px), turned 180 degrees when descending
        const RECT g{ br - t.S(20), t.S(12), br - t.S(12), client.bottom - line };
        if (const float turn = SortTurn(); turn != 0.0f && turn != -180.0f)
        {
            DrawGlyphTurned(hdc, m_glyphFont, L"\xEDDB", g, text, turn); // on its way (SetSort)
            continue;
        }
        const int mode = SetGraphicsMode(hdc, GM_ADVANCED);
        if (m_sortDesc)
        {
            const float cx = (g.left + g.right) / 2.0f, cy = (g.top + g.bottom) / 2.0f;
            const XFORM turn{ -1.0f, 0.0f, 0.0f, -1.0f, 2 * cx, 2 * cy };
            SetWorldTransform(hdc, &turn);
        }
        DrawGlyph(hdc, m_glyphFont, L"\xEDDB", g, text);
        ModifyWorldTransform(hdc, nullptr, MWT_IDENTITY);
        SetGraphicsMode(hdc, mode);
    }
    // Files' HeaderGridSplitterStyle after every shown column (12 wide, from the button's end): the 1px line in its
    // middle, over the buttons (Canvas.ZIndex 1: the next button's hover doesn't cover it)
    for (int i = ColName; i < kColumnCount; ++i)
    {
        if (m_colRight[i] <= m_colLeft[i])
            continue;
        const int mid = m_colRight[i] - t.S(8) + t.S(6);
        FillSolid(hdc, { mid, 0, mid + std::max(1, t.S(1)), client.bottom - line }, t.headerDivider);
    }
}

// WinUI CheckBox (DefaultCheckBoxStyle): 20px box, corner 4 (ControlCornerRadius), 1px stroke; checked / indeterminate in
// the accent with the 12px glyph (E73E / E9AE) 1px down; `under` is what the box is drawn over
void FileList::DrawCheck(HDC hdc, const RECT& box, Check state, bool hot, bool pressed, COLORREF under)
{
    const Theme& t = *m_theme;
    if (box.right <= box.left)
        return;
    const int i = pressed ? 2 : hot ? 1 : 0;
    if (state == Check::Off)
    {
        FillRound(hdc, box, t.S(4), Mix(under, t.checkFill[i], t.checkFillPct[i]));
        FrameRound(hdc, box, t.S(4), Mix(under, t.checkStroke[pressed ? 1 : 0], t.checkStrokePct[pressed ? 1 : 0]));
        return;
    }
    const COLORREF fill = Mix(under, t.accent, t.checkAccentPct[i]);
    FillRound(hdc, box, t.S(4), fill);
    const RECT glyph{ box.left, box.top + t.S(1), box.right, box.bottom + t.S(1) };
    DrawGlyph(hdc, t.iconsMid, state == Check::On ? L"\xE73E" : L"\xE9AE", glyph,
        pressed ? Mix(fill, t.textOnAccent, t.checkGlyphPressedPct) : t.textOnAccent);
}

// Everything in the items window that meets `clip`: group headers, rows / cells, the empty folder text
void FileList::DrawItems(HDC hdc, const RECT& clip)
{
    CacheColumns();
    RECT client{};
    GetClientRect(m_list, &client);
    FillSolid(hdc, clip, m_theme->content);
    if (m_blank)
        return; // 刷新: Files clears the items while the folder is listed again
    RECT content = clip;
    OffsetRect(&content, m_scroll.x, m_scroll.y);
    for (int g : m_layout.HeadersIn(content))
    {
        RECT h = m_layout.HeaderRect(g);
        OffsetRect(&h, -m_scroll.x, -m_scroll.y);
        if (m_mode == Mode::Details || m_mode == Mode::Column)
        {
            h.right = std::max<LONG>(h.right, client.right);
            DrawGroupHeader(hdc, g, h); // (ColumnLayoutPage's group header: the details one)
        }
        else
            DrawCellHeader(hdc, g, h);
    }
    for (int row : m_layout.RowsIn(content))
    {
        const RECT rc = RowRect(row);
        switch (m_mode)
        {
        case Mode::List: DrawListCell(hdc, row, rc); break;
        case Mode::Cards: DrawCard(hdc, row, rc); break;
        case Mode::Grid: DrawGridCell(hdc, row, rc); break;
        case Mode::Column: DrawColumnRow(hdc, row, rc); break;
        default: DrawRow(hdc, row, rc); break;
        }
    }
    DrawFocusFrame(hdc);
    if (m_listed && Count() == 0)
        DrawEmpty(hdc, client);
    RequestThumbs(); // (what was painted without its picture)
}

// Files' group header (DetailsLayoutPage GroupStyle.HeaderTemplate in a ListViewHeaderItem): Padding 12,8,12,0; the
// title 16px SemiBold, the count "{0} 个项目" 14px beside it (Margin 4), or under it (Margin 0,2) with the subtitle beside
// the title (ShowCountTextBelow); the 1px divider (SystemControlForegroundBaseLowBrush) 8 under the text
void FileList::DrawGroupHeader(HDC hdc, int group, const RECT& rc)
{
    const Theme& t = *m_theme;
    if (group < 0 || group >= static_cast<int>(m_groupInfo.size()))
        return;
    const GroupInfo& info = m_groupInfo[static_cast<size_t>(group)];
    const std::wstring count = Tr(L"{0} 个项目");
    std::wstring countText = count;
    if (const size_t at = countText.find(L"{0}"); at != std::wstring::npos)
        countText.replace(at, 3, std::to_wstring(m_layout.Group(group).count));
    const int left = rc.left + t.S(24), top = rc.top + t.S(8);
    const int titleW = TextWidth(hdc, m_groupFont, info.title);
    DrawLabel(hdc, m_groupFont, info.title, { left, top, std::min<LONG>(left + titleW, rc.right - t.S(12)), top + t.S(22) }, t.ink);
    const int beside = left + titleW + t.S(4);
    if (!info.countBelow)
        DrawLabel(hdc, t.ui, countText, { beside + t.S(4), top, rc.right - t.S(12), top + t.S(22) }, t.ink);
    else
    {
        DrawLabel(hdc, t.ui, info.subtitle, { beside, top, rc.right - t.S(12), top + t.S(22) }, t.ink);
        DrawLabel(hdc, t.ui, countText, { left, top + t.S(24), rc.right - t.S(12), top + t.S(44) }, t.ink);
    }
    const int line = rc.bottom - t.S(4) - std::max(1, t.S(1));
    FillSolid(hdc, { left - t.S(4), line, rc.right - t.S(24), line + std::max(1, t.S(1)) }, t.groupDivider);
}

// Files' FolderEmptyIndicator: "此文件夹为空。" centered, 125 from the top
void FileList::DrawEmpty(HDC hdc, const RECT& client)
{
    const Theme& t = *m_theme;
    DrawLabel(hdc, t.ui, Tr(L"此文件夹为空。"), { client.left, t.S(125), client.right, t.S(125) + t.S(20) }, t.ink, DT_CENTER);
}

void FileList::BuildLayers()
{
    const Theme& t = *m_theme;
    m_hoverKey = t.listHover;
    m_pressedKey = t.listPressed;
    // Keys: distinct from each other and from the window's own (chrome / layer / toolbar / the card)
    auto nudge = [&](COLORREF& c, COLORREF other)
    {
        for (int i = 0; i < 4 && (c == t.content || c == t.chrome || c == t.layer || c == t.toolbar || c == other); ++i)
            c = NudgeKey(c, i);
    };
    nudge(m_hoverKey, t.content);
    nudge(m_pressedKey, m_hoverKey);
    // Files: the file area card over the nav layer over the material; the row fills over the card
    m_layers = {
        { t.content, { t.layerFill, t.contentFill, 0 }, { t.layerPct, t.contentPct, 0 } },
        { m_hoverKey, { t.layerFill, t.contentFill, t.overlay }, { t.layerPct, t.contentPct, t.listHoverPct } },
        { m_pressedKey, { t.layerFill, t.contentFill, t.overlay }, { t.layerPct, t.contentPct, t.listPressedPct } } };
    BuildCellLayers();
}

void FileList::UpdateFonts()
{
    for (HFONT* f : { &m_headerFont, &m_glyphFont, &m_groupFont, &m_strongFont })
        if (*f)
        {
            DeleteObject(*f);
            *f = nullptr;
        }
    LOGFONTW lf{};
    if (m_theme->caption && GetObjectW(m_theme->caption, sizeof(lf), &lf))
    {
        lf.lfWeight = FW_SEMIBOLD; // DataGridHeader: 12px SemiBold
        m_headerFont = CreateFontIndirectW(&lf);
    }
    if (m_theme->icons && GetObjectW(m_theme->icons, sizeof(lf), &lf))
    {
        lf.lfHeight = -m_theme->S(8);
        m_glyphFont = CreateFontIndirectW(&lf);
    }
    if (m_theme->ui && GetObjectW(m_theme->ui, sizeof(lf), &lf))
    {
        const LONG body = lf.lfHeight;
        lf.lfHeight = -m_theme->S(16); // the group header's title: SubtitleTextBlockStyle at 16, SemiBold
        lf.lfWeight = FW_SEMIBOLD;
        m_groupFont = CreateFontIndirectW(&lf);
        lf.lfHeight = body;            // BodyStrongTextBlockStyle: the body size, SemiBold
        m_strongFont = CreateFontIndirectW(&lf);
    }
}

void FileList::ApplyTheme()
{
    if (!m_list || !m_theme)
        return;
    const Theme& t = *m_theme;
    UpdateFonts();
    BuildLayers();
    if (m_editBrush)
        DeleteObject(m_editBrush);
    m_editBrush = CreateSolidBrush(t.field);
    DarkMode::ApplyControl(m_list, t.dark); // scroll bars
    SendMessageW(m_header, WM_SETFONT, reinterpret_cast<WPARAM>(t.caption), FALSE);
    // Column widths follow the DPI (kept at 96 DPI)
    if (m_header && m_columnDpi != static_cast<int>(t.dpi))
        ApplyColumnWidths();
    m_scrolled = { -1, -1 };
    Relayout();
    InvalidateRect(m_list, nullptr, FALSE);
    if (m_header)
        InvalidateRect(m_header, nullptr, FALSE);
}
