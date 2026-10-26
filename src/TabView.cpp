#include "Tab.h"
#include "DarkMode.h"
#include "Share.h"
#include <mutex>
#include <shared_mutex>

namespace
{
// ShouldShow runs on the view's enumeration thread while the UI thread edits the filter
std::shared_mutex g_filterLock;

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool HasExt(const std::wstring& ext, std::initializer_list<const wchar_t*> list)
{
    for (const wchar_t* e : list)
        if (_wcsicmp(ext.c_str(), e) == 0)
            return true;
    return false;
}
}

IFACEMETHODIMP Tab::ShouldShow(IShellFolder* folder, PCIDLIST_ABSOLUTE, PCUITEMID_CHILD child)
{
    std::shared_lock lock(g_filterLock);
    if (m_filterLower.empty())
        return S_OK;
    STRRET ret{};
    wchar_t name[MAX_PATH]{};
    if (FAILED(folder->GetDisplayNameOf(child, SHGDN_INFOLDER, &ret)) || FAILED(StrRetToBufW(&ret, child, name, MAX_PATH)))
        return S_OK;
    CharLowerBuffW(name, static_cast<DWORD>(wcslen(name)));
    return wcsstr(name, m_filterLower.c_str()) ? S_OK : S_FALSE;
}

// Files keeps its own ShowHiddenItems / ShowProtectedSystemFiles. When they differ from the system's, the view is told to
// list everything (CDB2GVF_SHOWALLFILES, as file dialogs do) and IncludeObject drops what our settings hide.
namespace
{
bool IsClass(HWND hwnd, const wchar_t* name)
{
    wchar_t cls[32]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return wcscmp(cls, name) == 0;
}

// A new view's list paints before OnViewCreated themes it: its background is the palette's from the start
constexpr UINT_PTR kHoverSelectTimer = 0xFF14;
constexpr wchar_t kHoverProp[] = L"FastFiles.Hover"; // the item under the mouse + 1

LRESULT CALLBACK EarlyListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    if (msg == LVM_SETBKCOLOR && static_cast<COLORREF>(lp) != CLR_NONE)
        if (const Theme* t = reinterpret_cast<const ViewPrefs*>(ref)->theme)
            lp = t->content; // DefView sets the system window color first
    // Files' SelectFilesOnHover (BaseLayoutPage.FileListItem_PointerEntered): the item the mouse rests on for a second
    // gets selected, unless several items are selected already
    if (msg == WM_MOUSEMOVE && reinterpret_cast<const ViewPrefs*>(ref)->hoverSelect)
    {
        LVHITTESTINFO hit{ { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) } };
        const int item = ListView_HitTest(hwnd, &hit);
        const int item1 = item >= 0 && (hit.flags & LVHT_ONITEM) ? item + 1 : 0;
        if (reinterpret_cast<INT_PTR>(GetPropW(hwnd, kHoverProp)) != item1)
        {
            SetPropW(hwnd, kHoverProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(item1)));
            if (item1)
                SetTimer(hwnd, kHoverSelectTimer, 1000, nullptr);
            else
                KillTimer(hwnd, kHoverSelectTimer);
        }
    }
    if (msg == WM_TIMER && wp == kHoverSelectTimer)
    {
        KillTimer(hwnd, kHoverSelectTimer);
        const int item = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(hwnd, kHoverProp))) - 1;
        if (item >= 0 && ListView_GetSelectedCount(hwnd) <= 1) // the mouse left: WM_MOUSELEAVE stopped the timer
        {
            ListView_SetItemState(hwnd, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(hwnd, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE || msg == WM_NCDESTROY)
    {
        KillTimer(hwnd, kHoverSelectTimer);
        RemovePropW(hwnd, kHoverProp);
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, EarlyListProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK EarlyViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    if (msg == WM_PARENTNOTIFY && LOWORD(wp) == WM_CREATE)
    {
        const HWND child = reinterpret_cast<HWND>(lp);
        const Theme* t = reinterpret_cast<const ViewPrefs*>(ref)->theme;
        if (t && IsClass(child, WC_LISTVIEWW))
        {
            SetWindowSubclass(child, EarlyListProc, 0xFF13, ref);
            ListView_SetBkColor(child, t->content);
        }
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, EarlyViewProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK BrowserHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    if (msg == WM_PARENTNOTIFY && LOWORD(wp) == WM_CREATE && IsClass(reinterpret_cast<HWND>(lp), L"SHELLDLL_DefView"))
    {
        // DefView made its list inside its own WM_CREATE (before this notification), so the list exists but hasn't painted
        const HWND view = reinterpret_cast<HWND>(lp);
        SetWindowSubclass(view, EarlyViewProc, 0xFF12, ref);
        const Theme* t = reinterpret_cast<const ViewPrefs*>(ref)->theme;
        if (HWND list = FindWindowExW(view, nullptr, WC_LISTVIEWW, nullptr); list && t)
        {
            SetWindowSubclass(list, EarlyListProc, 0xFF13, ref);
            ListView_SetBkColor(list, t->content);
        }
    }
    if (msg == WM_ERASEBKGND)
        if (const Theme* t = reinterpret_cast<const ViewPrefs*>(ref)->theme)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillSolid(reinterpret_cast<HDC>(wp), rc, t->content);
            return 1;
        }
    if (msg == WM_PAINT)
    {
        // It paints itself white too (not only on erase) while it has no visible view: mid-navigation
        bool child = false;
        for (HWND c = GetWindow(hwnd, GW_CHILD); c && !child; c = GetWindow(c, GW_HWNDNEXT))
            child = IsWindowVisible(c) != FALSE;
        const Theme* t = reinterpret_cast<const ViewPrefs*>(ref)->theme;
        if (!child && t)
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillSolid(hdc, rc, t->content);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, BrowserHostProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
}

void ThemeBrowserHost(IExplorerBrowser* browser, const ViewPrefs* prefs)
{
    ComPtr<IOleWindow> window;
    HWND hwnd = nullptr;
    if (prefs && browser && SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&window))) && SUCCEEDED(window->GetWindow(&hwnd)) && hwnd)
        SetWindowSubclass(hwnd, BrowserHostProc, 0xFF11, reinterpret_cast<DWORD_PTR>(prefs));
}
DWORD HiddenViewFlags(const ViewPrefs* prefs)
{
    return prefs && prefs->ownHiddenFilter ? CDB2GVF_SHOWALLFILES : 0;
}

