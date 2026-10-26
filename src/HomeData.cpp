// Home location identity and the recent-files scan behind the home page.
#include "HomeView.h"
#include "Lang.h"
#include "Tags.h"

PCIDLIST_ABSOLUTE HomePidl()
{
    static const UniquePidl home = []
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        SHParseDisplayName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr, &pidl, 0, nullptr);
        return UniquePidl(pidl);
    }();
    return home.get();
}

bool IsHomePidl(PCIDLIST_ABSOLUTE pidl)
{
    PCIDLIST_ABSOLUTE home = HomePidl();
    if (!pidl || !home)
        return false;
    const UINT size = ILGetSize(pidl);
    if (size == ILGetSize(home) && memcmp(pidl, home, size) == 0)
        return true;
    // Same item with different bytes (e.g. reached from inside a view): only single-level IDLists can be Home
    return !ILIsEmpty(pidl) && ILIsEmpty(reinterpret_cast<PCUIDLIST_RELATIVE>(ILNext(pidl))) && ILIsEqual(pidl, home);
}

namespace
{
// {5A1F3C2E-8B47-4D6A-9E0C-FA57F11E5E77}: FastFiles' own, never registered
constexpr GUID kSettingsId = { 0x5a1f3c2e, 0x8b47, 0x4d6a, { 0x9e, 0x0c, 0xfa, 0x57, 0xf1, 0x1e, 0x5e, 0x77 } };

#pragma pack(push, 1)
struct RegItem // the desktop's "root regitem" IDList layout (what shell:::{CLSID} parses to)
{
    USHORT cb;
    BYTE type;  // 0x1F: desktop regitem
    BYTE order;
    GUID clsid;
    USHORT terminator;
};
#pragma pack(pop)
}

PCIDLIST_ABSOLUTE SettingsPidl()
{
    static const UniquePidl settings = []
    {
        auto* item = static_cast<RegItem*>(CoTaskMemAlloc(sizeof(RegItem)));
        if (!item)
            return UniquePidl();
        *item = { static_cast<USHORT>(sizeof(RegItem) - sizeof(USHORT)), 0x1F, 0x50, kSettingsId, 0 };
        return UniquePidl(reinterpret_cast<PIDLIST_ABSOLUTE>(item));
    }();
    return settings.get();
}

bool IsSettingsPidl(PCIDLIST_ABSOLUTE pidl)
{
    PCIDLIST_ABSOLUTE settings = SettingsPidl();
    if (!pidl || !settings)
        return false;
    const UINT size = ILGetSize(pidl);
    return size == ILGetSize(settings) && memcmp(pidl, settings, size) == 0;
}

std::wstring LocationName(PCIDLIST_ABSOLUTE pidl)
{
    return IsHomePidl(pidl) ? std::wstring(Tr(L"主页")) : IsSettingsPidl(pidl) ? std::wstring(Tr(L"设置")) : PidlName(pidl, SIGDN_NORMALDISPLAY);
}

HomeItem CopyHomeItem(const HomeItem& item)
{
    HomeItem copy;
    copy.pidl = ClonePidl(item.pidl.get());
    copy.name = item.name;
    copy.path = item.path;
    copy.link = item.link;
    copy.icon = item.icon;
    copy.missing = item.missing;
    copy.folder = item.folder;
    copy.freeBytes = item.freeBytes;
    copy.totalBytes = item.totalBytes;
    copy.time = item.time;
    return copy;
}

namespace
{
// Existence is only checked where it's cheap: local fixed disks. Shares and removable media are listed as-is.
bool MissingLocally(const std::wstring& path)
{
    if (path.size() < 3 || path[1] != L':')
        return false;
    const wchar_t root[] = { path[0], L':', L'\\', 0 };
    const UINT type = GetDriveTypeW(root);
    if (type != DRIVE_FIXED && type != DRIVE_RAMDISK)
        return false;
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}
}

std::vector<HomeItem> ScanRecentFiles(size_t max)
{
    std::vector<HomeItem> out;
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY, nullptr, &folder)))
        return out;
    const std::wstring dir = folder;
    CoTaskMemFree(folder);

    struct Link { std::wstring path; FILETIME time; };
    std::vector<Link> links;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW((dir + L"\\*.lnk").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            links.push_back({ dir + L"\\" + fd.cFileName, fd.ftLastWriteTime });
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) { return CompareFileTime(&a.time, &b.time) > 0; });

    ComPtr<IShellLinkW> shellLink;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) || FAILED(shellLink.As(&file)))
        return out;
    std::vector<std::wstring> seen;
    for (const Link& link : links)
    {
        if (out.size() >= max)
            break;
        // GetPath reads the target stored in the link; nothing is resolved, so offline shares cost nothing
        wchar_t target[MAX_PATH * 2]{};
        WIN32_FIND_DATAW data{};
        if (FAILED(file->Load(link.path.c_str(), STGM_READ)) ||
            shellLink->GetPath(target, ARRAYSIZE(target), &data, SLGP_RAWPATH) != S_OK || !target[0])
            continue;
        std::wstring path = target;
        wchar_t expanded[MAX_PATH * 2];
        if (const DWORD n = ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded)); n > 0 && n <= ARRAYSIZE(expanded))
            path = expanded;
        // Files' recent widget lists files; folders already have the sidebar
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || MissingLocally(path))
            continue;
        std::wstring key = path;
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(std::move(key));

        HomeItem item;
        item.path = path;
        item.link = link.path;
        item.time = link.time;
        const size_t slash = path.find_last_of(L'\\');
        item.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            item.icon = sfi.iIcon;
        out.push_back(std::move(item));
    }
    return out;
}

HomeTagGroup CopyTagGroup(const HomeTagGroup& group)
{
    HomeTagGroup copy{ group.id, group.name, group.color, {} };
    for (const auto& item : group.items)
        copy.items.push_back(CopyHomeItem(item));
    return copy;
}

// Files' FileTagsService.GetAllItemsGroupedByTagAsync + WidgetFileTagCardItem: name, real icon, folder or file
std::vector<HomeTagGroup> ScanTaggedItems(size_t maxPerTag)
{
    std::vector<HomeTagGroup> out;
    for (const FileTag& tag : Tags::List())
    {
        HomeTagGroup group{ tag.id, tag.name, tag.color, {} };
        for (const std::wstring& path : Tags::Items(tag.id, L""))
        {
            if (group.items.size() >= maxPerTag)
                break;
            if (MissingLocally(path))
                continue;
            HomeItem item;
            item.path = path;
            const size_t slash = path.find_last_of(L'\\');
            item.name = slash == std::wstring::npos || slash + 1 == path.size() ? path : path.substr(slash + 1);
            const DWORD attrs = path.size() > 2 && path[1] == L':' ? GetFileAttributesW(path.c_str()) : INVALID_FILE_ATTRIBUTES;
            item.folder = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (attrs != INVALID_FILE_ATTRIBUTES && SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)))
                item.pidl.reset(pidl);
            SHFILEINFOW sfi{};
            if (item.pidl ? SHGetFileInfoW(reinterpret_cast<LPCWSTR>(item.pidl.get()), 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)
                          : SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                item.icon = sfi.iIcon;
            group.items.push_back(std::move(item));
        }
        out.push_back(std::move(group));
    }
    return out;
}