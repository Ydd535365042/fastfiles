#include "FileList.h"
#include "Tab.h"
#include "Format.h"
#include "Lang.h"

// Files' layouts on the own list (Views/Layouts, 96 DPI). LayoutSizeKindHelper: the icon size per layout and size kind,
// details rows 28 / 36 / 40 / 44 / 48, list rows 24 / 32 / 36 / 40 / 44, grid cells 80..300 wide in steps of 20.
// GridLayoutPage (GridView Padding 8; GridViewItem Margin 0,0,4,4, Padding 0):
//  list  (ItemsWrapGrid Orientation Vertical: columns scrolling sideways): 260 wide, Padding 12,0, CornerRadius 4; the icon
//        box iconSize + 4, the 20-wide SelectionCheckbox in its place when shown; the name Margin 5,0, one line, trimmed;
//        LocalListItemContainerStyle (SubtleFillColorSecondary / Tertiary backgrounds, no border)
//  cards (Orientation Horizontal): a card (CardBackgroundFillColorDefault, 1px CardStrokeColorDefault, CornerRadius 8) with
//        the picture box (SystemFillColorNeutralBackground) beside (Small) or over the details box (Padding 12,4): the
//        name (BodyStrong, 2 lines, 1 at ExtraLarge), the type (caption, TextFillColorSecondary); at the bottom the
//        contextual property (not at Small), the size and the tags (12px, Spacing -4)
//  grid  : the picture in an ItemWidth square (Margin 12, Uniform), the name under it (60 high, centered, wrapping,
//        Margin 4,0,4,8) after the first tag's 12px dot
// Cards and grid: LocalRegularItemContainerStyle (CornerRadius 8; App.Theme.CardBackgroundFillColorTertiary hovered /
// pressed, CardBackgroundFillColorDefault selected with the 2px AccentFillColorDefault border); the checkbox top left
// (Width 32, Margin 6). Group headers (GridViewHeaderItem): MinHeight 44, Padding 12,8,12,0, Margin 0,0,0,4; the title
// 16px SemiBold, the count beside it (Spacing 8), the divider (SystemControlForegroundBaseLowBrush) after them.

namespace
{
std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

constexpr int kDetailsRow[5] = { 28, 36, 40, 44, 48 };
constexpr int kListRow[5] = { 24, 32, 36, 40, 44 };
constexpr int kListIcon[5] = { 16, 16, 20, 24, 32 }; // details / list / columns
constexpr int kCardIcon[4] = { 64, 64, 80, 96 };
// GridLayoutPage.CardsView*: the picture box and the details box (W x H) per CardsViewSizeKind
constexpr SIZE kCardBox[4] = { { 104, 104 }, { 240, 96 }, { 280, 128 }, { 320, 160 } };
constexpr SIZE kCardDetails[4] = { { 196, 104 }, { 240, 144 }, { 280, 144 }, { 320, 128 } };
constexpr int kGridName = 60;     // the grid name's TextBlock Height
constexpr int kItemMargin = 4;    // GridViewItem Margin 0,0,4,4

// Text wrapped into at most `maxLines` lines of `width` (at spaces where there are any, else between characters); the
// last line keeps the rest (DrawLabel trims it with an ellipsis: TextTrimming CharacterEllipsis)
std::vector<std::wstring> WrapLines(HDC hdc, HFONT font, const std::wstring& text, int width, int maxLines)
{
    std::vector<std::wstring> lines;
    HGDIOBJ old = SelectObject(hdc, font);
    size_t at = 0;
    while (at < text.size() && static_cast<int>(lines.size()) < maxLines)
    {
        if (static_cast<int>(lines.size()) == maxLines - 1)
        {
            lines.push_back(text.substr(at));
            break;
        }
        // How much fits
        int fit = 0;
        SIZE size{};
        GetTextExtentExPointW(hdc, text.c_str() + at, static_cast<int>(text.size() - at), std::max(1, width), &fit, nullptr, &size);
        if (at + static_cast<size_t>(fit) >= text.size())
        {
            lines.push_back(text.substr(at));
            break;
        }
        size_t end = at + static_cast<size_t>(std::max(1, fit));
        // At a space: the line ends there; else back to the last space in it (kept at the end of this line), else mid-word
        if (text[end] != L' ')
            if (const size_t space = text.find_last_of(L' ', end - 1); space != std::wstring::npos && space > at)
                end = space + 1;
        while (end < text.size() && text[end] == L' ')
            ++end;
        std::wstring part = text.substr(at, end - at);
        while (!part.empty() && part.back() == L' ')
            part.pop_back();
        lines.push_back(std::move(part));
        at = end;
    }
    SelectObject(hdc, old);
    if (lines.empty())
        lines.emplace_back();
    return lines;
}
}

