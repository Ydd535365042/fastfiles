#include "MainWindow.h"
#include "Lang.h"

namespace
{
void CloseSlot(TabSlot& slot)
{
    for (auto& pane : slot.panes)
        if (pane)
            pane->Close();
}
}

Tab* MainWindow::Active() const
{
    return m_active >= 0 && m_active < static_cast<int>(m_tabList.size()) ? m_tabList[m_active].Focused() : nullptr;
}

TabSlot* MainWindow::ActiveSlot()
{
    return m_active >= 0 && m_active < static_cast<int>(m_tabList.size()) ? &m_tabList[m_active] : nullptr;
}

int MainWindow::IndexOf(const Tab* tab, int* pane) const
{
    for (size_t i = 0; i < m_tabList.size(); ++i)
        for (int p = 0; p < 2; ++p)
            if (tab && m_tabList[i].panes[p].Get() == tab)
            {
                if (pane)
                    *pane = p;
                return static_cast<int>(i);
            }
    return -1;
}

// Files opens new tabs and windows on its home page
UniquePidl MainWindow::DefaultLocation() const
{
    return ClonePidl(HomePidl());
}

ComPtr<Tab> MainWindow::MakeTab(PCIDLIST_ABSOLUTE pidl)
{
    UniquePidl fallback;
    if (!pidl)
    {
        fallback = DefaultLocation();
        pidl = fallback.get();
    }
    ComPtr<Tab> tab;
    tab.Attach(new Tab(m_hwnd, this, pidl, &m_prefs));
    return tab;
}

int MainWindow::OpenInNewTab(PCIDLIST_ABSOLUTE pidl, bool activate)
{
    return NewTab(pidl, activate || m_settings.switchNewTab);
}

int MainWindow::NewTab(PCIDLIST_ABSOLUTE pidl, bool activate, bool append, bool restore)
{
    TabSlot slot;
    slot.panes[0] = MakeTab(pidl);
    // Files' AlwaysOpenDualPaneInNewTab: the second pane on the home page, when the window is wide enough for two
    // (MultiplePaneWidthThreshold, 750) and the tab isn't the settings page
    RECT client{};
    GetClientRect(m_hwnd, &client);
    if (!restore && m_settings.dualPaneNewTab && !(pidl && IsSettingsPidl(pidl)) && client.right - client.left > Scale(750, m_dpi))
    {
        slot.panes[1] = MakeTab(HomePidl());
        slot.horizontal = m_settings.paneHorizontal;
    }

    const int index = append || m_active < 0 ? static_cast<int>(m_tabList.size()) : m_active + 1;
    m_tabList.insert(m_tabList.begin() + index, std::move(slot));
    m_strip.Insert(index, L"", -1);
    if (m_active >= index)
        ++m_active;
    UpdateTabLabel(index);

    if (activate)
        ActivateTab(index);
    else if (m_active >= 0)
        m_strip.SetActive(m_active);
    return index;
}

void MainWindow::ActivateTab(int index)
{
    if (index < 0 || index >= static_cast<int>(m_tabList.size()))
        return;
    if (TabSlot* old = ActiveSlot(); old && m_active != index)
        for (auto& pane : old->panes)
            if (pane)
                pane->Hide();
    m_active = index;
    m_strip.SetActive(index);
    PlacePanes();
    m_tabList[index].Focused()->Focus();
    UpdateChrome();
    UpdateStatus();
}

void MainWindow::CloseTab(int index)
{
    RemoveTab(index, true);
}

