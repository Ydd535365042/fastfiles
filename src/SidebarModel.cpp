// Sidebar data: pins (Rust core), device scan (worker thread), rows, location matching, pin operations.
#include "Sidebar.h"
#include "Drives.h"
#include "Lang.h"
#include "Sandbox.h"
#include "Session.h"
#include "Tags.h"
#include "fastfiles_core.h"
#include <sddl.h>
#include <thread>

struct Sidebar::DevicePayload
{
    unsigned generation = 0;
    bool final = false; // false: replaces device entries; true: appends and ends the scan
    std::vector<Entry> entries;
};

struct Sidebar::ResolvePayload
{
    UniquePidl pidl;
    OpenMode mode = OpenMode::Here;
    std::wstring path;
};

struct Sidebar::ChildrenPayload
{
    struct Child
    {
        std::wstring path, name;
        bool hasChildren = false, hidden = false;
        int icon = -1;
    };
    unsigned generation = 0;
    std::wstring key;   // lowercase folder that was enumerated
    bool icons = false; // second pass: real icons for children already posted
    std::vector<Child> children;
};

namespace
{
const uint16_t* U16(const std::wstring& s) { return reinterpret_cast<const uint16_t*>(s.c_str()); }

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// Pin key form: trailing backslash trimmed except on drive roots (matches core/src/pins.rs)
std::wstring Key(std::wstring s)
{
    while (s.size() > 1 && s.back() == L'\\' && !(s.size() == 3 && s[1] == L':'))
        s.pop_back();
    return s;
}

std::wstring LastComponent(const std::wstring& path)
{
    const std::wstring trimmed = Key(path);
    const size_t slash = trimmed.find_last_of(L'\\');
    return slash == std::wstring::npos || slash + 1 >= trimmed.size() ? trimmed : trimmed.substr(slash + 1);
}

int FolderIcon()
{
    SHFILEINFOW sfi{};
    SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    return sfi.iIcon;
}
bool FillFromPidl(Sidebar::Entry& e, UniquePidl pidl)
{
    if (!pidl)
        return false;
    e.name = PidlName(pidl.get(), SIGDN_NORMALDISPLAY);
    e.parsing = Key(PidlName(pidl.get(), SIGDN_DESKTOPABSOLUTEPARSING));
    e.icon = PidlIcon(pidl.get());
    e.pidl = std::move(pidl);
    return true;
}

UniquePidl Parse(const std::wstring& path)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    return UniquePidl(SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) ? pidl : nullptr);
}

// Parsing these can touch the network or wait for media; resolve them off the UI thread when clicked
bool IsSlowPath(const std::wstring& path)
{
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
        return true;
    if (path.size() >= 2 && path[1] == L':')
    {
        const wchar_t root[] = { path[0], L':', L'\\', 0 };
        const UINT type = GetDriveTypeW(root);
        return type != DRIVE_FIXED && type != DRIVE_RAMDISK;
    }
    return false;
}

std::wstring RegString(HKEY key, const wchar_t* sub, const wchar_t* value)
{
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(key, sub, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
        return {};
    return buffer;
}

std::vector<std::wstring> RegSubkeys(HKEY root, const wchar_t* path)
{
    std::vector<std::wstring> out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i)
    {
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        out.emplace_back(name, len);
    }
    RegCloseKey(key);
    return out;
}
// --- Tree view (Files' FolderHelpers.HasSubfolders / EnumerateSubfolders) ---

std::wstring Pattern(const std::wstring& folder)
{
    return folder + (!folder.empty() && folder.back() == L'\\' ? L"*" : L"\\*");
}

std::wstring Join(const std::wstring& folder, const std::wstring& name)
{
    return folder + (!folder.empty() && folder.back() == L'\\' ? L"" : L"\\") + name;
}

