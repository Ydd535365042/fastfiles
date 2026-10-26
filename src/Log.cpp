#include "Log.h"
#include "Common.h"
#include "Session.h"
#include <mutex>

namespace
{
std::mutex g_lock;

bool Elevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    if (token)
        CloseHandle(token);
    return ok;
}

std::string Utf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

const wchar_t* LevelName(Log::Level level)
{
    switch (level)
    {
    case Log::Level::Information: return L"Information";
    case Log::Level::Warning: return L"Warning";
    case Log::Level::Error: return L"Error";
    default: return L"Critical";
    }
}

bool Ends(wchar_t c) { return wcschr(L":*?,\"'<>|\r\n", c) != nullptr; }

// LogPathHelper.RedactPathCore: every name becomes %Private%, the drive and the last extension stay
std::wstring RedactPath(const std::wstring& path)
{
    const size_t last = path.find_last_of(L"\\/");
    std::wstring out;
    for (size_t i = 0; i < path.size();)
    {
        if (path[i] == L'\\' || path[i] == L'/')
        {
            out += path[i++];
            continue;
        }
        const size_t end = std::min(path.find_first_of(L"\\/", i), path.size());
        const std::wstring segment = path.substr(i, end - i);
        if (i == 0 && segment.size() == 2 && segment[1] == L':')
            out += segment;
        else
        {
            out += L"%Private%";
            if (last == std::wstring::npos || i > last)
                if (const size_t dot = segment.rfind(L'.'); dot != std::wstring::npos && dot > 0)
                    out += segment.substr(dot);
        }
        i = end;
    }
    return out;
}

void Append(Log::Level level, const std::wstring& message)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t stamp[40]{};
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%04u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        static_cast<unsigned>(t.wMilliseconds) * 10);
    const std::string line = Utf8(std::wstring(stamp) + L"|" + LevelName(level) + L"|" + Log::Sanitize(message) + L"\r\n");
    const std::wstring path = Log::Path();
    if (path.empty())
        return;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    // Files: App.UnhandledException -> LogError; then Windows Error Reporting carries on
    wchar_t text[160]{};
    const auto* record = info ? info->ExceptionRecord : nullptr;
    HMODULE module = nullptr;
    if (record)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(record->ExceptionAddress), &module);
    wchar_t name[MAX_PATH]{};
    if (module)
        GetModuleFileNameW(module, name, MAX_PATH);
    swprintf_s(text, L"An unhandled error occurred: 0x%08lX at %s+0x%llX", record ? record->ExceptionCode : 0UL,
        module ? PathFindFileNameW(name) : L"?",
        record && module ? static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) - reinterpret_cast<ULONG_PTR>(module)) : 0ULL);
    if (g_lock.try_lock()) // never wait: the crash may have happened while writing
    {
        Append(Log::Level::Critical, text);
        g_lock.unlock();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}

namespace Log
{
std::wstring Path()
{
    return AppDataFile(Elevated() ? L"debug_privileged.log" : L"debug_unprivileged.log");
}

std::wstring Sanitize(const std::wstring& message)
{
    // Absolute drive ("C:\...") or UNC ("\\server\...") paths: names replaced, the rest of the text kept
    std::wstring out;
    for (size_t i = 0; i < message.size();)
    {
        auto ascii = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); };
        const bool boundary = i == 0 || !(ascii(message[i - 1]) || (message[i - 1] >= L'0' && message[i - 1] <= L'9') || message[i - 1] == L'_');
        const bool drive = boundary && i + 2 < message.size() && ascii(message[i]) && message[i + 1] == L':' && message[i + 2] == L'\\';
        const bool unc = boundary && i + 2 < message.size() && message[i] == L'\\' && message[i + 1] == L'\\' && message[i + 2] != L'?';
        if (!drive && !unc)
        {
            out += message[i++];
            continue;
        }
        size_t end = i + 2; // past "C:" / "\\"
        while (end < message.size() && !Ends(message[end]))
            ++end;
        std::wstring path = message.substr(i, end - i);
        size_t trimmed = path.find_last_not_of(L" .,;)]");
        path.resize(trimmed == std::wstring::npos ? 0 : trimmed + 1);
        out += path.size() <= 3 ? path : RedactPath(path);
        i += path.empty() ? 1 : path.size();
    }
    // The user name anywhere else (redirected profiles, messages)
    wchar_t user[256]{};
    DWORD n = ARRAYSIZE(user);
    if (GetUserNameW(user, &n) && user[0])
        for (size_t p = 0; (p = out.find(user, p)) != std::wstring::npos;)
        {
            out.replace(p, wcslen(user), L"%UserName%");
            p += 10;
        }
    return out;
}

void Write(Level level, const std::wstring& message)
{
    std::lock_guard<std::mutex> hold(g_lock);
    Append(level, message);
}

void Start()
{
    SetUnhandledExceptionFilter(OnCrash);
    // FileLogger.PurgeLogs(100)
    std::lock_guard<std::mutex> hold(g_lock);
    const std::wstring path = Path();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string text(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 4 << 20)), '\0');
    DWORD read = 0;
    if (size.QuadPart > static_cast<LONGLONG>(text.size()))
    {
        LARGE_INTEGER from{};
        from.QuadPart = size.QuadPart - static_cast<LONGLONG>(text.size());
        SetFilePointerEx(file, from, nullptr, FILE_BEGIN);
    }
    ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    text.resize(read);
    size_t lines = 0, cut = text.size();
    for (size_t p = text.size(); p > 0; --p)
        if (text[p - 1] == '\n' && p != text.size() && ++lines == 100)
        {
            cut = p;
            break;
        }
    if (lines < 100)
        return;
    HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(out, text.data() + cut, static_cast<DWORD>(text.size() - cut), &written, nullptr);
    CloseHandle(out);
}
}