int FileList::MaxSize(Mode mode)
{
    switch (mode)
    {
    case Mode::Cards: return 4;
    case Mode::Grid: return 12;
    default: return 5; // details / list / columns: Compact .. ExtraLarge
    }
}

int FileList::Size() const
{
    const ViewPrefs* prefs = m_events ? m_events->ListPrefs() : nullptr;
    constexpr int kDefaults[kModeCount] = { 2, 2, 1, 8, 2 }; // Files' defaults: Small, Small, Small, Large, Small
    const int i = static_cast<int>(m_mode);
    return std::clamp(prefs ? prefs->layoutSize[i] : kDefaults[i], 1, MaxSize(m_mode));
}

int FileList::IconSize() const
{
    const int size = Size();
    switch (m_mode)
    {
    case Mode::Cards: return kCardIcon[size - 1];
    case Mode::Grid: return size <= 1 ? 96 : size <= 8 ? 128 : 256;
    default: return kListIcon[size - 1];
    }
}

int FileList::RowHeight() const
{
    // GetListViewRowHeight / GetColumnsViewRowHeight are the same 24 .. 44
    return m_mode == Mode::List || m_mode == Mode::Column ? kListRow[Size() - 1] : kDetailsRow[Size() - 1];
}

// The cells (a GridViewItem: the template plus Margin 0,0,4,4; the panel sizes every cell like the first)
void FileList::LayoutMetrics(ItemsMetrics& m) const
{
    const Theme& t = *m_theme;
    const int size = Size();
    switch (m_mode)
    {
    case Mode::Details:
        m.itemHeight = t.S(RowHeight());
        m.padBottom = t.S(12); // ListView Padding 8,0,8,12
        return;
    case Mode::Column:
        // ColumnLayoutPage: the row (RowHeight) with Margin 0,2 in a ListViewItem (MinHeight 1, Margin 0; Compact: Margin -2,
        // so the rows overlap by 4 and sit 2 higher); ListView Padding 8,0,8,0
        m.itemHeight = t.S(RowHeight() + (size == 1 ? 0 : 4));
        return;
    case Mode::List:
        m.wrap = m.vertical = true;
        m.itemWidth = t.S(260);
        m.itemHeight = t.S(RowHeight());
        break;
    case Mode::Cards:
    {
        const SIZE box = kCardBox[size - 1], details = kCardDetails[size - 1];
        const bool beside = size == 1; // CardsViewOrientation: Horizontal at Small
        m.wrap = true;
        m.itemWidth = t.S((beside ? box.cx + details.cx : std::max(box.cx, details.cx)) + 2);
        m.itemHeight = t.S((beside ? std::max(box.cy, details.cy) : box.cy + details.cy) + 2);
        break;
    }
    case Mode::Grid:
    {
        const int w = 60 + 20 * size; // GetGridViewItemWidth: 80 .. 300
        m.wrap = true;
        m.itemWidth = t.S(w);
        m.itemHeight = t.S(w + kGridName + 8);
        break;
    }
    }
    // GridView Padding 8; the items' margins between them (and after the last one). The compact list:
    // LocalCompactListItemContainerStyle, Margin 0
    const int margin = m_mode == Mode::List && size == 1 ? 0 : t.S(kItemMargin);
    m.spacingX = m.spacingY = margin;
    m.padLeft = m.padTop = m.padRight = m.padBottom = t.S(8);
    m.padRight += margin;
    m.padBottom += margin;
}
void FileList::SetMode(Mode mode)
{
    if (mode == m_mode)
        return;
    if (m_edit)
        CommitRename(false);
    m_mode = mode;
    m_keyboardFocus = false; // (another page: no focus visual until the keyboard moves again)
    // Another layout page: from the top (a size change keeps the offsets, Files' "Restore correct scroll position")
    ScrollTo(0, 0);
    m_scroll = { 0, 0 };
    ApplyLayout();
}

void FileList::ApplyLayout()
{
    if (!m_list || !m_theme)
        return;
    if (m_edit)
        CommitRename(false);
    // Details: the icon column is the icon box (iconSize + 4) after the 24 of the list's and the item's padding
    if (m_mode == Mode::Details)
    {
        const int width = 24 + IconSize() + 4;
        if (m_width96[ColIcon] != width)
        {
            m_width96[ColIcon] = width;
            ApplyColumnWidths();
        }
    }
    ResetThumbs();
    m_hot = m_pressed = m_hotCheck = -1;
    Relayout();
    PlaceHeader();
    InvalidateRect(m_list, nullptr, FALSE);
}

