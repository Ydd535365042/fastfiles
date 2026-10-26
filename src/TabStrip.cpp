#include "TabStrip.h"
#include "Lang.h"
#include "resource.h"
#include <cmath>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

// Files' TabBar (UserControls/TabBar/TabBar.xaml over a WinUI TabView): tabs between 100 and 240 px, scroll buttons
// when they overflow, wheel selects the next/previous tab, a tab dragged out becomes a window (or moves to the window
// it's dropped on), files hovered over a tab select it after 1.3 s and can be dropped into it, folders dropped on "+"
// open as new tabs. The "标签操作菜单" button sits left of the tabs.

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.TabStrip";

// Drag source for a tab: Esc cancels, releasing the button drops
struct TabDropSource final : IDropSource
{
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = { QITABENT(TabDropSource, IDropSource), {} };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape)
            return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

bool HasFormat(IDataObject* data, CLIPFORMAT format)
{
    FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return data && data->QueryGetData(&fmt) == S_OK;
}

std::vector<BYTE> ReadFormat(IDataObject* data, CLIPFORMAT format)
{
    std::vector<BYTE> out;
    FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&fmt, &medium)))
        return out;
    if (const void* p = GlobalLock(medium.hGlobal))
    {
        out.assign(static_cast<const BYTE*>(p), static_cast<const BYTE*>(p) + GlobalSize(medium.hGlobal));
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return out;
}

// The payload starts with the source process id
bool FromThisProcess(const std::vector<BYTE>& payload)
{
    DWORD pid = 0;
    if (payload.size() < sizeof(pid))
        return false;
    memcpy(&pid, payload.data(), sizeof(pid));
    return pid == GetCurrentProcessId();
}

bool AnyFolder(IDataObject* data)
{
    ComPtr<IShellItemArray> items;
    SFGAOF any = 0;
    return data && SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) &&
        SUCCEEDED(items->GetAttributes(SIATTRIBFLAGS_OR, SFGAO_FOLDER, &any)) && (any & SFGAO_FOLDER);
}
}

CLIPFORMAT TabStrip::TabFormat()
{
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FastFiles.Tab"));
    return format;
}

bool TabStrip::Create(HWND parent, int id, const Theme* theme, TabStripEvents* events)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered)
        return false;
    m_theme = theme;
    m_events = events;
    SHFILEINFOW sfi{};
    m_images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    m_hwnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, MenuId(id),
        GetModuleHandleW(nullptr), this);
    if (!m_hwnd)
        return false;
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_dropHelper));
    RegisterDragDrop(m_hwnd, this);

    // One tool for the whole strip, moved onto whatever part is hot (like the sidebar)
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = m_hwnd;
    ti.uId = 1;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, m_theme->S(560));
    return true;
}

void TabStrip::SetTip(int index, const std::wstring& tip)
{
    if (index >= 0 && index < Count())
        m_items[index].tip = tip;
}

void TabStrip::SetHot(const Hit& hit)
{
    if (hit == m_hot)
        return;
    m_hot = hit;
    Invalidate();
    if (!m_tooltip)
        return;
    SendMessageW(m_tooltip, TTM_POP, 0, 0);
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = m_hwnd;
    ti.uId = 1;
    switch (hit.part)
    {
    case Part::Tab: case Part::TabClose: ti.rect = hit.part == Part::Tab ? m_items[hit.index].rc : m_items[hit.index].close; break;
    case Part::NewTab: ti.rect = m_new; break;
    case Part::Actions: ti.rect = m_actions; break;
    default: break; // empty rect: no tip
    }
    SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
}

std::wstring TabStrip::TipFor(const Hit& hit) const
{
    switch (hit.part)
    {
    case Part::Tab: return hit.index >= 0 && hit.index < Count() ? m_items[hit.index].tip : std::wstring();
    case Part::TabClose: return Tr(L"关闭标签页");
    case Part::NewTab: return m_newTabTip;
    case Part::Actions: return Tr(L"标签操作菜单");
    default: return {};
    }
}

void TabStrip::Insert(int index, const std::wstring& text, int icon)
{
    index = std::clamp(index, 0, Count());
    m_items.insert(m_items.begin() + index, Item{ text, icon });
    if (m_active >= index)
        ++m_active;
    Relayout();
}