HRESULT FilterHidden(const ViewPrefs* prefs, IShellView* view, PCUITEMID_CHILD child)
{
    const bool dots = prefs && !prefs->showDotFiles;
    if (!prefs || !view || !child || ((!prefs->ownHiddenFilter || (prefs->showHidden && prefs->showProtected)) && !dots))
        return S_OK;
    ComPtr<IFolderView> folderView;
    ComPtr<IShellFolder> folder;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(&folderView))) || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))))
        return S_OK;
    if (dots)
    {
        // Files' ShowDotFiles: names starting with "." (the file system name, so search results count too)
        STRRET ret{};
        wchar_t* name = nullptr;
        if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &ret)) && SUCCEEDED(StrRetToStrW(&ret, child, &name)))
        {
            const bool dot = PathFindFileNameW(name)[0] == L'.';
            CoTaskMemFree(name);
            if (dot)
                return S_FALSE;
        }
    }
    if (!prefs->ownHiddenFilter || (prefs->showHidden && prefs->showProtected))
        return S_OK;
    SFGAOF attrs = SFGAO_HIDDEN | SFGAO_SYSTEM;
    if (FAILED(folder->GetAttributesOf(1, &child, &attrs)) || !(attrs & SFGAO_HIDDEN))
        return S_OK;
    // Hidden + system is a protected operating system file (Explorer's "super hidden")
    const bool shown = (attrs & SFGAO_SYSTEM) ? prefs->showHidden && prefs->showProtected : prefs->showHidden;
    return shown ? S_OK : S_FALSE;
}

IFACEMETHODIMP Tab::IncludeObject(IShellView* view, PCUITEMID_CHILD child)
{
    return FilterHidden(m_prefs, view, child);
}

IFACEMETHODIMP Tab::GetViewFlags(DWORD* flags)
{
    *flags = HiddenViewFlags(m_prefs);
    return S_OK;
}

IFACEMETHODIMP Tab::GetEnumFlags(IShellFolder*, PCIDLIST_ABSOLUTE, HWND*, DWORD* flags)
{
    if (!*flags)
        *flags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
    return S_OK;
}

