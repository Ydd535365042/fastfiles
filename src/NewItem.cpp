#include "NewItem.h"
#include "Lang.h"
#include "Log.h"
#include <intshcut.h>
#include <mutex>
#include <thread>

namespace NewItem
{
namespace
{
std::mutex g_lock;
std::vector<Entry> g_entries;
bool g_ready = false, g_started = false;

std::wstring ReadString(HKEY key, const wchar_t* name)
{
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    std::wstring s(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(s.data()), &bytes) != ERROR_SUCCESS)
        return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    if (type == REG_EXPAND_SZ)
    {
        wchar_t out[MAX_PATH * 2]{};
        if (ExpandEnvironmentStringsW(s.c_str(), out, ARRAYSIZE(out)))
            s = out;
    }
    return s;
}

bool HasValue(HKEY key, const wchar_t* name)
{
    return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// Files' ParseShellNewRegistryEntry
bool Parse(HKEY shellNew, const std::wstring& extension, Entry& e)
{
    static const wchar_t* const markers[] = { L"NullFile", L"Name", L"FileName", L"Command", L"ItemName", L"Data" };
    if (std::none_of(std::begin(markers), std::end(markers), [&](const wchar_t* m) { return HasValue(shellNew, m); }))
        return false;
    e.extension = extension;
    e.templ = ReadString(shellNew, L"FileName");
    e.command = ReadString(shellNew, L"Command");
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(shellNew, L"Data", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0)
    {
        std::vector<BYTE> raw(bytes);
        RegQueryValueExW(shellNew, L"Data", nullptr, nullptr, raw.data(), &bytes);
        if (type == REG_SZ || type == REG_EXPAND_SZ) // Files: Encoding.UTF8.GetBytes(string)
        {
            std::wstring s(reinterpret_cast<const wchar_t*>(raw.data()), bytes / sizeof(wchar_t));
            s.resize(wcsnlen(s.c_str(), s.size()));
            const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            e.data.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), reinterpret_cast<char*>(e.data.data()), n, nullptr, nullptr);
        }
        else
            e.data = std::move(raw);
    }
    return true;
}

// Files' GetShellNewRegistryEntries: the first ShellNew key anywhere under HKCR\.ext
bool Find(HKEY key, const std::wstring& extension, Entry& e, int depth)
{
    wchar_t name[256];
    for (DWORD i = 0;; ++i)
    {
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            return false;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &sub) != ERROR_SUCCESS)
            continue;
        bool found = false;
        if (_wcsicmp(name, L"ShellNew") == 0)
            found = Parse(sub, extension, e);
        else if (depth < 4)
            found = Find(sub, extension, e, depth + 1);
        RegCloseKey(sub);
        if (found || _wcsicmp(name, L"ShellNew") == 0)
            return found;
    }
}

void Describe(Entry& e)
{
    // Files: DisplayType and the list-view thumbnail of a sample "file<ext>"
    SHFILEINFOW info{};
    const std::wstring sample = L"file" + e.extension;
    if (SHGetFileInfoW(sample.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_ICON | SHGFI_LARGEICON))
    {
        e.name = info.szTypeName;
        e.icon = info.hIcon;
    }
    if (e.name.empty())
        e.name = L"file " + e.extension;
}

void Scan()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<Entry> entries;
    wchar_t name[256];
    for (DWORD i = 0;; ++i)
    {
        DWORD len = ARRAYSIZE(name);
        const LONG r = RegEnumKeyExW(HKEY_CLASSES_ROOT, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r != ERROR_SUCCESS || name[0] != L'.')
            continue;
        // Files leaves out library, internet shortcut and shortcut types
        if (_wcsicmp(name, L".library-ms") == 0 || _wcsicmp(name, L".url") == 0 || _wcsicmp(name, L".lnk") == 0)
            continue;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, name, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        Entry e;
        if (Find(key, name, e, 0))
            entries.push_back(std::move(e));
        RegCloseKey(key);
    }
    if (std::none_of(entries.begin(), entries.end(), [](const Entry& e) { return _wcsicmp(e.extension.c_str(), L".txt") == 0; }))
        entries.push_back({ L".txt" });
    for (Entry& e : entries)
        Describe(e);
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
    {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_entries = std::move(entries);
        g_ready = true;
    }
    if (SUCCEEDED(co))
        CoUninitialize();
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// CreationCollisionOption.GenerateUniqueName: "name (2).ext"
std::wstring Unique(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext)
{
    std::wstring path = dir + L"\\" + stem + ext;
    for (int n = 2; Exists(path) && n < 10000; ++n)
        path = dir + L"\\" + stem + L" (" + std::to_wstring(n) + L")" + ext;
    return path;
}