void MainWindow::RemoveTab(int index, bool remember)
{
    if (index < 0 || index >= static_cast<int>(m_tabList.size()))
        return;
    if (m_tabList.size() == 1)
    {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    const bool wasActive = index == m_active;
    if (remember)
    {
        m_closedTabs.push_back(ClonePidl(m_tabList[index].Focused()->Location()));
        if (m_closedTabs.size() > 20)
            m_closedTabs.erase(m_closedTabs.begin());
    }
    CloseSlot(m_tabList[index]);
    m_tabList.erase(m_tabList.begin() + index);
    m_strip.Remove(index);
    if (wasActive)
    {
        m_active = -1;
        ActivateTab(std::min(index, static_cast<int>(m_tabList.size()) - 1));
    }
    else
    {
        if (m_active > index)
            --m_active;
        m_strip.SetActive(m_active);
    }
    RefreshTabHeaders();
}

int MainWindow::InsertSlot(int index, const SessionData::TabState& state)
{
    const int at = NewTab(state.pidl.get(), false, true, true);
    TabSlot& slot = m_tabList[at];
    if (state.second)
    {
        slot.panes[1] = MakeTab(state.second.get());
        slot.horizontal = state.horizontal;
        slot.ratio = state.ratio;
        slot.focus = state.focus;
    }
    const int to = std::clamp(index, 0, at);
    if (to != at)
    {
        // NewTab appended it; move it where it was dropped
        TabSlot moved = std::move(m_tabList[at]);
        m_tabList.erase(m_tabList.begin() + at);
        m_tabList.insert(m_tabList.begin() + to, std::move(moved));
        m_strip.Remove(at);
        m_strip.Insert(to, L"", -1);
        if (m_active >= to)
            ++m_active;
        m_strip.SetActive(m_active);
    }
    UpdateTabLabel(to);
    return to;
}

SessionData::TabState MainWindow::SlotState(int index) const
{
    SessionData::TabState state;
    if (index < 0 || index >= static_cast<int>(m_tabList.size()))
        return state;
    const TabSlot& slot = m_tabList[index];
    state.pidl = ClonePidl(slot.panes[0]->Location());
    if (slot.Split())
    {
        state.second = ClonePidl(slot.panes[1]->Location());
        state.horizontal = slot.horizontal;
        state.ratio = slot.ratio;
        state.focus = slot.focus;
    }
    return state;
}

void MainWindow::CloseOtherTabs(int keep)
{
    if (keep < 0 || keep >= static_cast<int>(m_tabList.size()))
        return;
    TabSlot kept = std::move(m_tabList[keep]);
    for (size_t i = 0; i < m_tabList.size(); ++i)
        if (static_cast<int>(i) != keep)
        {
            m_closedTabs.push_back(ClonePidl(m_tabList[i].Focused()->Location()));
            CloseSlot(m_tabList[i]);
        }
    while (m_closedTabs.size() > 20)
        m_closedTabs.erase(m_closedTabs.begin());
    m_tabList.clear();
    m_tabList.push_back(std::move(kept));
    m_strip.Clear();
    m_strip.Insert(0, L"", -1);
    m_active = -1;
    RefreshTabHeaders();
    ActivateTab(0);
}

void MainWindow::CloseTabsSide(int keep, bool left)
{
    if (keep < 0 || keep >= static_cast<int>(m_tabList.size()))
        return;
    Tab* kept = m_tabList[keep].panes[0].Get();
    for (int i = static_cast<int>(m_tabList.size()) - 1; i >= 0; --i)
    {
        const int pos = IndexOf(kept);
        if ((left && i < pos) || (!left && i > pos))
            CloseTab(i);
    }
}

void MainWindow::ReopenClosedTab()
{
    if (m_closedTabs.empty())
        return;
    UniquePidl pidl = std::move(m_closedTabs.back());
    m_closedTabs.pop_back();
    NewTab(pidl.get(), true);
}

void MainWindow::MoveTab(int from, int to)
{
    const int n = static_cast<int>(m_tabList.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    Tab* active = Active();
    TabSlot moving = std::move(m_tabList[from]);
    m_tabList.erase(m_tabList.begin() + from);
    m_tabList.insert(m_tabList.begin() + to, std::move(moving));
    m_active = IndexOf(active);
    m_strip.SetActive(m_active);
    RefreshTabHeaders(); // the strip moved its items; tooltips follow
}

void MainWindow::OpenNewWindow(PCIDLIST_ABSOLUTE pidl)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring target = PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING);
    // A trailing backslash would escape the closing quote ("C:\" -> C:")
    while (!target.empty() && target.back() == L'\\')
        target.pop_back();
    const std::wstring args = L"--window \"" + target + L"\"";
    ShellExecuteW(m_hwnd, nullptr, exe, args.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::OnCrumbNavigate(PCIDLIST_ABSOLUTE pidl, bool newTab)
{
    // Posted: the breadcrumb is still inside its click handler and would see its segments rebuilt
    if (PIDLIST_ABSOLUTE copy = ILCloneFull(pidl);
        copy && !PostMessageW(m_hwnd, WM_APP_NAVIGATE, static_cast<WPARAM>(newTab ? OpenMode::NewTab : OpenMode::Here), reinterpret_cast<LPARAM>(copy)))
        CoTaskMemFree(copy);
}

void MainWindow::OnCrumbEdit()
{
    FocusAddress();
}

void MainWindow::OnCrumbMenu(PCIDLIST_ABSOLUTE pidl, POINT screen)
{
    enum : UINT { MNewTab = 1, MNewWindow, MOtherPane, MSplitVertical, MSplitHorizontal };
    const TabSlot* slot = ActiveSlot();
    if (!slot || !pidl)
        return;
    UniquePidl target = ClonePidl(pidl);
    // Files' NavigationToolbar: 在新标签页/新窗口中打开 always; the pane items only with ShowOpenInNewPane
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MNewTab, Tr(L"在新标签页中打开"));
    AppendMenuW(menu, MF_STRING, MNewWindow, Tr(L"在新窗口中打开"));
    const bool panes = MenuOptions::Shown(MenuOptions::NewPane);
    if (panes && slot->Split())
        AppendMenuW(menu, MF_STRING, MOtherPane, Tr(L"在另一个窗格中打开"));
    else if (panes)
    {
        HMENU pane = CreatePopupMenu();
        AppendMenuW(pane, MF_STRING, MSplitVertical, Tr(L"垂直拆分"));
        AppendMenuW(pane, MF_STRING, MSplitHorizontal, Tr(L"水平拆分"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pane), Tr(L"在新面板中打开"));
    }
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd)
    {
    case MNewTab: NewTab(target.get(), true); break;
    case MNewWindow: OpenNewWindow(target.get()); break;
    case MOtherPane: OpenInOtherPane(target.get()); break;
    case MSplitVertical:
    case MSplitHorizontal:
        if (ActiveSlot() && !ActiveSlot()->Split())
            AddPane(target.get(), cmd == MSplitHorizontal, true); // Files' OpenSecondaryPane(path, arrangement)
        break;
    }
}

