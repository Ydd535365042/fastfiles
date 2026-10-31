#include "MainWindow.h"
#include "Backdrop.h"
#include "Lang.h"
#include "DWriteText.h"
#include "LayoutProbe.h"

namespace
{
constexpr UINT_PTR kCueSubclass = 0xFF31;

// The edit's own placeholder is drawn in the system gray text color whatever the theme (unreadable on the dark
// field): an empty edit draws it again in Files' PlaceholderForeground (TextFillColorSecondary = muted)
LRESULT CALLBACK CueProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_PAINT && GetWindowTextLengthW(hwnd) == 0)
    {
        wchar_t cue[256]{};
        if (SendMessageW(hwnd, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue), ARRAYSIZE(cue)) && cue[0])
        {
            const Theme& t = *reinterpret_cast<const Theme*>(ref);
            RECT rc{};
            SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rc));
            RECT client{};
            GetClientRect(hwnd, &client);
            HideCaret(hwnd);
            HDC hdc = GetDC(hwnd);
            FillSolid(hdc, client, t.field);
            HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            if (DWriteText::Available())
                DWriteText::DrawLabel(hdc, font, cue, rc, t.muted, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
            else
            {
                HGDIOBJ old = SelectObject(hdc, font);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, t.muted);
                DrawTextW(hdc, cue, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT | DT_TOP);
                SelectObject(hdc, old);
            }
            ReleaseDC(hwnd, hdc);
            ShowCaret(hwnd);
        }
    }
    else if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, CueProc, kCueSubclass);
    return r;
}

HWND MakeEdit(HWND parent, int id, const Theme* theme)
{
    HWND edit = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
        0, 0, 0, 0, parent, MenuId(id), GetModuleHandleW(nullptr), nullptr);
    if (edit)
        SetWindowSubclass(edit, CueProc, kCueSubclass, reinterpret_cast<DWORD_PTR>(theme));
    return edit;
}

// (MoveChild: resized without the old picture, so the toolbar's 筛选 / 选择 / 排序 / 布局 don't jitter on a splitter drag)
void Place(HWND w, int x, int y, int cx, int cy)
{
    MoveChild(w, x, y, std::max(0, cx), std::max(0, cy));
}
}

void MainWindow::CreateChildren()
{
    m_theme.Build(m_dpi);
    m_prefs.theme = &m_theme; // home pages draw with the window's palette
    m_prefs.settings = this;  // ... and settings pages ask us for their content
    m_contentBrush = CreateSolidBrush(m_theme.field);

    m_strip.Create(m_hwnd, IdTabs, &m_theme, this);
    m_nav.Create(m_hwnd, IdNavBar, &m_theme, this, {
        { BtnSidebar, Glyph::Sidebar, L"", Tr(L"显示/隐藏侧边栏 (Ctrl+B)") },
        { IdBack, Glyph::Back, L"", Tr(L"后退 (Alt+←)") },
        { IdForward, Glyph::Forward, L"", Tr(L"前进 (Alt+→)") },
        { IdUp, Glyph::Up, L"", Tr(L"上一级 (Alt+↑)") },
        { IdRefresh, Glyph::Refresh, L"", Tr(L"刷新 (F5)") } }, &m_theme.layer);
    m_nav.SetButtonLook(true);
    // Files' UpdateAppButton (AddressToolbarButtonStyle), hidden until a newer release is found
    m_updateBar.Create(m_hwnd, IdUpdateBar, &m_theme, this, { { BtnUpdate, Glyph::Download, L"", Tr(L"更新 FastFiles") } }, &m_theme.layer);
    m_updateBar.SetButtonLook(true);
    ShowWindow(m_updateBar.Hwnd(), SW_HIDE);

    m_address = MakeEdit(m_hwnd, IdAddress, &m_theme);
    SHAutoComplete(m_address, SHACF_FILESYS_DIRS | SHACF_URLHISTORY | SHACF_AUTOSUGGEST_FORCE_ON | SHACF_AUTOAPPEND_FORCE_OFF);
    ShowWindow(m_address, SW_HIDE);
    m_crumb.Create(m_hwnd, IdCrumb, this);
    m_search = MakeEdit(m_hwnd, IdSearch, &m_theme);
    m_filter = MakeEdit(m_hwnd, IdFilter, &m_theme);
    ShowWindow(m_filter, SW_HIDE);
    SendMessageW(m_filter, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Tr(L"筛选：只显示名称包含输入内容的项目")));

    CreateCommandBar();
    m_status.Create(m_hwnd, IdStatus, &m_theme);
    m_center.Create(m_hwnd, IdStatusCenter, &m_theme, &m_ops);
    m_info = std::make_unique<InfoPane>(this);
    if (!m_info->Create(m_hwnd, IdInfoPane, &m_theme))
        m_info.reset();

    m_sidebar = new Sidebar(this);
    if (!m_sidebar->Create(m_hwnd, IdSidebar, &m_theme))
    {
        m_sidebar->Release();
        m_sidebar = nullptr;
    }
    m_sideFoot.Create(m_hwnd, IdSideFoot, &m_theme, this, { { BtnSettings, Glyph::Settings, Tr(L"设置"), Tr(L"设置 (Ctrl+,)") } }, &m_theme.layer);
    m_sideFoot.SetSidebarLook(true);
    m_palette.Create(m_hwnd, &m_theme);
    m_suggest.Create(m_hwnd, &m_theme);
    SendMessageW(m_address, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Tr(L"输入路径，或输入 > 运行命令")));

    UpdateFonts();
    ApplyTheme(true);
    ApplySettings();
}

