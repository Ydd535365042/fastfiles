#include "Tab.h"
#include "Sandbox.h"

namespace
{
constexpr wchar_t kHostClass[] = L"FastFiles.TabHost";
constexpr size_t kMaxHistory = 100;
}

Tab::Tab(HWND parent, TabEvents* events, PCIDLIST_ABSOLUTE location, const ViewPrefs* prefs)
    : m_parent(parent), m_events(events), m_current(ClonePidl(location)), m_prefs(prefs)
{
}

IFACEMETHODIMP Tab::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = { QITABENT(Tab, IExplorerBrowserEvents), QITABENT(Tab, IFolderFilter), QITABENT(Tab, IServiceProvider),
        QITABENT(Tab, ICommDlgBrowser), QITABENT(Tab, ICommDlgBrowser2), {} };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP Tab::QueryService(REFGUID service, REFIID riid, void** ppv)
{
    *ppv = nullptr;
    if (IsEqualGUID(service, SID_SExplorerBrowserFrame))
        return QueryInterface(riid, ppv);
    return E_NOINTERFACE;
}

bool FolderTarget(IShellItem* item, UniquePidl& target)
{
    target.reset();
    SFGAOF attrs = 0;
    if (!item || FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_LINK, &attrs)))
        return false;
    if (attrs & SFGAO_LINK)
    {
        // A shortcut to a folder opens that folder; its stored target only (no resolving, no UI)
        ComPtr<IShellLinkW> link;
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&link))) || FAILED(link->GetIDList(&raw)) || !raw)
            return false;
        target.reset(raw);
        ComPtr<IShellItem> resolved;
        attrs = 0;
        if (FAILED(SHCreateItemFromIDList(target.get(), IID_PPV_ARGS(&resolved))) || FAILED(resolved->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs)))
        {
            target.reset();
            return false;
        }
    }
    // Zip files report SFGAO_FOLDER too, with SFGAO_STREAM: those keep their own open handler
    if (!(attrs & SFGAO_FOLDER) || (attrs & SFGAO_STREAM))
    {
        target.reset();
        return false;
    }
    if (!target)
    {
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(SHGetIDListFromObject(item, &raw)))
            return false;
        target.reset(raw);
    }
    return true;
}

IFACEMETHODIMP Tab::OnDefaultCommand(IShellView* shellView)
{
    ComPtr<IFolderView2> view;
    ComPtr<IShellItemArray> selection;
    ComPtr<IShellItem> item;
    DWORD count = 0;
    UniquePidl target;
    if (!m_host || !shellView || FAILED(shellView->QueryInterface(IID_PPV_ARGS(&view))) || FAILED(view->GetSelection(FALSE, &selection)) ||
        FAILED(selection->GetCount(&count)) || count != 1 || FAILED(selection->GetItemAt(0, &item)) || !FolderTarget(item.Get(), target))
        return Sandbox::Active() ? S_OK : S_FALSE; // several items, files: the view does its default (test mode: no app starts)
    // Posted: this runs inside the view, which navigating (or leaving search results) tears down
    if (!PostMessageW(m_host, kMsgOpen, m_prefs && m_prefs->openFoldersNewTab ? 1 : 0, reinterpret_cast<LPARAM>(target.get())))
        return S_FALSE;
    target.release();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) Tab::AddRef() { return InterlockedIncrement(&m_ref); }

IFACEMETHODIMP_(ULONG) Tab::Release()
{
    const ULONG ref = InterlockedDecrement(&m_ref);
    if (ref == 0)
        delete this;
    return ref;
}