void SplitName(const std::wstring& name, std::wstring& stem, std::wstring& ext)
{
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0)
    {
        stem = name;
        ext.clear();
        return;
    }
    stem = name.substr(0, dot);
    ext = name.substr(dot);
}

void Notify(const std::wstring& path, bool folder)
{
    SHChangeNotify(folder ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSH, path.c_str(), nullptr);
}

bool FullyQualified(const std::wstring& p)
{
    return (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
        (p.size() >= 3 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/'));
}

// Files' IsValidAbsolutePath: exists, fully qualified, not a drive root
bool ValidAbsolute(const std::wstring& p)
{
    if (!FullyQualified(p) || !Exists(p))
        return false;
    std::wstring trimmed = p;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        trimmed.pop_back();
    return !(trimmed.size() <= 3 && trimmed[1] == L':');
}

std::wstring FullPath(const std::wstring& p)
{
    wchar_t out[MAX_PATH * 4]{};
    const DWORD n = GetFullPathNameW(p.c_str(), ARRAYSIZE(out), out, nullptr);
    return n > 0 && n < ARRAYSIZE(out) ? std::wstring(out) : p;
}

// Files' PathHelpers.TryGetFullPath (where.exe): a program found on PATH, with an executable extension
bool OnPath(const std::wstring& name, std::wstring& full)
{
    if (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return false;
    wchar_t pathext[512]{};
    if (!GetEnvironmentVariableW(L"PATHEXT", pathext, ARRAYSIZE(pathext)))
        wcscpy_s(pathext, L".COM;.EXE;.BAT;.CMD");
    std::vector<std::wstring> exts;
    for (wchar_t* ctx = nullptr, *t = wcstok_s(pathext, L";", &ctx); t; t = wcstok_s(nullptr, L";", &ctx))
        exts.emplace_back(t);
    auto executable = [&](const std::wstring& p) { const wchar_t* e = PathFindExtensionW(p.c_str()); return std::any_of(exts.begin(), exts.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), e) == 0; }); };
    wchar_t found[MAX_PATH]{};
    if (*PathFindExtensionW(name.c_str()) && executable(name) && SearchPathW(nullptr, name.c_str(), nullptr, MAX_PATH, found, nullptr))
    {
        full = found;
        return true;
    }
    for (const auto& e : exts)
        if (SearchPathW(nullptr, (name + e).c_str(), nullptr, MAX_PATH, found, nullptr))
        {
            full = found;
            return true;
        }
    return false;
}

// new Uri(text).IsWellFormedOriginalString(), for what people type here: "scheme:rest" without spaces or backslashes
bool WellFormedUri(const std::wstring& s, std::wstring& host)
{
    host.clear();
    const size_t colon = s.find(L':');
    if (colon == std::wstring::npos || colon < 2 || !iswalpha(s[0]) || colon + 1 >= s.size())
        return false;
    for (size_t i = 0; i < colon; ++i)
        if (!(iswalnum(s[i]) || s[i] == L'+' || s[i] == L'-' || s[i] == L'.'))
            return false;
    if (s.find_first_of(L" \t\\\"<>") != std::wstring::npos)
        return false;
    if (s.compare(colon + 1, 2, L"//") == 0)
    {
        std::wstring authority = s.substr(colon + 3, s.find_first_of(L"/?#", colon + 3) - (colon + 3));
        if (const size_t at = authority.rfind(L'@'); at != std::wstring::npos)
            authority = authority.substr(at + 1);
        if (const size_t port = authority.find(L':'); port != std::wstring::npos)
            authority = authority.substr(0, port);
        host = authority;
    }
    return true;
}

std::wstring Join(const std::vector<std::wstring>& parts, size_t from)
{
    std::wstring out;
    for (size_t i = from; i < parts.size(); ++i)
        out += parts[i] + L" ";
    return out;
}
}

void StartScan()
{
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started)
            return;
        g_started = true;
    }
    std::thread(Scan).detach();
}

bool Ready()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_ready;
}

std::vector<Entry> Entries()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_entries;
}

const Entry* EntryFor(const std::vector<Entry>& entries, const std::wstring& extension)
{
    if (extension.empty())
        return nullptr;
    for (const Entry& e : entries)
        if (_wcsicmp(e.extension.c_str(), extension.c_str()) == 0)
            return &e;
    return nullptr;
}