void Tab::InstallFilter()
{
    // Installed only once something needs it, so a plain tab pays nothing per item
    ComPtr<IFolderFilterSite> site;
    if (!m_filterInstalled && m_browser && SUCCEEDED(m_browser.As(&site)) &&
        SUCCEEDED(site->SetFilter(static_cast<IFolderFilter*>(this))))
        m_filterInstalled = true;
}

void Tab::SetFilter(const std::wstring& text)
{
    {
        std::unique_lock lock(g_filterLock);
        if (text == m_filter)
            return;
        m_filter = text;
        m_filterLower = Lower(text);
    }
    if (InColumns())
    {
        m_columns->SetFilter(m_filterLower); // on the active column
        return;
    }
    if (m_list)
        m_list->SetFilter(m_filterLower); // (also while hidden: it's cleared on navigating anyway)
    if (InList())
        return;
    InstallFilter();
    Refresh();
}

void Tab::ApplyPrefs()
{
    ApplyCheckboxes();
    ApplyLayoutPref(false);
}

// The shell's list draws itself: give it the palette, plus the dark scroll bars / header / selection of DarkMode_ItemsView
void Tab::ApplyTheme()
{
    ComPtr<IShellView> shellView;
    if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&shellView))))
        ApplyThemeTo(shellView.Get(), m_darkApplied);
    if (m_columns)
        m_columns->ApplyTheme(); // (its lists too)
    if (m_list)
        m_list->ApplyTheme();
}

void Tab::ApplyThemeTo(IShellView* shellView, bool& darkApplied)
{
    const Theme* t = m_prefs ? m_prefs->theme : nullptr;
    if (!t || !shellView)
        return;
    ComPtr<IVisualProperties> visual;
    if (SUCCEEDED(shellView->QueryInterface(IID_PPV_ARGS(&visual))))
    {
        visual->SetColor(VPCF_BACKGROUND, t->content);
        visual->SetColor(VPCF_TEXT, t->ink);
        visual->SetColor(VPCF_SUBTEXT, t->muted);
        visual->SetColor(VPCF_SORTCOLUMN, Mix(t->content, t->ink, t->dark ? 3 : 3)); // tint of the sorted column in Details
    }
    HWND viewWindow = nullptr;
    if (FAILED(shellView->GetWindow(&viewWindow)) || !viewWindow)
        return;
    if (HWND list = FindWindowExW(viewWindow, nullptr, WC_LISTVIEWW, nullptr))
    {
        // Explorer's own ItemsView classes in both themes (re-applied: the view resets them). The browser's default
        // "Explorer" class draws a line between every details column; ours are optional (ViewProc, columnLines).
        DarkMode::ApplyControl(list, t->dark, L"ItemsView", L"DarkMode_ItemsView");
        if (HWND header = ListView_GetHeader(list))
            DarkMode::ApplyControl(header, t->dark, L"ItemsView", L"DarkMode_ItemsView");
        darkApplied = t->dark;
        DarkMode::ColorHeaderText(list, &t->muted);
        // Files' AppThemeFontFamily covers the file list too; the standard font leaves the shell's own
        if (!t->face.empty())
        {
            SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(t->ui), TRUE);
            if (HWND header = ListView_GetHeader(list))
                SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(t->ui), TRUE);
        }
        InvalidateRect(list, nullptr, TRUE);
    }
}

void Tab::ApplyCheckboxes()
{
    const DWORD flags = m_prefs && m_prefs->checkboxes ? FWF_CHECKSELECT : 0;
    ComPtr<IFolderView2> view;
    if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))))
        view->SetCurrentFolderFlags(FWF_CHECKSELECT, flags);
    // The own lists draw their boxes from the prefs (Files' column rows have none: their template leaves them out)
    ForEachList([](FileList* list)
    {
        if (list->List())
            InvalidateRect(list->List(), nullptr, FALSE);
    });
}

std::vector<PCIDLIST_ABSOLUTE> Tab::History(bool back) const
{
    std::vector<PCIDLIST_ABSOLUTE> out;
    const auto& list = back ? m_back : m_forward;
    for (auto it = list.rbegin(); it != list.rend(); ++it)
        out.push_back(it->pidl.get());
    return out;
}