void TabStrip::SetLoading(int index, bool loading)
{
    if (index < 0 || index >= Count() || m_items[index].loading == loading)
        return;
    m_items[index].loading = loading;
    if (loading)
        SetTimer(m_hwnd, kSpinTimer, 33, nullptr);
    // Tests read how many tabs show the ring (window property, like MainWindow's FastFiles.Tray)
    const auto spinning = std::count_if(m_items.begin(), m_items.end(), [](const Item& i) { return i.loading; });
    if (spinning)
        SetPropW(m_hwnd, L"FastFiles.Loading", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(spinning)));
    else
        RemovePropW(m_hwnd, L"FastFiles.Loading");
    Invalidate();
}

// WinUI ProgressRing (indeterminate): an accent arc that turns and grows / shrinks, over a 16px icon box
void TabStrip::DrawProgressRing(HDC hdc, const RECT& r) const
{
    const Theme& t = *m_theme;
    StartGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float stroke = static_cast<float>(t.S(2)) * 0.8f + 0.4f;
    const float size = static_cast<float>(std::min(r.right - r.left, r.bottom - r.top)) - stroke;
    const Gdiplus::RectF box(r.left + stroke / 2, r.top + stroke / 2, size, size);
    const double ms = static_cast<double>(GetTickCount64() % 2000);
    const float start = static_cast<float>(ms / 2000.0 * 720.0 - 90.0);
    const float sweep = static_cast<float>(30.0 + 240.0 * (0.5 - 0.5 * std::cos(ms / 2000.0 * 2.0 * 3.14159265358979)));
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(t.accent), GetGValue(t.accent), GetBValue(t.accent)), stroke);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&pen, box, start, sweep);
}

void TabStrip::Set(int index, const std::wstring& text, int icon)
{
    if (index < 0 || index >= Count() || (m_items[index].text == text && m_items[index].icon == icon))
        return;
    m_items[index].text = text;
    m_items[index].icon = icon;
    Invalidate();
}

void TabStrip::Remove(int index)
{
    if (index < 0 || index >= Count())
        return;
    m_items.erase(m_items.begin() + index);
    if (m_active > index || m_active >= Count())
        --m_active;
    SetHot({});
    Relayout();
}

void TabStrip::Clear()
{
    m_items.clear();
    m_active = -1;
    SetHot({});
    m_scroll = 0;
    Relayout();
}

void TabStrip::SetActive(int index)
{
    if (index != m_active)
    {
        m_active = index;
        EnsureVisible(index);
        Invalidate();
    }
}

void TabStrip::SetWindowActive(bool active)
{
    if (active != m_windowActive)
    {
        m_windowActive = active;
        Invalidate();
    }
}

void TabStrip::SetMaximized(bool maximized)
{
    if (maximized != m_maximized)
    {
        m_maximized = maximized;
        Invalidate();
    }
}

void TabStrip::SetActionsVisible(bool visible)
{
    if (visible != m_showActions)
    {
        m_showActions = visible;
        Relayout();
    }
}

RECT TabStrip::ItemRect(int index) const
{
    return index >= 0 && index < Count() ? m_items[index].rc : RECT{};
}

void TabStrip::Relayout()
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const Theme& t = *m_theme;
    const int caption = t.S(46);
    m_close = { rc.right - caption, 0, rc.right, rc.bottom };
    m_max = { m_close.left - caption, 0, m_close.left, rc.bottom };
    m_min = { m_max.left - caption, 0, m_max.left, rc.bottom };

    const int tabTop = rc.bottom - t.S(34);
    const int cy = (tabTop + rc.bottom) / 2;
    int left = t.S(8);
    if (m_showActions)
    {
        // Files: 30x30 button in the TabView's strip header
        const int size = t.S(30);
        m_actions = { left, cy - size / 2, left + size, cy + size / 2 };
        left = m_actions.right + t.S(4);
    }
    else
        m_actions = {};
    const int plus = t.S(36), drag = t.S(48), arrow = t.S(28);
    const int n = Count();
    // WinUI TabView (TabWidthMode Equal): 100-240 px per tab, scroll buttons once even 100 doesn't fit
    const int minW = t.S(100), maxW = t.S(240);
    int avail = std::max(0, static_cast<int>(m_min.left) - drag - plus - left);
    m_overflow = n * minW > avail;
    if (m_overflow)
        avail = std::max(0, avail - 2 * arrow);
    m_tabWidth = n ? (m_overflow ? minW : std::clamp(avail / n, minW, maxW)) : 0;
    const int viewLeft = m_overflow ? left + arrow : left;
    const int content = n * m_tabWidth;
    m_view = { viewLeft, tabTop, viewLeft + std::min(content, avail), rc.bottom };
    m_scroll = std::clamp(m_scroll, 0, std::max(0, content - static_cast<int>(m_view.right - m_view.left)));
    if (m_overflow)
    {
        m_left = { left, cy - arrow / 2, left + arrow, cy + arrow / 2 };
        m_right = { m_view.right, cy - arrow / 2, m_view.right + arrow, cy + arrow / 2 };
    }
    else
        m_left = m_right = {};
    int x = viewLeft - m_scroll;
    for (auto& item : m_items)
    {
        item.rc = { x, tabTop, x + m_tabWidth, rc.bottom };
        const int size = t.S(24);
        item.close = { item.rc.right - t.S(6) - size, cy - size / 2, item.rc.right - t.S(6), cy + size / 2 };
        x += m_tabWidth;
    }
    const int size = t.S(28);
    const int px = (m_overflow ? m_right.right : m_view.right) + t.S(4);
    m_new = { px, cy - size / 2, px + size + t.S(4), cy + size / 2 };
    Invalidate();
}

