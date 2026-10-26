#include "Tab.h"
#include "Sandbox.h"

// FastFiles' own file list in a tab (FileList.h): file system folders in Files' details, list, cards and grid layouts.
// Virtual folders, zip folders and network folders the adaptive layout decides later stay the shell's view. While
// the list is up the shell's browser is gone (a hidden one would come back showing its old folder); Tab's commands
// (TabCommands.cpp, TabView.cpp) go to the list. FASTFILES_OWNLIST=0 (tests) keeps the shell's view everywhere.

bool Tab::OwnList()
{
    static const bool on = []
    {
        wchar_t value[8]{};
        return !(GetEnvironmentVariableW(L"FASTFILES_OWNLIST", value, ARRAYSIZE(value)) > 0 && value[0] == L'0');
    }();
    return on;
}

FileList::Mode Tab::ListMode(ViewLayout layout)
{
    switch (FilesLayout(static_cast<int>(layout)))
    {
    case ViewLayout::List: return FileList::Mode::List;
    case ViewLayout::Tiles: return FileList::Mode::Cards;
    case ViewLayout::LargeIcons: return FileList::Mode::Grid;
    default: return FileList::Mode::Details;
    }
}

int Tab::ListLayout(const LayoutDecision& d) const
{
    return static_cast<int>(FilesLayout(d.layout >= 0 ? d.layout : m_prefs ? m_prefs->defaultLayout : 0));
}

// Files' details, list, cards and grid of a file system folder
bool Tab::WantsList(PCIDLIST_ABSOLUTE pidl, const LayoutDecision& d) const
{
    if (!OwnList() || !m_prefs || !m_prefs->theme || !pidl || ILIsEmpty(pidl) || IsPagePidl(pidl) || d.columns || d.adaptiveLater)
        return false;
    const std::wstring path = PidlName(pidl, SIGDN_FILESYSPATH);
    const DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY); // (zip files and what's inside them: the shell's)
}