bool IsDots(const wchar_t* name)
{
    return name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

bool HasSubfolders(const std::wstring& folder)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(Pattern(folder).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    do
        found = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !IsDots(data.cFileName);
    while (!found && FindNextFileW(find, &data));
    FindClose(find);
    return found;
}

// Same filters as the file list: hidden items with "show hidden", hidden+system ones only with protected files shown.
// Files caps a level at 1000 folders and sorts them naturally.
std::vector<Sidebar::ChildrenPayload::Child> EnumerateSubfolders(const std::wstring& folder, bool showHidden, bool showProtected, bool showDots)
{
    std::vector<Sidebar::ChildrenPayload::Child> out;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(Pattern(folder).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return out;
    do
    {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || IsDots(data.cFileName))
            continue;
        const bool hidden = (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        const bool system = (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
        if ((hidden && !showHidden) || (hidden && system && !showProtected) || (!showDots && data.cFileName[0] == L'.'))
            continue; // Files' LocationItem: ShowDotFiles
        Sidebar::ChildrenPayload::Child c;
        c.name = data.cFileName;
        c.path = Join(folder, c.name);
        c.hidden = hidden;
        out.push_back(std::move(c));
    } while (out.size() < 1000 && FindNextFileW(find, &data));
    FindClose(find);
    for (auto& c : out)
        c.hasChildren = HasSubfolders(c.path);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

std::wstring ParentFolder(const std::wstring& path)
{
    const std::wstring trimmed = Key(path);
    const size_t slash = trimmed.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return {};
    std::wstring parent = trimmed.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':')
        parent += L'\\';
    return parent;
}

// --- Worker-thread scanners (COM initialized by the caller) ---

void AddDrive(std::vector<Sidebar::Entry>& out, const wchar_t* root, Sidebar::Section section)
{
    Sidebar::Entry e;
    e.section = section;
    if (!FillFromPidl(e, Parse(root)))
        return;
    const UINT type = GetDriveTypeW(root);
    e.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
    e.treePath = root;
    e.hasChildren = HasSubfolders(root);
    ULARGE_INTEGER avail{}, total{}, free{};
    if (GetDiskFreeSpaceExW(root, &avail, &total, &free))
    {
        e.freeBytes = avail.QuadPart;
        e.totalBytes = total.QuadPart;
    }
    out.push_back(std::move(e));
}

void ScanLibraries(std::vector<Sidebar::Entry>& out)
{
    ComPtr<IShellItem> libraries;
    ComPtr<IEnumShellItems> items;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_UsersLibraries, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&libraries))) ||
        FAILED(libraries->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))))
        return;
    for (ComPtr<IShellItem> item; items->Next(1, &item, nullptr) == S_OK; item.Reset())
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        Sidebar::Entry e;
        e.section = Sidebar::SecLibraries;
        if (SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)) && FillFromPidl(e, UniquePidl(pidl)))
            out.push_back(std::move(e));
    }
}

std::wstring CurrentUserSid()
{
    std::wstring result;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return result;
    BYTE buffer[256];
    DWORD size = 0;
    PWSTR text = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &text))
    {
        result = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return result;
}

