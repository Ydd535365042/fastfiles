// Home page window: layout and painting. Input and menus are in HomeInput.cpp, data in HomeData.cpp.
#include "HomeView.h"
#include "Format.h"
#include "Lang.h"
#include <commoncontrols.h>

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Home";

std::wstring FormatBytes(ULONGLONG bytes)
{
    return Format::Size(bytes); // Files' ToSizeString
}

std::wstring FolderOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring FormatTime(const FILETIME& ft)
{
    return Format::DateShort(ft); // Files' date format (ToShortLabel)
}
}

HomeView::~HomeView()
{
    Destroy();
}

bool HomeView::Create(HWND parent, const Theme* theme)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    m_theme = theme;
    if (!registered || !CreateWindowExW(0, kClass, Tr(L"主页"), WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 0, 0,
            parent, nullptr, GetModuleHandleW(nullptr), this))
        return false;
    m_bar.Attach(m_hwnd, true, kBarTimer, m_theme, [this](int y) { m_smooth.Reset(y); ScrollTo(y); });
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = m_hwnd;
    ti.uId = 1;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    Relayout();
    return true;
}

void HomeView::Destroy()
{
    m_smooth.Reset(m_scroll); // drops it from the frame clock before the window goes
    // The tooltip is a WS_POPUP created with m_hwnd (a child) as parent, so Windows makes the top-level main window its
    // owner: DestroyWindow(m_hwnd) leaves it alive (one tooltips_class32 leaked per Home page destroyed), destroy it here
    if (m_tooltip)
        DestroyWindow(m_tooltip);
    if (m_hwnd)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
    m_tooltip = nullptr;
    if (m_title)
        DeleteObject(m_title);
    m_title = nullptr;
    for (auto& [key, icon] : m_iconCache)
        DestroyIcon(icon);
    m_iconCache.clear();
}

void HomeView::Relayout()
{
    if (m_title)
        DeleteObject(m_title);
    LOGFONTW lf{};
    GetObjectW(m_theme->ui, sizeof(lf), &lf);
    lf.lfWeight = FW_SEMIBOLD;
    m_title = CreateFontIndirectW(&lf);
    if (m_tooltip)
        SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, m_theme->S(420));
    for (auto& [key, icon] : m_iconCache)
        DestroyIcon(icon);
    m_iconCache.clear(); // icon sizes follow the DPI
    Layout();
}

void HomeView::SetData(std::shared_ptr<const HomeData> data, unsigned collapsed, unsigned hidden)
{
    m_data = std::move(data);
    m_collapsed = collapsed;
    m_hidden = hidden;
    Layout();
}

const wchar_t* HomeView::SectionName(int section)
{
    static const wchar_t* const names[SecCount] = { Tr(L"快速访问"), Tr(L"驱动器"), Tr(L"网络位置"), Tr(L"最近使用的文件"), Tr(L"标签") };
    return section >= 0 && section < SecCount ? names[section] : L"";
}

const std::vector<HomeItem>* HomeView::Items(Section section) const
{
    if (!m_data)
        return nullptr;
    switch (section)
    {
    case SecQuick: return &m_data->quick;
    case SecDrives: return &m_data->drives;
    case SecNetwork: return &m_data->network;
    case SecRecent: return &m_data->recent;
    default: return nullptr; // SecTags: per group, see GroupAt
    }
}

const HomeTagGroup* HomeView::GroupAt(int cell) const
{
    if (!m_data || cell < 0 || cell >= static_cast<int>(m_cells.size()) || m_cells[cell].section != SecTags)
        return nullptr;
    const int g = m_cells[cell].group;
    return g >= 0 && g < static_cast<int>(m_data->tags.size()) ? &m_data->tags[g] : nullptr;
}

const HomeItem* HomeView::ItemAt(int cell) const
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()) || m_cells[cell].item < 0)
        return nullptr;
    const int i = m_cells[cell].item;
    if (m_cells[cell].section == SecTags)
    {
        const HomeTagGroup* group = GroupAt(cell);
        return group && i < static_cast<int>(group->items.size()) ? &group->items[i] : nullptr;
    }
    const auto* items = Items(m_cells[cell].section);
    return items && i < static_cast<int>(items->size()) ? &(*items)[i] : nullptr;
}
// --- Layout: sections stacked like the blueprint's .home (padding 16/20, 18 between widgets); cards wrap ---