bool Tab::EnterList(PCIDLIST_ABSOLUTE pidlIn, ViewLayout layout)
{
    const UniquePidl target = ClonePidl(pidlIn); // may point at m_current, which Complete replaces
    if (!m_host || !m_prefs || !m_prefs->theme)
        return false;
    if (!m_list)
    {
        m_list = std::make_unique<FileList>(this, m_prefs->theme);
        if (!m_list->Create(m_host))
        {
            m_list.reset();
            return false;
        }
        m_list->SetHiddenColumns(m_prefs->hiddenColumns); // the default columns (Files' Show*Column)
    }
    m_list->SetMode(ListMode(layout));
    m_list->SetSortState(SortFor(target.get())); // the folder's sort / grouping before it's listed
    if (!m_list->Open(target.get(), L""))
        return false;
    const bool hadFocus = HasFocus();
    if (m_onHome)
    {
        m_onHome = false;
        for (HWND page : { m_home ? m_home->Hwnd() : nullptr, m_settings ? m_settings->Hwnd() : nullptr })
            if (page)
                ShowWindow(page, SW_HIDE);
    }
    // History, and what to select: the --select item or the folder we came up from (Files' ScrollToPreviousFolder...)
    Complete(target.get());
    m_currentColumns = false;
    if (m_pendingSelect && !m_pendingUnselect)
    {
        const UniquePidl full(ILCombine(m_current.get(), reinterpret_cast<PCUIDLIST_RELATIVE>(m_pendingSelect.get())));
        if (const std::wstring path = PidlName(full.get(), SIGDN_FILESYSPATH); !path.empty())
            m_list->SelectWhenListed(PathFindFileNameW(path.c_str()));
    }
    m_pendingSelect.reset();
    m_pendingUnselect = false;
    m_applyLayout = -1;
    m_pendingTrack = m_pendingAdaptiveLater = false;
    m_leavingColumns = false;
    m_listShown = true;
    SetBrowserShown(false);
    RECT rc{};
    GetClientRect(m_host, &rc);
    SetWindowPos(m_list->Hwnd(), HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    if (m_columns)
        DestroyColumns();
    DropBrowser();
    if (hadFocus)
        SetFocus(m_list->List());
    m_events->OnTabNavigated(this);
    return true;
}

// A layout picked for a folder the shell's view (or the column view) shows: the own list takes over (same folder, no
// history entry); already in it, just its mode
bool Tab::SwitchToList(ViewLayout layout)
{
    if (m_results || m_onHome || !m_current)
        return false;
    if (InList())
    {
        m_list->SetMode(ListMode(layout));
        return true;
    }
    LayoutDecision d;
    d.layout = static_cast<int>(layout);
    const UniquePidl here = ClonePidl(m_current.get());
    return WantsList(here.get(), d) && EnterList(here.get(), layout);
}

void Tab::HideList()
{
    if (!m_listShown)
        return;
    m_listShown = false;
    if (m_list)
        ShowWindow(m_list->Hwnd(), SW_HIDE);
}

void Tab::DropBrowser()
{
    if (!m_browser || m_results)
        return;
    if (m_cookie)
        m_browser->Unadvise(m_cookie);
    m_cookie = 0;
    ComPtr<IFolderFilterSite> site;
    if (m_filterInstalled && SUCCEEDED(m_browser.As(&site)))
        site->SetFilter(nullptr);
    m_filterInstalled = false;
    IUnknown_SetSite(m_browser.Get(), nullptr);
    m_browser->Destroy();
    m_browser.Reset();
    m_darkApplied = false;
}

void Tab::OnListOpen(PCIDLIST_ABSOLUTE folder)
{
    // Posted: this runs inside the list's notification (navigating replaces its rows)
    if (PIDLIST_ABSOLUTE copy = ILCloneFull(folder);
        copy && !PostMessageW(m_host, kMsgOpen, m_prefs && m_prefs->openFoldersNewTab ? 1 : 0, reinterpret_cast<LPARAM>(copy)))
        CoTaskMemFree(copy);
}

void Tab::OnListUp()
{
    PostMessageW(m_host, kMsgUp, 0, 0);
}

// The list's right-click menu: the shell's own for the selection (or the folder's background), with the window's items
// around it (MainWindow::OnTabViewMenu), as TabMenu.cpp does for the shell's view
void Tab::OnListMenu(HWND list, POINT screen, bool background)
{
    // The folder's list, or a column's (which becomes the active one: the menu acts on it)
    const auto owner = [this, list]() -> FileList*
    {
        if (m_list && (m_list->List() == list || m_list->Hwnd() == list))
            return m_list.get();
        const int column = m_columns ? m_columns->IndexOfList(list) : -1;
        return column >= 0 ? m_columns->ListAt(column) : nullptr;
    };
    FileList* from = owner();
    if (m_viewMenu || !from)
        return;
    if (const int column = m_columns ? m_columns->IndexOfList(list) : -1; column >= 0)
        m_columns->Activate(column, false);
    ComPtr<IContextMenu> shell = from->MenuFor(background);
    HMENU menu = shell ? CreatePopupMenu() : nullptr;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const UINT flags = CMF_NORMAL | CMF_EXPLORE | (background ? 0 : CMF_CANRENAME | CMF_ITEMMENU) | (shift ? CMF_EXTENDEDVERBS : 0);
    if (!menu || FAILED(shell->QueryContextMenu(menu, 0, TabViewMenu::kShellFirst, TabViewMenu::kShellLast, flags)))
    {
        if (menu)
            DestroyMenu(menu);
        return;
    }
    TabViewMenu info{ menu, background, shell.Get() };
    info.ownList = true;
    m_events->OnTabViewMenu(this, info);
    ComPtr<Tab> keep(this); // the menu loop pumps messages
    m_viewMenu = shell;
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_host, nullptr);
    m_viewMenu.Reset();
    from = m_host && IsWindow(list) ? owner() : nullptr; // (the menu loop may have closed it)
    const bool alive = from != nullptr;
    const bool fromShell = cmd >= TabViewMenu::kShellFirst && cmd <= TabViewMenu::kShellLast;
    wchar_t verb[64]{};
    if (alive && fromShell &&
        FAILED(shell->GetCommandString(cmd - TabViewMenu::kShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
        verb[0] = L'\0';
    bool handled = false;
    // 打开: a folder opens here (files: their default verb; none in test mode)
    if (alive && !background && (_wcsicmp(verb, L"open") == 0 || _wcsicmp(verb, L"explore") == 0))
        handled = from->OpenSelection();
    if (alive && !handled && _wcsicmp(verb, L"rename") == 0)
        handled = from->BeginRename(); // Files' rename box (the shell's needs a folder view to edit in)
    // 粘贴 / 删除 / 共享: Files runs them itself (status center, its own dialogs, its share sheet)
    if (alive && !handled && (_wcsicmp(verb, L"paste") == 0 || _wcsicmp(verb, L"delete") == 0 || _wcsicmp(verb, L"Windows.ModernShare") == 0))
        handled = m_events->OnTabVerb(this, verb, background);
    if (alive && !handled && fromShell && Sandbox::VerbAllowed(verb))
    {
        CMINVOKECOMMANDINFOEX invoke{ sizeof(invoke) };
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | (GetKeyState(VK_CONTROL) < 0 ? CMIC_MASK_CONTROL_DOWN : 0) |
            (shift ? CMIC_MASK_SHIFT_DOWN : 0);
        invoke.hwnd = m_parent;
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - TabViewMenu::kShellFirst);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - TabViewMenu::kShellFirst);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screen;
        shell->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
    }
    // After InvokeCommand, as the shell's own hosts do: some handlers (WinRAR's, 7-Zip's) still look at the menu
    // they filled in QueryContextMenu while they run (Files never disposes it before invoking either)
    DestroyMenu(menu);
    m_events->OnTabViewCommand(this, alive && cmd >= TabViewMenu::kAppFirst && cmd <= TabViewMenu::kAppLast ? cmd : 0);
}