// OneDrive accounts plus every Cloud Files sync root registered for this user (Dropbox, iCloud, Box, ...)
void ScanCloud(std::vector<Sidebar::Entry>& out)
{
    std::vector<std::wstring> roots;
    for (const auto& account : RegSubkeys(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts"))
        if (auto folder = RegString(HKEY_CURRENT_USER, (L"Software\\Microsoft\\OneDrive\\Accounts\\" + account).c_str(), L"UserFolder"); !folder.empty())
            roots.push_back(folder);
    const std::wstring sid = CurrentUserSid();
    const wchar_t base[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SyncRootManager";
    for (const auto& provider : RegSubkeys(HKEY_LOCAL_MACHINE, base))
    {
        const std::wstring sub = std::wstring(base) + L"\\" + provider + L"\\UserSyncRoots";
        HKEY key = nullptr;
        if (sid.empty() || RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        wchar_t name[256], data[MAX_PATH * 2];
        for (DWORD i = 0;; ++i)
        {
            DWORD nameLen = 256, dataSize = sizeof(data), type = 0;
            if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataSize) != ERROR_SUCCESS)
                break;
            if (type == REG_SZ && _wcsicmp(name, sid.c_str()) == 0)
                roots.emplace_back(data);
        }
        RegCloseKey(key);
    }
    for (const auto& root : roots)
    {
        const DWORD attrs = GetFileAttributesW(root.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (std::any_of(out.begin(), out.end(), [&](const Sidebar::Entry& e) { return _wcsicmp(e.parsing.c_str(), Key(root).c_str()) == 0; }))
            continue;
        Sidebar::Entry e;
        e.section = Sidebar::SecCloud;
        if (!FillFromPidl(e, Parse(root)))
            continue;
        e.treePath = Key(root);
        e.hasChildren = HasSubfolders(e.treePath);
        out.push_back(std::move(e));
    }
}
// Registry only: parsing \\wsl$\<distro> would boot the VM, so these stay lazy until clicked
void ScanWsl(std::vector<Sidebar::Entry>& out)
{
    const wchar_t base[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss";
    const int icon = FolderIcon();
    for (const auto& id : RegSubkeys(HKEY_CURRENT_USER, base))
    {
        const std::wstring name = RegString(HKEY_CURRENT_USER, (std::wstring(base) + L"\\" + id).c_str(), L"DistributionName");
        if (name.empty())
            continue;
        Sidebar::Entry e;
        e.section = Sidebar::SecWsl;
        e.name = name;
        e.lazyPath = L"\\\\wsl$\\" + name;
        e.parsing = e.lazyPath;
        e.icon = icon;
        // Rooted path, so Files makes it expandable; probing for subfolders would boot the VM, so assume it has some
        e.treePath = e.lazyPath;
        e.hasChildren = true;
        out.push_back(std::move(e));
    }
}

void AddNetworkFolder(std::vector<Sidebar::Entry>& out)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    Sidebar::Entry e;
    e.section = Sidebar::SecNetwork;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_NetworkFolder, 0, nullptr, &pidl)) && FillFromPidl(e, UniquePidl(pidl)))
        out.push_back(std::move(e));
}
}

const wchar_t* Sidebar::SectionName(int section)
{
    static const wchar_t* const names[SecCount] = { Tr(L"主页"), Tr(L"固定"), Tr(L"库"), Tr(L"驱动器"), Tr(L"云盘"), Tr(L"网络"), L"WSL", Tr(L"标签") };
    return section >= 0 && section < SecCount ? names[section] : L"";
}

// --- Pins ---

void Sidebar::LoadPins()
{
    if (m_pinFile.empty())
        m_pinFile = AppDataFile(L"pins.txt");
    bool exists = false;
    FfBuffer buffer = ff_pins_load(U16(m_pinFile), m_pinFile.size(), &exists);
    std::string text = buffer.data ? std::string(reinterpret_cast<const char*>(buffer.data), buffer.len) : std::string();
    ff_buffer_free(buffer);
    if (!exists)
    {
        // First run: the folders Files and Explorer pin by default
        for (const KNOWNFOLDERID& id : { FOLDERID_Desktop, FOLDERID_Downloads, FOLDERID_Documents, FOLDERID_Pictures, FOLDERID_Music, FOLDERID_Videos })
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)))
            {
                const std::wstring item = path;
                ff_pins_add(U16(m_pinFile), m_pinFile.size(), U16(item), item.size(), -1);
                CoTaskMemFree(path);
            }
        }
        buffer = ff_pins_load(U16(m_pinFile), m_pinFile.size(), &exists);
        text = buffer.data ? std::string(reinterpret_cast<const char*>(buffer.data), buffer.len) : std::string();
        ff_buffer_free(buffer);
    }

    std::vector<Entry> fresh;
    Entry home;
    home.section = SecHome;
    if (FillFromPidl(home, Parse(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}")))
    {
        home.name = SectionName(SecHome);
        fresh.push_back(std::move(home));
    }
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLen);
    for (size_t start = 0; start < wide.size();)
    {
        size_t end = wide.find(L'\n', start);
        if (end == std::wstring::npos)
            end = wide.size();
        const std::wstring line = wide.substr(start, end - start);
        start = end + 1;
        if (line.empty())
            continue;
        Entry e;
        e.section = SecPinned;
        if (IsSlowPath(line) || !FillFromPidl(e, Parse(line)))
        {
            e.missing = !IsSlowPath(line);
            e.name = LastComponent(line);
            e.icon = FolderIcon();
            e.lazyPath = line;
        }
        e.parsing = Key(line); // the pin key, exactly as stored
        fresh.push_back(std::move(e));
    }
    for (auto& e : m_entries)
        if (e.section != SecHome && e.section != SecPinned)
            fresh.push_back(std::move(e));
    m_entries = std::move(fresh);
    Rebuild();
    MatchLocation();
}
// --- Devices: scanned on a worker so a sleeping network drive or an empty DVD drive can't stall the UI ---