void HomeView::Layout()
{
    if (!m_hwnd)
        return;
    const Theme& t = *m_theme;
    RECT client;
    GetClientRect(m_hwnd, &client);
    const int left = t.S(20), right = std::max<int>(left + t.S(160), client.right - t.S(20));
    const int width = right - left, gap = t.S(8);
    m_cells.clear();
    int y = t.S(16);
    HDC dc = GetDC(m_hwnd);
    for (const Section section : { SecQuick, SecDrives, SecNetwork, SecTags, SecRecent }) // Files' widget order
    {
        const int s = section;
        const auto* items = Items(section);
        const int count = section == SecTags ? (m_data ? static_cast<int>(m_data->tags.size()) : 0) : items ? static_cast<int>(items->size()) : 0;
        if ((s == SecNetwork && count == 0) || (m_hidden & (1u << s)))
            continue; // like Files: no network widget when there's nothing to show; widgets can be turned off
        if (y > t.S(16))
            y += t.S(18);
        m_cells.push_back({ section, kHeader, { left - t.S(6), y, left + t.S(220), y + t.S(28) } });
        y += t.S(28) + t.S(4);
        if (m_collapsed & (1u << s))
            continue;
        if (count == 0)
        {
            m_cells.push_back({ section, kHint, { left, y, right, y + t.S(36) } });
            y += t.S(36);
            continue;
        }
        if (s == SecTags)
        {
            // FileTagsWidget: UniformGridLayout (min 220 x 260, up to 6 columns, 8 apart); each container has a title row
            // and its items in an AdaptiveGridView (DesiredWidth 180, 32px rows); what doesn't fit stays behind "查看更多"
            const int minW = t.S(220), boxH = t.S(260), titleH = t.S(40), rowH = t.S(32), pad = t.S(4);
            const int cols = std::clamp((width + gap) / (minW + gap), 1, 6);
            const int boxW = (width - gap * (cols - 1)) / cols;
            for (int g = 0; g < count; ++g)
            {
                const HomeTagGroup& group = m_data->tags[g];
                const int x = left + (g % cols) * (boxW + gap), top = y + (g / cols) * (boxH + gap);
                m_cells.push_back({ section, kContainer, { x, top, x + boxW, top + boxH }, g });
                const int nameW = TextWidth(dc, m_title, group.name);
                const int titleW = std::min(boxW - t.S(48), t.S(16 + 8 + 16) + nameW);
                m_cells.push_back({ section, kTagTitle, { x + t.S(4), top + t.S(4), x + t.S(4) + titleW, top + titleH - t.S(4) }, g });
                m_cells.push_back({ section, kTagMore, { x + boxW - t.S(36), top + t.S(4), x + boxW - t.S(4), top + titleH - t.S(4) }, g });
                const int innerW = boxW - pad * 2;
                const int itemCols = std::max(1, innerW / t.S(180));
                const int itemW = innerW / itemCols;
                const int rows = std::max(1, (boxH - titleH - pad * 2) / rowH);
                const int shown = std::min(static_cast<int>(group.items.size()), rows * itemCols);
                for (int i = 0; i < shown; ++i)
                {
                    const int ix = x + pad + (i % itemCols) * itemW, iy = top + titleH + pad + (i / itemCols) * rowH;
                    m_cells.push_back({ section, i, { ix, iy, ix + itemW, iy + rowH }, g });
                }
            }
            y += ((count + cols - 1) / cols) * (boxH + gap) - gap;
            continue;
        }
        if (s == SecRecent)
        {
            for (int i = 0; i < count; ++i)
            {
                m_cells.push_back({ section, i, { left, y, right, y + t.S(44) } });
                y += t.S(44);
            }
            continue;
        }
        // Card grid: as many columns as fit at the minimum card width, stretched to fill the row
        const int minW = s == SecQuick ? t.S(180) : t.S(240);
        const int cardH = s == SecQuick ? t.S(60) : t.S(74);
        const int cols = std::max(1, (width + gap) / (minW + gap));
        const int cardW = (width - gap * (cols - 1)) / cols;
        for (int i = 0; i < count; ++i)
        {
            const int col = i % cols, row = i / cols;
            const int x = left + col * (cardW + gap), top = y + row * (cardH + gap);
            m_cells.push_back({ section, i, { x, top, x + cardW, top + cardH } });
        }
        y += ((count + cols - 1) / cols) * (cardH + gap) - gap;
    }
    ReleaseDC(m_hwnd, dc);
    m_content = y + t.S(20);
    const int cells = static_cast<int>(m_cells.size());
    if (m_hot >= cells) m_hot = -1;
    if (m_pressed >= cells) m_pressed = -1;
    if (m_focus >= cells) m_focus = cells - 1;
    m_tipCell = -1;
    m_smooth.ClampTarget(std::max(0, m_content - static_cast<int>(client.bottom)));
    ScrollTo(m_scroll);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

int HomeView::CellAt(POINT client) const
{
    const POINT pt{ client.x, client.y + m_scroll };
    for (size_t i = 0; i < m_cells.size(); ++i)
        if (Interactive(m_cells[i].item) && PtInRect(&m_cells[i].rc, pt))
            return static_cast<int>(i);
    return -1;
}

RECT HomeView::ClientRect(int cell) const
{
    RECT r = m_cells[cell].rc;
    OffsetRect(&r, 0, -m_scroll);
    return r;
}

void HomeView::ScrollTo(int offset)
{
    RECT client{};
    GetClientRect(m_hwnd, &client);
    offset = std::clamp(offset, 0, std::max(0, m_content - static_cast<int>(client.bottom)));
    if (offset != m_scroll)
    {
        m_scroll = offset;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    if (m_hwnd && m_theme)
    {
        const RECT strip{ client.right - m_theme->S(12), 0, client.right, client.bottom };
        m_bar.Set(m_content > client.bottom ? m_content : 0, client.bottom, m_scroll, strip, m_theme->S(32));
    }
}

void HomeView::EnsureVisible(int cell)
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()))
        return;
    RECT client;
    GetClientRect(m_hwnd, &client);
    const RECT& r = m_cells[cell].rc;
    int y = m_scroll;
    if (r.top < m_scroll + m_theme->S(8))
        y = r.top - m_theme->S(8);
    else if (r.bottom > m_scroll + client.bottom - m_theme->S(8))
        y = r.bottom - client.bottom + m_theme->S(8);
    if (y == m_scroll)
        return;
    m_smooth.Reset(y); // a key / selection jump, not the wheel: goes straight there
    ScrollTo(y);
}

