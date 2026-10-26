#include "Tab.h"
#include "Lang.h"

// File-name search results shown in this tab (Files: search results replace the folder contents, with the same
// columns, context menu and drag-and-drop). IExplorerBrowser::FillFromObject only works on a browser that hasn't
// navigated yet, so results get their own browser: it's swapped in over the folder's view (which stays alive, hidden,
// scroll position and all) and destroyed when the results are left. The tab's location stays the searched folder, so
// the address bar, breadcrumb and session keep pointing there; Back / Up / opening a folder leaves the results.

namespace
{
// System.ItemNameDisplay (FMTID_Storage, 10), defined locally like TabCommands.cpp to avoid INITGUID
constexpr PROPERTYKEY kKeyName{ { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 10 };
// System.ItemFolderNameDisplay (FMTID_Storage, 2): the sort while results stream in. Items added to a results folder are
// appended, not sorted in, and re-setting the same sort is a no-op, so the switch to name at the end is what sorts them.
constexpr PROPERTYKEY kKeyFolder{ { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 2 };

void SortResults(IFolderView2* view, const PROPERTYKEY& key)
{
    const SORTCOLUMN column{ key, SORT_ASCENDING };
    view->SetSortColumns(&column, 1);
}

}

// The browser's own window directly under the host (the view lives somewhere below it)
HWND Tab::BrowserWindow(IExplorerBrowser* browser) const
{
    ComPtr<IShellView> view;
    HWND hwnd = nullptr;
    if (!browser || FAILED(browser->GetCurrentView(IID_PPV_ARGS(&view))) || FAILED(view->GetWindow(&hwnd)))
        return nullptr;
    while (hwnd && GetParent(hwnd) != m_host)
        hwnd = GetParent(hwnd);
    return hwnd;
}

// Swap in the results browser (once; a new search in the same results reuses it) and empty it
bool Tab::EnterResults()
{
    if (!m_host || !m_current)
        return false;
    if (!m_results)
    {
        ComPtr<IExplorerBrowser> results;
        DWORD cookie = 0;
        if (!NewBrowser(results, cookie))
            return false;
        // Swap first: FillFromObject reports its navigation (to us) before returning. From the home page there may be
        // no folder browser at all (tag results); the page is hidden and comes back when the results are left.
        // Over the column view the columns stay underneath (hidden) and come back the same way.
        const HWND folderWindow = m_onHome || m_columns ? nullptr : BrowserWindow(m_browser.Get());
        if (m_columns && !m_onHome)
        {
            m_resultsOverColumns = true;
            ShowWindow(m_columns->Hwnd(), SW_HIDE);
        }
        if (m_listShown && m_list && !m_onHome)
            ShowWindow(m_list->Hwnd(), SW_HIDE); // the own list comes back when the results are left
        m_mainBrowser = std::move(m_browser);
        m_mainCookie = m_cookie;
        m_mainFilterInstalled = m_filterInstalled;
        m_mainDarkApplied = m_darkApplied;
        m_browser = results;
        m_cookie = cookie;
        m_filterInstalled = false;
        m_darkApplied = false;
        m_results = true;
        m_resultsOverHome = m_onHome;
        if (m_onHome)
        {
            m_onHome = false;
            if (HWND page = PageWindow())
                ShowWindow(page, SW_HIDE);
        }
        if (FAILED(m_browser->FillFromObject(nullptr, EBF_NODROPTARGET)))
        {
            EndResults();
            return false;
        }
        if (folderWindow)
            ShowWindow(folderWindow, SW_HIDE);
        if (!m_filter.empty())
            InstallFilter();
    }
    if (!m_resultsFolder)
        AttachResultsFolder();
    else
        m_resultsFolder->RemoveAll();
    m_resultsAdded = 0;
    m_searchFinal = {};
    ComPtr<IFolderView2> view;
    if (SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))))
        SortResults(view.Get(), kKeyFolder); // empty view: instant
    return true;
}