void TabStrip::ScrollBy(int delta)
{
    const int content = Count() * m_tabWidth;
    const int scroll = std::clamp(m_scroll + delta, 0, std::max(0, content - static_cast<int>(m_view.right - m_view.left)));
    if (scroll != m_scroll)
    {
        m_scroll = scroll;
        Relayout();
    }
}

void TabStrip::EnsureVisible(int index)
{
    if (!m_overflow || index < 0 || index >= Count())
        return;
    const int width = m_view.right - m_view.left;
    const int start = index * m_tabWidth, end = start + m_tabWidth;
    if (start < m_scroll)
        ScrollBy(start - m_scroll);
    else if (end > m_scroll + width)
        ScrollBy(end - (m_scroll + width));
}

TabStrip::Hit TabStrip::HitTest(POINT pt) const
{
    if (PtInRect(&m_close, pt)) return { Part::Close };
    if (PtInRect(&m_max, pt)) return { Part::Maximize };
    if (PtInRect(&m_min, pt)) return { Part::Minimize };
    if (PtInRect(&m_actions, pt)) return { Part::Actions };
    if (PtInRect(&m_new, pt)) return { Part::NewTab };
    if (PtInRect(&m_left, pt)) return { Part::ScrollLeft };
    if (PtInRect(&m_right, pt)) return { Part::ScrollRight };
    if (!PtInRect(&m_view, pt))
        return {};
    for (int i = 0; i < Count(); ++i)
    {
        const Item& item = m_items[i];
        if (!PtInRect(&item.rc, pt))
            continue;
        if (PtInRect(&item.close, pt))
            return { Part::TabClose, i };
        return { Part::Tab, i };
    }
    return {};
}

int TabStrip::InsertIndexAt(int x) const
{
    // Files' TabStripDrop: before the first tab whose right half the point is left of
    for (int i = 0; i < Count(); ++i)
        if (x < (m_items[i].rc.left + m_items[i].rc.right) / 2)
            return i;
    return Count();
}

void TabStrip::PaintCaption(HDC hdc, const RECT& r, Part part, const wchar_t* glyph)
{
    const Theme& t = *m_theme;
    const bool hot = m_hot.part == part, down = hot && m_pressed.part == part;
    COLORREF ink = m_windowActive ? t.chromeInk : t.chromeMuted;
    if (part == Part::Close && hot)
    {
        FillSolid(hdc, r, down ? t.closePressed : t.closeHover);
        ink = t.closeInk;
    }
    else if (hot)
    {
        FillSolid(hdc, r, down ? t.chromePressed : t.chromeHover);
    }
    DrawGlyph(hdc, t.iconsSmall, glyph, r, ink);
}