bool ValidName(const std::wstring& name)
{
    if (name.find_first_not_of(L" \t\r\n") == std::wstring::npos || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return false;
    static const wchar_t* const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
        L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    for (const wchar_t* r : reserved)
    {
        const size_t n = wcslen(r);
        if (name.size() >= n && _wcsnicmp(name.c_str(), r, n) == 0 && (name.size() == n || name[n] == L'.'))
            return false;
    }
    return true;
}

std::wstring FilterName(const std::wstring& name)
{
    std::wstring out;
    for (wchar_t c : name)
        if (!wcschr(L"\\/:*?\"<>|", c))
            out += c;
    return out;
}

std::wstring CreateFolder(const std::wstring& dir, const std::wstring& name, bool* denied)
{
    const std::wstring path = Unique(dir, name, L"");
    if (!CreateDirectoryW(path.c_str(), nullptr))
    {
        const DWORD error = GetLastError();
        if (denied)
            *denied = error == ERROR_ACCESS_DENIED;
        Log::Warning(L"Failed to create folder " + path + L" (" + std::to_wstring(error) + L")");
        return {};
    }
    Notify(path, true);
    return path;
}

std::wstring CreateNewFile(const std::wstring& dir, const std::wstring& fileName, const std::vector<Entry>& entries, bool* denied)
{
    std::wstring stem, ext;
    SplitName(fileName, stem, ext);
    const std::wstring path = Unique(dir, stem, ext);
    // Files: the ShellNew entry of the final extension decides (template copy + Data), else an empty file
    const Entry* entry = EntryFor(entries, ext);
    bool ok = false;
    DWORD error = 0;
    if (entry && !entry->templ.empty())
    {
        std::wstring templ = entry->templ;
        if (!FullyQualified(templ)) // a bare name lives in one of the template folders
            for (const KNOWNFOLDERID& id : { FOLDERID_Templates, FOLDERID_CommonTemplates })
            {
                PWSTR folder = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder)))
                {
                    const std::wstring candidate = std::wstring(folder) + L"\\" + entry->templ;
                    CoTaskMemFree(folder);
                    if (Exists(candidate))
                    {
                        templ = candidate;
                        break;
                    }
                }
            }
        ok = CopyFileW(templ.c_str(), path.c_str(), TRUE) != FALSE;
        error = ok ? 0 : GetLastError();
        if (ok)
            SetFileAttributesW(path.c_str(), GetFileAttributesW(path.c_str()) & ~FILE_ATTRIBUTE_READONLY);
        else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            Log::Warning(L"ShellNew template not found: " + templ);
    }
    if (!ok && error != ERROR_ACCESS_DENIED)
    {
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok = h != INVALID_HANDLE_VALUE;
        error = ok ? 0 : GetLastError();
        if (ok)
            CloseHandle(h);
    }
    if (ok && entry && !entry->data.empty())
    {
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, entry->data.data(), static_cast<DWORD>(entry->data.size()), &written, nullptr);
            CloseHandle(h);
        }
    }
    if (!ok)
    {
        if (denied)
            *denied = error == ERROR_ACCESS_DENIED;
        Log::Warning(L"Failed to create file " + path + L" (" + std::to_wstring(error) + L")");
        return {};
    }
    Notify(path, false);
    return path;
}