bool Tab::StartSearch(const std::wstring& query)
{
    if (!m_host || m_onHome || !m_current || !CreateBrowser())
        return false; // (in the column view: m_current is the active column, the folder searched)
    const std::wstring root = PidlName(m_current.get(), SIGDN_FILESYSPATH);
    if (root.empty())
        return false; // virtual folders: the caller falls back to Windows Search
    if (!EnterResults())
        return false;
    m_query = query;
    m_tagId.clear();
    m_tagRoot.clear();
    m_search.Start(root, query, m_prefs && m_prefs->showHidden);
    SetTimer(m_host, kSearchTimer, 60, nullptr);
    m_events->OnTabNavigated(this);
    return true;
}

// Files' sidebar tags open "tag:<name>" results; the core lists the tagged files (no folder walk)
bool Tab::StartTagSearch(const std::wstring& tagId, const std::wstring& tagName, bool here)
{
    if (!m_host || !m_current || tagId.empty())
        return false;
    // Inside a file system folder (typed "tag:" in its search box): that folder and below. Elsewhere: everywhere.
    std::wstring root;
    if (here && !m_onHome && !(m_results && m_resultsOverHome))
        root = PidlName(m_current.get(), SIGDN_FILESYSPATH);
    if (!EnterResults())
        return false;
    m_query = tagName;
    m_tagId = tagId;
    m_tagRoot = root;
    m_search.StartTagged(m_tagId, m_tagRoot);
    SetTimer(m_host, kSearchTimer, 60, nullptr);
    m_events->OnTabNavigated(this);
    return true;
}

void Tab::AttachResultsFolder()
{
    ComPtr<IFolderView2> view;
    if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))))
        view->GetFolder(IID_PPV_ARGS(&m_resultsFolder));
    if (m_resultsFolder)
    {
        HookView();
        // The results folder picks tiles for itself; use the user's default layout (details unless changed)
        static const FOLDERVIEWMODE modes[] = { FVM_DETAILS, FVM_LIST, FVM_TILE, FVM_CONTENT, FVM_SMALLICON, FVM_ICON, FVM_ICON, FVM_ICON };
        static const int sizes[] = { 16, 16, 48, 32, 16, 48, 96, 256 };
        // Files: search results open in details (adaptive and columns don't apply to them)
        const int layout = m_prefs ? ShellLayoutIndex(m_prefs->defaultLayout) : 0;
        view->SetViewModeAndIconSize(modes[layout], sizes[layout]);
        ApplyTheme();
        SetTimer(m_host, kThemeTimer, 80, nullptr);
    }
}

void Tab::PumpSearch()
{
    if (!m_results)
    {
        KillTimer(m_host, kSearchTimer);
        return;
    }
    if (!m_resultsFolder)
        AttachResultsFolder();
    if (m_resultsFolder)
    {
        // Batches keep the UI responsive while thousands of results arrive
        for (auto& pidl : m_search.Take(400))
            if (SUCCEEDED(m_resultsFolder->AddIDList(pidl.get(), nullptr)))
                ++m_resultsAdded;
    }
    const FileSearch::Progress p = m_search.Read();
    if (!m_search.Active() || (p.done && m_resultsFolder))
    {
        if (m_search.Active())
            m_searchFinal = p;
        m_search.Cancel(); // finished: drop the job (its threads have exited)
        KillTimer(m_host, kSearchTimer);
        // All in: sort by name, unless the user picked a column meanwhile
        ComPtr<IFolderView2> view;
        SORTCOLUMN column{};
        if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))) && SUCCEEDED(view->GetSortColumns(&column, 1)) &&
            column.propkey.pid == kKeyFolder.pid && IsEqualGUID(column.propkey.fmtid, kKeyFolder.fmtid))
            SortResults(view.Get(), kKeyName);
    }
}