LRESULT CALLBACK Tab::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == kThemeTimer)
    {
        KillTimer(hwnd, wp);
        if (auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            self->ApplyTheme();
        return 0;
    }
    if (msg == WM_TIMER && wp == kSearchTimer)
    {
        if (auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            self->PumpSearch();
        else
            KillTimer(hwnd, wp);
        return 0;
    }
    if (msg == WM_TIMER && (wp == kRenameTimer || wp == kSelectTimer))
    {
        if (auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            wp == kRenameTimer ? self->TryPendingRename() : self->TryPendingSelect();
        else
            KillTimer(hwnd, wp);
        return 0;
    }
    if (msg == WM_TIMER && wp == kAdaptiveTimer)
    {
        if (auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            self->CheckNetworkAdaptive();
        else
            KillTimer(hwnd, wp);
        return 0;
    }
    if (msg == WM_SIZE)
    {
        auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        RECT rc{ 0, 0, LOWORD(lp), HIWORD(lp) };
        if (self && self->m_browser)
            self->m_browser->SetRect(nullptr, rc);
        if (self && self->m_columns)
            self->m_columns->Move(rc);
        if (self && self->m_list)
            self->m_list->Move(rc);
        for (HWND page : { self && self->m_home ? self->m_home->Hwnd() : nullptr, self && self->m_settings ? self->m_settings->Hwnd() : nullptr })
            if (page)
                SetWindowPos(page, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
    {
        // Seen only between views (the browser covers it otherwise): the palette, not the class's white
        auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->m_prefs && self->m_prefs->theme)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillSolid(reinterpret_cast<HDC>(wp), rc, self->m_prefs->theme->content);
            return 1;
        }
    }
    if (msg == kMsgLeaveResults)
    {
        UniquePidl pidl(reinterpret_cast<ITEMIDLIST_ABSOLUTE*>(lp));
        if (auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); self && pidl)
            self->Navigate(pidl.get()); // Go ends the results first
        return 0;
    }
    if (msg == kMsgOpen)
    {
        UniquePidl pidl(reinterpret_cast<ITEMIDLIST_ABSOLUTE*>(lp));
        auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && pidl && wp == 1)
            self->m_events->OnTabOpen(self, pidl.get(), OpenMode::NewTab, true); // Files' OpenPathInNewTab(path, true)
        else if (self && pidl && wp == 2)
        {
            self->m_pending = Pending::None;
            self->Go(pidl.get(), ModeHint::Normal); // a folder the column view can't list (OnColumnCantShow)
        }
        else if (self && pidl)
            self->Navigate(pidl.get());
        return 0;
    }
    if (msg == kMsgUp)
    {
        if (auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            self->Up();
        return 0;
    }
    if (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR)
    {
        // Owner-drawn shell submenus (发送到, 打开方式, 新建) of the own list's menu, which the host owns
        auto* self = reinterpret_cast<Tab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->m_viewMenu && (msg != WM_DRAWITEM || reinterpret_cast<DRAWITEMSTRUCT*>(lp)->CtlType == ODT_MENU) &&
            (msg != WM_MEASUREITEM || reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->CtlType == ODT_MENU))
        {
            LRESULT result = 0;
            ComPtr<IContextMenu3> menu3;
            ComPtr<IContextMenu2> menu2;
            if (SUCCEEDED(self->m_viewMenu.As(&menu3)) ? SUCCEEDED(menu3->HandleMenuMsg2(msg, wp, lp, &result))
                : SUCCEEDED(self->m_viewMenu.As(&menu2)) && SUCCEEDED(menu2->HandleMenuMsg(msg, wp, lp)))
                return msg == WM_MENUCHAR ? result : (msg == WM_INITMENUPOPUP ? 0 : TRUE);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Tab::CreateHost(const RECT& rc)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kHostClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered)
        return false;
    m_host = CreateWindowExW(WS_EX_CONTROLPARENT, kHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, m_parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_host)
        return false;
    SetWindowLongPtrW(m_host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

// The shell browser is only created once the tab leaves Home, so a tab that never does costs no shell view
bool Tab::CreateBrowser()
{
    if (m_browser)
        return true;
    if (!NewBrowser(m_browser, m_cookie))
        return false;
    if (m_onHome || m_columns || m_listShown)
        SetBrowserShown(false); // stays under the home page / the columns / the own list until its first folder has loaded
    return true;
}

// A browser filling the host, events coming to this tab (the folder view, or a search results view)
bool Tab::NewBrowser(ComPtr<IExplorerBrowser>& browser, DWORD& cookie)
{
    browser.Reset();
    cookie = 0;
    if (!m_host || FAILED(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser))))
        return false;

    ComPtr<IFolderViewOptions> fvo;
    if (SUCCEEDED(browser.As(&fvo)))
        fvo->SetFolderViewOptions(FVO_VISTALAYOUT, FVO_VISTALAYOUT);

    RECT client{};
    GetClientRect(m_host, &client);
    // Default layout from settings: used by folders that don't have a remembered view yet (adaptive/columns: details)
    static const FOLDERVIEWMODE modes[] = { FVM_DETAILS, FVM_LIST, FVM_TILE, FVM_CONTENT, FVM_SMALLICON, FVM_ICON, FVM_ICON, FVM_ICON };
    const int layout = m_prefs ? ShellLayoutIndex(m_prefs->defaultLayout) : 0;
    FOLDERSETTINGS fs{ static_cast<UINT>(modes[layout]), FWF_NOWEBVIEW | FWF_FULLROWSELECT };
    // Must precede Initialize: the control's WS_BORDER is decided when its window is created
    // (Test mode: views aren't remembered in Explorer's Shell Bags in the registry)
    browser->SetOptions(Sandbox::Active() ? EBO_NOBORDER | EBO_NOPERSISTVIEWSTATE : EBO_NOBORDER);
    // Site first: the view looks for ICommDlgBrowser when it's created. Holds a reference: cleared before Destroy.
    IUnknown_SetSite(browser.Get(), static_cast<IServiceProvider*>(this));
    if (FAILED(browser->Initialize(m_host, &client, &fs)))
    {
        IUnknown_SetSite(browser.Get(), nullptr);
        browser.Reset();
        return false;
    }
    browser->SetPropertyBag(L"FastFiles");
    ThemeBrowserHost(browser.Get(), m_prefs);
    browser->Advise(static_cast<IExplorerBrowserEvents*>(this), &cookie);
    return true;
}

void Tab::Show(const RECT& rc)
{
    const bool first = !m_host;
    if (first && !CreateHost(rc))
        return;
    Move(rc);
    ShowWindow(m_host, SW_SHOW);
    if (!first)
        return;
    if (IsPagePidl(m_current.get()))
        ShowHome();
    else if (m_current)
        OpenLocation(m_current.get(), m_currentColumns ? ModeHint::Columns : ModeHint::None); // restored tabs too
}

void Tab::Go(PCIDLIST_ABSOLUTE pidl, ModeHint hint)
{
    // Copy first: pidl may point into the history lists that Complete reshuffles
    const UniquePidl target = ClonePidl(pidl);
    const bool overColumns = m_results && m_resultsOverColumns;
    EndResults();
    if (IsPagePidl(target.get()))
    {
        EnterHome(target.get());
        return;
    }
    if (!m_host)
    {
        m_onHome = false;
        Complete(target.get()); // not shown yet: just record it, the first Show opens it
        return;
    }
    // Inside the column view already shown (Files' SetSelectedPathOrNavigate): no new page, no history
    if (hint == ModeHint::None && !overColumns && AdjustColumns(target.get()))
        return;
    CheckLeave();
    OpenLocation(target.get(), hint); // the home page / the columns stay up until this completes
}

void Tab::EnterHome(PCIDLIST_ABSOLUTE pidl)
{
    // Copy first: pidl may point into the history lists that Complete reshuffles
    const UniquePidl target = ClonePidl(pidl);
    CheckLeave();
    const bool hadFocus = HasFocus();
    Complete(target.get());
    m_currentColumns = false;
    if (m_host)
    {
        ShowHome();
        if (m_columns)
        {
            DestroyColumns();
            if (hadFocus && PageWindow())
                SetFocus(PageWindow());
        }
    }
    m_events->OnTabNavigated(this);
}

void Tab::ShowHome()
{
    const bool settings = IsSettingsPidl(m_current.get());
    HWND page = nullptr;
    if (settings)
    {
        if (!m_settings && m_prefs->settings)
        {
            m_settings = std::make_unique<SettingsView>(m_prefs->settings);
            if (!m_settings->Create(m_host, m_prefs->theme, m_settingsPage))
                m_settings.reset();
        }
        page = m_settings ? m_settings->Hwnd() : nullptr;
    }
    else
    {
        if (!m_home)
        {
            m_home = std::make_unique<HomeView>(this);
            if (!m_home->Create(m_host, m_prefs->theme))
                m_home.reset();
            else
                m_home->SetData(m_prefs->home, m_prefs->homeCollapsed, m_prefs->homeHidden);
        }
        page = m_home ? m_home->Hwnd() : nullptr;
    }
    if (!page)
        return;
    const bool hadFocus = HasFocus();
    RECT rc{};
    GetClientRect(m_host, &rc);
    SetWindowPos(page, HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // Only one of the two pages is ever up
    if (HWND other = settings ? (m_home ? m_home->Hwnd() : nullptr) : (m_settings ? m_settings->Hwnd() : nullptr))
        ShowWindow(other, SW_HIDE);
    m_onHome = true;
    SetBrowserShown(false);
    HideList();
    if (hadFocus)
        SetFocus(page);
}

void Tab::HideHome()
{
    if (!m_onHome)
        return;
    const bool hadFocus = HasFocus();
    m_onHome = false;
    SetBrowserShown(true);
    for (HWND page : { m_home ? m_home->Hwnd() : nullptr, m_settings ? m_settings->Hwnd() : nullptr })
        if (page)
            ShowWindow(page, SW_HIDE);
    if (hadFocus)
        Focus();
}

HWND Tab::PageWindow() const
{
    if (IsSettingsPidl(m_current.get()))
        return m_settings ? m_settings->Hwnd() : nullptr;
    return m_home ? m_home->Hwnd() : nullptr;
}

void Tab::ShowSettingsPage(int page)
{
    m_settingsPage = page;
    if (m_settings)
        m_settings->ShowPage(page);
    if (!OnSettings())
        Navigate(SettingsPidl());
}

void Tab::RebuildSettings()
{
    if (m_settings)
        m_settings->Rebuild();
}

void Tab::SetBrowserShown(bool shown)
{
    // The explorer browser's own top-level child window(s) under the host; the pages and the columns are siblings
    const HWND home = m_home ? m_home->Hwnd() : nullptr;
    const HWND settings = m_settings ? m_settings->Hwnd() : nullptr;
    const HWND columns = m_columns ? m_columns->Hwnd() : nullptr;
    const HWND list = m_list ? m_list->Hwnd() : nullptr;
    for (HWND child = GetWindow(m_host, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        if (child != home && child != settings && child != columns && child != list)
            ShowWindow(child, shown ? SW_SHOWNA : SW_HIDE);
}

void Tab::ApplyHome()
{
    if (m_home)
        m_home->SetData(m_prefs->home, m_prefs->homeCollapsed, m_prefs->homeHidden);
}

void Tab::RelayoutHome()
{
    if (m_home)
        m_home->Relayout();
    if (m_settings)
        m_settings->Relayout();
}

void Tab::OnHomeOpen(PCIDLIST_ABSOLUTE pidl, OpenMode mode)
{
    if (mode != OpenMode::Here)
    {
        m_events->OnTabOpen(this, pidl, mode);
        return;
    }
    // Posted: the home page is still inside its click handler, and navigating hides it
    if (PIDLIST_ABSOLUTE copy = ILCloneFull(pidl); copy && !PostMessageW(m_host, kMsgOpen, 0, reinterpret_cast<LPARAM>(copy)))
        CoTaskMemFree(copy);
}

void Tab::OnHomeReveal(PCIDLIST_ABSOLUTE item)
{
    UniquePidl parent = ClonePidl(item);
    if (!parent || ILIsEmpty(parent.get()))
        return;
    SelectAfterNavigate(ILFindLastID(item));
    ILRemoveLastID(parent.get());
    OnHomeOpen(parent.get(), OpenMode::Here);
}

void Tab::OnHomeAction(HomeAction action, const std::wstring& key, PCIDLIST_ABSOLUTE pidl, int section)
{
    m_events->OnTabHomeAction(this, action, key, pidl, section);
}

void Tab::Hide()
{
    if (m_host)
        ShowWindow(m_host, SW_HIDE);
}

void Tab::Move(const RECT& rc)
{
    if (m_host)
        SetWindowPos(m_host, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Tab::Close()
{
    CheckLeave();
    EndResults();
    m_columns.reset();
    m_list.reset();
    m_listShown = false;
    if (m_browser)
    {
        if (m_cookie)
            m_browser->Unadvise(m_cookie);
        m_cookie = 0;
        // The filter site holds a reference to us; drop it or the tab never frees
        ComPtr<IFolderFilterSite> site;
        if (m_filterInstalled && SUCCEEDED(m_browser.As(&site)))
            site->SetFilter(nullptr);
        m_filterInstalled = false;
        IUnknown_SetSite(m_browser.Get(), nullptr);
        m_browser->Destroy();
        m_browser.Reset();
    }
    m_home.reset();
    m_settings.reset();
    if (m_host)
    {
        SetWindowLongPtrW(m_host, GWLP_USERDATA, 0);
        DestroyWindow(m_host);
        m_host = nullptr;
    }
}

void Tab::Focus()
{
    if (m_onHome)
    {
        if (HWND page = PageWindow())
            SetFocus(page);
        return;
    }
    if (FileList* list = List())
    {
        SetFocus(list->List());
        return;
    }
    ComPtr<IInputObject> input;
    if (IExplorerBrowser* browser = ActiveBrowser(); browser && SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&input))))
        input->UIActivateIO(TRUE, nullptr);
}

bool Tab::HasFocus() const
{
    const HWND focus = GetFocus();
    return m_host && focus && (focus == m_host || IsChild(m_host, focus));
}

bool Tab::TranslateAccelerator(MSG* msg)
{
    if (m_onHome || List())
        return false; // (the own lists take their keys as they come; the column view's Left / Right are its lists')
    ComPtr<IInputObject> input;
    IExplorerBrowser* browser = ActiveBrowser();
    return browser && SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&input))) && input->TranslateAcceleratorIO(msg) == S_OK;
}

void Tab::Navigate(PCIDLIST_ABSOLUTE pidl)
{
    m_pending = Pending::None;
    Go(pidl);
}

void Tab::Back()
{
    GoHistory(true, 1);
}

void Tab::Forward()
{
    GoHistory(false, 1);
}

void Tab::Up()
{
    if (m_results)
    {
        // Up from results: back to the folder that was searched (Explorer does the same)
        m_pending = Pending::None;
        UniquePidl scope = ClonePidl(m_current.get());
        Go(scope.get());
        return;
    }
    if (InColumns())
    {
        // Files' ColumnsLayoutPage.NavigateUp: the last column closes; with one left, its parent (Home from a drive root)
        m_pending = Pending::None;
        if (m_columns->Count() > 1)
        {
            m_columns->DismissLast();
            return;
        }
        const std::wstring path = PidlName(m_current.get(), SIGDN_FILESYSPATH);
        if (path.empty() || (path.size() < MAX_PATH && PathIsRootW(path.c_str())))
        {
            Go(HomePidl());
            return;
        }
        UniquePidl parent = ClonePidl(m_current.get());
        ILRemoveLastID(parent.get());
        Go(parent.get());
        return;
    }
    if (InList() && m_current && !ILIsEmpty(m_current.get()))
    {
        // The shell's SBSP_PARENT: the folder above (This PC above a drive)
        m_pending = Pending::None;
        UniquePidl parent = ClonePidl(m_current.get());
        ILRemoveLastID(parent.get());
        Go(parent.get());
        return;
    }
    if (m_onHome || !m_browser || !m_current || ILIsEmpty(m_current.get()))
        return;
    m_pending = Pending::None;
    m_browser->BrowseToIDList(nullptr, SBSP_PARENT);
}

void Tab::Refresh()
{
    if (OnSettings())
    {
        RebuildSettings();
        return;
    }
    if (m_onHome)
    {
        m_events->OnTabHomeAction(this, HomeAction::Refresh, {}, nullptr, -1);
        return;
    }
    if (m_results && !m_tagId.empty())
    {
        // F5 on a tag's files: list them again (same scope)
        EnterResults();
        m_search.StartTagged(m_tagId, m_tagRoot);
        SetTimer(m_host, kSearchTimer, 60, nullptr);
        m_events->OnTabNavigated(this);
        return;
    }
    if (m_results && !m_query.empty())
    {
        const std::wstring query = m_query;
        StartSearch(query); // F5 on results searches again
        return;
    }
    if (FileList* list = List())
    {
        list->Reload();
        return;
    }
    if (ComPtr<IShellView> view = ShellView())
        view->Refresh();
}

void Tab::Reload()
{
    if (m_onHome || m_results || !m_current)
    {
        Refresh();
        return;
    }
    if (InColumns())
    {
        m_columns->ReloadViews(); // every column
        return;
    }
    if (FileList* list = List())
    {
        list->ApplyPrefs(); // (it lists everything; what shows is filtered here)
        return;
    }
    // Re-enumerating asks ICommDlgBrowser2::GetViewFlags and IncludeObject again (browsing to the same folder is a no-op)
    ComPtr<IShellView> view;
    if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))))
        view->Refresh();
}

