#include "MainWindow.h"
#include "Lang.h"

// Tab strip behaviour from Files' TabBar.xaml.cs: wheel over the strip switches tabs, a tab dragged out becomes a new
// window (TabDroppedOutside) or moves to the window whose strip it's dropped on (TabStripDrop), files dropped on a tab
// go into its folder, folders dropped on "+" open as new tabs, and the "标签操作菜单" button. Plus Files' compact
// overlay (精简模式: ToggleCompactOverlay F12, Enter Ctrl+Alt+Up, Exit Ctrl+Alt+Down): a 400x350 always-on-top window.

void MainWindow::OnTabWheel(int delta)
{
    // Files: TabView_PointerWheelChanged; wheel up selects the next tab unless 反转标签页滚轮切换方向
    const bool next = (delta > 0) == !m_settings.reverseTabWheel;
    const int index = m_active + (next ? 1 : -1);
    if (index >= 0 && index < static_cast<int>(m_tabList.size()))
        ActivateTab(index);
}

// Process id (so a tab dropped back on its own strip is a reorder), then the whole tab
std::vector<BYTE> MainWindow::TabPayload(int index)
{
    std::vector<BYTE> out;
    const std::vector<BYTE> tab = EncodeTab(SlotState(index));
    if (tab.empty())
        return out;
    const DWORD pid = GetCurrentProcessId();
    out.resize(sizeof(pid));
    memcpy(out.data(), &pid, sizeof(pid));
    out.insert(out.end(), tab.begin(), tab.end());
    return out;
}