void Tab::GoHistory(bool back, int steps)
{
    if (back && steps == 1 && m_results)
    {
        // Back from results: the searched folder again
        m_pending = Pending::None;
        UniquePidl scope = ClonePidl(m_current.get());
        Go(scope.get());
        return;
    }
    const auto& list = back ? m_back : m_forward;
    if (!m_host || steps < 1 || steps > static_cast<int>(list.size()))
        return;
    m_pending = back ? Pending::Back : Pending::Forward;
    m_pendingSteps = steps;
    const HistoryEntry& entry = list[list.size() - steps];
    Go(entry.pidl.get(), entry.columns ? ModeHint::Columns : ModeHint::Normal); // Go copies it before history rotates
}

bool Tab::SelectPath(const std::wstring& path)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!m_current || m_results || FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)))
        return false;
    UniquePidl item(pidl);
    UniquePidl parent = ClonePidl(item.get());
    ILRemoveLastID(parent.get());
    if (!ILIsEqual(parent.get(), m_current.get()))
        return false;
    if (FileList* list = List())
    {
        list->SelectWhenListed(PathFindFileNameW(path.c_str())); // a just-created item: once the next listing has it
        return true;
    }
    m_pendingSelect.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(ILFindLastID(item.get()))));
    m_pendingUnselect = false;
    m_selectTries = 0;
    SetTimer(m_host, kSelectTimer, 60, nullptr);
    TryPendingSelect();
    return true;
}

std::vector<std::wstring> Tab::SelectedFileSystemPaths() const
{
    if (FileList* list = List())
        return list->SelectedPaths();
    std::vector<std::wstring> out;
    ComPtr<IShellItemArray> selection = Selection();
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)))
        return out;
    for (DWORD i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(selection->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

bool Tab::SelectedFileSystemItems(std::vector<std::wstring>& paths, std::vector<UniquePidl>& pidls, std::vector<std::wstring>& names) const
{
    paths.clear();
    pidls.clear();
    names.clear();
    ComPtr<IShellItemArray> selection = Selection();
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)) || count == 0)
        return false;
    for (DWORD i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr, name = nullptr;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(selection->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            paths.clear();
            pidls.clear();
            names.clear();
            return false;
        }
        paths.emplace_back(path);
        CoTaskMemFree(path);
        pidls.emplace_back(SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)) ? pidl : nullptr);
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        {
            names.emplace_back(name);
            CoTaskMemFree(name);
        }
        else
            names.emplace_back();
    }
    return true;
}

std::vector<UniquePidl> Tab::SelectedFolders() const
{
    std::vector<UniquePidl> out;
    ComPtr<IShellItemArray> selection = Selection();
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)) || count == 0 || count > 64)
        return out;
    for (DWORD i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> item;
        SFGAOF attrs = 0;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(selection->GetItemAt(i, &item)) || FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs)) ||
            !(attrs & SFGAO_FOLDER) || (attrs & SFGAO_STREAM) || FAILED(SHGetIDListFromObject(item.Get(), &pidl)))
            return {};
        out.emplace_back(pidl);
    }
    return out;
}

void Tab::TryPendingSelect()
{
    ComPtr<IShellView> shellView = ShellView();
    auto view = View();
    if (!m_pendingSelect || ++m_selectTries > 25 || !view || !shellView)
    {
        if (!m_pendingSelect || m_selectTries > 25)
        {
            KillTimer(m_host, kSelectTimer);
            m_pendingSelect.reset();
            m_pendingUnselect = false;
        }
        return;
    }
    const auto child = reinterpret_cast<PCUITEMID_CHILD>(m_pendingSelect.get());
    if (m_pendingUnselect)
    {
        // 向上导航时滚动至上一目录 off: the shell selects the folder we came from by itself (whenever enumeration gets
        // there); while that is the whole selection, take it back, for the same ~1.5 s
        int count = 0, focused = -1;
        PITEMID_CHILD current = nullptr;
        if (SUCCEEDED(view->ItemCount(SVGIO_SELECTION, &count)) && count == 1 && SUCCEEDED(view->GetFocusedItem(&focused)) && focused >= 0 &&
            SUCCEEDED(view->Item(focused, &current)))
        {
            const bool match = ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(current), reinterpret_cast<PCIDLIST_ABSOLUTE>(child));
            CoTaskMemFree(current);
            if (match)
            {
                shellView->SelectItem(child, SVSI_DESELECT);
                KillTimer(m_host, kSelectTimer); // once: a later click on it is the user's
                m_pendingSelect.reset();
                m_pendingUnselect = false;
            }
        }
        return;
    }
    // Until enumeration reaches the item this selects nothing; keep trying for ~1.5 s
    shellView->SelectItem(child, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED);
    int focused = -1;
    PITEMID_CHILD current = nullptr;
    if (SUCCEEDED(view->GetFocusedItem(&focused)) && focused >= 0 && SUCCEEDED(view->Item(focused, &current)))
    {
        const bool match = ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(current), reinterpret_cast<PCIDLIST_ABSOLUTE>(child));
        CoTaskMemFree(current);
        if (match)
        {
            KillTimer(m_host, kSelectTimer);
            m_pendingSelect.reset();
        }
    }
}