void Sidebar::ScanDevices()
{
    if (m_scanning)
    {
        m_rescan = true;
        return;
    }
    m_scanning = true;
    const unsigned generation = ++m_generation;
    const HWND target = m_hwnd;
    std::thread([target, generation]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr); // no "insert a disk" dialogs
        auto post = [&](DevicePayload* p)
        {
            if (!PostMessageW(target, kMsgDevices, 0, reinterpret_cast<LPARAM>(p)))
                delete p;
        };
        // First batch: everything local; mapped network drives follow because they may hang on an offline server
        auto local = new DevicePayload{ generation, false, {} };
        ScanLibraries(local->entries);
        std::vector<std::wstring> remote;
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if (!(mask & (1u << i)))
                continue;
            const wchar_t root[] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', 0 };
            const UINT type = GetDriveTypeW(root);
            if (type == DRIVE_REMOTE)
                remote.emplace_back(root);
            else if (type != DRIVE_UNKNOWN && type != DRIVE_NO_ROOT_DIR)
                AddDrive(local->entries, root, SecDrives);
        }
        ScanCloud(local->entries);
        AddNetworkFolder(local->entries);
        ScanWsl(local->entries);
        post(local);
        auto rest = new DevicePayload{ generation, true, {} };
        for (const auto& root : remote)
            AddDrive(rest->entries, root.c_str(), SecNetwork);
        post(rest);
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void Sidebar::OnDevices(DevicePayload* payload)
{
    std::unique_ptr<DevicePayload> owned(payload);
    if (payload->generation != m_generation)
        return;
    if (!payload->final)
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [](const Entry& e) { return e.section != SecHome && e.section != SecPinned && e.section != SecTags; }), m_entries.end());
    for (auto& e : payload->entries)
        m_entries.push_back(std::move(e));
    if (payload->final)
    {
        m_scanning = false;
        if (m_rescan)
        {
            m_rescan = false;
            ScanDevices();
        }
    }
    Rebuild();
    MatchLocation();
}
// --- Rows ---

// Files' flat tree (SidebarViewModel.FlatTree): every row is 32px; sections are depth 0, their items depth 1, subfolders
// deeper. A 12px gap precedes a section whose predecessor is expanded (Home counts as expanded).
void Sidebar::Rebuild(bool notify)
{
    const Theme& t = *m_theme;
    const int rowH = t.S(32);
    m_rows.clear();
    int y = t.S(8); // ScrollViewer Padding="0,8,0,0"
    bool prevExpanded = false;
    for (int s = 0; s < SecCount; ++s)
    {
        if (m_hidden & (1u << s))
            continue;
        std::vector<int> members;
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].section == s)
                members.push_back(static_cast<int>(i));
        if (members.empty() && s != SecPinned)
            continue;
        const Section section = static_cast<Section>(s);
        if (prevExpanded)
            y += t.S(12);
        if (s == SecHome)
        {
            // Home is a section-level item with no children
            Row home;
            home.section = section;
            home.entry = members.front();
            home.top = y;
            home.height = rowH;
            m_rows.push_back(home);
            y += rowH;
            prevExpanded = true;
            continue;
        }
        Row header;
        header.header = true;
        header.section = section;
        header.top = y;
        header.height = rowH;
        m_rows.push_back(header);
        y += rowH;
        prevExpanded = !(m_collapsed & (1u << s));
        if (!prevExpanded)
            continue;
        for (int index : members)
        {
            Row item;
            item.section = section;
            item.entry = index;
            item.depth = 1;
            item.top = y;
            item.height = rowH;
            m_rows.push_back(item);
            y += rowH;
            const Entry& e = m_entries[index];
            if (!e.treePath.empty())
                if (auto it = m_trees.find(Lower(e.treePath)); it != m_trees.end() && it->second->expanded)
                    AppendTree(*it->second, section, 2, y);
        }
    }
    const int rows = static_cast<int>(m_rows.size());
    if (m_hot >= rows) m_hot = -1;
    if (m_pressed >= rows) m_pressed = -1;
    if (m_chevronPressed >= rows) m_chevronPressed = -1;
    if (m_ejectPressed >= rows) m_ejectPressed = -1;
    if (m_focus >= rows) m_focus = rows - 1;
    m_tipRow = -1;
    MatchLocation();
    {
        RECT client{};
        if (m_hwnd)
            GetClientRect(m_hwnd, &client);
        m_smooth.ClampTarget(std::max(0, ContentHeight() - static_cast<int>(client.bottom)));
    }
    ScrollTo(m_scroll);
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (notify)
        m_events->OnSidebarChanged();
}