namespace
{
// Files' GetSelectedTabInfoAsync for one pane: header text and tooltip (the pane's path)
void PaneInfo(const Tab* tab, std::wstring& header, std::wstring& tip)
{
    PCIDLIST_ABSOLUTE pidl = tab->Location();
    header = !tab->TagId().empty() ? Tr(L"标签：") + tab->Query() : tab->InResults() ? Tr(L"搜索：") + tab->Query() : LocationName(pidl);
    tip = IsPagePidl(pidl) ? LocationName(pidl) : PidlName(pidl, SIGDN_FILESYSPATH);
    if (tip.empty())
        tip = PidlName(pidl, SIGDN_DESKTOPABSOLUTEEDITING);
}

// Files' AncestorHints: "C:\...\parent", "C:\...\grandparent", ... for a file system path
std::vector<std::wstring> AncestorHints(const std::wstring& path)
{
    std::vector<std::wstring> hints;
    if (path.size() < 3 || PathIsRelativeW(path.c_str()))
        return hints;
    const wchar_t* afterRoot = PathSkipRootW(path.c_str());
    if (!afterRoot)
        return hints;
    std::wstring root(path.c_str(), afterRoot);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    if (root.size() >= 2 && root[1] == L':')
        root = std::wstring(1, static_cast<wchar_t>(towupper(root[0]))) + L":";
    const std::wstring prefix = (root.empty() ? std::wstring() : root + L"\\") + L"...";
    std::wstring dir = path;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    for (;;)
    {
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos || slash < root.size())
            break;
        dir.resize(slash);
        const size_t start = dir.find_last_of(L"\\/");
        if (start == std::wstring::npos || dir.size() <= root.size())
            break;
        hints.push_back(prefix + L"\\" + dir.substr(start + 1));
    }
    return hints;
}
}

void MainWindow::UpdateTabLabel(int index)
{
    if (index < 0 || index >= static_cast<int>(m_tabList.size()))
        return;
    // Files: a split tab reads "left | right", tooltip "left path | right path", icon of the left pane
    TabSlot& slot = m_tabList[index];
    PaneInfo(slot.panes[0].Get(), slot.description, slot.tip);
    if (slot.Split())
    {
        std::wstring header, tip;
        PaneInfo(slot.panes[1].Get(), header, tip);
        slot.description += L" | " + header;
        slot.tip += L" | " + tip;
    }
    PCIDLIST_ABSOLUTE where = slot.panes[0]->Location();
    slot.icon = IsSettingsPidl(where) ? TabStrip::kSettingsIcon : where && IsHomePidl(where) ? TabStrip::kHomeIcon : PidlIcon(where);
    RefreshTabHeaders();
}