void TabStrip::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.chrome);
    const int sep = std::max(1, t.S(1));
    auto button = [&](const RECT& r, Part part, const wchar_t* glyph, HFONT font, bool enabled)
    {
        if (IsRectEmpty(&r))
            return;
        const bool hot = enabled && (m_hot.part == part || (part == Part::NewTab && m_dropKind == DropKind::NewTab));
        if (hot)
            FillRound(hdc, r, t.S(5), m_pressed.part == part ? t.chromePressed : t.chromeHover);
        const COLORREF ink = enabled ? t.chromeInk : Mix(t.chromeMuted, t.chrome, 40);
        if (glyph)
            DrawGlyph(hdc, font, glyph, r, ink);
        else
        {
            // The tab actions button shows the FastFiles logo (the user's choice; Files draws its Panes.Single
            // ThemedIcon here), 16x16 centered in the 30x30 button like Files' icon; the flyout is unchanged
            const int s = t.S(16), x = (r.left + r.right - s) / 2, y = (r.top + r.bottom - s) / 2;
            if (!m_logo || m_logoSize != s)
            {
                if (m_logo)
                    DestroyIcon(m_logo);
                m_logo = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, s, s, 0));
                m_logoSize = s;
            }
            if (m_logo)
                DrawIconEx(hdc, x, y, m_logo, s, s, 0, nullptr, DI_NORMAL);
            else
                DrawPaneIcon(hdc, { x, y, x + s, y + s }, ink);
        }
    };

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, m_view.left, m_view.top, m_view.right, m_view.bottom);
    for (int i = 0; i < Count(); ++i)
    {
        const Item& item = m_items[i];
        if (item.rc.right <= m_view.left || item.rc.left >= m_view.right)
            continue;
        const bool active = i == m_active;
        const bool hot = (m_hot.index == i && (m_hot.part == Part::Tab || m_hot.part == Part::TabClose)) ||
            (m_dropKind == DropKind::Files && m_dropIndex == i);
        RECT r = item.rc;
        r.left += t.S(1);
        r.right -= t.S(1);
        if (active)
            FillTopRound(hdc, r, t.S(7), t.layer);
        else
        {
            if (hot)
                FillRound(hdc, { r.left, r.top + t.S(2), r.right, r.bottom - t.S(4) }, t.S(6), t.chromeHover);
            // Divider between two inactive, non-hovered neighbours
            const bool nextActive = i + 1 == m_active;
            const bool nextHot = m_hot.index == i + 1 && (m_hot.part == Part::Tab || m_hot.part == Part::TabClose);
            if (i + 1 < Count() && !hot && !nextActive && !nextHot)
                FillSolid(hdc, { item.rc.right - sep, r.top + t.S(9), item.rc.right, r.bottom - t.S(9) }, t.line);
        }
        const int iconSize = GetSystemMetrics(SM_CXSMICON);
        const int cy = (r.top + r.bottom - (active ? 0 : t.S(2))) / 2;
        int x = r.left + t.S(10);
        if (item.loading) // Files' LoadingStates.Loading: the ProgressRing in the icon's place
            DrawProgressRing(hdc, { x, cy - iconSize / 2, x + iconSize, cy - iconSize / 2 + iconSize });
        else if (item.icon == kSettingsIcon) // Files: FontIconSource Glyph E713 for the Settings tab
            DrawGlyph(hdc, t.icons, Glyph::Settings, { x, cy - iconSize / 2, x + iconSize, cy + iconSize / 2 }, active ? t.ink : t.chromeInk);
        else if (item.icon == kHomeIcon) // Files: SidebarSectionIcons.For(SectionType.Home) for the Home tab
            DrawSectionIcon(hdc, 0, { x, cy - iconSize / 2, x + iconSize, cy - iconSize / 2 + iconSize }, t.dpi);
        else if (m_images && item.icon >= 0)
            ImageList_Draw(m_images, item.icon, hdc, x, cy - iconSize / 2, ILD_TRANSPARENT);
        x += iconSize + t.S(8);
        const bool showClose = active || hot;
        const int textRight = showClose ? item.close.left - t.S(2) : r.right - t.S(8);
        DrawLabel(hdc, t.ui, item.text, { x, cy - t.S(10), textRight, cy + t.S(10) }, active ? t.ink : t.chromeMuted);
        if (showClose)
        {
            RECT c = item.close;
            OffsetRect(&c, 0, cy - (c.top + c.bottom) / 2);
            if (m_hot.part == Part::TabClose && m_hot.index == i)
                FillRound(hdc, c, t.S(4), active ? t.hover : Mix(t.chromeHover, t.chromeInk, 8));
            DrawGlyph(hdc, t.iconsSmall, Glyph::Close, c, active ? t.ink : t.chromeMuted);
        }
    }
    // Where a dragged tab would land
    if (m_dropKind == DropKind::Tab && m_dropIndex >= 0)
    {
        const int x = m_dropIndex < Count() ? m_items[m_dropIndex].rc.left : (Count() ? m_items.back().rc.right : m_view.left);
        FillRound(hdc, { x - t.S(1), m_view.top + t.S(6), x + t.S(2), m_view.bottom - t.S(6) }, t.S(1), t.accent);
    }
    RestoreDC(hdc, saved);

    const int content = Count() * m_tabWidth;
    button(m_actions, Part::Actions, nullptr, nullptr, true);
    button(m_left, Part::ScrollLeft, Glyph::ChevronLeft, t.iconsSmall, m_scroll > 0);
    button(m_right, Part::ScrollRight, Glyph::ChevronRight, t.iconsSmall, m_scroll < content - (m_view.right - m_view.left));
    button(m_new, Part::NewTab, Glyph::Add, t.iconsMid, true);
    PaintCaption(hdc, m_min, Part::Minimize, Glyph::Minimize);
    PaintCaption(hdc, m_max, Part::Maximize, m_maximized ? Glyph::Restore : Glyph::Maximize);
    PaintCaption(hdc, m_close, Part::Close, Glyph::Close);
    // Windows 10 draws no top edge once the caption is removed; paint a hairline ourselves
    if (!m_maximized)
        FillSolid(hdc, { rc.left, rc.top, rc.right, rc.top + sep }, m_windowActive ? Mix(t.line, t.muted, 30) : t.line);
}