void Sidebar::AppendTree(Node& node, Section section, int depth, int& y)
{
    const int rowH = m_theme->S(32);
    for (auto& child : node.children)
    {
        child->entry.section = section;
        Row row;
        row.section = section;
        row.child = child.get();
        row.depth = depth;
        row.top = y;
        row.height = rowH;
        m_rows.push_back(row);
        y += rowH;
        if (child->expanded)
            AppendTree(*child, section, depth + 1, y);
    }
}

int Sidebar::ContentHeight() const
{
    return m_rows.empty() ? 0 : m_rows.back().top + m_rows.back().height + m_theme->S(16); // MenuItemsHost bottom margin
}

// --- Tree view ---

Sidebar::Entry* Sidebar::EntryOf(int row)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || m_rows[row].header)
        return nullptr;
    const Row& r = m_rows[row];
    return r.child ? &r.child->entry : r.entry >= 0 ? &m_entries[r.entry] : nullptr;
}

const Sidebar::Entry* Sidebar::EntryOf(int row) const
{
    return const_cast<Sidebar*>(this)->EntryOf(row);
}

bool Sidebar::EnsurePidl(Entry& e)
{
    if (!e.pidl && !e.lazyPath.empty())
        e.pidl = Parse(e.lazyPath);
    return e.pidl != nullptr;
}

Sidebar::Node* Sidebar::NodeOf(int row, bool create)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || m_rows[row].header)
        return nullptr;
    const Row& r = m_rows[row];
    if (r.child)
        return r.child;
    if (r.entry < 0 || m_entries[r.entry].treePath.empty())
        return nullptr;
    const std::wstring& path = m_entries[r.entry].treePath;
    const std::wstring key = Lower(path);
    auto it = m_trees.find(key);
    if (it == m_trees.end())
    {
        if (!create)
            return nullptr;
        auto node = std::make_unique<Node>();
        node->key = key;
        node->path = path;
        it = m_trees.emplace(key, std::move(node)).first;
    }
    return it->second.get();
}

// Files: a chevron when the folder has (or may have) subfolders: HasUnrealizedChildren or a non-empty child list
bool Sidebar::Expandable(int row) const
{
    const Entry* e = EntryOf(row);
    if (!e || e->treePath.empty() || m_rows[row].section == SecPinned)
        return false;
    const Node* node = const_cast<Sidebar*>(this)->NodeOf(row, false);
    if (node && node->loaded)
        return !node->children.empty();
    return e->hasChildren;
}

bool Sidebar::Expanded(int row) const
{
    const Node* node = const_cast<Sidebar*>(this)->NodeOf(row, false);
    return node && node->expanded;
}

void Sidebar::SetExpanded(int row, bool expanded)
{
    Node* node = NodeOf(row, true);
    if (!node || node->expanded == expanded)
        return;
    node->expanded = expanded;
    // First expansion loads; re-expanding resyncs (Files stops watching collapsed folders and catches up on reopen)
    if (expanded)
        LoadChildren(*node);
    Rebuild(false);
}