unsigned Tab::SelectionKinds(unsigned* every) const
{
    unsigned kinds = 0, all = ~0u;
    if (every)
        *every = 0;
    ComPtr<IShellItemArray> selection = Selection();
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)) || count == 0)
        return 0;
    for (DWORD i = 0; i < std::min<DWORD>(count, 64); ++i)
    {
        ComPtr<IShellItem> item;
        SFGAOF attrs = 0;
        PWSTR name = nullptr;
        unsigned k = SelOther;
        if (SUCCEEDED(selection->GetItemAt(i, &item)))
        {
            item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs);
            if ((attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM))
            {
                k = SelFolder;
                // 共享 on a folder: the system's network sharing (授予访问权限 > 特定用户...), for a real disk folder
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    if (Share::FolderShareable(path))
                        k |= SelShareFolder;
                    CoTaskMemFree(path);
                }
            }
            else if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)))
            {
                const std::wstring ext = PathFindExtensionW(name);
                CoTaskMemFree(name);
                if (HasExt(ext, { L".png", L".bmp", L".jpg", L".jpeg", L".jfif", L".gif", L".tiff", L".tif", L".jxr" }))
                    k = SelImage;
                else if (HasExt(ext, { L".exe", L".msi", L".com" }))
                    k = SelRunnable;
                else if (HasExt(ext, { L".bat", L".cmd" }))
                    k = SelRunnable | SelBatch;
                else if (HasExt(ext, { L".ahk" }))
                    k = SelBatch;
                else if (HasExt(ext, { L".ps1" }))
                    k = SelPowerShell;
                else if (HasExt(ext, { L".ttf", L".otf", L".ttc", L".fon" }))
                    k = SelFont;
                else if (HasExt(ext, { L".zip", L".msix", L".appx", L".msixbundle", L".appxbundle", L".7z", L".rar", L".tar", L".mcpack",
                             L".mcworld", L".mrpack", L".jar", L".gz", L".lzh" }))
                    k = SelArchive | SelZip; // Files' IsZipFile: the extract commands take these (7z.dll beyond plain zips)
                else if (HasExt(ext, { L".mp4", L".m4v", L".mp4v", L".3g2", L".3gp2", L".3gp", L".3gpp", L".mpg", L".mp2", L".mpeg",
                             L".mpe", L".mpv", L".mkv", L".ogg", L".avi", L".wmv", L".mov", L".qt", L".mp3", L".m4a", L".oga", L".wav",
                             L".wma", L".aac", L".flac" }))
                    k = SelMedia;
                else if (HasExt(ext, { L".inf" }))
                    k = SelInf;
                else if (HasExt(ext, { L".cer", L".crt", L".der", L".pfx" }))
                    k = SelCertificate;
                // Files' IsItemShareable: a file (a zip too), not hidden, not a .lnk (a .url shares its link)
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    if (Share::Shareable(path))
                        k |= SelShareable;
                    CoTaskMemFree(path);
                }
            }
        }
        kinds |= k;
        all &= k;
    }
    if (every)
        *every = all;
    return kinds;
}

bool Tab::IsRecycleBin() const
{
    PIDLIST_ABSOLUTE bin = nullptr;
    if (!m_current || FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &bin)))
        return false;
    const bool same = ILIsEqual(bin, m_current.get()) != FALSE;
    CoTaskMemFree(bin);
    return same;
}