void MainWindow::OpenTabInNewWindow(int index, const POINT* drop)
{
    const std::vector<BYTE> tab = EncodeTab(SlotState(index));
    if (tab.empty())
        return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = L"--window";
    if (drop)
        args += L" --pos " + std::to_wstring(drop->x) + L"," + std::to_wstring(drop->y);
    args += L" --tab " + BytesToHex(tab);
    ShellExecuteW(m_hwnd, nullptr, exe, args.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::OnTabTornOut(int index, POINT screen)
{
    if (index < 0 || index >= static_cast<int>(m_tabList.size()) || m_tabList.size() < 2)
        return;
    // Files: Items.Remove(tab), then OpenTabInNewWindowAsync(tab, droppedPoint.X, droppedPoint.Y); not a "closed" tab
    OpenTabInNewWindow(index, &screen);
    RemoveTab(index, false);
}

void MainWindow::OnTabMovedAway(int index)
{
    // Files' TabDragCompleted: the other window handled the drop, so CloseTab (which records it as closed).
    // The last tab leaving closes this window.
    CloseTab(index);
}

void MainWindow::OnTabArrived(const std::vector<BYTE>& payload, int index)
{
    SessionData::TabState state;
    if (payload.size() <= sizeof(DWORD) || !DecodeTab(payload.data() + sizeof(DWORD), payload.size() - sizeof(DWORD), state))
        return; // malformed
    // Files' TabStripDrop: insert at the drop index and select it
    ActivateTab(InsertSlot(index, state));
    SetForegroundWindow(m_hwnd);
}

PCIDLIST_ABSOLUTE MainWindow::TabFolder(int index)
{
    if (index < 0 || index >= static_cast<int>(m_tabList.size()))
        return nullptr;
    Tab* tab = m_tabList[index].Focused();
    // Home and search/tag results have no single folder to drop into
    if (tab->OnHome() || tab->InResults() || PidlName(tab->Location(), SIGDN_FILESYSPATH).empty())
        return nullptr;
    return tab->Location();
}

void MainWindow::OnFoldersToNewTabs(const std::vector<UniquePidl>& folders)
{
    // Files: TabBarAddNewTabButton_Drop opens each folder with OpenPathInNewTab(path, focus: true)
    for (const auto& folder : folders)
        NewTab(folder.get(), true);
}

void MainWindow::OnTabActions(const RECT& button)
{
    // Files' tab actions flyout (TabBar.xaml): items that can't run aren't loaded (x:Load), not greyed
    enum : UINT { ANewWindow = 1, AEnterCompact, AExitCompact, ASplitV, ASplitH, AArrangeV, AArrangeH, AClosePane };
    const TabSlot* slot = ActiveSlot();
    const bool split = slot && slot->Split();
    auto label = [this](int id, const wchar_t* text)
    {
        // KeyboardAcceleratorTextOverride = HotKeyText: the user's current binding
        for (const auto& def : Commands())
            if (def.id == id)
            {
                const auto chords = EffectiveChords(def);
                return chords.empty() ? std::wstring(text) : std::wstring(text) + L"\t" + FormatShortcut(chords.front());
            }
        return std::wstring(text);
    };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ANewWindow, label(ActNewWindow, Tr(L"新建窗口")).c_str());
    // Enter/Exit compact overlay carry no accelerator text in Files' flyout
    AppendMenuW(menu, MF_STRING, m_compact ? AExitCompact : AEnterCompact, m_compact ? Tr(L"退出精简模式") : Tr(L"进入精简模式"));
    if (!split)
    {
        HMENU splitMenu = CreatePopupMenu();
        AppendMenuW(splitMenu, MF_STRING, ASplitV, label(ActSplitVertical, Tr(L"垂直拆分")).c_str());
        AppendMenuW(splitMenu, MF_STRING, ASplitH, label(ActSplitHorizontal, Tr(L"水平拆分")).c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(splitMenu), Tr(L"拆分窗格"));
    }
    else
    {
        HMENU arrange = CreatePopupMenu();
        AppendMenuW(arrange, MF_STRING | (!slot->horizontal ? MF_CHECKED : 0), AArrangeV, Tr(L"垂直排列"));
        AppendMenuW(arrange, MF_STRING | (slot->horizontal ? MF_CHECKED : 0), AArrangeH, Tr(L"水平排列"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(arrange), Tr(L"排列面板"));
        AppendMenuW(menu, MF_STRING, AClosePane, label(ActClosePane, Tr(L"关闭窗格")).c_str());
    }
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom, m_hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd)
    {
    case ANewWindow: RunAction(ActNewWindow); break;
    case AEnterCompact: SetCompactOverlay(true); break;
    case AExitCompact: SetCompactOverlay(false); break;
    case ASplitV: RunAction(ActSplitVertical); break;
    case ASplitH: RunAction(ActSplitHorizontal); break;
    case AArrangeV:
    case AArrangeH:
        if (TabSlot* s = ActiveSlot(); s && s->Split())
        {
            s->horizontal = cmd == AArrangeH;
            PlacePanes();
        }
        break;
    case AClosePane: RunAction(ActClosePane); break;
    }
}

void MainWindow::SetCompactOverlay(bool on)
{
    if (on == m_compact)
        return;
    if (on)
    {
        // Files: AppWindow.SetPresenter(CompactOverlay) + Resize(400, 350); the overlay can't be maximized
        if (m_fullScreen)
            ToggleFullScreen();
        GetWindowPlacement(m_hwnd, &m_compactRestore);
        if (IsZoomed(m_hwnd))
            ShowWindow(m_hwnd, SW_RESTORE);
        m_compact = true;
        RECT rc{};
        GetWindowRect(m_hwnd, &rc);
        // A tracked popup menu (the tab actions flyout, owned by m_hwnd) leaves the topmost band's internal
        // z-order bookkeeping for m_hwnd stuck once it closes: a single HWND_TOPMOST SetWindowPos after that
        // reports success but the window never actually re-enters the topmost band (WS_EX_TOPMOST doesn't stick,
        // even checked immediately). Demoting to HWND_NOTOPMOST first resets that state, so the following
        // HWND_TOPMOST then takes effect reliably; a plain no-menu toggle is unaffected by the extra step.
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, rc.left, rc.top, 400, 350, SWP_NOACTIVATE);
        SetWindowPos(m_hwnd, HWND_TOPMOST, rc.left, rc.top, 400, 350, SWP_NOACTIVATE);
    }
    else
    {
        // Files: SetPresenter(Overlapped) brings back the size and state from before
        m_compact = false;
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPlacement(m_hwnd, &m_compactRestore);
    }
    m_strip.SetMaximized(IsZoomed(m_hwnd) != FALSE);
}

void MainWindow::SetStartPosition(POINT screen)
{
    m_hasStartPosition = true;
    m_startPosition = screen;
}

void MainWindow::SetStartTab(SessionData::TabState tab)
{
    m_startTab = std::move(tab);
}