void Sidebar::LoadChildren(Node& node)
{
    if (node.loading)
        return;
    node.loading = true;
    const bool showHidden = m_showHidden, showProtected = m_showHidden && m_showProtected, showDots = m_showDots;
    const unsigned generation = m_treeGeneration;
    const HWND target = m_hwnd;
    std::thread([target, generation, showHidden, showProtected, showDots, key = node.key, path = node.path]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        auto list = new ChildrenPayload{ generation, key, false, EnumerateSubfolders(path, showHidden, showProtected, showDots) };
        // Paint with the generic folder icon first, then the real ones (desktop.ini icons) in a second pass
        auto icons = new ChildrenPayload{ generation, key, true, list->children };
        const int generic = FolderIcon();
        for (auto& c : list->children)
            c.icon = generic;
        if (!PostMessageW(target, kMsgChildren, 0, reinterpret_cast<LPARAM>(list)))
            delete list;
        bool any = false;
        for (auto& c : icons->children)
        {
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(c.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                c.icon = sfi.iIcon;
            any |= c.icon != generic;
        }
        if (!any || !PostMessageW(target, kMsgChildren, 0, reinterpret_cast<LPARAM>(icons)))
            delete icons;
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void Sidebar::OnChildren(ChildrenPayload* payload)
{
    std::unique_ptr<ChildrenPayload> owned(payload);
    if (payload->generation != m_treeGeneration)
        return;
    bool changed = false;
    ForEachNode(payload->key, [&](Node& node)
    {
        if (payload->icons)
        {
            for (const auto& c : payload->children)
                for (auto& child : node.children)
                    if (child->path == c.path && child->entry.icon != c.icon)
                    {
                        child->entry.icon = c.icon;
                        changed = true;
                    }
            return;
        }
        // Merge by path so expanded subfolders keep their state (Files' ResyncSubfoldersAsync)
        std::vector<std::unique_ptr<Node>> fresh;
        fresh.reserve(payload->children.size());
        for (const auto& c : payload->children)
        {
            const std::wstring key = Lower(c.path);
            std::unique_ptr<Node> child;
            for (auto& old : node.children)
                if (old && old->key == key)
                {
                    child = std::move(old);
                    break;
                }
            if (!child)
            {
                child = std::make_unique<Node>();
                child->key = key;
                child->path = c.path;
                child->entry.icon = c.icon;
            }
            Entry& e = child->entry;
            if (e.name != c.name)
                e.pidl.reset();
            e.name = c.name;
            e.parsing = c.path;
            e.lazyPath = c.path;
            e.treePath = c.path;
            e.hasChildren = c.hasChildren;
            e.hidden = c.hidden;
            fresh.push_back(std::move(child));
        }
        node.children = std::move(fresh);
        node.loaded = true;
        node.loading = false;
        changed = true;
    });
    if (changed)
        Rebuild(false);
}

void Sidebar::ForEachNode(const std::wstring& key, const std::function<void(Node&)>& fn)
{
    std::function<void(Node&)> walk = [&](Node& node)
    {
        if (node.key == key)
            fn(node);
        else if (key.size() > node.key.size() && key.compare(0, node.key.size(), node.key) == 0)
            for (auto& child : node.children)
                walk(*child);
    };
    for (auto& [root, node] : m_trees)
        walk(*node);
}

void Sidebar::ReloadNode(Node& node)
{
    if (node.expanded)
    {
        node.loading = false;
        LoadChildren(node);
        for (auto& child : node.children)
            ReloadNode(*child);
    }
    else
    {
        node.children.clear();
        node.loaded = node.loading = false;
    }
}

void Sidebar::SetHiddenFilter(bool showHidden, bool showProtected, bool showDots)
{
    if (showHidden == m_showHidden && showProtected == m_showProtected && showDots == m_showDots)
        return;
    m_showHidden = showHidden;
    m_showProtected = showProtected;
    m_showDots = showDots;
    ReloadTrees();
}

void Sidebar::ReloadTrees()
{
    ++m_treeGeneration;
    for (auto& [key, node] : m_trees)
        ReloadNode(*node);
    Rebuild(false);
}

void Sidebar::Eject(int row)
{
    const Entry* e = EntryOf(row);
    if (!e || !e->pidl)
        return;
    Drives::Eject(m_hwnd, PidlName(e->pidl.get(), SIGDN_FILESYSPATH)); // nothing in test mode
}

int Sidebar::PinnedCount() const
{
    return static_cast<int>(std::count_if(m_entries.begin(), m_entries.end(), [](const Entry& e) { return e.section == SecPinned; }));
}

int Sidebar::PinnedOrdinal(int entry) const
{
    int ordinal = 0;
    for (int i = 0; i < entry && i < static_cast<int>(m_entries.size()); ++i)
        ordinal += m_entries[i].section == SecPinned;
    return ordinal;
}

int Sidebar::DragOrdinal() const
{
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].section == SecPinned && m_entries[i].parsing == m_dragKey)
            return PinnedOrdinal(static_cast<int>(i));
    return -1;
}

void Sidebar::MovePin(int from, int to)
{
    if (from != to && ff_pins_move(U16(m_pinFile), m_pinFile.size(), static_cast<size_t>(from), static_cast<size_t>(to)))
        LoadPins();
}

// --- Tags (Files' sidebar "Tags" section): one entry per tag, in the user's order ---

void Sidebar::LoadTags()
{
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [](const Entry& e) { return e.section == SecTags; }), m_entries.end());
    for (const auto& tag : Tags::List())
    {
        Entry e;
        e.section = SecTags;
        e.name = tag.name;
        e.tagId = tag.id;
        e.tagColor = tag.color;
        e.parsing = L"tag:" + tag.id;
        m_entries.push_back(std::move(e));
    }
    Rebuild();
    MatchLocation();
}