void MainWindow::RefreshTabHeaders()
{
    // Files' RefreshTabPathHints: tabs with the same name get the nearest parent folder that tells them apart
    const size_t n = m_tabList.size();
    std::vector<std::wstring> headers(n);
    for (size_t i = 0; i < n; ++i)
        headers[i] = m_tabList[i].description;
    std::vector<bool> done(n, false);
    for (size_t i = 0; i < n; ++i)
    {
        if (done[i] || m_tabList[i].description.empty())
            continue;
        std::vector<size_t> group;
        for (size_t j = i; j < n; ++j)
            if (!done[j] && CompareStringOrdinal(m_tabList[j].description.c_str(), -1, m_tabList[i].description.c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                group.push_back(j);
                done[j] = true;
            }
        if (group.size() < 2 || m_tabList[i].description.find(L" | ") != std::wstring::npos)
            continue;
        std::vector<std::vector<std::wstring>> hints;
        for (size_t t : group)
            hints.push_back(AncestorHints(m_tabList[t].tip));
        for (size_t a = 0; a < group.size(); ++a)
            for (size_t d = 0; d < hints[a].size(); ++d)
            {
                bool unique = true;
                for (size_t b = 0; b < group.size() && unique; ++b)
                    if (b != a && d < hints[b].size() && hints[b][d] == hints[a][d])
                        unique = false;
                if (unique)
                {
                    headers[group[a]] = hints[a][d] + L"\\" + m_tabList[group[a]].description;
                    break;
                }
            }
    }
    for (size_t i = 0; i < n; ++i)
    {
        m_strip.Set(static_cast<int>(i), headers[i], m_tabList[i].icon);
        m_strip.SetTip(static_cast<int>(i), m_tabList[i].tip);
    }
}

void MainWindow::UpdateChrome()
{
    Tab* tab = Active();
    PCIDLIST_ABSOLUTE pidl = tab ? tab->Location() : nullptr;
    const bool home = IsPagePidl(pidl); // Home or Settings: shown by name, nothing above them
    std::wstring address = home ? LocationName(pidl) : PidlName(pidl, SIGDN_FILESYSPATH);
    if (address.empty())
        address = PidlName(pidl, SIGDN_DESKTOPABSOLUTEEDITING);
    if (!m_editing)
        SetWindowTextW(m_address, address.c_str());
    m_crumb.SetLocation(pidl);
    if (m_sidebar && tab && !tab->TagId().empty())
        m_sidebar->SetTagLocation(tab->TagId());
    else if (m_sidebar)
        m_sidebar->SetLocation(pidl);
    // Files: the footer's SettingsButton is the sidebar's selected item while the Settings page is open
    m_sideFoot.SetChecked(BtnSettings, pidl && IsSettingsPidl(pidl));
    ++m_navSerial;
    m_barKey = -1;
    m_selKey = -1;
    m_inRecycleBin = tab && tab->IsRecycleBin();
    // The filter box mirrors the active tab (tabs clear their filter when they leave a folder)
    if (m_filterVisible && tab)
    {
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(m_filter)) + 1, L'\0');
        text.resize(GetWindowTextW(m_filter, text.data(), static_cast<int>(text.size())));
        if (text != tab->Filter())
            SetWindowTextW(m_filter, tab->Filter().c_str());
    }
    m_sizeKey = -1;
    UpdateGit(false);

    const std::wstring title = tab && !tab->TagId().empty() ? Tr(L"标签“") + tab->Query() + L"”"
        : tab && tab->InResults() ? L"“" + tab->Query() + Tr(L"”的搜索结果") : LocationName(pidl);
    SetWindowTextW(m_hwnd, title.empty() ? L"FastFiles" : (title + L" - FastFiles").c_str());

    m_nav.SetEnabled(IdBack, tab && tab->CanBack());
    m_nav.SetEnabled(IdForward, tab && tab->CanForward());
    m_nav.SetEnabled(IdUp, pidl && !ILIsEmpty(pidl) && !home);
    UpdateToggleButtons();
    const std::wstring cue = Tr(L"搜索 ") + title;
    SendMessageW(m_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    if (StatusWanted() != m_statusShown)
        Layout(); // Home / Settings came or went
}

void MainWindow::OnTabNavigated(Tab* tab)
{
    if (tab->OnHome())
        EnsureHomeFresh();
    const int index = IndexOf(tab);
    if (index < 0)
        return;
    UpdateTabLabel(index);
    if (index == m_active && tab == Active())
    {
        UpdateChrome();
        UpdateStatus();
    }
}

void MainWindow::OnTabNavigationFailed(Tab* tab)
{
    if (IndexOf(tab) == m_active && tab == Active())
    {
        MessageBeep(MB_ICONWARNING);
        SetStatusText(Tr(L"无法打开此位置"), true);
        UpdateChrome();
    }
}