void MainWindow::UpdateFonts()
{
    m_theme.Build(m_dpi);
    for (HWND w : { m_address, m_search, m_filter })
    {
        SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(m_theme.ui), TRUE);
        SendMessageW(w, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    m_crumb.SetTheme(&m_theme);
    if (m_sidebar)
        m_sidebar->Relayout();
    if (m_info)
        m_info->Relayout();
    m_strip.Relayout();
    m_nav.Relayout();
    m_cmd.Relayout();
    if (m_sideFoot.Hwnd())
        m_sideFoot.Relayout();
    m_palette.Relayout();
    m_center.Relayout();
    ForEachPane([](Tab* tab) { tab->RelayoutHome(); }); // home and settings pages
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::Layout()
{
    if (!m_strip.Hwnd() || !m_status.Hwnd() || !m_cmd.Hwnd() || !m_crumb.Hwnd())
        return;
    const LayoutProbe::Scope probe(m_hwnd, L"layout");
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const Theme& t = m_theme;
    const int width = rc.right;
    m_statusShown = StatusWanted();
    ShowWindow(m_status.Hwnd(), m_statusShown ? SW_SHOWNA : SW_HIDE);
    const int title = t.S(kTitleRow), nav = t.S(kNavRow), status = m_statusShown ? t.S(kStatusRow) : 0;
    const int cmd = m_toolbarVisible ? t.S(kCmdRow) : 0;
    const int filter = m_filterVisible ? t.S(kFilterRow) : 0;
    const int line = std::max(1, t.S(1));

    Place(m_strip.Hwnd(), 0, 0, width, title);

    // Nav row: [sidebar back forward up refresh] [omnibar ........] [search] [status center]
    const int navW = m_nav.IdealWidth();
    Place(m_nav.Hwnd(), t.S(4), title, navW, nav - line);
    const int boxH = t.S(32), mid = title + nav / 2;
    const int searchW = std::clamp(width / 4, t.S(160), t.S(300));
    // Files' RightSideActionsStackPanel: the status center button (36x32) right of the omnibar, 4 apart
    const bool center = m_center.Button() && m_center.Wanted();
    int navRight = width - t.S(10);
    if (center)
    {
        Place(m_center.Button(), navRight - t.S(36), mid - boxH / 2, t.S(36), boxH);
        navRight -= t.S(36) + t.S(4);
    }
    if (m_center.Button())
        ShowWindow(m_center.Button(), center ? SW_SHOWNA : SW_HIDE);
    // Files' UpdateAppButton: in the same row, left of the status center
    const bool update = m_updateBar.Hwnd() && UpdateButtonShown();
    if (update)
    {
        const int w = m_updateBar.IdealWidth();
        Place(m_updateBar.Hwnd(), navRight - w, title, w, nav - line);
        navRight -= w + t.S(4);
    }
    if (m_updateBar.Hwnd())
        ShowWindow(m_updateBar.Hwnd(), update ? SW_SHOWNA : SW_HIDE);
    const int searchRight = navRight;
    m_searchFrame = { searchRight - searchW, mid - boxH / 2, searchRight, mid + boxH / 2 };
    m_omni = { t.S(4) + navW + t.S(6), m_searchFrame.top, m_searchFrame.left - t.S(8), m_searchFrame.bottom };

    HDC hdc = GetDC(m_hwnd);
    HGDIOBJ old = SelectObject(hdc, t.ui);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    ReleaseDC(m_hwnd, hdc);
    const int editH = tm.tmHeight + t.S(2);

    const int inset = t.S(2);
    Place(m_crumb.Hwnd(), m_omni.left + inset, m_omni.top + inset, m_omni.right - m_omni.left - inset * 2, boxH - inset * 2);
    Place(m_address, m_omni.left + t.S(10), mid - editH / 2, m_omni.right - m_omni.left - t.S(20), editH);
    Place(m_search, m_searchFrame.left + t.S(34), mid - editH / 2, searchW - t.S(44), editH);

    // Body: sidebar | splitter | (command bar / file view / status bar)
    m_bodyTop = title + nav;
    const int side = m_sidebarVisible ? t.S(m_sidebarWidth) : 0;
    const int innerLeft = m_sidebarVisible ? side + t.S(kSplitter) : 0;
    // Sidebar list, then the settings button pinned under it
    const int foot = t.S(kSideFoot);
    if (m_sidebar && m_sidebarVisible)
        m_sidebar->Move({ 0, m_bodyTop, side - line, static_cast<int>(rc.bottom) - foot });
    if (m_sideFoot.Hwnd())
    {
        Place(m_sideFoot.Hwnd(), t.S(4), rc.bottom - foot + line, side - line - t.S(8), foot - line);
        ShowWindow(m_sideFoot.Hwnd(), m_sidebarVisible ? SW_SHOWNA : SW_HIDE);
    }
    // Files' MainPage: the toolbar and the file area are rounded cards on the sidebar layer (8 right and bottom, 4 between);
    // children sit kCardInset inside so the parent's anti-aliased corners and stroke stay visible
    const int right = width - t.S(kCardMargin), bottom = static_cast<int>(rc.bottom) - t.S(kCardMargin);
    const int pad = std::max(1, t.S(kCardInset));
    m_cmdCard = cmd ? RECT{ innerLeft, m_bodyTop, right, m_bodyTop + cmd } : RECT{};
    Place(m_cmd.Hwnd(), m_cmdCard.left + pad, m_cmdCard.top + pad, m_cmdCard.right - m_cmdCard.left - 2 * pad, cmd - 2 * pad);
    const int cardTop = m_bodyTop + (cmd ? cmd + t.S(kCardGap) : 0);
    // Details/preview pane: its own card. Files' InfoPane.UpdatePosition: on the right while the content area is wider
    // than 700, under the file area otherwise (MinHeight 140); the file area keeps at least 300 / 120
    m_infoBottom = right - innerLeft <= t.S(700);
    bool info = false;
    if (m_infoBottom)
    {
        const int infoH = std::min(t.S(m_infoHeight), bottom - cardTop - t.S(120) - t.S(kCardGap));
        info = m_info && m_infoVisible && infoH >= t.S(140);
        const int fileBottom = info ? bottom - infoH - t.S(kCardGap) : bottom;
        m_fileCard = { innerLeft, cardTop, right, std::max(cardTop, fileBottom) };
        m_infoCard = info ? RECT{ innerLeft, fileBottom + t.S(kCardGap), right, bottom } : RECT{};
    }
    else
    {
        const int infoW = std::min(t.S(m_infoWidth), right - innerLeft - t.S(300));
        info = m_info && m_infoVisible && infoW >= t.S(180);
        const int fileRight = info ? right - infoW - t.S(kCardGap) : right;
        m_fileCard = { innerLeft, cardTop, fileRight, std::max(cardTop, bottom) };
        m_infoCard = info ? RECT{ fileRight + t.S(kCardGap), cardTop, right, m_fileCard.bottom } : RECT{};
    }
    m_infoRect = info ? RECT{ m_infoCard.left + pad, m_infoCard.top + pad, m_infoCard.right - pad, m_infoCard.bottom - pad } : RECT{};
    if (m_info)
    {
        if (info)
            m_info->Move(m_infoRect);
        ShowWindow(m_info->Hwnd(), info ? SW_SHOWNA : SW_HIDE);
    }
    const int innerL = m_fileCard.left + pad, innerR = m_fileCard.right - pad, innerB = m_fileCard.bottom - pad;
    Place(m_status.Hwnd(), innerL, innerB - status, innerR - innerL, status);
    const int filterTop = m_fileCard.top + pad;
    m_filterFrame = filter ? RECT{ innerL + t.S(8), filterTop + t.S(4), innerR - t.S(8), filterTop + filter - t.S(4) } : RECT{};
    if (filter)
        Place(m_filter, m_filterFrame.left + t.S(34), (m_filterFrame.top + m_filterFrame.bottom - editH) / 2, m_filterFrame.right - m_filterFrame.left - t.S(44), editH);
    const int viewTop = filterTop + filter;
    m_viewRect = { innerL, viewTop, innerR, std::max(viewTop, innerB - status) };
    PlacePanes();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool MainWindow::StatusWanted() const
{
    // Files hides the status bar on its own pages, unless another pane of a split tab is a folder
    const Tab* tab = Active();
    const TabSlot* slot = m_active >= 0 && m_active < static_cast<int>(m_tabList.size()) ? &m_tabList[m_active] : nullptr;
    return m_statusVisible && !(tab && tab->OnHome() && !(slot && slot->Split()));
}

void MainWindow::SaveDragFrame()
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
        return;
    HDC wnd = GetDCEx(m_hwnd, nullptr, DCX_CACHE); // (not clipped to the children: their pixels too)
    if (!wnd)
        return;
    // Grown only (with room: a drag making the window bigger doesn't make a new bitmap every step)
    if (!m_dragDc || rc.right > m_dragCapacity.cx || rc.bottom > m_dragCapacity.cy)
    {
        FreeDragFrame();
        const SIZE cap{ rc.right + 128, rc.bottom + 128 };
        m_dragDc = CreateCompatibleDC(wnd);
        // (its pixels readable: DragSeam looks for where to cut it)
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = cap.cx;
        bi.bmiHeader.biHeight = -cap.cy;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_dragBmp = m_dragDc ? CreateDIBSection(wnd, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        m_dragBits = static_cast<const DWORD*>(bits);
        if (!m_dragBmp || !m_dragBits)
        {
            FreeDragFrame();
            ReleaseDC(m_hwnd, wnd);
            return;
        }
        m_dragOld = SelectObject(m_dragDc, m_dragBmp);
        m_dragCapacity = cap;
    }
    BitBlt(m_dragDc, 0, 0, rc.right, rc.bottom, wnd, 0, 0, SRCCOPY);
    GdiFlush();
    m_dragSize = { rc.right, rc.bottom };
    ReleaseDC(m_hwnd, wnd);
}

int MainWindow::DragSeam(bool vertical, int a0, int a1, int lo, int hi, int from, int to) const
{
    // Cut where the picture is the same color right across: what a smaller size cuts out there (from - to columns /
    // rows after the cut) or a bigger one doubles (the one before it) is all the one before it. Rows / columns where
    // it isn't are counted, the cut where the fewest are (the right / bottom most of those: the closest to the side
    // pinned parts). Not the picture's edges across (the rounded corners).
    if (!m_dragBits || from <= 0 || to <= 0)
        return -1;
    const int cut = std::max(0, from - to);
    hi = std::min(hi, from - cut);
    // (no room between what's pinned to each side, a narrow window's list columns reaching the scroll bar: cut right
    // before the side pinned right / bottom anyway, the cut columns doubled or cut out)
    lo = std::max(std::min(lo, hi - 1), 1);
    const int across = vertical ? m_dragSize.cx : m_dragSize.cy;
    a0 = std::max(a0, 4);
    a1 = std::min(a1, across - 4);
    if (hi - lo < 1 || a1 - a0 < 1)
        return -1;
    const auto alike = [](DWORD a, DWORD b)
    {
        if (a == b)
            return true;
        const int dr = static_cast<int>((a >> 16) & 255) - static_cast<int>((b >> 16) & 255);
        const int dg = static_cast<int>((a >> 8) & 255) - static_cast<int>((b >> 8) & 255);
        const int db = static_cast<int>(a & 255) - static_cast<int>(b & 255);
        return dr >= -6 && dr <= 6 && dg >= -6 && dg <= 6 && db >= -6 && db <= 6;
    };
    const int stride = m_dragCapacity.cx;
    // cost[s]: lines across where [s - 1, s + cut] along isn't all one color; a line's last change before s + cut
    // (last[]) is all it takes: one at s or after it counts
    std::vector<int> cost(static_cast<size_t>(hi), 0);
    if (vertical)
    {
        std::vector<int> last(static_cast<size_t>(across), -1);
        for (int y = 1; y < hi + cut; ++y)
        {
            const DWORD* row = m_dragBits + static_cast<size_t>(y) * stride;
            const DWORD* up = row - stride;
            for (int x = a0; x < a1; ++x)
                if (!alike(row[x], up[x]))
                    last[x] = y;
            const int s = y - cut;
            if (s >= lo && s < hi)
                for (int x = a0; x < a1; ++x)
                    cost[s] += last[x] >= s ? 1 : 0;
        }
    }
    else
    {
        for (int y = a0; y < a1; ++y)
        {
            const DWORD* row = m_dragBits + static_cast<size_t>(y) * stride;
            int lastChange = -1;
            for (int x = 1; x < hi + cut; ++x)
            {
                if (!alike(row[x], row[x - 1]))
                    lastChange = x;
                const int s = x - cut;
                if (s >= lo && s < hi && lastChange >= s)
                    ++cost[s];
            }
        }
    }
    int least = -1;
    for (int s = lo; s < hi; ++s)
        if (least < 0 || cost[s] < least)
            least = cost[s];
    // A row: the bottom most (right above the status bar's line, the cards' bottoms). A column: the middle of the
    // widest gap (the right most of those): between the tabs and the caption buttons, the toolbar's groups, the list's
    // last column and the status bar's right text. The right most column alone could be one between parts pinned
    // right (the 6 px between that text and the card's edge took it as pinned left).
    int best = -1, runStart = -1, bestLen = 0;
    for (int s = lo; s <= hi; ++s)
    {
        const bool in = s < hi && cost[s] == least;
        if (in && runStart < 0)
            runStart = s;
        if (!in && runStart >= 0)
        {
            const int len = s - runStart;
            if (vertical || len >= bestLen)
            {
                best = vertical ? s - 1 : runStart + len / 2;
                bestLen = len;
            }
            runStart = -1;
        }
    }
    return best;
}

int MainWindow::DragCuts(bool cutX, bool cutY, int width, int height, DragCut (&cuts)[5]) const
{
    // The old layout's (the picture's) parts, each cut on its own between what's pinned to each side of it
    const Theme& t = m_theme;
    const int w = m_dragSize.cx, h = m_dragSize.cy;
    const int title = t.S(kTitleRow), bodyTop = title + t.S(kNavRow), pad = std::max(1, t.S(kCardInset));
    // (columns: right of the sidebar, left of the details pane / the scroll bar; rows: under the list's header, above
    // the status bar / the cards' bottoms)
    int contentL = m_fileCard.left + pad;
    const int contentR = m_fileCard.right - pad - t.S(16);
    // (the list's columns are pinned left, the gaps between them too: right of its last one, details headers)
    struct Find { HWND main; RECT view; int right; } find{ m_hwnd, m_viewRect, -1 };
    EnumChildWindows(m_hwnd, [](HWND w, LPARAM lp) -> BOOL
    {
        auto& f = *reinterpret_cast<Find*>(lp);
        wchar_t cls[16]{};
        if (!IsWindowVisible(w) || !GetClassNameW(w, cls, 16) || wcscmp(cls, WC_HEADERW) != 0)
            return TRUE;
        const int count = Header_GetItemCount(w);
        RECT item{};
        if (count <= 0 || !Header_GetItemRect(w, Header_OrderToIndex(w, count - 1), &item))
            return TRUE;
        RECT shown{};
        GetClientRect(w, &shown);
        item.right = std::min(item.right, shown.right);
        MapWindowPoints(w, f.main, reinterpret_cast<POINT*>(&item), 2);
        if (item.right > f.view.left && item.left < f.view.right)
            f.right = std::max(f.right, static_cast<int>(item.right));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&find));
    if (find.right >= 0)
        contentL = std::max(contentL, find.right + t.S(2));
    const int listT = m_viewRect.top + t.S(32), listB = m_viewRect.bottom;
    int n = 0;
    if (cutX)
    {
        // Rows: the tab row (the tabs | the caption buttons), the nav row (the omnibar's empty end | its right end,
        // the search box), the command bar (its groups), the list (its columns | the details pane, the scroll bar),
        // the status bar (its left text | its right text; it's in no gap of the list's)
        const int cmdBottom = IsRectEmpty(&m_cmdCard) ? bodyTop : static_cast<int>(m_cmdCard.bottom);
        const int statusTop = std::clamp(listB, cmdBottom, h);
        const int rows[6] = { 0, title, bodyTop, cmdBottom, cutY ? h : statusTop, h };
        // (the caption buttons: 3 x 46, TabStrip::Relayout)
        const int lo[5] = { t.S(48), static_cast<int>(m_omni.left) + t.S(40), static_cast<int>(m_cmdCard.left) + pad, contentL,
            static_cast<int>(m_fileCard.left) + pad + t.S(8) };
        const int hi[5] = { w - 3 * t.S(46) - t.S(2), static_cast<int>(m_omni.right) - t.S(4), static_cast<int>(m_cmdCard.right) - pad,
            contentR, static_cast<int>(m_fileCard.right) - pad - t.S(4) };
        for (int i = 0; i < 5; ++i)
            if (rows[i + 1] > rows[i])
                cuts[n++] = { { 0, rows[i], w, rows[i + 1] },
                    DragSeam(false, rows[i], i == 3 ? statusTop : rows[i + 1], lo[i], hi[i], w, width), -1, -1 };
        // A corner: the list down to the bottom cut across too, between the list's rows and what's pinned to the
        // bottom (the status bar, the settings button under the sidebar), what's under that cut on its own way
        if (cutY && n > 0)
        {
            DragCut& c = cuts[n - 1];
            c.seamY = DragSeam(true, 0, w, listT, m_sidebarVisible ? std::min(listB, h - t.S(kSideFoot)) : listB, h, height);
            c.seamX2 = DragSeam(false, c.seamY >= 0 ? c.seamY : statusTop, h, lo[4], hi[4], w, width);
        }
        return n;
    }
    // Columns: the sidebar (its list | the settings button under it), the rest (the list's rows | the status bar, the
    // cards' bottoms; the tab and nav rows are above)
    const int cols[3] = { 0, m_sidebarVisible ? static_cast<int>(m_fileCard.left) : 0, w };
    const int lo[2] = { bodyTop + t.S(8), listT }, hi[2] = { h - t.S(kSideFoot), listB };
    for (int i = 0; i < 2; ++i)
        if (cols[i + 1] > cols[i])
            cuts[n++] = { { cols[i], 0, cols[i + 1], h }, -1, DragSeam(true, cols[i], cols[i + 1], lo[i], hi[i], h, height), -1 };
    return n;
}

void MainWindow::PutDragFrame(HDC dc, const DragCut& cut, int width, int height) const
{
    // Each side of a cut: before it where it was in the client (pinned left / top), after it where it was from the
    // client's right / bottom; between them the column / row before the cut doubled, or cut out. Not cut that way:
    // where it was in the client (Windows' own way).
    struct Part { int to, from, toLen, fromLen; };
    const auto parts = [](int base, int from, int to, int seam, Part (&p)[3]) -> int
    {
        if (seam < 0)
        {
            p[0] = { base, base, std::min(from, to), std::min(from, to) };
            return 1;
        }
        const int d = to - from, s = seam - base;
        int n = 0;
        p[n++] = { base, base, s, s };
        if (d > 0)
            p[n++] = { base + s, base + s - 1, d, 1 };
        const int fromRight = s + std::max(0, -d);
        p[n++] = { base + fromRight + d, base + fromRight, from - fromRight, from - fromRight };
        return n;
    };
    const RECT& b = cut.band;
    const int bw = b.right - b.left, bh = b.bottom - b.top;
    Part xs[3], xs2[3], ys[3];
    const int nx = parts(b.left, bw, cut.seamX >= 0 ? bw + width - m_dragSize.cx : width - b.left, cut.seamX, xs);
    const int nx2 = parts(b.left, bw, cut.seamX2 >= 0 ? bw + width - m_dragSize.cx : width - b.left, cut.seamX2, xs2);
    const int ny = parts(b.top, bh, cut.seamY >= 0 ? bh + height - m_dragSize.cy : height - b.top, cut.seamY, ys);
    SetStretchBltMode(dc, COLORONCOLOR);
    for (int j = 0; j < ny; ++j)
    {
        // (under the row cut: its own column cut)
        const bool under = cut.seamY >= 0 && j == ny - 1;
        for (int i = 0; i < (under ? nx2 : nx); ++i)
        {
            const Part& x = under ? xs2[i] : xs[i];
            const Part& y = ys[j];
            if (x.toLen <= 0 || y.toLen <= 0 || x.fromLen <= 0 || y.fromLen <= 0)
                continue;
            if (x.toLen == x.fromLen && y.toLen == y.fromLen)
                BitBlt(dc, x.to, y.to, x.toLen, y.toLen, m_dragDc, x.from, y.from, SRCCOPY);
            else
                StretchBlt(dc, x.to, y.to, x.toLen, y.toLen, m_dragDc, x.from, y.from, x.fromLen, y.fromLen, SRCCOPY);
        }
    }
}

void MainWindow::FreeDragFrame()
{
    if (m_dragDc)
    {
        if (m_dragOld)
            SelectObject(m_dragDc, m_dragOld);
        DeleteDC(m_dragDc);
    }
    if (m_dragBmp)
        DeleteObject(m_dragBmp);
    m_dragDc = nullptr;
    m_dragBmp = nullptr;
    m_dragOld = nullptr;
    m_dragBits = nullptr;
    m_dragSize = m_dragCapacity = {};
}

void MainWindow::PaintUncovered(const RECT& was, const RECT& wasWindow, bool dragged, RECT (&strips)[10])
{
    for (RECT& s : strips)
        s = {};
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    POINT origin{};
    ClientToScreen(m_hwnd, &origin);
    // (was / wasWindow on the screen)
    // What the old picture doesn't cover. Where Windows left it: a size at once (WVR_REDRAW) where it was on the
    // screen; a frame drag at the window's top left or (a left / top edge) where it was on the screen, both seen, so
    // there both. Before the layout: only the flat background there (the tab row's, the layer's, through the
    // material), the rest of the layout isn't known yet; over the children too (the grown side reaches the sidebar /
    // the tab row, which have that background). The strips are returned: after the layout everything there gets its
    // real paint from its own window.
    const RECT old{ was.left - origin.x, was.top - origin.y, was.right - origin.x, was.bottom - origin.y }; // (client)
    strips[0] = { 0, 0, std::max(0L, old.left), rc.bottom };
    strips[1] = { std::max(0L, old.right), 0, rc.right, rc.bottom };
    strips[2] = { 0, 0, rc.right, std::max(0L, old.top) };
    strips[3] = { 0, std::max(0L, old.bottom), rc.right, rc.bottom };
    if (dragged)
    {
        strips[4] = { was.right - was.left, 0, rc.right, rc.bottom };
        strips[5] = { 0, was.bottom - was.top, rc.right, rc.bottom };
    }
    // Windows 8 / 10: the old picture's rounded corners were never painted (outside the window's region then); only
    // what was outside it (corner) is filled, the rest of the corner box is the old picture
    HRGN corner = nullptr;
    if (m_round.Custom() && !IsRectEmpty(&wasWindow) && m_round.Radius() > 0)
    {
        const RECT& w = wasWindow;
        const int d = 2 * m_round.Radius();
        corner = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
        if (HRGN round = CreateRoundRectRgn(w.left - origin.x, w.top - origin.y, w.right - origin.x + 1, w.bottom - origin.y + 1, d, d))
        {
            CombineRgn(corner, corner, round, RGN_DIFF);
            DeleteObject(round);
        }
        const LONG box = m_round.Radius() + 2;
        strips[6] = { old.left, old.top, old.left + box, old.top + box };
        strips[7] = { old.right - box, old.top, old.right, old.top + box };
        strips[8] = { old.left, old.bottom - box, old.left + box, old.bottom };
        strips[9] = { old.right - box, old.bottom - box, old.right, old.bottom };
        for (int i = 6; i < 10; ++i)
            IntersectRect(&strips[i], &strips[i], &rc);
    }
    RECT all{};
    for (RECT& s : strips)
    {
        if (s.right <= s.left || s.bottom <= s.top)
            s = {};
        UnionRect(&all, &all, &s);
    }
    HDC hdc = IsRectEmpty(&all) ? nullptr : GetDCEx(m_hwnd, nullptr, DCX_CACHE);
    if (!hdc)
    {
        if (corner)
            DeleteObject(corner);
        return;
    }
    {
        const Theme& t = m_theme;
        PaintBuffer buffer(hdc, rc.right, rc.bottom, all);
        const int title = t.S(kTitleRow);
        const bool material = buffer.bits && Backdrop::Active();
        for (int i = 0; i < 10; ++i)
        {
            const RECT& s = strips[i];
            if (IsRectEmpty(&s))
                continue;
            // Each strip on its own (their union spans the whole window once the corners are in): filled again
            // first, so where two overlap the material isn't laid over itself
            FillSolid(buffer.dc, { s.left, s.top, s.right, std::min<LONG>(s.bottom, title) }, t.chrome);
            FillSolid(buffer.dc, { s.left, std::max<LONG>(s.top, title), s.right, s.bottom }, t.layer);
            if (material)
            {
                GdiFlush();
                Backdrop::Compose(m_hwnd, buffer.bits, buffer.width, s);
            }
            if (i >= 6 && corner)
                SelectClipRgn(hdc, corner);
            BitBlt(hdc, s.left, s.top, s.right - s.left, s.bottom - s.top, buffer.dc, s.left, s.top, SRCCOPY);
            if (i >= 6 && corner)
                SelectClipRgn(hdc, nullptr);
        }
    }
    if (corner)
        DeleteObject(corner);
    ReleaseDC(m_hwnd, hdc);
}

void MainWindow::PaintChrome(HDC hdc, const RECT& rc)
{
    const Theme& t = m_theme;
    const int line = std::max(1, t.S(1));
    const int title = t.S(kTitleRow);

    // Tab row on the window background; nav row, sidebar and the gutters around the cards on the layer
    FillSolid(hdc, { 0, 0, rc.right, title }, t.chrome);
    FillSolid(hdc, { 0, title, rc.right, rc.bottom }, t.layer);
    if (m_sidebarVisible)
    {
        const int foot = rc.bottom - t.S(kSideFoot);
        FillSolid(hdc, { t.S(8), foot, m_viewRect.left - t.S(kSplitter) - t.S(8), foot + line }, t.line);
    }
    // Cards: CornerRadius 8, CardStrokeColorDefault
    const int radius = t.S(kCardRadius);
    if (m_toolbarVisible && !IsRectEmpty(&m_cmdCard))
    {
        FillRound(hdc, m_cmdCard, radius, t.toolbar);
        FrameRound(hdc, m_cmdCard, radius, t.cardEdge);
    }
    FillRound(hdc, m_fileCard, radius, t.content);
    FrameRound(hdc, m_fileCard, radius, t.cardEdge);
    if (!IsRectEmpty(&m_infoCard))
    {
        FillRound(hdc, m_infoCard, radius, t.toolbar); // App.Theme.InfoPane: CardBackgroundFillColorSecondary
        FrameRound(hdc, m_infoCard, radius, t.cardEdge);
    }
    if (m_filterVisible)
    {
        FillRound(hdc, m_filterFrame, t.S(4), t.field);
        FrameRound(hdc, m_filterFrame, t.S(4), GetFocus() == m_filter ? t.accent : t.fieldStroke);
        DrawGlyph(hdc, t.iconsMid, Glyph::Filter, { m_filterFrame.left + t.S(8), m_filterFrame.top, m_filterFrame.left + t.S(30), m_filterFrame.bottom }, t.muted);
    }

    // Dual pane: accent bar over the focused pane, a faint one over the other, splitter line between them
    if (m_active >= 0 && m_active < static_cast<int>(m_tabList.size()) && m_tabList[m_active].Split())
    {
        const TabSlot& slot = m_tabList[m_active];
        RECT panes[2], split{};
        PaneRects(slot, panes, &split);
        const int bar = std::max(1, t.S(kPaneBar));
        for (int i = 0; i < 2; ++i)
            FillSolid(hdc, { panes[i].left, panes[i].top - bar, panes[i].right, panes[i].top }, i == slot.focus ? t.accent : t.line);
        FillSolid(hdc, split, t.content);
        if (slot.horizontal)
            FillSolid(hdc, { split.left, (split.top + split.bottom - line) / 2, split.right, (split.top + split.bottom + line) / 2 }, t.line);
        else
            FillSolid(hdc, { (split.left + split.right - line) / 2, split.top, (split.left + split.right + line) / 2, split.bottom }, t.line);
    }

    const HWND focus = GetFocus();
    FillRound(hdc, m_omni, t.S(6), t.field);
    FrameRound(hdc, m_omni, t.S(6), focus == m_address ? t.accent : t.fieldStroke);
    FillRound(hdc, m_searchFrame, t.S(6), t.field);
    FrameRound(hdc, m_searchFrame, t.S(6), focus == m_search ? t.accent : t.fieldStroke);
    DrawGlyph(hdc, t.iconsMid, Glyph::Search, { m_searchFrame.left + t.S(8), m_searchFrame.top, m_searchFrame.left + t.S(30), m_searchFrame.bottom }, t.muted);
}

bool MainWindow::OverInfoSplitter(int x, int y) const
{
    // The gap between the file card and the details card (beside it or under it)
    if (m_infoCard.right <= m_infoCard.left)
        return false;
    if (m_infoBottom)
        return x >= m_infoCard.left && x < m_infoCard.right && y >= m_fileCard.bottom - Scale(2, m_dpi) && y < m_infoCard.top + Scale(2, m_dpi);
    return y >= m_infoCard.top && y < m_infoCard.bottom && x >= m_fileCard.right - Scale(2, m_dpi) && x < m_infoCard.left + Scale(2, m_dpi);
}
bool MainWindow::OverSplitter(int x, int y) const
{
    const int left = Scale(m_sidebarWidth, m_dpi);
    return m_sidebarVisible && y >= m_bodyTop && x >= left - Scale(2, m_dpi) && x < left + Scale(kSplitter, m_dpi);
}

// WM_COMMAND from the nav bar / command bar / tests. Nav buttons hand the focus back to the file list.
void MainWindow::OnCommand(int id)
{
    if (const int command = CustomCommand(id))
        id = command; // a customized command bar button
    RunAction(id);
    if (id >= IdBack && id <= IdRefresh)
        if (Tab* active = Active())
            active->Focus();
}

void MainWindow::SetEditMode(bool editing)
{
    if (editing == m_editing)
        return;
    m_editing = editing;
    ShowWindow(m_address, editing ? SW_SHOW : SW_HIDE);
    ShowWindow(m_crumb.Hwnd(), editing ? SW_HIDE : SW_SHOW);
    InvalidateRect(m_hwnd, &m_omni, FALSE);
}

void MainWindow::OnDestroy()
{
    if (m_trayShown)
    {
        m_primary = false;
        m_hiddenToTray = false;
        m_settings.background = false;
        UpdateTray();
    }
    RemovePropW(m_hwnd, L"FastFiles.Primary");
    m_sizer.Cancel();
    FreeDragFrame(); // (closed during a frame drag: no WM_EXITSIZEMOVE)
    if (m_moving)
        Backdrop::SetMoving(false);
    KillTimer(m_hwnd, kStatusTimer);
    KillTimer(m_hwnd, kInfoTimer);
    m_info.reset();
    ForEachPane([](Tab* tab) { tab->Close(); });
    m_tabList.clear();
    m_active = -1;
    if (m_sidebar)
    {
        m_sidebar->Destroy();
        m_sidebar->Release();
        m_sidebar = nullptr;
    }
    if (m_contentBrush)
        DeleteObject(m_contentBrush);
    m_contentBrush = nullptr;
}