void Sidebar::SetTagLocation(const std::wstring& id)
{
    m_location = Lower(L"tag:" + id);
    MatchLocation();
}

// --- Location: highlight the entry the current folder is in (exact match, else deepest pinned/drive ancestor) ---

void Sidebar::SetLocation(PCIDLIST_ABSOLUTE pidl)
{
    m_location = Lower(Key(PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING)));
    MatchLocation();
}

void Sidebar::MatchLocation()
{
    int best = -1;
    size_t bestLen = 0;
    bool bestExact = false;
    for (size_t i = 0; i < m_rows.size() && !m_location.empty(); ++i)
    {
        const Entry* e = EntryOf(static_cast<int>(i));
        if (!e)
            continue;
        const std::wstring key = Lower(e->parsing);
        if (key.empty())
            continue;
        const bool exact = key == m_location;
        // Only file-system style keys contain their subfolders; shell locations (::{GUID}) must match exactly
        const bool fileSystem = (key.size() >= 2 && key[1] == L':') || key.rfind(L"\\\\", 0) == 0;
        const bool ancestor = !exact && fileSystem && m_location.size() > key.size() && m_location.compare(0, key.size(), key) == 0 &&
            (key.back() == L'\\' || m_location[key.size()] == L'\\');
        if (!exact && !ancestor)
            continue;
        if ((exact && !bestExact) || (exact == bestExact && key.size() > bestLen))
        {
            best = static_cast<int>(i);
            bestLen = key.size();
            bestExact = exact;
        }
    }
    if (best != m_selected)
    {
        m_selected = best;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}
// --- Pin operations (all go through the Rust core, which writes pins.txt atomically) ---

bool Sidebar::IsPinned(PCIDLIST_ABSOLUTE pidl) const
{
    const std::wstring key = Lower(Key(PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING)));
    return !key.empty() && std::any_of(m_entries.begin(), m_entries.end(),
        [&](const Entry& e) { return e.section == SecPinned && Lower(e.parsing) == key; });
}

bool Sidebar::PinKeys(const std::vector<std::wstring>& keys, int index)
{
    bool changed = false;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const std::wstring key = Key(keys[i]);
        if (key.empty())
            continue;
        const int64_t at = index < 0 ? -1 : static_cast<int64_t>(index) + static_cast<int64_t>(i);
        changed |= ff_pins_add(U16(m_pinFile), m_pinFile.size(), U16(key), key.size(), at);
    }
    if (changed)
        LoadPins();
    return changed;
}

bool Sidebar::Pin(PCIDLIST_ABSOLUTE pidl, int index)
{
    return PinKeys({ PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING) }, index);
}

bool Sidebar::Unpin(PCIDLIST_ABSOLUTE pidl)
{
    return UnpinKey(PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING));
}