void TabStrip::Activate(const Hit& hit)
{
    const HWND root = GetAncestor(m_hwnd, GA_ROOT);
    switch (hit.part)
    {
    case Part::TabClose: m_events->OnTabClose(hit.index); break;
    case Part::NewTab: m_events->OnTabNew(); break;
    case Part::Actions:
    {
        RECT screen = m_actions;
        MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&screen), 2);
        m_events->OnTabActions(screen);
        break;
    }
    case Part::Minimize: PostMessageW(root, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case Part::Maximize: PostMessageW(root, WM_SYSCOMMAND, IsZoomed(root) ? SC_RESTORE : SC_MAXIMIZE, 0); break;
    case Part::Close: PostMessageW(root, WM_SYSCOMMAND, SC_CLOSE, 0); break;
    default: break;
    }
}

void TabStrip::MoveItem(int from, int to)
{
    if (from == to || from < 0 || from >= Count() || to < 0 || to >= Count())
        return;
    Item moving = std::move(m_items[from]);
    m_items.erase(m_items.begin() + from);
    m_items.insert(m_items.begin() + to, std::move(moving));
    if (m_active == from)
        m_active = to;
    else if (from < m_active && to >= m_active)
        --m_active;
    else if (from > m_active && to <= m_active)
        ++m_active;
    Relayout();
    m_events->OnTabMoved(from, to);
}

void TabStrip::DragTo(POINT pt)
{
    if (!m_dragging)
    {
        if (abs(pt.x - m_dragStart.x) <= GetSystemMetrics(SM_CXDRAG) && abs(pt.y - m_dragStart.y) <= GetSystemMetrics(SM_CYDRAG))
            return;
        m_dragging = true;
    }
    // Off the strip: the tab becomes an OLE drag (another window's strip, or a new window)
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (pt.y < rc.top - m_theme->S(24) || pt.y > rc.bottom + m_theme->S(24))
    {
        const int index = m_dragIndex;
        m_dragIndex = -1;
        m_dragging = false;
        ReleaseCapture();
        BeginTearOut(index);
        return;
    }
    // Near the ends of a scrolled strip: keep scrolling
    if (m_overflow && pt.x < m_view.left + m_theme->S(16))
        ScrollBy(-m_theme->S(16));
    else if (m_overflow && pt.x > m_view.right - m_theme->S(16))
        ScrollBy(m_theme->S(16));
    for (int i = 0; i < Count(); ++i)
    {
        const RECT& r = m_items[i].rc;
        if (i != m_dragIndex && pt.x >= r.left && pt.x < r.right)
        {
            MoveItem(m_dragIndex, i);
            m_dragIndex = i;
            break;
        }
    }
}

void TabStrip::BeginTearOut(int index)
{
    const std::vector<BYTE> payload = m_events->TabPayload(index);
    if (payload.empty())
        return;
    ComPtr<IDataObject> data;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data))))
        return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, payload.size());
    if (!mem)
        return;
    memcpy(GlobalLock(mem), payload.data(), payload.size());
    GlobalUnlock(mem);
    FORMATETC fmt{ TabFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{ TYMED_HGLOBAL };
    medium.hGlobal = mem;
    if (FAILED(data->SetData(&fmt, &medium, TRUE)))
    {
        GlobalFree(mem);
        return;
    }
    const POINT start = m_pressScreen;
    const ULONGLONG began = m_pressTime;
    SetHot({});
    m_tearIndex = index;
    m_selfDrop = false;
    TabDropSource source;
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data.Get(), &source, DROPEFFECT_MOVE, &effect);
    const int torn = m_tearIndex;
    m_tearIndex = -1;
    Invalidate();
    if (hr == DRAGDROP_S_CANCEL || m_selfDrop)
        return;
    if (effect & DROPEFFECT_MOVE)
    {
        m_events->OnTabMovedAway(torn); // another window took it
        return;
    }
    // Dropped where nothing takes tabs. Files ignores a short flick (under 1 s and under 100 px) and a lone tab.
    POINT end;
    GetCursorPos(&end);
    const double distance = std::hypot(static_cast<double>(end.x - start.x), static_cast<double>(end.y - start.y));
    if (Count() <= 1 || (GetTickCount64() - began < 1000 && distance < 100))
        return;
    m_events->OnTabTornOut(torn, end);
}