PCIDLIST_ABSOLUTE Tab::SingleSelectedItem(UniquePidl& holder) const
{
    ComPtr<IShellItemArray> selection = Selection();
    ComPtr<IShellItem> item;
    DWORD count = 0;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (selection && SUCCEEDED(selection->GetCount(&count)) && count == 1 &&
        SUCCEEDED(selection->GetItemAt(0, &item)) && SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)))
        holder.reset(pidl);
    return holder.get();
}

PCIDLIST_ABSOLUTE Tab::SingleSelectedFolder(UniquePidl& holder) const
{
    ComPtr<IShellItemArray> selection = Selection();
    ComPtr<IShellItem> item;
    DWORD count = 0;
    SFGAOF attrs = 0;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (selection && SUCCEEDED(selection->GetCount(&count)) && count == 1 &&
        SUCCEEDED(selection->GetItemAt(0, &item)) && SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attrs)) && (attrs & SFGAO_FOLDER) &&
        SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)))
        holder.reset(pidl);
    return holder.get();
}

// Files' LayoutIncreaseSize / LayoutDecreaseSize (Ctrl+加号 / 减号, Ctrl+wheel): the layout's size one step; past its
// largest / smallest LayoutCycler goes on to the next layout (详细信息, 列表, 卡片, 网格, 列视图, round) at its smallest
// / largest size. The sizes are every folder's (saved by the window); the layout is the folder's choice.
bool Tab::StepIconSize(int direction)
{
    static const int sizes[] = { 16, 32, 48, 64, 96, 128, 192, 256 };
    if (m_onHome || !m_current || direction == 0)
        return false;
    static const ViewLayout order[] = { ViewLayout::Details, ViewLayout::List, ViewLayout::Tiles, ViewLayout::LargeIcons, ViewLayout::Columns };
    const ViewLayout shown = DisplayedLayout();
    if ((InList() || InColumns()) && m_prefs)
    {
        const FileList::Mode mode = InColumns() ? FileList::Mode::Column : m_list->GetMode(); // (ColumnsViewSize)
        const int size = m_prefs->layoutSize[static_cast<int>(mode)] + (direction > 0 ? 1 : -1);
        if (size >= 1 && size <= FileList::MaxSize(mode))
        {
            m_events->OnTabLayoutSize(this, static_cast<int>(mode), size);
            return true;
        }
    }
    if ((InList() || InColumns()) && !m_results)
    {
        int at = 0;
        while (at < 4 && order[at] != shown)
            ++at;
        for (int i = 1; i <= 5; ++i)
        {
            const ViewLayout next = order[((at + (direction > 0 ? i : -i)) % 5 + 5) % 5];
            if (next == ViewLayout::Columns && !CanColumns())
                continue;
            const int mode = static_cast<int>(next == ViewLayout::Columns ? FileList::Mode::Column : ListMode(next));
            m_events->OnTabLayoutSize(this, mode, direction > 0 ? 1 : FileList::MaxSize(static_cast<FileList::Mode>(mode)));
            m_events->OnTabCycleLayout(this, next);
            return true;
        }
        return false;
    }
    auto view = View();
    FOLDERVIEWMODE mode = FVM_AUTO;
    int size = 16;
    if (!view || FAILED(view->GetViewModeAndIconSize(&mode, &size)))
        return false;
    int index = 0;
    while (index + 1 < static_cast<int>(ARRAYSIZE(sizes)) && sizes[index] < size)
        ++index;
    index = std::clamp(index + direction, 0, static_cast<int>(ARRAYSIZE(sizes)) - 1);
    // Details/list stay at 16; growing from there switches to icons like Explorer's Ctrl+wheel
    if (direction > 0 && (mode == FVM_DETAILS || mode == FVM_LIST || mode == FVM_SMALLICON))
        mode = FVM_ICON, index = 2;
    else if (index == 0)
        mode = FVM_DETAILS;
    return SUCCEEDED(view->SetViewModeAndIconSize(mode, sizes[index]));
}

ComPtr<IShellView> Tab::ShellView() const
{
    ComPtr<IShellView> view;
    if (InList())
        return view; // (a shell browser loading another layout under the list isn't the view yet)
    if (IExplorerBrowser* browser = ActiveBrowser(); browser && !m_onHome)
        browser->GetCurrentView(IID_PPV_ARGS(&view));
    return view;
}
