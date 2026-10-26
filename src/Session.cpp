#include "Session.h"
#include "Sandbox.h"
#include "fastfiles_core.h"
#include <sstream>

namespace
{
std::wstring SessionPath()
{
    return AppDataFile(L"session.txt");
}

std::string ToHex(const BYTE* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0xF];
    }
    return out;
}

UniquePidl FromHex(const std::string& hex)
{
    if (hex.size() < 4 || hex.size() % 2 != 0)
        return nullptr;
    const size_t size = hex.size() / 2;
    auto* buffer = static_cast<BYTE*>(CoTaskMemAlloc(size));
    if (!buffer)
        return nullptr;
    for (size_t i = 0; i < size; ++i)
        buffer[i] = static_cast<BYTE>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    auto pidl = UniquePidl(reinterpret_cast<ITEMIDLIST_ABSOLUTE*>(buffer));
    // A valid IDList ends with a zero-length terminator exactly at the end of the buffer
    if (ILGetSize(pidl.get()) != size)
        return nullptr;
    return pidl;
}
}

namespace
{
std::wstring RoamingDir()
{
    if (Sandbox::Active())
        return Sandbox::Dir() + L"legacy\\"; // test mode: a stand-in for %APPDATA%\FastFiles
    std::wstring result;
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &appData)))
    {
        result = std::wstring(appData) + L"\\FastFiles\\";
        CoTaskMemFree(appData);
    }
    return result;
}

bool Writable(const std::wstring& dir)
{
    // A probe file that removes itself on close
    const std::wstring probe = dir + L".write-test-" + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(f);
    return true;
}

// Data next to the program: <exe folder>\data\ (the installer creates it writable for users). A first start copies the
// files an older version kept in %APPDATA%\FastFiles (they stay where they are). No write access there: %APPDATA%.
std::wstring DataDir()
{
    static const std::wstring dir = []
    {
        std::wstring exe(MAX_PATH, L'\0');
        DWORD n = 0;
        while ((n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()))) == exe.size())
            exe.resize(exe.size() * 2);
        exe.resize(n);
        const size_t slash = exe.find_last_of(L'\\');
        const std::wstring roaming = RoamingDir();
        if (slash == std::wstring::npos)
        {
            if (!roaming.empty())
                CreateDirectoryW(roaming.c_str(), nullptr);
            return roaming;
        }
        const std::wstring local = exe.substr(0, slash + 1) + L"data\\";
        CreateDirectoryW(local.c_str(), nullptr);
        if (!Writable(local))
        {
            if (!roaming.empty())
                CreateDirectoryW(roaming.c_str(), nullptr);
            return roaming;
        }
        const bool empty = GetFileAttributesW((local + L"settings.txt").c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW((local + L"session.txt").c_str()) == INVALID_FILE_ATTRIBUTES;
        if (empty && !roaming.empty())
        {
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((roaming + L"*").c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        CopyFileW((roaming + fd.cFileName).c_str(), (local + fd.cFileName).c_str(), TRUE); // never replaces
                while (FindNextFileW(find, &fd));
                FindClose(find);
            }
        }
        return local;
    }();
    return dir;
}
}

std::wstring AppDataFile(const wchar_t* name)
{
    // Test mode: the test's own folder (FASTFILES_DATA_NEXT_TO_EXE=1: the data folder logic, run from a copy of the
    // program in the test's folder, with its stand-in for %APPDATA%)
    static const bool nextToExe = [] { wchar_t v[4]{}; return GetEnvironmentVariableW(L"FASTFILES_DATA_NEXT_TO_EXE", v, 4) && v[0] == L'1'; }();
    if (Sandbox::Active() && !nextToExe)
        return Sandbox::Dir() + name;
    const std::wstring dir = DataDir();
    return dir.empty() ? std::wstring() : dir + name;
}