// --- Drop target ---

IFACEMETHODIMP TabStrip::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = { QITABENT(TabStrip, IDropTarget), {} };
    return QISearch(this, qit, riid, ppv);
}

void TabStrip::ClearDropTarget()
{
    if (m_itemTarget)
        m_itemTarget->DragLeave();
    m_itemTarget.Reset();
    m_itemTargetIndex = -1;
}

IFACEMETHODIMP TabStrip::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    m_dragData = data;
    m_dragKeys = keys;
    m_dragFolders = !HasFormat(data, TabFormat()) && AnyFolder(data);
    if (m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        m_dropHelper->DragEnter(m_hwnd, data, &p, *effect);
    }
    UpdateDrop(pt, keys, effect);
    return S_OK;
}

IFACEMETHODIMP TabStrip::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    m_dragKeys = keys;
    UpdateDrop(pt, keys, effect);
    if (m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        m_dropHelper->DragOver(&p, *effect);
    }
    return S_OK;
}

IFACEMETHODIMP TabStrip::DragLeave()
{
    KillTimer(m_hwnd, kHoverTimer);
    if (m_dropHelper)
        m_dropHelper->DragLeave();
    ClearDropTarget();
    m_dragData.Reset();
    m_dropKind = DropKind::None;
    m_dropIndex = m_hoverIndex = -1;
    Invalidate();
    return S_OK;
}

void TabStrip::UpdateDrop(POINTL screen, DWORD keys, DWORD* effect)
{
    POINT pt{ screen.x, screen.y };
    ScreenToClient(m_hwnd, &pt);
    const DWORD allowed = *effect;
    const DropKind before = m_dropKind;
    const int beforeIndex = m_dropIndex;
    const Hit hit = HitTest(pt);
    if (HasFormat(m_dragData.Get(), TabFormat()))
    {
        // A tab: insert where it's dropped (this window's own tab just moves)
        ClearDropTarget();
        m_dropKind = DropKind::Tab;
        m_dropIndex = InsertIndexAt(pt.x);
        *effect = allowed & DROPEFFECT_MOVE;
    }
    else if (hit.part == Part::NewTab)
    {
        ClearDropTarget();
        m_dropKind = m_dragFolders ? DropKind::NewTab : DropKind::None;
        m_dropIndex = -1;
        *effect = m_dragFolders ? (allowed & DROPEFFECT_LINK) : DROPEFFECT_NONE;
    }
    else if ((hit.part == Part::Tab || hit.part == Part::TabClose) && m_events->TabFolder(hit.index))
    {
        // Files on a tab: its folder takes them (copy/move like dropping into the view)
        m_dropKind = DropKind::Files;
        m_dropIndex = hit.index;
        if (hit.index != m_itemTargetIndex)
        {
            ClearDropTarget();
            ComPtr<IShellFolder> parent;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(SHBindToParent(m_events->TabFolder(hit.index), IID_PPV_ARGS(&parent), &child)))
                parent->GetUIObjectOf(m_hwnd, 1, &child, IID_IDropTarget, nullptr, reinterpret_cast<void**>(m_itemTarget.GetAddressOf()));
            m_itemTargetIndex = hit.index;
            if (m_itemTarget && FAILED(m_itemTarget->DragEnter(m_dragData.Get(), keys, screen, effect)))
                m_itemTarget.Reset();
        }
        else if (m_itemTarget)
            m_itemTarget->DragOver(keys, screen, effect);
        if (!m_itemTarget)
            *effect = DROPEFFECT_NONE;
    }
    else
    {
        ClearDropTarget();
        m_dropKind = DropKind::None;
        m_dropIndex = -1;
        *effect = DROPEFFECT_NONE;
    }
    // Files: hovering a tab that accepts the drop selects it after 1.3 s
    const int hover = m_dropKind == DropKind::Files && *effect != DROPEFFECT_NONE ? m_dropIndex : -1;
    if (hover != m_hoverIndex)
    {
        m_hoverIndex = hover;
        if (hover >= 0 && hover != m_active)
            SetTimer(m_hwnd, kHoverTimer, kHoverMs, nullptr);
        else
            KillTimer(m_hwnd, kHoverTimer);
    }
    if (before != m_dropKind || beforeIndex != m_dropIndex)
        Invalidate();
}