bool Tab::GetCounts(int& items, int& selected) const
{
    items = selected = 0;
    if (FileList* list = List())
    {
        items = list->Count();
        selected = list->SelectedCount();
        return true;
    }
    ComPtr<IFolderView2> view = View();
    if (!view)
        return false;
    view->ItemCount(SVGIO_ALLVIEW, &items);
    view->ItemCount(SVGIO_SELECTION, &selected);
    return true;
}

void Tab::Complete(PCIDLIST_ABSOLUTE pidl)
{
    const bool same = m_current && ILIsEqual(m_current.get(), pidl);
    // Coming back up into an ancestor: remember which child we came from so it can be selected again
    // (Files' ScrollToPreviousFolderWhenNavigatingUp; off, the same child is remembered to undo the shell's own selection)
    m_pendingSelect.reset();
    m_pendingUnselect = false;
    if (m_current && !same && ILIsParent(pidl, m_current.get(), FALSE))
    {
        UniquePidl walk = ClonePidl(m_current.get());
        while (walk && !ILIsParent(pidl, walk.get(), TRUE) && ILRemoveLastID(walk.get()))
        {
        }
        if (walk && ILIsParent(pidl, walk.get(), TRUE))
        {
            m_pendingSelect.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(ILFindLastID(walk.get()))));
            m_pendingUnselect = m_prefs && !m_prefs->selectPrevious;
        }
    }
    if (!same && !m_filter.empty())
        SetFilter(L"");
    // Entries remember whether they were the column view; the caller sets m_currentColumns for the new location
    if (m_pending == Pending::Back && !m_back.empty())
    {
        for (int i = 0; i < m_pendingSteps && !m_back.empty(); ++i)
        {
            m_forward.push_back({ std::move(m_current), m_currentColumns });
            m_current = std::move(m_back.back().pidl);
            m_currentColumns = m_back.back().columns;
            m_back.pop_back();
        }
    }
    else if (m_pending == Pending::Forward && !m_forward.empty())
    {
        for (int i = 0; i < m_pendingSteps && !m_forward.empty(); ++i)
        {
            m_back.push_back({ std::move(m_current), m_currentColumns });
            m_current = std::move(m_forward.back().pidl);
            m_currentColumns = m_forward.back().columns;
            m_forward.pop_back();
        }
    }
    else if (m_current && !same)
    {
        m_back.push_back({ std::move(m_current), m_currentColumns });
        if (m_back.size() > kMaxHistory)
            m_back.erase(m_back.begin());
        m_forward.clear();
    }
    m_pending = Pending::None;
    m_pendingSteps = 1;
    m_current = ClonePidl(pidl);
    if (m_requestedSelect)
    {
        m_pendingSelect = std::move(m_requestedSelect);
        m_pendingUnselect = false;
    }
}

