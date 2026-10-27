#include "MainWindow.h"
#include "Lang.h"
#include "DWriteText.h"

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

void Place(HWND w, int x, int y, int cx, int cy)
{
    SetWindowPos(w, nullptr, x, y, std::max(0, cx), std::max(0, cy), SWP_NOZORDER | SWP_NOACTIVATE);
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