Target ParseTarget(const std::wstring& text)
{
    Target t;
    const size_t a = text.find_first_not_of(L" \t\r\n"), b = text.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos)
        return t;
    const std::wstring trimmed = text.substr(a, b - a + 1);
    std::wstring host;
    auto asUri = [&](const std::wstring& s)
    {
        t.exists = false;
        t.valid = WellFormedUri(s, host);
        t.fullPath = s;
        t.arguments.clear();
    };
    auto directory = [&] { return (GetFileAttributesW(t.fullPath.c_str()) & FILE_ATTRIBUTE_DIRECTORY) && Exists(t.fullPath); };
    if (trimmed[0] == L'"')
    {
        const size_t end = trimmed.find(L'"', 1);
        if (end == std::wstring::npos)
            return t;
        const std::wstring quoted = trimmed.substr(1, end - 1), rest = trimmed.substr(end + 1);
        std::wstring found;
        if (ValidAbsolute(quoted))
        {
            t.valid = t.exists = true;
            t.fullPath = FullPath(quoted);
            t.arguments = directory() ? std::wstring() : rest;
        }
        else if (OnPath(quoted, found))
        {
            t.valid = t.exists = true;
            t.fullPath = found;
            t.arguments = rest;
        }
        else
            asUri(quoted);
    }
    else
    {
        // Files splits at every space; the rest becomes the arguments ("a b " - trailing space trimmed below)
        std::vector<std::wstring> parts;
        for (size_t s = 0;;)
        {
            const size_t sp = trimmed.find(L' ', s);
            parts.push_back(trimmed.substr(s, sp == std::wstring::npos ? std::wstring::npos : sp - s));
            if (sp == std::wstring::npos)
                break;
            s = sp + 1;
        }
        const std::wstring& first = parts.front();
        std::wstring found;
        if (ValidAbsolute(first))
        {
            t.valid = t.exists = true;
            t.fullPath = FullPath(first);
            t.arguments = directory() ? std::wstring() : Join(parts, 1);
        }
        else if (ValidAbsolute(trimmed))
        {
            t.valid = t.exists = true;
            t.fullPath = FullPath(trimmed);
        }
        else if (OnPath(first, found))
        {
            t.valid = t.exists = true;
            t.fullPath = found;
            t.arguments = Join(parts, 1);
        }
        else
            asUri(trimmed);
    }
    while (!t.arguments.empty() && t.arguments.back() == L' ')
        t.arguments.pop_back();
    while (!t.arguments.empty() && t.arguments.front() == L' ')
        t.arguments.erase(0, 1);
    // AutoFillName: the target's file name, or the URL's host
    if (t.exists)
    {
        std::wstring p = t.fullPath;
        while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/'))
            p.pop_back();
        t.autoName = p.substr(p.find_last_of(L"\\/") + 1);
    }
    else if (t.valid)
        t.autoName = host;
    return t;
}

std::wstring ShortcutName(const std::wstring& name)
{
    // Files' GetShortcutNamingPreference: Explorer's NamingTemplates\ShortcutNameTemplate ("%s.lnk" style), else "{0} - 快捷方式"
    wchar_t value[260]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\NamingTemplates", L"ShortcutNameTemplate",
            RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS)
    {
        std::wstring suffix = value;
        while (!suffix.empty() && (suffix.front() == L'"' || suffix.front() == L'%' || suffix.front() == L's'))
            suffix.erase(0, 1);
        while (!suffix.empty() && suffix.back() == L'"')
            suffix.pop_back();
        const size_t lnk = suffix.size() >= 4 ? suffix.size() - 4 : std::wstring::npos;
        if (lnk != std::wstring::npos && _wcsicmp(suffix.c_str() + lnk, L".lnk") == 0)
            suffix.resize(lnk); // the extension is added by the caller
        return name + suffix;
    }
    return name + Tr(L" - 快捷方式");
}

std::wstring CreateShortcut(const std::wstring& dir, const std::wstring& name, const Target& target, bool* denied)
{
    const std::wstring path = Unique(dir, ShortcutName(name), target.exists ? L".lnk" : L".url");
    HRESULT hr = E_FAIL;
    ComPtr<IPersistFile> file;
    if (target.exists)
    {
        ComPtr<IShellLinkW> link;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (SUCCEEDED(hr))
            hr = link->SetPath(target.fullPath.c_str());
        if (SUCCEEDED(hr) && !target.arguments.empty())
            hr = link->SetArguments(target.arguments.c_str());
        if (SUCCEEDED(hr))
            hr = link.As(&file);
    }
    else
    {
        ComPtr<IUniformResourceLocatorW> url;
        hr = CoCreateInstance(CLSID_InternetShortcut, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&url));
        if (SUCCEEDED(hr))
            hr = url->SetURL(target.fullPath.c_str(), 0);
        if (SUCCEEDED(hr))
            hr = url.As(&file);
    }
    if (SUCCEEDED(hr))
        hr = file->Save(path.c_str(), TRUE);
    if (FAILED(hr))
    {
        if (denied)
            *denied = hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        Log::Warning(L"Failed to create shortcut " + path);
        return {};
    }
    Notify(path, false);
    return path;
}

HBITMAP IconBitmap(HICON icon, int size)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp)
        return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    DeleteDC(dc);
    // Icons without an alpha channel come out with alpha 0: make their drawn pixels opaque
    auto* px = static_cast<DWORD*>(bits);
    const int n = size * size;
    if (std::none_of(px, px + n, [](DWORD p) { return (p >> 24) != 0; }))
        for (int i = 0; i < n; ++i)
            if (px[i] & 0x00FFFFFF)
                px[i] |= 0xFF000000;
    return bmp;
}
}
