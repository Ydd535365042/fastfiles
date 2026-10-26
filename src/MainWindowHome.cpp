#include "MainWindow.h"
#include "Lang.h"
#include "Sandbox.h"
#include "Tags.h"
#include "ContentDialog.h"
#include <thread>

// Home page data: quick access / drives / network mirror the sidebar; recent files come from a worker-thread scan.
// One HomeData snapshot is shared by every pane (ViewPrefs::home).

namespace
{
constexpr ULONGLONG kRecentMaxAge = 5000; // re-scan recent files when Home is shown after this long

HomeItem FromEntry(const Sidebar::Entry& e)
{
    HomeItem item;
    item.pidl = ClonePidl(e.pidl.get());
    item.name = e.name;
    item.path = !e.parsing.empty() ? e.parsing : e.lazyPath;
    item.icon = e.icon;
    item.missing = e.missing;
    item.freeBytes = e.freeBytes;
    item.totalBytes = e.totalBytes;
    return item;
}

std::wstring RecentFolder()
{
    std::wstring result;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DEFAULT, nullptr, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}

// Only shortcuts directly inside the Recent folder may be deleted by "remove from recent"
bool IsRecentLink(const std::wstring& link)
{
    const std::wstring folder = RecentFolder();
    if (folder.empty() || link.size() <= folder.size() + 1 || link.find(L"..") != std::wstring::npos)
        return false;
    if (_wcsnicmp(link.c_str(), folder.c_str(), folder.size()) != 0 || link[folder.size()] != L'\\')
        return false;
    const std::wstring name = link.substr(folder.size() + 1);
    return name.find(L'\\') == std::wstring::npos && name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".lnk") == 0;
}
}

void MainWindow::RefreshHome()
{
    auto data = std::make_shared<HomeData>();
    std::vector<std::wstring> roots;
    if (m_sidebar)
        for (const auto& e : m_sidebar->Entries())
        {
            if (e.section == Sidebar::SecPinned)
                data->quick.push_back(FromEntry(e));
            else if (e.section == Sidebar::SecDrives)
                data->drives.push_back(FromEntry(e));
            else if (e.section == Sidebar::SecNetwork)
                data->network.push_back(FromEntry(e));
            // The column view starts at a pinned or cloud folder that contains the folder (Files' ColumnsLayoutPage)
            if ((e.section == Sidebar::SecPinned || e.section == Sidebar::SecCloud) && e.parsing.size() > 2 &&
                (e.parsing[1] == L':' || e.parsing.rfind(L"\\\\", 0) == 0))
                roots.push_back(e.parsing);
        }
    m_prefs.columnRoots = std::move(roots);
    for (const auto& item : m_recent)
        data->recent.push_back(CopyHomeItem(item));
    data->recentReady = m_recentReady;
    for (const auto& group : m_tagged)
        data->tags.push_back(CopyTagGroup(group));
    data->tagsReady = m_taggedReady;
    m_prefs.home = std::move(data);
    ForEachPane([](Tab* tab) { tab->ApplyHome(); });
}