SessionData LoadSession()
{
    SessionData data;
    const std::wstring path = SessionPath();
    if (path.empty())
        return data;

    const FfBuffer buffer = ff_store_read(reinterpret_cast<const uint16_t*>(path.c_str()), path.size());
    std::istringstream file(buffer.data ? std::string(reinterpret_cast<const char*>(buffer.data), buffer.len) : std::string());
    ff_buffer_free(buffer);
    std::string line;
    while (std::getline(file, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try
        {
            if (key == "window")
            {
                std::istringstream in(value);
                long l, t, r, b;
                UINT show;
                if (in >> l >> t >> r >> b >> show && r > l && b > t)
                {
                    data.normal = { l, t, r, b };
                    data.showCmd = show;
                    data.hasPlacement = true;
                }
            }
            else if (key == "sidebar")
                data.sidebarWidth = std::clamp(std::stoi(value), 120, 800);
            else if (key == "sidebarVisible")
                data.sidebarVisible = value != "0";
            else if (key == "checkboxes")
                data.checkboxes = value == "1";
            else if (key == "toolbarVisible")
                data.toolbarVisible = value != "0";
            else if (key == "infoPane")
            {
                std::istringstream in(value);
                int visible = 0, width = 300, page = 0, height = 300;
                if (in >> visible >> width >> page)
                {
                    data.infoVisible = visible != 0;
                    data.infoWidth = std::clamp(width, 180, 900);
                    data.infoPage = page == 1 ? 1 : 0;
                    if (in >> height) // older sessions have no bottom height
                        data.infoHeight = std::clamp(height, 140, 900);
                }
            }
            else if (key == "sections")
            {
                std::istringstream in(value);
                if (in >> data.hiddenSections >> data.collapsedSections)
                    data.hasSections = true;
            }
            else if (key == "active")
                data.activeTab = std::max(0, std::stoi(value));
            else if (key == "tab")
            {
                if (auto pidl = FromHex(value))
                    data.tabs.push_back({ std::move(pidl) });
            }
            else if (key == "pane" && !data.tabs.empty() && !data.tabs.back().second)
            {
                // Second pane of the tab just read: "<horizontal> <ratio> <focus> <hex>"
                std::istringstream in(value);
                int horizontal = 0, ratio = 500, focus = 0;
                std::string hex;
                if (in >> horizontal >> ratio >> focus >> hex)
                    if (auto pidl = FromHex(hex))
                    {
                        auto& tab = data.tabs.back();
                        tab.second = std::move(pidl);
                        tab.horizontal = horizontal != 0;
                        tab.ratio = std::clamp(ratio, 150, 850);
                        tab.focus = focus == 1 ? 1 : 0;
                    }
            }
            else if (key == "home")
                data.homeCollapsed = static_cast<unsigned>(std::stoul(value)) & 0x1F; // HomeView::SecCount bits
        }
        catch (...)
        {
        }
    }
    return data;
}

void SaveSession(const SessionData& data)
{
    const std::wstring path = SessionPath();
    if (path.empty())
        return;
    std::ostringstream file;
    if (data.hasPlacement)
        file << "window=" << data.normal.left << ' ' << data.normal.top << ' ' << data.normal.right << ' '
             << data.normal.bottom << ' ' << data.showCmd << '\n';
    file << "sidebar=" << data.sidebarWidth << '\n';
    file << "sidebarVisible=" << (data.sidebarVisible ? 1 : 0) << '\n';
    file << "checkboxes=" << (data.checkboxes ? 1 : 0) << '\n';
    file << "toolbarVisible=" << (data.toolbarVisible ? 1 : 0) << '\n';
    file << "infoPane=" << (data.infoVisible ? 1 : 0) << ' ' << data.infoWidth << ' ' << data.infoPage << ' ' << data.infoHeight << '\n';
    if (data.hasSections)
        file << "sections=" << data.hiddenSections << ' ' << data.collapsedSections << '\n';
    file << "home=" << data.homeCollapsed << '\n';
    file << "active=" << data.activeTab << '\n';
    for (const auto& tab : data.tabs)
    {
        if (!tab.pidl)
            continue;
        file << "tab=" << ToHex(reinterpret_cast<const BYTE*>(tab.pidl.get()), ILGetSize(tab.pidl.get())) << '\n';
        if (tab.second)
            file << "pane=" << (tab.horizontal ? 1 : 0) << ' ' << tab.ratio << ' ' << tab.focus << ' '
                 << ToHex(reinterpret_cast<const BYTE*>(tab.second.get()), ILGetSize(tab.second.get())) << '\n';
    }
    const std::string text = file.str();
    ff_store_write(reinterpret_cast<const uint16_t*>(path.c_str()), path.size(), reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

// --- One tab as bytes: [version 1][flags: 1 horizontal, 2 focus on second][ratio u16][size u32][pidl][size u32][pidl or none] ---

namespace
{
// Walks the IDList inside the buffer only (ILGetSize would read past a missing terminator)
bool IsPidl(const BYTE* data, size_t size)
{
    size_t offset = 0;
    while (offset + sizeof(USHORT) <= size)
    {
        USHORT cb = 0;
        memcpy(&cb, data + offset, sizeof(cb));
        if (cb == 0)
            return offset + sizeof(USHORT) == size;
        if (cb < sizeof(USHORT))
            return false;
        offset += cb;
    }
    return false;
}

UniquePidl CopyPidl(const BYTE* data, size_t size)
{
    if (!IsPidl(data, size))
        return nullptr;
    auto* buffer = static_cast<BYTE*>(CoTaskMemAlloc(size));
    if (!buffer)
        return nullptr;
    memcpy(buffer, data, size);
    return UniquePidl(reinterpret_cast<ITEMIDLIST_ABSOLUTE*>(buffer));
}

void AppendPidl(std::vector<BYTE>& out, PCIDLIST_ABSOLUTE pidl)
{
    const uint32_t size = pidl ? ILGetSize(pidl) : 0;
    const size_t at = out.size();
    out.resize(at + sizeof(size) + size);
    memcpy(out.data() + at, &size, sizeof(size));
    if (size)
        memcpy(out.data() + at + sizeof(size), pidl, size);
}
}

std::vector<BYTE> EncodeTab(const SessionData::TabState& tab)
{
    std::vector<BYTE> out;
    if (!tab.pidl)
        return out;
    const uint16_t ratio = static_cast<uint16_t>(std::clamp(tab.ratio, 0, 1000));
    out.push_back(1);
    out.push_back(static_cast<BYTE>((tab.horizontal ? 1 : 0) | (tab.second && tab.focus == 1 ? 2 : 0)));
    out.resize(out.size() + sizeof(ratio));
    memcpy(out.data() + 2, &ratio, sizeof(ratio));
    AppendPidl(out, tab.pidl.get());
    AppendPidl(out, tab.second.get());
    return out;
}

bool DecodeTab(const BYTE* data, size_t size, SessionData::TabState& tab)
{
    if (!data || size < 4 || data[0] != 1)
        return false;
    uint16_t ratio = 500;
    memcpy(&ratio, data + 2, sizeof(ratio));
    size_t offset = 4;
    UniquePidl panes[2];
    for (auto& pane : panes)
    {
        uint32_t length = 0;
        if (offset + sizeof(length) > size)
            return false;
        memcpy(&length, data + offset, sizeof(length));
        offset += sizeof(length);
        if (length > size - offset)
            return false;
        if (length)
        {
            pane = CopyPidl(data + offset, length);
            if (!pane)
                return false;
        }
        offset += length;
    }
    if (!panes[0])
        return false;
    tab.pidl = std::move(panes[0]);
    tab.second = std::move(panes[1]);
    tab.horizontal = (data[1] & 1) != 0;
    tab.focus = tab.second && (data[1] & 2) ? 1 : 0;
    tab.ratio = std::clamp<int>(ratio, 100, 900);
    return true;
}

std::wstring BytesToHex(const std::vector<BYTE>& bytes)
{
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (BYTE b : bytes)
    {
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

std::vector<BYTE> HexToBytes(const std::wstring& hex)
{
    auto nibble = [](wchar_t c) -> int
    {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    std::vector<BYTE> out;
    if (hex.size() % 2 != 0)
        return out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        out.push_back(static_cast<BYTE>(hi << 4 | lo));
    }
    return out;
}