IFACEMETHODIMP TabStrip::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    KillTimer(m_hwnd, kHoverTimer);
    UpdateDrop(pt, keys, effect);
    if (m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        m_dropHelper->Drop(data, &p, *effect);
    }
    if (m_dropKind == DropKind::Tab)
    {
        const std::vector<BYTE> payload = ReadFormat(data, TabFormat());
        if (FromThisProcess(payload) && m_tearIndex >= 0)
        {
            // Back on its own strip: a reorder
            const int to = m_dropIndex > m_tearIndex ? m_dropIndex - 1 : m_dropIndex;
            MoveItem(m_tearIndex, std::clamp(to, 0, Count() - 1));
            m_selfDrop = true;
            *effect = DROPEFFECT_NONE;
        }
        else if (!payload.empty())
        {
            m_events->OnTabArrived(payload, m_dropIndex);
            *effect = DROPEFFECT_MOVE;
        }
    }
    else if (m_dropKind == DropKind::NewTab)
    {
        std::vector<UniquePidl> folders;
        ComPtr<IShellItemArray> items;
        DWORD count = 0;
        if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) && SUCCEEDED(items->GetCount(&count)))
            for (DWORD i = 0; i < count; ++i)
            {
                ComPtr<IShellItem> item;
                SFGAOF attrs = 0;
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs)) &&
                    (attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM) && SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)))
                    folders.emplace_back(pidl);
            }
        if (!folders.empty())
            m_events->OnFoldersToNewTabs(folders);
        *effect = folders.empty() ? DROPEFFECT_NONE : DROPEFFECT_LINK;
    }
    else if (m_dropKind == DropKind::Files && m_itemTarget)
    {
        // Files' TabBar drop: copy / move into the tab's folder through the status center (right drags keep the menu)
        if (RouteFileDrop(m_hwnd, data, m_events->TabFolder(m_itemTargetIndex), effect, (m_dragKeys & MK_RBUTTON) != 0))
            m_itemTarget->DragLeave();
        else
            m_itemTarget->Drop(data, keys, pt, effect);
        m_itemTarget.Reset(); // consumed by Drop; no DragLeave
        m_itemTargetIndex = -1;
    }
    else
        *effect = DROPEFFECT_NONE;
    ClearDropTarget();
    m_dragData.Reset();
    m_dropKind = DropKind::None;
    m_dropIndex = m_hoverIndex = -1;
    Invalidate();
    return S_OK;
}

// --- Window ---

