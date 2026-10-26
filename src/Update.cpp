#include "Update.h"
#include "Lang.h"
#include "Sandbox.h"
#include "Settings.h"
#include <winhttp.h>
#include <functional>
#include <thread>

namespace Update
{
namespace
{
std::wstring ExePath()
{
    wchar_t path[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return n > 0 && n < ARRAYSIZE(path) ? std::wstring(path) : std::wstring();
}

std::wstring FolderOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Env(const wchar_t* name)
{
    wchar_t value[256]{};
    const DWORD n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return n > 0 && n < ARRAYSIZE(value) ? std::wstring(value) : std::wstring();
}

std::vector<int> Parts(const std::wstring& version)
{
    std::vector<int> parts;
    size_t i = !version.empty() && (version[0] == L'v' || version[0] == L'V') ? 1 : 0;
    while (i < version.size() && iswdigit(version[i]))
    {
        int n = 0;
        while (i < version.size() && iswdigit(version[i]))
            n = std::min(n * 10 + (version[i++] - L'0'), 1000000);
        parts.push_back(n);
        if (i < version.size() && version[i] == L'.')
            ++i;
        else
            break;
    }
    return parts;
}

// Where downloads go: next to the exe for a portable swap (same volume, so the rename can't fail half way),
// a folder of our own under %TEMP% for the setup; the test's data folder in test mode
std::wstring DownloadDir()
{
    if (Sandbox::Active())
        return Sandbox::Dir() + L"update";
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"FastFiles-update";
}

void Post(HWND notify, Result* r)
{
    if (!PostMessageW(notify, kMsgUpdate, 0, reinterpret_cast<LPARAM>(r)))
        delete r;
}

Result* Fail(std::wstring error)
{
    auto* r = new Result;
    r->kind = Result::Kind::Failed;
    r->error = std::move(error);
    return r;
}

// One GET through WinHTTP (follows the redirect GitHub sends for release assets). Writes to `out` when given,
// collects the body in `body` otherwise. status: the HTTP status, 0 when the request never got an answer.
struct Http
{
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    ~Http()
    {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

bool Get(const std::wstring& url, const wchar_t* accept, int& status, std::string* body, HANDLE out,
    const std::function<void(unsigned long long, unsigned long long)>& progress)
{
    status = 0;
    URL_COMPONENTS parts{ sizeof(parts) };
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    Http h;
    // The system proxy: WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY is Windows 8.1+, the default proxy works everywhere
    h.session = WinHttpOpen(L"FastFiles/" FASTFILES_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session)
        h.session = WinHttpOpen(L"FastFiles/" FASTFILES_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session)
        return false;
    // Windows 8.0 doesn't turn TLS 1.2 on by itself (GitHub requires it)
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(h.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(h.session, 15000, 15000, 30000, 30000);
    h.connect = WinHttpConnect(h.session, host, parts.nPort, 0);
    if (!h.connect)
        return false;
    const wchar_t* types[] = { accept, nullptr };
    h.request = WinHttpOpenRequest(h.connect, L"GET", path, nullptr, WINHTTP_NO_REFERER, types, WINHTTP_FLAG_SECURE);
    if (!h.request)
        return false;
    if (!WinHttpSendRequest(h.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h.request, nullptr))
        return false;
    DWORD code = 0, size = sizeof(code);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
    status = static_cast<int>(code);
    if (code != 200)
        return false;
    wchar_t length[32]{};
    DWORD lengthSize = sizeof(length);
    unsigned long long total = 0, done = 0;
    if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &lengthSize, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(length, nullptr, 10);
    std::vector<char> buffer(64 * 1024);
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(h.request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            return false;
        if (read == 0)
            break;
        if (out)
        {
            DWORD written = 0;
            if (!WriteFile(out, buffer.data(), read, &written, nullptr) || written != read)
                return false;
        }
        else if (body)
        {
            body->append(buffer.data(), read);
            if (body->size() > 4 * 1024 * 1024)
                return false; // a release description is never this big
        }
        done += read;
        if (progress)
            progress(done, total);
    }
    return total == 0 || done == total;
}

// The string value of the first "key": "..." at or after `from` (JSON escapes undone); npos-safe
std::wstring JsonString(const std::string& json, const char* key, size_t from = 0, size_t* end = nullptr)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t i = json.find(needle, from);
    if (i == std::string::npos)
        return {};
    i = json.find(':', i + needle.size());
    if (i == std::string::npos)
        return {};
    i = json.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos || json[i] != '"')
        return {};
    std::string value;
    for (++i; i < json.size() && json[i] != '"'; ++i)
    {
        if (json[i] != '\\' || i + 1 >= json.size())
        {
            value += json[i];
            continue;
        }
        const char c = json[++i];
        if (c == 'u' && i + 4 < json.size())
        {
            const unsigned cp = static_cast<unsigned>(strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            // Only plain ASCII matters here (URLs, version tags); anything else is kept as '?'
            value += cp < 0x80 ? static_cast<char>(cp) : '?';
        }
        else
            value += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
    }
    if (end)
        *end = i;
    return FromUtf8(value.data(), value.size());
}

// The exe's own version resource: catches an error page or a cut-off file saved as the update
std::wstring FileVersion(const std::wstring& file)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
        std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionLS));
}

bool UnderTemp(const std::wstring& path)
{
    wchar_t temp[MAX_PATH]{}, longTemp[MAX_PATH * 2]{}, longPath[MAX_PATH * 2]{};
    GetTempPathW(MAX_PATH, temp);
    if (!GetLongPathNameW(temp, longTemp, ARRAYSIZE(longTemp)) || !GetLongPathNameW(path.c_str(), longPath, ARRAYSIZE(longPath)))
        return false;
    const size_t n = wcslen(longTemp);
    return n > 3 && _wcsnicmp(longPath, longTemp, n) == 0;
}
}

std::wstring CurrentVersion()
{
    if (Sandbox::Active())
        if (std::wstring v = Env(L"FASTFILES_TEST_VERSION"); !v.empty())
            return v;
    return FASTFILES_VERSION;
}

bool Installed()
{
    const std::wstring dir = FolderOf(ExePath());
    return !dir.empty() && GetFileAttributesW((dir + L"\\Uninstall.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}

int Compare(const std::wstring& a, const std::wstring& b)
{
    std::vector<int> x = Parts(a), y = Parts(b);
    const size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i])
            return x[i] > y[i] ? 1 : -1;
    return 0;
}

void TestLog(const std::wstring& line)
{
    if (!Sandbox::Active())
        return;
    const std::string text = ToUtf8(line + L"\r\n");
    HANDLE f = CreateFileW((Sandbox::Dir() + L"update-log.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(f);
}

void CheckAsync(HWND notify)
{
    const bool installed = Installed();
    const std::wstring current = CurrentVersion();
    std::thread([notify, installed, current]
    {
        std::string body;
        int status = 0;
        const bool ok = Get(std::wstring(L"https://api.github.com/repos/") + kRepo + L"/releases/latest", L"application/vnd.github+json", status, &body, nullptr, {});
        if (!ok && status == 404)
        {
            // No release published yet: nothing newer
            auto* r = new Result;
            r->kind = Result::Kind::Checked;
            Post(notify, r);
            return;
        }
        if (!ok)
        {
            Post(notify, Fail(status == 0 ? Tr(L"无法连接到 GitHub，请检查网络连接。")
                : status == 403 || status == 429 ? Tr(L"GitHub 暂时拒绝了请求，请稍后再试。")
                : Tr(L"检查更新失败。") + std::wstring(L" (HTTP ") + std::to_wstring(status) + L")"));
            return;
        }
        std::wstring tag = JsonString(body, "tag_name");
        const std::wstring version = !tag.empty() && (tag[0] == L'v' || tag[0] == L'V') ? tag.substr(1) : tag;
        if (Parts(version).empty())
        {
            Post(notify, Fail(Tr(L"检查更新失败。")));
            return;
        }
        auto* r = new Result;
        r->kind = Result::Kind::Checked;
        if (Compare(version, current) > 0)
        {
            // The asset for this kind of install: the setup when installed, the single exe when portable
            std::wstring url;
            for (size_t at = 0, end = 0; url.empty(); at = end)
            {
                const std::wstring u = JsonString(body, "browser_download_url", at, &end);
                if (u.empty())
                    break;
                const std::wstring name = u.substr(u.find_last_of(L'/') + 1);
                const bool setup = name.size() > 14 && _wcsicmp(name.c_str() + name.size() - 14, L"_x64_Setup.exe") == 0;
                if (installed ? setup : _wcsicmp(name.c_str(), L"FastFiles.exe") == 0)
                    url = u;
            }
            if (url.empty())
            {
                delete r;
                Post(notify, Fail(Tr(L"新版本没有提供这种安装方式的下载文件。")));
                return;
            }
            r->release.version = version;
            r->release.url = url;
        }
        TestLog(L"check " + (r->release.version.empty() ? std::wstring(L"latest") : L"available " + r->release.version) +
            L" current " + current + (installed ? L" installed" : L" portable"));
        Post(notify, r);
    }).detach();
}

void DownloadAsync(HWND notify, Release release)
{
    const bool installed = Installed();
    const std::wstring exe = ExePath();
    std::thread([notify, release, installed, exe]
    {
        // Portable: FastFiles.exe.new next to the exe. Installed: the setup in our own temp folder.
        std::wstring file;
        if (installed)
        {
            const std::wstring dir = DownloadDir();
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            file = dir + L"\\" + release.url.substr(release.url.find_last_of(L'/') + 1);
        }
        else
            file = exe + L".new";
        const std::wstring part = file + L".part";
        HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE)
        {
            Post(notify, Fail(Tr(L"无法保存下载的文件。")));
            return;
        }
        int last = -1, status = 0;
        const bool ok = Get(release.url, L"application/octet-stream", status, nullptr, out, [&](unsigned long long done, unsigned long long total)
        {
            const int percent = total ? static_cast<int>(done * 100 / total) : 0;
            if (percent == last)
                return;
            last = percent;
            auto* r = new Result;
            r->kind = Result::Kind::Progress;
            r->percent = percent;
            Post(notify, r);
        });
        CloseHandle(out);
        // The file must be the release it claims to be (an exe whose version is the tag)
        if (!ok || Compare(FileVersion(part), release.version) != 0 || !MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(part.c_str());
            Post(notify, Fail(status == 0 && !ok ? Tr(L"无法连接到 GitHub，请检查网络连接。") : Tr(L"下载更新失败。")));
            return;
        }
        TestLog(L"downloaded " + file);
        auto* r = new Result;
        r->kind = Result::Kind::Downloaded;
        r->release = release;
        r->file = file;
        Post(notify, r);
    }).detach();
}

bool Apply(HWND owner, const std::wstring& file, std::wstring& error)
{
    const std::wstring exe = ExePath();
    if (Installed())
    {
        // Files' sideload update: the new package over the old one, then the app starts again. Our setup closes
        // FastFiles through the Restart Manager, installs over the same folder, then starts it as the user (/LAUNCH).
        const std::wstring args = L"/SILENT /LAUNCH \"/DIR=" + FolderOf(exe) + L"\"";
        if (Sandbox::Active())
        {
            TestLog(L"[dry-run] run " + file + L" " + args);
            return true;
        }
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.fMask = SEE_MASK_NOASYNC;
        info.hwnd = owner;
        info.lpVerb = L"open"; // the setup's manifest asks for administrator: the UAC prompt
        info.lpFile = file.c_str();
        info.lpParameters = args.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
        {
            error = GetLastError() == ERROR_CANCELLED ? Tr(L"已取消更新。") : Tr(L"无法启动安装程序。");
            return false;
        }
        return true;
    }
    // Portable: the running exe can be renamed (not overwritten), so the new one takes its name and starts once
    // this process has quit (--wait-pid, as RestartApp). Test mode: only an exe under %TEMP% (a test's own copy).
    if (Sandbox::Active() && !UnderTemp(exe))
    {
        TestLog(L"[dry-run] swap " + exe + L" <- " + file);
        DeleteFileW(file.c_str());
        return true;
    }
    const std::wstring old = exe + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        error = Tr(L"无法替换程序文件。");
        return false;
    }
    if (!MoveFileExW(file.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        error = Tr(L"无法替换程序文件。");
        return false;
    }
    TestLog(L"swapped " + exe);
    const std::wstring args = L"--wait-pid " + std::to_wstring(GetCurrentProcessId());
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
    {
        error = Tr(L"更新已完成，请重新启动 FastFiles。");
        return false;
    }
    return true;
}

void CleanUp()
{
    const std::wstring exe = ExePath();
    if (!exe.empty() && GetFileAttributesW((exe + L".old").c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW((exe + L".old").c_str());
    // A finished setup download (the setup itself may still be running: then it stays for the next start)
    const std::wstring dir = DownloadDir();
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(find, &fd));
    FindClose(find);
    RemoveDirectoryW(dir.c_str());
}

void OpenRepo()
{
    if (Sandbox::Active())
    {
        TestLog(std::wstring(L"[dry-run] open ") + kRepoUrl);
        return;
    }
    ShellExecuteW(nullptr, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
}
}