bool Sidebar::UnpinKey(const std::wstring& raw)
{
    const std::wstring key = Lower(Key(raw));
    bool changed = false;
    for (const auto& e : m_entries)
        if (e.section == SecPinned && Lower(e.parsing) == key)
            changed |= ff_pins_remove(U16(m_pinFile), m_pinFile.size(), U16(e.parsing), e.parsing.size());
    if (changed)
        LoadPins();
    return changed;
}

// --- Lazy entries: parse on a worker, then open ---

void Sidebar::OpenLazy(const std::wstring& name, const std::wstring& path, OpenMode mode)
{
    m_events->OnSidebarNotice(Tr(L"正在连接 ") + name + L"…");
    const HWND target = m_hwnd;
    std::thread([target, mode, path]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        auto payload = new ResolvePayload{ Parse(path), mode, path };
        if (!PostMessageW(target, kMsgResolved, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void Sidebar::OnResolved(ResolvePayload* payload)
{
    std::unique_ptr<ResolvePayload> owned(payload);
    if (!payload->pidl)
    {
        MessageBeep(MB_ICONWARNING);
        m_events->OnSidebarNotice(Tr(L"无法打开 ") + payload->path);
        return;
    }
    m_events->OnSidebarOpen(payload->pidl.get(), payload->mode);
}

// --- Shell change notifications: drives come and go, pinned folders get renamed or deleted ---

void Sidebar::OnShellChange(HANDLE lock, DWORD process)
{
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    HANDLE locked = SHChangeNotification_Lock(lock, process, &pidls, &event);
    if (!locked)
        return;
    if (event & (SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED | SHCNE_NETSHARE | SHCNE_NETUNSHARE |
                 SHCNE_FREESPACE | SHCNE_UPDATEIMAGE))
        m_pendingDevices = true;
    if (event & SHCNE_RENAMEFOLDER && pidls[0] && pidls[1])
    {
        // Keep pins pointing at a renamed folder (or one inside it), in place
        const std::wstring from = Lower(Key(PidlName(pidls[0], SIGDN_DESKTOPABSOLUTEPARSING)));
        const std::wstring to = Key(PidlName(pidls[1], SIGDN_DESKTOPABSOLUTEPARSING));
        if (!from.empty() && !to.empty())
            for (size_t i = 0; i < m_entries.size(); ++i)
            {
                const Entry& e = m_entries[i];
                const std::wstring key = Lower(e.parsing);
                if (e.section != SecPinned || !(key == from || (key.size() > from.size() && key.compare(0, from.size(), from) == 0 && key[from.size()] == L'\\')))
                    continue;
                const std::wstring renamed = to + e.parsing.substr(from.size());
                const int ordinal = PinnedOrdinal(static_cast<int>(i));
                ff_pins_remove(U16(m_pinFile), m_pinFile.size(), U16(e.parsing), e.parsing.size());
                ff_pins_add(U16(m_pinFile), m_pinFile.size(), U16(renamed), renamed.size(), ordinal);
                m_pendingPins = true;
            }
    }
    if (event & SHCNE_RMDIR)
        m_pendingPins = true; // a pinned folder may now be missing
    // Tree view: a folder appeared, vanished or was renamed under an expanded folder (Files' per-folder watcher)
    if ((event & (SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER)) && !m_trees.empty())
        for (int i = 0; i < 2 && pidls[i]; ++i)
        {
            const std::wstring path = PidlName(pidls[i], SIGDN_FILESYSPATH);
            for (const std::wstring& folder : { ParentFolder(path), path })
            {
                const std::wstring key = Lower(Key(folder.size() == 2 ? folder + L"\\" : folder));
                bool known = false;
                if (!key.empty())
                    ForEachNode(key, [&](Node& node) { known |= node.expanded && node.loaded; });
                if (known && std::find(m_pendingTreeReloads.begin(), m_pendingTreeReloads.end(), key) == m_pendingTreeReloads.end())
                    m_pendingTreeReloads.push_back(key);
            }
            if (!(event & SHCNE_RENAMEFOLDER))
                break;
        }
    SHChangeNotification_Unlock(locked);
    if (m_pendingPins || m_pendingDevices || !m_pendingTreeReloads.empty())
        SetTimer(m_hwnd, kRefreshTimer, 400, nullptr);
}