// GridViewItem's background for the row's state (ListViewItemPresenter: pressed wins over hovered)
FileList::Back FileList::CellBack(int row) const
{
    const bool selected = m_sel.Selected(row);
    const bool hot = row == m_hot || row == m_dropRow, pressed = row == m_pressed;
    if (selected)
        return pressed ? BackSelectedPressed : hot ? BackHover : BackSelected;
    return hot || pressed ? BackHover : BackNone;
}

// Files' UpdateCheckboxVisibility (every layout): 项目复选框 on and selected, or the mouse over the box's area
bool FileList::CheckShown(int row) const
{
    const ViewPrefs* prefs = m_events->ListPrefs();
    return (prefs && prefs->checkboxes && m_sel.Selected(row)) || row == m_hotCheck || row == m_pressedCheck;
}

void FileList::DrawCheckFor(HDC hdc, int row, const RECT& hit, COLORREF under)
{
    if (!CheckShown(row))
        return;
    const Theme& t = *m_theme;
    const bool hot = row == m_hotCheck, down = row == m_pressedCheck;
    // A 20px box: centered in the hit area (details / list), at the top left of the 32px CheckBox (cards / grid)
    RECT box{};
    if (m_mode == Mode::Details || m_mode == Mode::List)
    {
        const int mid = (hit.top + hit.bottom) / 2, cx = (hit.left + hit.right) / 2;
        box = { cx - t.S(10), mid - t.S(10), cx + t.S(10), mid + t.S(10) };
    }
    else
        box = { hit.left, hit.top + t.S(6), hit.left + t.S(20), hit.top + t.S(26) };
    DrawCheck(hdc, box, m_sel.Selected(row) ? Check::On : Check::Off, hot, down && hot, under);
}

// The item's picture fitted into `box` (Image Stretch Uniform): the thumbnail when made, else the system's icon
void FileList::DrawPicture(HDC hdc, int row, const RECT& box, bool dim)
{
    if (row < 0 || row >= Count() || box.right <= box.left || box.bottom <= box.top)
        return;
    Item& item = m_items[m_order[row]];
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    if (!item.thumbAsked && WantsThumbs())
        m_wantThumbs.push_back(row);
    if (item.thumb && item.thumbW > 0 && item.thumbH > 0)
    {
        const double scale = std::min(static_cast<double>(bw) / item.thumbW, static_cast<double>(bh) / item.thumbH);
        const int w = std::max(1, static_cast<int>(item.thumbW * scale + 0.5)), h = std::max(1, static_cast<int>(item.thumbH * scale + 0.5));
        const int x = box.left + (bw - w) / 2, y = box.top + (bh - h) / 2;
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, static_cast<HBITMAP>(item.thumb.get()));
        SetStretchBltMode(hdc, HALFTONE);
        const BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(dim ? 102 : 255), AC_SRC_ALPHA }; // DimItemOpacity 0.4
        AlphaBlend(hdc, x, y, w, h, mem, 0, 0, item.thumbW, item.thumbH, blend);
        SelectObject(mem, old);
        DeleteDC(mem);
        if (item.link && m_linkOverlay >= 0 && m_icons)
        {
            // The shortcut arrow at the picture's bottom left (Files' ShortcutGlyphElement)
            int cx = 16, cy = 16;
            ImageList_GetIconSize(m_icons, &cx, &cy);
            IMAGELISTDRAWPARAMS p{ sizeof(p) };
            p.himl = m_icons;
            p.i = m_linkOverlay;
            p.hdcDst = hdc;
            p.x = x;
            p.y = y + h - cy;
            p.rgbBk = p.rgbFg = CLR_NONE;
            p.fStyle = ILD_TRANSPARENT;
            ImageList_DrawIndirect(&p);
        }
        return;
    }
    if (item.icon < 0)
        return;
    // The system's icon at the largest size that fits (16 / 32 / 48 / 256), centered
    HIMAGELIST list = m_icons;
    int cx = 16, cy = 16;
    if (list)
        ImageList_GetIconSize(list, &cx, &cy);
    for (HIMAGELIST candidate : m_iconLists)
    {
        int w = 0, h = 0;
        if (candidate && ImageList_GetIconSize(candidate, &w, &h) && w <= bw && h <= bh && w > cx)
            list = candidate, cx = w, cy = h;
    }
    if (!list)
        return;
    IMAGELISTDRAWPARAMS p{ sizeof(p) };
    p.himl = list;
    p.i = item.icon;
    p.hdcDst = hdc;
    p.x = box.left + (bw - cx) / 2;
    p.y = box.top + (bh - cy) / 2;
    p.rgbBk = p.rgbFg = CLR_NONE;
    p.fStyle = ILD_TRANSPARENT;
    p.fState = dim ? ILS_ALPHA : ILS_NORMAL;
    p.Frame = dim ? 102 : 0;
    ImageList_DrawIndirect(&p);
    if (item.link && m_linkOverlay >= 0)
    {
        p.i = m_linkOverlay;
        ImageList_DrawIndirect(&p);
    }
}
namespace
{
// ListViewItemPresenter over a cell: the 2px selected border (AccentFillColorDefault, Secondary hovered, Tertiary pressed);
// the focus visual after keyboard navigation outside it (DefaultGridViewItemStyle FocusVisualMargin -3: the 2px
// FocusStrokeColorOuter, the 1px inner stroke in the background color)
void DrawCellFrame(HDC hdc, const Theme& t, const RECT& rc, int radius, COLORREF under, bool selected, bool hot, bool pressed, bool focus)
{
    auto frame = [&](const RECT& r, int width, int corner, COLORREF color)
    {
        for (int k = 0; k < width; ++k)
            FrameRound(hdc, { r.left + k, r.top + k, r.right - k, r.bottom - k }, std::max(0, corner - k), color);
    };
    if (selected)
        frame(rc, t.S(2), radius, Mix(under, t.accent, pressed ? t.checkAccentPct[2] : hot ? t.checkAccentPct[1] : t.checkAccentPct[0]));
    if (focus)
    {
        const int out = t.S(3);
        const RECT f{ rc.left - out, rc.top - out, rc.right + out, rc.bottom + out };
        frame(f, t.S(2), radius + out, t.focusStroke);
        frame({ f.left + t.S(2), f.top + t.S(2), f.right - t.S(2), f.bottom - t.S(2) }, std::max(1, t.S(1)), radius + out - t.S(2), t.content);
    }
}
}