void Tab::SelectAfterNavigate(PCUITEMID_CHILD child)
{
    m_requestedSelect.reset(child ? reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(child)) : nullptr);
}

IFACEMETHODIMP Tab::OnNavigationComplete(PCIDLIST_ABSOLUTE pidl)
{
    if (m_results)
    {
        // The results folder itself coming up (FillFromObject): not a navigation, the location stays the scope
        ComPtr<IFolderView2> view;
        ComPtr<IResultsFolder> results;
        if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))) && SUCCEEDED(view->GetFolder(IID_PPV_ARGS(&results))))
        {
            if (!m_resultsFolder)
                AttachResultsFolder();
            return S_OK;
        }
        // Opened a folder from the results: go there in the folder's own browser (this one is torn down, which can't
        // happen inside its own event)
        if (PIDLIST_ABSOLUTE copy = ILCloneFull(pidl); copy && !PostMessageW(m_host, kMsgLeaveResults, 0, reinterpret_cast<LPARAM>(copy)))
            CoTaskMemFree(copy);
        return S_OK;
    }
    if (IsHomePidl(pidl))
    {
        // Reached from inside the view (rare): show our page instead of the shell's Quick access
        EnterHome(pidl);
        return S_OK;
    }
    const bool fromColumns = m_columns != nullptr; // the folder browser loaded under the columns: they go now
    const bool fromList = m_listShown;             // ... or under the own list
    const bool hadFocus = HasFocus();
    HideHome();
    Complete(pidl);
    m_currentColumns = false;
    if (fromColumns)
    {
        DestroyColumns();
        SetBrowserShown(true);
    }
    if (fromList)
    {
        HideList();
        SetBrowserShown(true);
        if (hadFocus)
            Focus();
    }
    m_applyLayout = -1;
    if (m_pendingTrack)
    {
        // The adaptive layout shows this folder; a change by hand before leaving makes it the folder's choice
        m_layoutKey = m_pendingKey;
        m_layoutShown = static_cast<int>(GetLayout());
        if (m_pendingAdaptiveLater)
        {
            m_adaptiveTicks = 0;
            m_adaptiveCount = -1;
            SetTimer(m_host, kAdaptiveTimer, 300, nullptr);
        }
    }
    m_pendingTrack = m_pendingAdaptiveLater = false;
    HookView();
    ApplyCheckboxes();
    ApplyLayoutPref(true);
    ApplyTheme();
    SetTimer(m_host, kThemeTimer, 80, nullptr); // again once the view has finished setting itself up
    if (m_pendingSelect)
    {
        m_selectTries = 0;
        SetTimer(m_host, kSelectTimer, 60, nullptr);
        TryPendingSelect();
    }
    m_events->OnTabNavigated(this);
    return S_OK;
}

IFACEMETHODIMP Tab::OnNavigationFailed(PCIDLIST_ABSOLUTE)
{
    m_pending = Pending::None;
    m_leavingColumns = false; // the columns stay
    m_applyLayout = -1;
    m_pendingTrack = m_pendingAdaptiveLater = false;
    m_events->OnTabNavigationFailed(this);
    return S_OK;
}