void HomeView::SetHot(int cell)
{
    if (cell == m_hot)
        return;
    m_hot = cell;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (m_tooltip)
    {
        SendMessageW(m_tooltip, TTM_POP, 0, 0);
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = m_hwnd;
        ti.uId = 1;
        if (cell >= 0)
            ti.rect = ClientRect(cell);
        SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

std::wstring HomeView::TipFor(int cell) const
{
    if (cell >= 0 && cell < static_cast<int>(m_cells.size()) && m_cells[cell].section == SecTags)
    {
        if (m_cells[cell].item == kTagTitle)
            return Tr(L"查看更多");
        if (m_cells[cell].item == kTagMore)
            return Tr(L"更多选项...");
        const HomeItem* tagged = ItemAt(cell);
        return tagged ? tagged->path : std::wstring(); // Files: ToolTip = Path
    }
    const HomeItem* item = ItemAt(cell);
    if (!item)
        return {};
    std::wstring tip = item->path.empty() || item->path.rfind(L"::", 0) == 0 ? item->name : item->path;
    if (item->totalBytes)
        tip += L"\n" + FormatBytes(item->freeBytes) + Tr(L" 可用，共 ") + FormatBytes(item->totalBytes);
    if (m_cells[cell].section == SecRecent)
        tip += Tr(L"\n上次打开：") + FormatTime(item->time);
    if (item->missing)
        tip += Tr(L"\n找不到此位置");
    return tip;
}
// --- Painting (blueprint .wg h3 / .card / .bar / .rows) ---

void HomeView::DrawIcon(HDC hdc, int index, int x, int y, int size)
{
    if (index < 0)
        return;
    const long long key = (static_cast<long long>(size) << 32) | static_cast<unsigned>(index);
    HICON icon = nullptr;
    for (const auto& [k, h] : m_iconCache)
        if (k == key)
            icon = h;
    if (!icon)
    {
        // Smallest system list that is at least as big as the target, so icons are scaled down, never up
        const int list = size <= 16 ? SHIL_SMALL : size <= 32 ? SHIL_LARGE : size <= 48 ? SHIL_EXTRALARGE : SHIL_JUMBO;
        ComPtr<IImageList> images;
        if (FAILED(SHGetImageList(list, IID_PPV_ARGS(&images))) || FAILED(images->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon)
            return;
        m_iconCache.emplace_back(key, icon);
    }
    DrawIconEx(hdc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
}

void HomeView::PaintCard(HDC hdc, const Cell& cell, const HomeItem& item, RECT r, bool hot, bool down)
{
    const Theme& t = *m_theme;
    FillRound(hdc, r, t.S(6), down ? t.cardPressed : hot ? t.cardHover : t.card);
    FrameRound(hdc, r, t.S(6), t.line);
    const int icon = cell.section == SecQuick ? t.S(32) : t.S(40);
    const int ix = r.left + t.S(12);
    DrawIcon(hdc, item.icon, ix, (r.top + r.bottom - icon) / 2, icon);
    const int tx = ix + icon + t.S(10), tr = r.right - t.S(12);
    const COLORREF ink = item.missing ? Mix(t.ink, t.card, 50) : t.ink;
    if (cell.section == SecQuick)
    {
        const int mid = (r.top + r.bottom) / 2;
        DrawLabel(hdc, t.ui, item.name, { tx, mid - t.S(19), tr, mid + t.S(1) }, ink);
        DrawLabel(hdc, t.caption, item.missing ? Tr(L"找不到此位置") : Tr(L"已固定"), { tx, mid + t.S(1), tr, mid + t.S(19) }, t.muted);
        return;
    }
    if (!item.totalBytes)
    {
        // Network folder / share without capacity: name plus where it points
        const bool shell = item.path.empty() || item.path.rfind(L"::", 0) == 0;
        const int mid = (r.top + r.bottom) / 2;
        DrawLabel(hdc, t.ui, item.name, shell ? RECT{ tx, r.top, tr, r.bottom } : RECT{ tx, mid - t.S(19), tr, mid + t.S(1) }, ink);
        if (!shell)
            DrawLabel(hdc, t.caption, item.path, { tx, mid + t.S(1), tr, mid + t.S(19) }, t.muted);
        return;
    }
    const int top = (r.top + r.bottom) / 2 - t.S(27);
    DrawLabel(hdc, t.ui, item.name, { tx, top, tr, top + t.S(20) }, ink);
    DrawLabel(hdc, t.caption, Tr(L"可用 ") + FormatBytes(item.freeBytes) + Tr(L"，共 ") + FormatBytes(item.totalBytes),
        { tx, top + t.S(20), tr, top + t.S(38) }, t.muted);
    // Capacity bar, red under 10% free (same rule as the sidebar). WinUI ProgressBar: the 1px track
    // (ProgressBarTrackHeight) across the whole width, centred under the 3px indicator (ProgressBarMinHeight, radius 1.5)
    const int barTop = top + t.S(45), barBottom = barTop + std::max(3, t.S(3));
    const int trackTop = (barTop + barBottom) / 2;
    FillSolid(hdc, { tx, trackTop, tr, trackTop + std::max(1, t.S(1)) }, t.progressTrack);
    const ULONGLONG used = item.totalBytes - std::min(item.freeBytes, item.totalBytes);
    const int w = static_cast<int>((tr - tx) * (used / static_cast<double>(item.totalBytes)));
    if (w > 0)
        FillRound(hdc, { tx, barTop, tx + std::max(w, t.S(3)), barBottom }, std::max(1, (barBottom - barTop) / 2),
            item.freeBytes * 10 < item.totalBytes ? t.closeHover : t.accent);
}

void HomeView::PaintRecent(HDC hdc, const HomeItem& item, RECT r, bool hot, bool down)
{
    const Theme& t = *m_theme;
    if (hot || down)
        FillRound(hdc, r, t.S(5), down ? t.pressed : t.hover);
    const int icon = t.S(16);
    const int ix = r.left + t.S(10);
    DrawIcon(hdc, item.icon, ix, (r.top + r.bottom - icon) / 2, icon);
    const int dateW = t.S(150);
    const int tx = ix + icon + t.S(12), tr = std::max(tx + t.S(40), static_cast<int>(r.right) - dateW - t.S(12));
    const int mid = (r.top + r.bottom) / 2;
    DrawLabel(hdc, t.ui, item.name, { tx, mid - t.S(19), tr, mid + t.S(1) }, t.ink);
    DrawLabel(hdc, t.caption, FolderOf(item.path), { tx, mid + t.S(1), tr, mid + t.S(19) }, t.muted);
    DrawLabel(hdc, t.caption, FormatTime(item.time), { tr + t.S(12), r.top, r.right - t.S(10), r.bottom }, t.muted, DT_RIGHT);
}

// FileTagsWidget container: ControlFillColorDefault card with CardStroke, a title row (tag icon in the tag's color, the
// name in semibold, a "..." button) over a 1px line, and the items as 32px rows (16px icon, name)
void HomeView::PaintTagCell(HDC hdc, const Cell& cell, RECT r, bool hot, bool down)
{
    const Theme& t = *m_theme;
    const HomeTagGroup* group = m_data && cell.group >= 0 && cell.group < static_cast<int>(m_data->tags.size()) ? &m_data->tags[cell.group] : nullptr;
    if (!group)
        return;
    switch (cell.item)
    {
    case kContainer:
        FillRound(hdc, r, t.S(4), t.card);
        FrameRound(hdc, r, t.S(4), t.cardStroke);
        FillSolid(hdc, { r.left + 1, r.top + t.S(40), r.right - 1, r.top + t.S(40) + std::max(1, t.S(1)) }, t.cardStroke);
        return;
    case kTagTitle:
    {
        if (hot || down)
            FillRound(hdc, r, t.S(4), down ? t.pressed : t.hover);
        const int ix = r.left + t.S(8);
        DrawGlyph(hdc, t.icons, Glyph::Tag, { ix, r.top, ix + t.S(16), r.bottom }, group->color);
        DrawLabel(hdc, m_title, group->name, { ix + t.S(24), r.top, r.right - t.S(8), r.bottom - t.S(2) }, t.ink);
        return;
    }
    case kTagMore:
        if (hot || down)
            FillRound(hdc, r, t.S(4), down ? t.pressed : t.hover);
        DrawGlyph(hdc, t.iconsSmall, Glyph::More, r, t.ink);
        return;
    default:
        if (cell.item < 0 || cell.item >= static_cast<int>(group->items.size()))
            return;
        const HomeItem& item = group->items[cell.item];
        if (hot || down)
            FillRound(hdc, r, t.S(4), down ? t.pressed : t.hover);
        const int icon = t.S(16), ix = r.left + t.S(8);
        DrawIcon(hdc, item.icon, ix, (r.top + r.bottom - icon) / 2, icon);
        DrawLabel(hdc, t.ui, item.name, { ix + icon + t.S(8), r.top, r.right - t.S(8), r.bottom }, t.ink, DT_END_ELLIPSIS);
        return;
    }
}

void HomeView::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.content);
    const bool focused = GetFocus() == m_hwnd;
    for (size_t i = 0; i < m_cells.size(); ++i)
    {
        const Cell& cell = m_cells[i];
        const RECT r = ClientRect(static_cast<int>(i));
        if (r.bottom < rc.top || r.top > rc.bottom)
            continue;
        const bool hot = static_cast<int>(i) == m_hot && !m_bar.Captured();
        const bool down = static_cast<int>(i) == m_pressed && hot;
        if (cell.section == SecTags && cell.item != kHeader && cell.item != kHint)
        {
            PaintTagCell(hdc, cell, r, hot, down);
        }
        else if (cell.item == kHeader)
        {
            if (hot)
                FillRound(hdc, r, t.S(5), t.hover);
            const bool collapsed = (m_collapsed & (1u << cell.section)) != 0;
            const std::wstring title = SectionName(cell.section);
            const int w = TextWidth(hdc, m_title, title);
            DrawLabel(hdc, m_title, title, { r.left + t.S(6), r.top, r.right, r.bottom }, t.ink);
            DrawGlyph(hdc, t.iconsSmall, collapsed ? Glyph::ChevronRight : Glyph::ChevronDown,
                { r.left + t.S(14) + w, r.top, r.left + t.S(30) + w, r.bottom }, hot ? t.ink : t.muted);
        }
        else if (cell.item == kHint)
        {
            const wchar_t* hint = cell.section == SecQuick || cell.section == SecTags ? L""
                : cell.section == SecRecent ? (m_data && m_data->recentReady ? Tr(L"最近没有打开过文件") : Tr(L"正在读取最近使用的文件…"))
                : Tr(L"正在读取驱动器…");
            DrawLabel(hdc, t.caption, hint, { r.left + t.S(2), r.top, r.right, r.bottom }, t.muted);
        }
        else if (const HomeItem* item = ItemAt(static_cast<int>(i)))
        {
            if (cell.section == SecRecent)
                PaintRecent(hdc, *item, r, hot, down);
            else
                PaintCard(hdc, cell, *item, r, hot, down);
        }
        if (focused && m_keyboardFocus && static_cast<int>(i) == m_focus)
            FrameRound(hdc, { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, t.S(6), t.ink);
    }
    m_bar.Paint(hdc, rc);
}