LRESULT CALLBACK TabStrip::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* created = static_cast<TabStrip*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        created->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<TabStrip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self && self->m_hwnd == hwnd ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TabStrip::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case TCM_GETITEMCOUNT: // lets test scripts count tabs like a stock tab control
        return Count();
    case TCM_GETCURSEL:
        return m_active;
    case TCM_GETITEMW:
        // Header text, as a stock tab control answers it (test scripts read it with memory in this process)
        if (auto* item = reinterpret_cast<TCITEMW*>(lp); item && static_cast<int>(wp) >= 0 && static_cast<int>(wp) < Count())
        {
            if ((item->mask & TCIF_TEXT) && item->pszText && item->cchTextMax > 0)
                wcsncpy_s(item->pszText, item->cchTextMax, m_items[wp].text.c_str(), _TRUNCATE);
            if ((item->mask & kTipMask) && item->pszText && item->cchTextMax > 0)
                wcsncpy_s(item->pszText, item->cchTextMax, m_items[wp].tip.c_str(), _TRUNCATE);
            if (item->mask & TCIF_IMAGE)
                item->iImage = m_items[wp].icon;
            return TRUE;
        }
        return FALSE;
    case TCM_GETITEMRECT:
        if (auto* r = reinterpret_cast<RECT*>(lp); r && static_cast<int>(wp) >= 0 && static_cast<int>(wp) < Count())
        {
            *r = m_items[wp].rc;
            return TRUE;
        }
        return FALSE;
    case WM_NCHITTEST:
    {
        if (m_dragging || GetCapture() == m_hwnd)
            return HTCLIENT;
        POINT client = pt;
        ScreenToClient(m_hwnd, &client);
        const int frame = GetSystemMetricsForDpi(SM_CYFRAME, m_theme->dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, m_theme->dpi);
        const Hit hit = HitTest(client);
        // Top resize band (restored windows only) and empty strip belong to the frame
        if ((!m_maximized && client.y < frame && hit.part != Part::Close && hit.part != Part::Maximize && hit.part != Part::Minimize) || hit.part == Part::None)
            return HTTRANSPARENT;
        return HTCLIENT;
    }
    case WM_SIZE:
        Relayout();
        EnsureVisible(m_active);
        return 0;
    case WM_ERASEBKGND:
        return 1;
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
        }
        if (m_dragIndex >= 0 && (wp & MK_LBUTTON))
            DragTo(pt);
        SetHot(HitTest(pt));
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHot({});
        return 0;
    case WM_NOTIFY:
        if (auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp); info->hdr.hwndFrom == m_tooltip && info->hdr.code == TTN_GETDISPINFOW)
        {
            m_tipText = TipFor(m_hot);
            info->lpszText = m_tipText.empty() ? nullptr : m_tipText.data();
        }
        return 0;
    case WM_MOUSEWHEEL:
        m_events->OnTabWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_LBUTTONDOWN:
    {
        m_pressed = HitTest(pt);
        if (m_pressed.part == Part::None)
            return 0;
        SetCapture(m_hwnd);
        if (m_pressed.part == Part::Tab)
        {
            m_events->OnTabSelect(m_pressed.index);
            m_dragIndex = m_pressed.index;
            m_dragStart = pt;
            m_dragging = false;
            GetCursorPos(&m_pressScreen);
            m_pressTime = GetTickCount64();
        }
        else if (m_pressed.part == Part::ScrollLeft || m_pressed.part == Part::ScrollRight)
        {
            // One tab per click; held down it keeps going
            ScrollBy(m_pressed.part == Part::ScrollLeft ? -m_tabWidth : m_tabWidth);
            SetTimer(m_hwnd, kScrollTimer, 400, nullptr);
        }
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        const Hit pressed = m_pressed;
        const bool dragged = m_dragging;
        KillTimer(m_hwnd, kScrollTimer);
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        m_pressed = {};
        m_dragIndex = -1;
        m_dragging = false;
        Invalidate();
        if (!dragged && pressed.part != Part::None && pressed.part != Part::Tab && pressed.part != Part::ScrollLeft &&
            pressed.part != Part::ScrollRight && HitTest(pt) == pressed)
            Activate(pressed);
        return 0;
    }
    case WM_TIMER:
        if (wp == kScrollTimer)
        {
            SetTimer(m_hwnd, kScrollTimer, 80, nullptr);
            if (m_pressed.part == Part::ScrollLeft || m_pressed.part == Part::ScrollRight)
                ScrollBy(m_pressed.part == Part::ScrollLeft ? -m_tabWidth : m_tabWidth);
            else
                KillTimer(m_hwnd, kScrollTimer);
        }
        else if (wp == kSpinTimer)
        {
            if (std::any_of(m_items.begin(), m_items.end(), [](const Item& i) { return i.loading; }))
                Invalidate();
            else
                KillTimer(m_hwnd, kSpinTimer);
        }
        else if (wp == kHoverTimer)
        {
            KillTimer(m_hwnd, kHoverTimer);
            if (m_hoverIndex >= 0 && m_hoverIndex < Count())
                m_events->OnTabSelect(m_hoverIndex);
        }
        return 0;
    case WM_CAPTURECHANGED:
        KillTimer(m_hwnd, kScrollTimer);
        m_pressed = {};
        m_dragIndex = -1;
        m_dragging = false;
        Invalidate();
        return 0;
    case WM_MBUTTONUP:
        if (const Hit hit = HitTest(pt); hit.part == Part::Tab || hit.part == Part::TabClose)
            m_events->OnTabClose(hit.index);
        return 0;
    case WM_RBUTTONUP:
        if (const Hit hit = HitTest(pt); hit.part == Part::Tab || hit.part == Part::TabClose)
        {
            POINT screen = pt;
            ClientToScreen(m_hwnd, &screen);
            m_events->OnTabMenu(hit.index, screen);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        return 0;
    case WM_DESTROY:
        RevokeDragDrop(m_hwnd);
        KillTimer(m_hwnd, kScrollTimer);
        KillTimer(m_hwnd, kHoverTimer);
        if (m_logo)
        {
            DestroyIcon(m_logo);
            m_logo = nullptr;
        }
        // A WS_POPUP created with this child as parent is owned by the top-level window, not by m_hwnd: destroy it here
        if (m_tooltip)
        {
            DestroyWindow(m_tooltip);
            m_tooltip = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