// List: LocalListItemContainerStyle (GridViewItem: SubtleFillColorSecondary hovered and selected-pressed, Tertiary
// pressed / selected)
void FileList::DrawListCell(HDC hdc, int row, const RECT& rc)
{
    const Theme& t = *m_theme;
    const Item& item = m_items[m_order[row]];
    const bool selected = m_sel.Selected(row), hot = row == m_hot || row == m_dropRow, pressed = row == m_pressed;
    COLORREF fill = t.content;
    if (selected)
        fill = pressed ? m_hoverKey : m_pressedKey;
    else if (pressed)
        fill = m_pressedKey;
    else if (hot)
        fill = m_hoverKey;
    if (fill != t.content)
        FillRound(hdc, rc, t.S(4), fill);
    // The icon column (Padding 12): the icon box (iconSize + 4), or the 20-wide checkbox in its place
    const int column = std::max(t.S(IconSize() + 4), t.S(20)), left = rc.left + t.S(12);
    const RECT hit{ left, rc.top, left + column, rc.bottom };
    const bool dim = (item.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    if (CheckShown(row))
        DrawCheckFor(hdc, row, hit, fill);
    else
    {
        const int icon = t.S(IconSize()), cx = (hit.left + hit.right) / 2, cy = (rc.top + rc.bottom) / 2;
        DrawPicture(hdc, row, { cx - icon / 2, cy - icon / 2, cx - icon / 2 + icon, cy - icon / 2 + icon }, dim);
    }
    DrawLabel(hdc, t.ui, item.display, { hit.right + t.S(5), rc.top, rc.right - t.S(12) - t.S(5), rc.bottom },
        dim ? Mix(fill, t.ink, 40) : t.ink);
}

// ColumnLayoutPage's row: the ListViewItem's backplate (the ListView's Padding 8 + 4; 2 inside the row's Margin 0,2), the
// accent pill when selected, then the template (Padding 12,0): the icon box (iconSize + 4), the name (Margin 5,0), and
// on the right the first tag's dot (12, Margin 8,0,4,0) and the folder chevron (E76C, 12px, TextFillColorSecondary).
// The row leading to the next column keeps the selected background (PathTracedRowStyle)
void FileList::DrawColumnRow(HDC hdc, int row, const RECT& rcIn)
{
    const Theme& t = *m_theme;
    const Item& item = m_items[m_order[row]];
    // Compact: Margin -2 (the cell is 4 shorter than the row it shows)
    RECT rc = rcIn;
    if (Size() == 1)
        InflateRect(&rc, 0, t.S(2));
    const bool selected = m_sel.Selected(row);
    const bool traced = !selected && !m_traced.empty() && Lower(item.name) == m_traced;
    const bool hot = row == m_hot || row == m_dropRow, pressed = row == m_pressed;
    COLORREF fill = t.content;
    if (pressed ? !selected : selected && hot)
        fill = m_pressedKey;
    else if (selected || traced || hot || pressed)
        fill = m_hoverKey;
    const RECT bg{ rc.left + t.S(12), rc.top + t.S(4), rc.right - t.S(12), rc.bottom - t.S(4) };
    if (fill != t.content)
        FillRound(hdc, bg, t.S(4), fill);
    if (selected)
    {
        const int mid = (rc.top + rc.bottom) / 2;
        FillRound(hdc, { bg.left, mid - t.S(8), bg.left + t.S(3), mid + t.S(8) }, std::max(1, t.S(3) / 2), t.accent);
    }
    if (m_keyboardFocus && row == FocusedIndex() && GetFocus() == m_list)
    {
        FrameRound(hdc, bg, t.S(4), t.focusStroke);
        FrameRound(hdc, { bg.left + 1, bg.top + 1, bg.right - 1, bg.bottom - 1 }, t.S(4), t.focusStroke);
    }
    // The template's content: the backplate's box less the template's Padding 12,0
    const int left = bg.left + t.S(12), right = bg.right - t.S(12), cy = (rc.top + rc.bottom) / 2;
    const bool dim = (item.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    const int box = t.S(IconSize() + 4), icon = t.S(IconSize());
    const int bx = left + box / 2;
    DrawPicture(hdc, row, { bx - icon / 2, cy - icon / 2, bx - icon / 2 + icon, cy - icon / 2 + icon }, dim);
    // Right to left: the chevron on folders, the tag's dot before it (StackPanel Spacing 4)
    int end = right;
    if (item.attrs & FILE_ATTRIBUTE_DIRECTORY)
    {
        const int g = t.S(12);
        DrawGlyph(hdc, t.iconsMid, L"\xE76C", { end - g, cy - g, end, cy + g }, Mix(fill, t.secondaryText, t.secondaryTextPct));
        end -= g + t.S(4);
    }
    if (const FileTag* tag = item.tags.empty() ? nullptr : Tags::Find(m_tags, item.tags.front()))
    {
        end -= t.S(4);
        FillRound(hdc, { end - t.S(12), cy - t.S(6), end, cy + t.S(6) }, t.S(6), tag->color);
        end -= t.S(12) + t.S(8);
    }
    DrawLabel(hdc, t.ui, item.display, { left + box + t.S(5), rc.top, end - t.S(5), rc.bottom }, dim ? Mix(fill, t.ink, 40) : t.ink);
}

int FileList::NameWidth() const
{
    if (!m_list || !m_theme)
        return 0;
    HDC hdc = GetDC(m_list);
    int widest = 0;
    for (int row = 0; row < std::min(Count(), 2000); ++row)
        widest = std::max(widest, TextWidth(hdc, m_theme->ui, m_items[m_order[row]].display));
    ReleaseDC(m_list, hdc);
    return widest;
}

void FileList::SetTraced(const std::wstring& name)
{
    const std::wstring lower = Lower(name);
    if (lower == m_traced)
        return;
    m_traced = lower;
    if (m_list)
        InvalidateRect(m_list, nullptr, FALSE);
}

void FileList::DrawCard(HDC hdc, int row, const RECT& rc)
{
    const Theme& t = *m_theme;
    const Item& item = m_items[m_order[row]];
    const int size = Size();
    const SIZE boxSize = kCardBox[size - 1], details = kCardDetails[size - 1];
    const bool beside = size == 1;
    const Back back = CellBack(row);
    const bool selected = m_sel.Selected(row), hot = row == m_hot || row == m_dropRow, pressed = row == m_pressed;
    const int radius = t.S(8), line = t.S(1);
    // The card: its stroke over the presenter's background, the face inside
    FillRound(hdc, rc, radius, m_cardStroke[back]);
    const RECT face{ rc.left + line, rc.top + line, rc.right - line, rc.bottom - line };
    FillRound(hdc, face, radius - line, m_cardFace[back]);
    // The picture box, rounded where it meets the card's corners
    const RECT box{ face.left, face.top, face.left + t.S(boxSize.cx), face.top + t.S(boxSize.cy) };
    FillRound(hdc, box, radius - line, m_cardBox[back]);
    if (beside)
        FillSolid(hdc, { (box.left + box.right) / 2, box.top, box.right, box.bottom }, m_cardBox[back]);
    else
        FillSolid(hdc, { box.left, (box.top + box.bottom) / 2, box.right, box.bottom }, m_cardBox[back]);
    const bool dim = (item.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    const int iconH = t.S(IconSize()), mid = (box.top + box.bottom) / 2;
    DrawPicture(hdc, row, { box.left, mid - iconH / 2, box.right, mid - iconH / 2 + iconH }, dim);
    // The details box (Padding 12,4): five equal rows, the name and type in the first two, the rest at the bottom
    const RECT d = beside ? RECT{ box.right, face.top, box.right + t.S(details.cx), face.top + t.S(details.cy) }
                          : RECT{ face.left, box.bottom, face.left + t.S(details.cx), box.bottom + t.S(details.cy) };
    const RECT in{ d.left + t.S(12), d.top + t.S(4), d.right - t.S(12), d.bottom - t.S(4) };
    const int split = in.top + 2 * (in.bottom - in.top) / 5;
    const COLORREF under = m_cardFace[back];
    const COLORREF sub = Mix(under, t.secondaryText, t.secondaryTextPct); // TextFillColorSecondary
    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, in.left, in.top, in.right, split);
    int y = in.top;
    for (const std::wstring& text : WrapLines(hdc, m_strongFont, item.display, in.right - in.left, size == 4 ? 1 : 2))
    {
        DrawLabel(hdc, m_strongFont, text, { in.left, y, in.right, y + t.S(20) }, dim ? Mix(under, t.ink, 40) : t.ink);
        y += t.S(20);
    }
    y += t.S(4); // StackPanel Spacing 4
    DrawLabel(hdc, t.caption, item.type, { in.left, y, in.right, y + t.S(16) }, sub);
    RestoreDC(hdc, saved);
    // Bottom up: the tags (MinHeight 16), the size, the contextual property (not at Small); Spacing 4
    saved = SaveDC(hdc);
    IntersectClipRect(hdc, in.left, split, in.right, in.bottom);
    int bottom = in.bottom;
    int x = in.left;
    for (const std::wstring& id : item.tags)
        if (const FileTag* tag = Tags::Find(m_tags, id))
        {
            DrawGlyph(hdc, t.iconsMid, Glyph::Tag, { x, bottom - t.S(14), x + t.S(12), bottom - t.S(2) }, tag->color);
            x += t.S(12) - t.S(4);
        }
    bottom -= t.S(16) + t.S(4);
    if (!(item.attrs & FILE_ATTRIBUTE_DIRECTORY))
        DrawLabel(hdc, t.caption, Format::Size(item.size), { in.left, bottom - t.S(16), in.right, bottom }, sub);
    bottom -= t.S(16) + t.S(4);
    if (size > 1)
        DrawLabel(hdc, t.caption, ContextText(item), { in.left, bottom - t.S(16), in.right, bottom }, sub);
    RestoreDC(hdc, saved);
    DrawCellFrame(hdc, t, rc, radius, m_cardStroke[back], selected, hot, pressed, false);
    DrawCheckFor(hdc, row, CheckRect(rc), m_cardBox[back]);
}

// The focus visual of the focused cell after keyboard navigation, over its neighbours (drawn after every cell)
void FileList::DrawFocusFrame(HDC hdc)
{
    const int row = FocusedIndex();
    if (m_mode == Mode::Details || m_mode == Mode::Column || !m_keyboardFocus || row < 0 || GetFocus() != m_list)
        return; // (rows draw theirs)
    const Theme& t = *m_theme;
    DrawCellFrame(hdc, t, RowRect(row), t.S(m_mode == Mode::List ? 4 : 8), t.content, false, false, false, true);
}

void FileList::DrawGridCell(HDC hdc, int row, const RECT& rc)
{
    const Theme& t = *m_theme;
    const Item& item = m_items[m_order[row]];
    const Back back = CellBack(row);
    const bool selected = m_sel.Selected(row), hot = row == m_hot || row == m_dropRow, pressed = row == m_pressed;
    const int radius = t.S(8);
    const COLORREF under = m_cellFill[back];
    if (back != BackNone)
        FillRound(hdc, rc, radius, under);
    // The picture: an ItemWidth square, the image in it with Margin 12
    const int w = rc.right - rc.left;
    const bool dim = (item.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    DrawPicture(hdc, row, { rc.left + t.S(12), rc.top + t.S(12), rc.right - t.S(12), rc.top + w - t.S(12) }, dim);
    // The name under it (Margin 4,0,4,8; 60 high, up to three centered lines), after the first tag's dot
    const RECT name{ rc.left + t.S(4), rc.top + w, rc.right - t.S(4), rc.top + w + t.S(kGridName) };
    const FileTag* tag = item.tags.empty() ? nullptr : Tags::Find(m_tags, item.tags.front());
    const int dot = tag ? t.S(16) : 0;
    const int maxText = std::max(1, static_cast<int>(name.right - name.left) - dot);
    const std::vector<std::wstring> lines = WrapLines(hdc, t.ui, item.display, maxText, 3);
    int textW = 0;
    for (const std::wstring& text : lines)
        textW = std::max(textW, TextWidth(hdc, t.ui, text));
    textW = std::min(textW, maxText);
    const int left = name.left + (static_cast<int>(name.right - name.left) - dot - textW) / 2;
    if (tag)
    {
        const int cy = (name.top + name.bottom) / 2;
        FillRound(hdc, { left, cy - t.S(6), left + t.S(12), cy + t.S(6) }, t.S(6), tag->color);
    }
    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, name.left, name.top, name.right, name.bottom);
    int y = name.top;
    for (const std::wstring& text : lines)
    {
        DrawLabel(hdc, t.ui, text, { left + dot, y, left + dot + textW, y + t.S(20) }, dim ? Mix(under, t.ink, 40) : t.ink, DT_CENTER);
        y += t.S(20);
    }
    RestoreDC(hdc, saved);
    DrawCellFrame(hdc, t, rc, radius, under, selected, hot, pressed, false);
    DrawCheckFor(hdc, row, CheckRect(rc), under);
}
// GridViewHeaderItem (Margin 0,0,4,0 on its root, Padding 12,8,12,0) around the template (Padding 0,0,0,4, ColumnSpacing /
// RowSpacing 8): the title, the count beside it (or the subtitle beside it and the count under it), then the divider
// filling the rest, centered on the header
void FileList::DrawCellHeader(HDC hdc, int group, const RECT& rc)
{
    const Theme& t = *m_theme;
    if (group < 0 || group >= static_cast<int>(m_groupInfo.size()))
        return;
    const GroupInfo& info = m_groupInfo[static_cast<size_t>(group)];
    std::wstring countText = Tr(L"{0} 个项目");
    if (const size_t at = countText.find(L"{0}"); at != std::wstring::npos)
        countText.replace(at, 3, std::to_wstring(m_layout.Group(group).count));
    const int right = rc.right - t.S(4), left = rc.left + t.S(12), top = rc.top + t.S(8), limit = right - t.S(12);
    const int titleW = TextWidth(hdc, m_groupFont, info.title);
    DrawLabel(hdc, m_groupFont, info.title, { left, top, std::min(left + titleW, limit), top + t.S(22) }, t.ink);
    int end = left + titleW;
    const int countW = TextWidth(hdc, t.ui, countText);
    if (!info.countBelow)
    {
        DrawLabel(hdc, t.ui, countText, { end + t.S(8), top, std::min(end + t.S(8) + countW, limit), top + t.S(22) }, t.ink);
        end += t.S(8) + countW;
    }
    else
    {
        // (the collapsed count column between them keeps its spacing)
        const int subW = TextWidth(hdc, t.ui, info.subtitle);
        DrawLabel(hdc, t.ui, info.subtitle, { end + t.S(16), top, std::min(end + t.S(16) + subW, limit), top + t.S(22) }, t.ink);
        end += t.S(16) + subW;
        const int below = top + t.S(22) + t.S(8);
        DrawLabel(hdc, t.ui, countText, { left, below, std::min(left + countW, limit), below + t.S(20) }, t.ink);
        end = std::max(end, left + countW);
    }
    const int lineY = (rc.top + rc.bottom - t.S(4)) / 2, from = end + t.S(12);
    if (from < right)
        FillSolid(hdc, { from, lineY, right, lineY + std::max(1, t.S(1)) }, t.groupDivider);
}

// The rename box: over the name (details: the name column; list: beside the icon; cards: the details box's top, Margin
// -2,0; grid: the popup under the picture, as wide as the cell)
RECT FileList::NameRect(int row) const
{
    const Theme& t = *m_theme;
    const RECT rc = RowRect(row);
    if (rc.bottom <= rc.top)
        return {};
    switch (m_mode)
    {
    case Mode::Details:
        return { rc.left + m_colLeft[ColName] + t.S(12), rc.top + t.S(4), rc.left + m_colRight[ColName] - t.S(4), rc.bottom - t.S(4) };
    case Mode::List:
    {
        const int column = std::max(t.S(IconSize() + 4), t.S(20));
        const int mid = (rc.top + rc.bottom) / 2, h = std::min<int>(t.S(32), rc.bottom - rc.top);
        return { rc.left + t.S(12) + column, mid - h / 2, rc.right - t.S(12), mid - h / 2 + h };
    }
    case Mode::Cards:
    {
        const int size = Size();
        const SIZE box = kCardBox[size - 1], details = kCardDetails[size - 1];
        const int l = rc.left + t.S(1) + (size == 1 ? t.S(box.cx) : 0), top = rc.top + t.S(1) + (size == 1 ? 0 : t.S(box.cy));
        return { l + t.S(10), top + t.S(4), l + t.S(details.cx) - t.S(10), top + t.S(4) + t.S(32) };
    }
    case Mode::Grid:
    {
        const int w = rc.right - rc.left;
        return { rc.left, rc.top + w, rc.right, rc.top + w + t.S(32) };
    }
    case Mode::Column:
    {
        // ListViewTextBoxItemName: the name's grid column (after the icon box), the row's height
        const int mid = (rc.top + rc.bottom) / 2, h = t.S(RowHeight());
        return { rc.left + t.S(24) + t.S(IconSize() + 4), mid - h / 2, rc.right - t.S(24), mid - h / 2 + h };
    }
    }
    return {};
}

// Files' ContextualProperty: 分辨率 / 持续时间 / 版本 when the item has one (the thumbnail worker reads them), else 已修改
std::wstring FileList::ContextText(const Item& item) const
{
    if (!item.context.empty())
        return item.context;
    return std::wstring(Tr(L"已修改")) + L": " + Format::DateShort(item.modified);
}

bool FileList::WantsThumbs() const
{
    // (16px: the system's small icons are those already)
    return m_mode == Mode::Cards || IconSize() > 16;
}

// The cells' keys: each a stack of WinUI fills over the file area card (the nav layer and the card under it)
void FileList::BuildCellLayers()
{
    const Theme& t = *m_theme;
    // The presenter's background per state: none, CardBackgroundFillColorTertiary, CardBackgroundFillColorDefault,
    // SubtleFillColorSecondary
    const COLORREF presenter[kBackCount] = { t.overlay, t.cardHoverBase, t.cardFillBase, t.overlay };
    const int presenterPct[kBackCount] = { 0, t.cardHoverPct, t.cardFillPct, t.listHoverPct };
    std::vector<COLORREF> used{ t.content, t.chrome, t.layer, t.toolbar, m_hoverKey, m_pressedKey };
    auto add = [&](COLORREF& key, std::initializer_list<std::pair<COLORREF, int>> fills)
    {
        Backdrop::Layer layer{ 0, { t.layerFill, t.contentFill }, { t.layerPct, t.contentPct } };
        COLORREF c = t.content;
        int k = 2;
        for (const auto& [fill, pct] : fills)
            if (pct > 0 && k < Backdrop::kLayerFills)
            {
                layer.fill[k] = fill;
                layer.pct[k++] = pct;
                c = Mix(c, fill, pct);
            }
        for (int i = 0; i < 8 && std::find(used.begin(), used.end(), c) != used.end(); ++i)
            c = NudgeKey(c, i);
        used.push_back(c);
        layer.key = key = c;
        m_layers.push_back(layer);
    };
    for (int b = 0; b < kBackCount; ++b)
    {
        const std::pair<COLORREF, int> back{ presenter[b], presenterPct[b] };
        add(m_cardFace[b], { back, { t.cardFillBase, t.cardFillPct } });
        add(m_cardBox[b], { back, { t.cardFillBase, t.cardFillPct }, { t.neutralBase, t.neutralPct } });
        add(m_cardStroke[b], { back, { t.cardStrokeBase, t.cardStrokePct } });
        if (b == BackNone)
            m_cellFill[b] = t.content;
        else
            add(m_cellFill[b], { back });
    }
}