void MainWindow::ScanRecent()
{
    if (m_recentScanning)
        return;
    m_recentScanning = true;
    const HWND target = m_hwnd;
    std::thread([target]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        auto items = new std::vector<HomeItem>(ScanRecentFiles(20));
        if (!PostMessageW(target, kMsgRecent, 0, reinterpret_cast<LPARAM>(items)))
            delete items;
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void MainWindow::OnRecentScanned(std::vector<HomeItem>* items)
{
    std::unique_ptr<std::vector<HomeItem>> owned(items);
    m_recentScanning = false;
    m_recentScanned = GetTickCount64();
    m_recentReady = true;
    m_recent = std::move(*owned);
    RefreshHome();
}

void MainWindow::ScanTagged()
{
    if (m_taggedScanning)
    {
        m_taggedDirty = true; // a change arrived mid-scan: scan again when this one is done
        return;
    }
    m_taggedScanning = true;
    m_taggedDirty = false;
    const HWND target = m_hwnd;
    std::thread([target]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        auto groups = new std::vector<HomeTagGroup>(ScanTaggedItems(60));
        if (!PostMessageW(target, kMsgTagged, 0, reinterpret_cast<LPARAM>(groups)))
            delete groups;
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void MainWindow::OnTaggedScanned(std::vector<HomeTagGroup>* groups)
{
    std::unique_ptr<std::vector<HomeTagGroup>> owned(groups);
    m_taggedScanning = false;
    m_taggedScanned = GetTickCount64();
    m_taggedReady = true;
    m_tagged = std::move(*owned);
    RefreshHome();
    if (m_taggedDirty)
        ScanTagged();
}

// Called whenever a pane shows Home: recent files are cheap to re-read but change behind our back
void MainWindow::EnsureHomeFresh()
{
    if (!m_prefs.home)
        RefreshHome();
    // Tags widget (off by default, like Files): items can be renamed or deleted outside FastFiles
    if (!(m_prefs.homeHidden & (1u << HomeView::SecTags)) && (!m_taggedReady || GetTickCount64() - m_taggedScanned > kRecentMaxAge))
        ScanTagged();
    if (m_prefs.homeHidden & (1u << HomeView::SecRecent))
        return; // widget turned off: don't read the Recent folder at all
    if (!m_recentReady || GetTickCount64() - m_recentScanned > kRecentMaxAge)
        ScanRecent();
}

void MainWindow::OnSidebarChanged()
{
    // Fires from inside Sidebar::Create too, before m_sidebar is assigned; the first pane's Home builds it then
    if (m_sidebar)
        RefreshHome();
}

void MainWindow::OnTabHomeAction(Tab*, HomeAction action, const std::wstring& key, PCIDLIST_ABSOLUTE pidl, int section)
{
    switch (action)
    {
    case HomeAction::Pin:
        if (m_sidebar && pidl)
            m_sidebar->Pin(pidl);
        break;
    case HomeAction::Unpin:
        if (m_sidebar && !(pidl && m_sidebar->Unpin(pidl)))
            m_sidebar->UnpinKey(key);
        break;
    case HomeAction::RemoveRecent:
        // Deletes only the shortcut in the Recent folder, never the file itself
        if (IsRecentLink(key) && !Sandbox::Active() && DeleteFileW(key.c_str())) // test mode: Windows' Recent folder isn't touched
        {
            m_recent.erase(std::remove_if(m_recent.begin(), m_recent.end(),
                [&](const HomeItem& item) { return _wcsicmp(item.link.c_str(), key.c_str()) == 0; }), m_recent.end());
            RefreshHome();
        }
        break;
    case HomeAction::ClearRecent:
        // Files clears its Recent widget on click with no confirmation (RecentFilesWidgetViewModel.ExecuteClearRecentItemsCommand);
        // FastFiles also clears Explorer's shared list, so it keeps asking first. No Files dialog to match, so the text stays.
        // MB_DEFBUTTON2 in the old MessageBox: Enter must not clear the list, so the close button stays default
        if (ContentDialog::Confirm(m_hwnd, &m_theme, L"FastFiles",
                Tr(L"清除最近使用的文件列表？\n\n这也会清除资源管理器和“开始”菜单中的最近使用记录，文件本身不受影响。"),
                Tr(L"清除"), Tr(L"取消"), true))
        {
            if (!Sandbox::Active()) // test mode: Windows' Recent list isn't cleared
                SHAddToRecentDocs(SHARD_PIDL, nullptr);
            m_recent.clear();
            RefreshHome();
            ScanRecent();
        }
        break;
    case HomeAction::Refresh:
        RefreshHome();
        ScanRecent();
        if (!(m_prefs.homeHidden & (1u << HomeView::SecTags)))
            ScanTagged();
        break;
    case HomeAction::OpenTag:
        // Files' ViewMoreCommand: the tag's items (tag search) in this tab; section 1 = new tab (Ctrl / middle click)
        for (const FileTag& tag : Tags::List())
            if (tag.id == key)
            {
                OpenTag(tag.id, tag.name, section == 1 ? OpenMode::NewTab : OpenMode::Here, false);
                break;
            }
        break;
    case HomeAction::OpenAllTagged:
    {
        // Files' OpenAllTaggedActions: files open with their apps, folders in new tabs
        std::vector<std::wstring> files;
        std::vector<UniquePidl> folders;
        if (m_prefs.home)
            for (const auto& group : m_prefs.home->tags)
                if (group.id == key)
                    for (const auto& item : group.items)
                    {
                        if (item.folder && item.pidl)
                            folders.push_back(ClonePidl(item.pidl.get()));
                        else if (!item.folder)
                            files.push_back(item.path);
                    }
        const size_t total = files.size() + folders.size();
        if (total == 0)
            break;
        if (total > 10)
        {
            // Files' OpenAllTaggedActions ("全部打开") opens every tagged item without asking first, however many
            // there are; FastFiles asks past 10, so there's no Files confirmation text to match, just the title.
            // MB_DEFBUTTON2 in the old MessageBox: Enter must not open everything, so the close button stays default
            const std::wstring text = Tr(L"将打开 ") + std::to_wstring(total) + Tr(L" 个项目。确定吗？");
            if (!ContentDialog::Confirm(m_hwnd, &m_theme, Tr(L"全部打开"), text, Tr(L"打开"), Tr(L"取消"), true))
                break;
        }
        for (const std::wstring& path : files)
        {
            const size_t slash = path.find_last_of(L'\\');
            const std::wstring dir = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_ASYNCOK;
            sei.hwnd = m_hwnd;
            sei.lpFile = path.c_str();
            sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        for (const auto& folder : folders)
            OpenInNewTab(folder.get());
        break;
    }
    case HomeAction::ToggleSection:
        if (section >= 0 && section < HomeView::SecCount)
        {
            m_prefs.homeCollapsed ^= 1u << section;
            ForEachPane([](Tab* tab) { tab->ApplyHome(); });
        }
        break;
    }
}