// Back to the folder's own view; safe to call when not in results
void Tab::EndResults()
{
    if (!m_results)
        return;
    m_search.Cancel();
    if (m_host)
        KillTimer(m_host, kSearchTimer);
    m_resultsFolder.Reset();
    if (m_browser)
    {
        if (m_cookie)
            m_browser->Unadvise(m_cookie);
        ComPtr<IFolderFilterSite> site;
        if (m_filterInstalled && SUCCEEDED(m_browser.As(&site)))
            site->SetFilter(nullptr);
        IUnknown_SetSite(m_browser.Get(), nullptr);
        m_browser->Destroy();
        m_browser.Reset();
    }
    // The folder's own browser (none when tag results were opened from a home page that never browsed anywhere)
    m_browser = std::move(m_mainBrowser);
    m_cookie = m_mainCookie;
    m_filterInstalled = m_mainFilterInstalled;
    m_darkApplied = m_mainDarkApplied;
    m_mainCookie = 0;
    m_mainFilterInstalled = m_mainDarkApplied = false;
    m_results = false;
    m_query.clear();
    m_tagId.clear();
    m_tagRoot.clear();
    m_resultsAdded = 0;
    m_searchFinal = {};
    const bool overHome = m_resultsOverHome;
    m_resultsOverHome = false;
    const bool overColumns = m_resultsOverColumns;
    m_resultsOverColumns = false;
    if (!m_host)
        return;
    if (overHome)
    {
        ShowHome(); // keeps any folder browser hidden underneath
        return;
    }
    if (overColumns && m_columns)
    {
        ShowColumnsSurface();
        m_columns->SetFilter(m_filterLower);
        ApplyTheme();
        return;
    }
    if (m_listShown && m_list)
    {
        RECT rc{};
        GetClientRect(m_host, &rc);
        SetWindowPos(m_list->Hwnd(), HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        ApplyTheme();
        return;
    }
    if (m_browser)
    {
        RECT rc{};
        GetClientRect(m_host, &rc);
        m_browser->SetRect(nullptr, rc);
        if (const HWND folderWindow = BrowserWindow(m_browser.Get()))
            ShowWindow(folderWindow, SW_SHOWNA);
        // The folder view kept its own filter state; bring it in line with the current filter text
        if (!m_filter.empty())
            InstallFilter();
        ApplyTheme();
    }
}

std::wstring Tab::SearchStatus() const
{
    if (!m_results)
        return {};
    const FileSearch::Progress p = m_search.Active() ? m_search.Read() : m_searchFinal;
    const std::wstring q = L"“" + m_query + L"”";
    if (!m_tagId.empty())
    {
        if (m_search.Active())
            return Tr(L"正在查找标签 ") + q + Tr(L" 的项目…");
        std::wstring text = Tr(L"标签 ") + q + L"：" + std::to_wstring(m_resultsAdded) + Tr(L" 个项目");
        if (!m_tagRoot.empty())
            text += Tr(L"（在 ") + m_tagRoot + Tr(L" 中）");
        return text;
    }
    wchar_t seconds[32];
    swprintf_s(seconds, L"%.1f", static_cast<double>(p.elapsedMs) / 1000.0);
    if (m_search.Active())
        return Tr(L"正在搜索 ") + q + Tr(L"…  已找到 ") + std::to_wstring(m_resultsAdded) + Tr(L" 个，已查看 ") +
               std::to_wstring(p.files + p.folders) + Tr(L" 项");
    std::wstring text = Tr(L"找到 ") + std::to_wstring(m_resultsAdded) + Tr(L" 个 ") + q + Tr(L"，查看了 ") + std::to_wstring(p.files + p.folders) +
                        Tr(L" 项，用时 ") + seconds + Tr(L" 秒");
    if (p.truncated)
        text += Tr(L"（只显示前 ") + std::to_wstring(FileSearch::kLimit) + Tr(L" 个）");
    if (p.skipped)
        text += L"，" + std::to_wstring(p.skipped) + Tr(L" 个文件夹无权访问");
    return text;
}
