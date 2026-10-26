#include "Sandbox.h"

namespace
{
std::wstring ReadUtf8(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got)
        bytes.append(buf, got);
    CloseHandle(file);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    return text;
}
}

namespace Sandbox
{
const std::wstring& Dir()
{
    static const std::wstring dir = []
    {
        wchar_t value[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"FASTFILES_DATA", value, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return std::wstring();
        std::wstring d = value;
        if (d.back() != L'\\')
            d += L'\\';
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

bool Active()
{
    return !Dir().empty();
}

bool CopyText(const std::wstring& text)
{
    if (!Active())
        return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
    HANDLE file = CreateFileW((Dir() + L"clipboard.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
        CloseHandle(file);
    }
    return true;
}

bool CopyFiles(const std::vector<std::wstring>& paths)
{
    if (!Active())
        return false;
    std::wstring text = L"copy\n";
    for (const auto& path : paths)
    {
        text += path;
        text += L'\n';
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
    HANDLE file = CreateFileW((Dir() + L"clipboard-files.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
        CloseHandle(file);
    }
    return true;
}

bool PasteFiles(std::vector<std::wstring>& paths, bool& move)
{
    paths.clear();
    move = false;
    if (!Active())
        return false;
    const std::wstring text = ReadUtf8(Dir() + L"clipboard-files.txt");
    size_t start = 0;
    bool first = true;
    while (start < text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
        if (first)
            move = line == L"cut";
        else if (!line.empty())
            paths.push_back(line);
        first = false;
        start = end + 1;
    }
    return true;
}

bool HasPasteFiles()
{
    std::vector<std::wstring> paths;
    bool move = false;
    return PasteFiles(paths, move) && !paths.empty();
}

std::wstring RecycledDir()
{
    if (!Active())
        return {};
    const std::wstring dir = Dir() + L"recycled";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

bool VerbAllowed(const char* verb)
{
    if (!Active() || !verb || IS_INTRESOURCE(verb))
        return true;
    // Renaming, the property sheet and the in-place verbs of our own test files; everything else (open, wallpaper,
    // install, run as administrator, add, share, pin, shell copy/cut to the real clipboard, delete to the real bin) no
    for (const char* ok : { "rename", "properties", "rotate90", "rotate270" })
        if (_stricmp(verb, ok) == 0)
            return true;
    return false;
}

bool VerbAllowed(const wchar_t* verb)
{
    if (!Active())
        return true;
    char narrow[64]{};
    if (!verb || !*verb || !WideCharToMultiByte(CP_ACP, 0, verb, -1, narrow, sizeof(narrow), nullptr, nullptr))
        return false;
    return VerbAllowed(static_cast<const char*>(narrow));
}
}