void MainWindow::OnSidebarOpen(PCIDLIST_ABSOLUTE pidl, OpenMode mode)
{
    if (mode == OpenMode::NewWindow)
    {
        OpenNewWindow(pidl);
        return;
    }
    // Posted, not run inline: the sidebar is still inside its click handler
    if (PIDLIST_ABSOLUTE copy = ILCloneFull(pidl); copy && !PostMessageW(m_hwnd, WM_APP_NAVIGATE, static_cast<WPARAM>(mode), reinterpret_cast<LPARAM>(copy)))
        CoTaskMemFree(copy);
}

void MainWindow::OnTabOpen(Tab* tab, PCIDLIST_ABSOLUTE pidl, OpenMode mode, bool activate)
{
    if (mode == OpenMode::NewWindow)
    {
        OpenNewWindow(pidl);
        return;
    }
    // "Other pane" is relative to the pane whose home page asked
    int pane = 0;
    if (mode == OpenMode::OtherPane && IndexOf(tab, &pane) == m_active && m_active >= 0 && m_tabList[m_active].Split() && pane != m_tabList[m_active].focus)
        SetPaneFocus(pane, false);
    const WPARAM wp = static_cast<WPARAM>(mode) | (activate ? kNavigateActivate : 0);
    if (PIDLIST_ABSOLUTE copy = ILCloneFull(pidl); copy && !PostMessageW(m_hwnd, WM_APP_NAVIGATE, wp, reinterpret_cast<LPARAM>(copy)))
        CoTaskMemFree(copy);
}

void MainWindow::OnSidebarNotice(const std::wstring& text)
{
    SetStatusText(text, true);
}

void MainWindow::OnTabSelect(int index)
{
    ActivateTab(index);
}

void MainWindow::OnTabClose(int index)
{
    CloseTab(index);
}

void MainWindow::OnTabMoved(int from, int to)
{
    MoveTab(from, to);
}

void MainWindow::OnTabNew()
{
    // Home page by default, as in Files; Settings > 常规 can make it the current folder
    Tab* tab = Active();
    NewTab(m_settings.newTab == AppSettings::NewTab::Current && tab ? tab->Location() : nullptr, true);
}

void MainWindow::OnTabMenu(int index, POINT screen)
{
    // Files' TabFlyout (TabBar.xaml): a flat MenuFlyout, no separators, and no standalone "close tab" item (the tab's
    // own close button and Ctrl+W cover that) - 新建标签页, 复制标签页, 将标签页移动到新窗口, 关闭左侧选项卡,
    // 关闭右侧选项卡, 关闭其他选项卡, 重新打开标签页.
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CmdNewTab, Tr(L"新建标签页\tCtrl+T"));
    if (index >= 0)
    {
        const UINT single = m_tabList.size() > 1 ? 0 : MF_GRAYED;
        AppendMenuW(menu, MF_STRING, CmdDuplicate, Tr(L"复制标签页\tCtrl+Shift+K"));
        AppendMenuW(menu, MF_STRING | single, CmdMoveToWindow, Tr(L"将标签页移到新窗口")); // Files: IsEnabled = Items.Count > 1
        AppendMenuW(menu, MF_STRING | (index > 0 ? 0 : MF_GRAYED), CmdCloseLeft, Tr(L"关闭左侧标签页"));
        AppendMenuW(menu, MF_STRING | (index + 1 < static_cast<int>(m_tabList.size()) ? 0 : MF_GRAYED), CmdCloseRight, Tr(L"关闭右侧标签页"));
        AppendMenuW(menu, MF_STRING | single, CmdCloseOthers, Tr(L"关闭其他标签页"));
    }
    AppendMenuW(menu, MF_STRING | (m_closedTabs.empty() ? MF_GRAYED : 0), CmdReopen, Tr(L"重新打开关闭的标签页\tCtrl+Shift+T"));
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd)
    {
    case CmdNewTab: NewTab(DefaultLocation().get(), true); break;
    case CmdDuplicate: NewTab(m_tabList[index].Focused()->Location(), true); break;
    case CmdClose: CloseTab(index); break;
    case CmdCloseOthers: CloseOtherTabs(index); break;
    case CmdCloseLeft: CloseTabsSide(index, true); break;
    case CmdCloseRight: CloseTabsSide(index, false); break;
    case CmdReopen: ReopenClosedTab(); break;
    case CmdMoveToWindow:
        // Files' MoveTabToNewWindow: the whole tab (both panes), then CloseTab
        OpenTabInNewWindow(index, nullptr);
        CloseTab(index);
        break;
    }
}
