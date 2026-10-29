#include "Operations.h"
#include "Lang.h"
#include "Sandbox.h"
#include <restartmanager.h>
#include <sherrors.h>

// Files' ShellFilesystemOperations / FileOperationsHelpers: copy, move and delete through IFileOperation with the
// shell's own UI off (FOF_SILENT | FOF_NOERRORUI) and progress fed to the status center. Totals come from a walk first
// (Files' FileSizeCalculator); access denied asks to retry as administrator, a locked file asks to retry.

namespace
{
using Job = Operations::Job;

bool Cancelled(const Job& j) { return std::atomic_ref<bool>(const_cast<bool&>(j.cancel)).load(std::memory_order_relaxed); }

void Store(Job& j, int index, uint64_t value) { std::atomic_ref<uint64_t>(j.progress[index]).store(value, std::memory_order_relaxed); }
uint64_t Load(const Job& j, int index) { return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(j.progress[index])).load(std::memory_order_relaxed); }

std::wstring Join(const std::wstring& dir, const std::wstring& name)
{
    return dir.empty() || dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

std::wstring Leaf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Long(const std::wstring& path)
{
    // \\?\ for paths past MAX_PATH (UNC: \\?\UNC\server\share)
    if (path.size() < MAX_PATH - 12 || path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    return path.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}

// Files' FileSizeCalculator: every file and folder under the sources, the counters updated as it goes so the card
// shows "已发现 N 个项目" while it runs
void Walk(Job& j)
{
    uint64_t bytes = 0, items = 0;
    std::vector<std::wstring> stack;
    for (const auto& s : j.sources)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(Long(s).c_str(), GetFileExInfoStandard, &a))
            continue;
        ++items;
        if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            bytes += (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        else if (!(a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) // junctions are copied as links, not followed
            stack.push_back(s);
    }
    Store(j, 0, bytes);
    Store(j, 2, items);
    ULONGLONG last = GetTickCount64();
    while (!stack.empty() && !Cancelled(j))
    {
        const std::wstring dir = std::move(stack.back());
        stack.pop_back();
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW(Long(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
                continue;
            ++items;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(Join(dir, fd.cFileName));
            }
            else
                bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        if (const ULONGLONG now = GetTickCount64(); now - last > 100)
        {
            last = now;
            Store(j, 0, bytes);
            Store(j, 2, items);
        }
    }
    Store(j, 0, bytes);
    Store(j, 2, items);
    j.counted.store(true, std::memory_order_release);
}

bool IsCancelCode(HRESULT hr)
{
    return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT || hr == COPYENGINE_E_USER_CANCELLED || hr == COPYENGINE_S_USER_IGNORED ||
           hr == COPYENGINE_S_DONT_PROCESS_CHILDREN || hr == COPYENGINE_S_USER_RETRY;
}

// Files' CopyEngineResult.Convert: Unauthorized / InUse
bool NeedsAdmin(HRESULT hr)
{
    return hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD) ||
           hr == COPYENGINE_E_ACCESS_DENIED_SRC || hr == COPYENGINE_E_ACCESS_DENIED_DEST || hr == COPYENGINE_E_REQUIRES_ELEVATION ||
           hr == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
}

bool InUse(HRESULT hr)
{
    return hr == COPYENGINE_E_SHARING_VIOLATION_SRC || hr == COPYENGINE_E_SHARING_VIOLATION_DEST || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
           hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION);
}

std::wstring ErrorText(HRESULT hr)
{
    // The copy engine's own codes have no system text: Files' wording for the ones it names, else their Win32 meaning
    if (hr == COPYENGINE_E_SHARING_VIOLATION_SRC || hr == COPYENGINE_E_SHARING_VIOLATION_DEST)
        return Tr(L"另一程序正在使用您尝试访问的文件");
    if (hr == COPYENGINE_E_ACCESS_DENIED_SRC || hr == COPYENGINE_E_ACCESS_DENIED_DEST || hr == COPYENGINE_E_REQUIRES_ELEVATION)
        hr = E_ACCESSDENIED;
    else if (hr == COPYENGINE_E_PATH_TOO_DEEP_SRC || hr == COPYENGINE_E_PATH_TOO_DEEP_DEST || hr == COPYENGINE_E_NEWFILE_NAME_TOO_LONG ||
             hr == COPYENGINE_E_NEWFOLDER_NAME_TOO_LONG)
        hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    else if (hr == COPYENGINE_E_DISK_FULL || hr == COPYENGINE_E_DISK_FULL_CLEAN)
        hr = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    else if (hr == COPYENGINE_E_SAME_FILE || hr == COPYENGINE_E_DEST_SUBTREE || hr == COPYENGINE_E_DEST_SAME_TREE)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = text ? text : L"";
    if (text)
        LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' '))
        out.pop_back();
    wchar_t code[16];
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    return out.empty() ? code : out + L" (" + code + L")";
}

// Files' WhoIsLocking (Restart Manager): the programs holding `path`
std::wstring LockingApps(const std::wstring& path)
{
    DWORD session = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS)
        return {};
    std::wstring names;
    LPCWSTR files[] = { path.c_str() };
    UINT needed = 0, count = 0;
    DWORD reasons = 0;
    if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS &&
        RmGetList(session, &needed, &count, nullptr, &reasons) == ERROR_MORE_DATA && needed)
    {
        std::vector<RM_PROCESS_INFO> info(needed);
        count = needed;
        if (RmGetList(session, &needed, &count, info.data(), &reasons) == ERROR_SUCCESS)
            for (UINT i = 0; i < count; ++i)
                names += (names.empty() ? L"" : L", ") + std::wstring(info[i].strAppName) + L" (PID: " +
                         std::to_wstring(info[i].Process.dwProcessId) + L")";
    }
    RmEndSession(session);
    return names;
}

// The worker waits for the window's answer (Operations::Answer, or Cancel = no)
bool AskWindow(Job& j, HWND owner, Operations::Ask ask, const std::wstring& detail)
{
    if (Cancelled(j))
        return false;
    {
        std::lock_guard guard(j.lock);
        if (!j.answered)
            j.answered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ResetEvent(j.answered);
        j.ask = ask;
        j.askDetail = detail;
        j.answer = false;
    }
    PostMessageW(owner, Operations::kMsgAsk, j.id, 0);
    WaitForSingleObject(j.answered, INFINITE);
    std::lock_guard guard(j.lock);
    j.ask = Operations::Ask::None;
    return j.answer && !Cancelled(j);
}

// IFileOperationProgressSink on the worker's stack: progress into the job's counters, the result of every item
class Sink final : public IFileOperationProgressSink
{
public:
    // before / share: this run's place in the whole job (fractions), so progress keeps rising across runs
    Sink(Job& job, double before, double share) : m_job(job), m_before(before), m_share(share) {}
    std::vector<HRESULT> results; // per top-level source (S_OK until something failed)
    std::vector<std::wstring> failedPaths;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink))
        {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return Go(); }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return Go(); }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR name) override { return Current(item, name); }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override { return Result(item, hr); }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR name) override { return Current(item, name); }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override { return Result(item, hr); }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem* item) override { return Current(item, nullptr); }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* item, HRESULT hr, IShellItem*) override { return Result(item, hr); }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return Go(); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return Go(); }
    IFACEMETHODIMP UpdateProgress(UINT total, UINT done) override
    {
        // Files: ProcessedSize = TotalSize × the shell's percentage (items likewise)
        if (total)
        {
            const double whole = m_before + m_share * std::min(1.0, static_cast<double>(done) / total);
            Store(m_job, 1, static_cast<uint64_t>(whole * static_cast<double>(Load(m_job, 0))));
            Store(m_job, 3, static_cast<uint64_t>(whole * static_cast<double>(Load(m_job, 2))));
            m_job.started.store(true, std::memory_order_release);
        }
        return Go();
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }

private:
    HRESULT Go() const { return Cancelled(m_job) ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK; }
    HRESULT Current(IShellItem* item, LPCWSTR name)
    {
        std::wstring text = name ? name : L"";
        PWSTR display = nullptr;
        if (text.empty() && item && SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)))
        {
            text = display;
            CoTaskMemFree(display);
        }
        std::lock_guard guard(m_job.lock);
        m_job.current = std::move(text);
        return Go();
    }
    HRESULT Result(IShellItem* item, HRESULT hr)
    {
        if (FAILED(hr) && !IsCancelCode(hr))
        {
            PWSTR path = nullptr;
            std::wstring p;
            if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                p = path;
                CoTaskMemFree(path);
            }
            failedPaths.push_back(p);
            // Credit the failure to the top-level source it's in
            for (size_t i = 0; i < m_job.sources.size() && i < results.size(); ++i)
            {
                const std::wstring& s = m_job.sources[i];
                if (_wcsicmp(p.c_str(), s.c_str()) == 0 ||
                    (p.size() > s.size() && _wcsnicmp(p.c_str(), s.c_str(), s.size()) == 0 && (p[s.size()] == L'\\' || s.back() == L'\\')))
                {
                    if (SUCCEEDED(results[i]))
                        results[i] = hr;
                    break;
                }
            }
            if (p.empty() || std::all_of(results.begin(), results.end(), [](HRESULT r) { return SUCCEEDED(r); }))
                for (auto& r : results)
                    if (SUCCEEDED(r))
                    {
                        r = hr; // unknown item: blame the first one still clean so the failure isn't lost
                        break;
                    }
        }
        return Go();
    }
    Job& m_job;
    double m_before, m_share;
};

// One IFileOperation run over `indices` of the job's sources
HRESULT RunGroup(Job& j, HWND owner, int kind /*0 copy 1 move 2 delete*/, const std::vector<size_t>& indices, DWORD flags, const std::wstring& dest,
    Sink& sink)
{
    ComPtr<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr))
        return hr;
    op->SetOperationFlags(flags);
    op->SetOwnerWindow(owner); // the UAC prompt's parent
    DWORD cookie = 0;
    op->Advise(&sink, &cookie);
    ComPtr<IShellItem> folder;
    if (kind != 2 && FAILED(hr = SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&folder))))
    {
        op->Unadvise(cookie);
        return hr;
    }
    int queued = 0;
    for (size_t i : indices)
    {
        ComPtr<IShellItem> item;
        // The selection's own items when we have them: a recycle bin item deleted by its bin pidl drops its $I record too
        HRESULT itemHr = i < j.pidls.size() && j.pidls[i] ? SHCreateItemFromIDList(j.pidls[i].get(), IID_PPV_ARGS(&item))
                                                          : SHCreateItemFromParsingName(j.sources[i].c_str(), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(itemHr))
            itemHr = kind == 0 ? op->CopyItem(item.Get(), folder.Get(), nullptr, nullptr)
                   : kind == 1 ? op->MoveItem(item.Get(), folder.Get(), nullptr, nullptr)
                               : op->DeleteItem(item.Get(), nullptr);
        if (FAILED(itemHr))
        {
            if (SUCCEEDED(sink.results[i]))
                sink.results[i] = itemHr; // gone since the selection was taken, or not a file system item
            continue;
        }
        ++queued;
    }
    if (queued)
        hr = op->PerformOperations();
    op->Unadvise(cookie);
    if (Cancelled(j))
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted)
    {
        // With FOF_NOERRORUI an aborted run is a failure the sink already recorded. Nothing recorded: the user
        // stopped it in the system's progress window (copy / move show it), which is a cancel, not an error.
        if (std::all_of(indices.begin(), indices.end(), [&](size_t i) { return SUCCEEDED(sink.results[i]); }) && !indices.empty())
        {
            if (!(flags & FOF_SILENT))
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            sink.results[indices.front()] = E_FAIL;
        }
    }
    return hr;
}

// The file the in-use question names: the item that failed, or the first source
std::wstring LockedFile(const Sink& sink, const std::vector<size_t>& failed, const Job& j)
{
    for (const auto& p : sink.failedPaths)
        if (!p.empty())
            return p;
    return failed.empty() ? std::wstring() : j.sources[failed.front()];
}

void Finish(Job& j, Operations::State s)
{
    j.state.store(static_cast<int>(s), std::memory_order_release);
}

void RunWithRetries(Job& j, HWND owner, int kind, DWORD baseFlags, DWORD replaceFlags)
{
    Walk(j);
    if (Cancelled(j))
        return Finish(j, Operations::State::Cancelled);
    // Files splits the work: the "generate new name" group (FOF_RENAMEONCOLLISION), then the "replace" group
    std::vector<size_t> pending;
    for (size_t i = 0; i < j.sources.size(); ++i)
        if (kind == 2 || j.collisions[i] != Operations::Collision::Skip)
            pending.push_back(i);
        else
            ++j.skipped;
    bool elevated = false;
    std::vector<HRESULT> results(j.sources.size(), S_OK);
    for (;;)
    {
        std::vector<size_t> groups[2];
        for (size_t i : pending)
            groups[kind != 2 && j.collisions[i] == Operations::Collision::Replace ? 1 : 0].push_back(i);
        bool again = false;
        double before = 0;
        for (int g = 0; g < 2 && !Cancelled(j) && !again; ++g)
        {
            if (groups[g].empty())
                continue;
            const double share = static_cast<double>(groups[g].size()) / static_cast<double>(std::max<size_t>(1, pending.size()));
            Sink sink(j, before, share);
            sink.results.assign(j.sources.size(), S_OK);
            DWORD flags = g == 0 ? baseFlags : replaceFlags;
            if (elevated)
                flags |= FOFX_SHOWELEVATIONPROMPT | FOFX_REQUIREELEVATION;
            const HRESULT hr = RunGroup(j, owner, kind, groups[g], flags, j.target, sink);
            // Cancelled in the system's progress window (×): the card says cancelled, the rest doesn't run
            if (FAILED(hr) && IsCancelCode(hr))
                return Finish(j, Operations::State::Cancelled);
            for (size_t i : groups[g])
                results[i] = sink.results[i];
            if (FAILED(hr) && !IsCancelCode(hr) && std::all_of(groups[g].begin(), groups[g].end(), [&](size_t i) { return SUCCEEDED(results[i]); }))
                results[groups[g].front()] = hr; // the whole run failed before any item reported
            before += share;
            // Files' FileInUseDialog: asked while this run's sink still knows the locked file
            std::vector<size_t> failed;
            for (size_t i : groups[g])
                if (FAILED(results[i]))
                    failed.push_back(i);
            if (!failed.empty() && InUse(results[failed.front()]))
            {
                const std::wstring file = LockedFile(sink, failed, j);
                if (AskWindow(j, owner, Operations::Ask::InUse, file + L"\n" + LockingApps(file)))
                {
                    // Retry what failed, plus the group that hasn't run yet
                    pending = failed;
                    for (size_t i : failed)
                        results[i] = S_OK;
                    if (g == 0)
                        pending.insert(pending.end(), groups[1].begin(), groups[1].end());
                    again = true;
                }
            }
        }
        if (Cancelled(j))
            return Finish(j, Operations::State::Cancelled);
        if (again)
            continue;
        std::vector<size_t> failed;
        for (size_t i = 0; i < results.size(); ++i)
            if (FAILED(results[i]))
                failed.push_back(i);
        if (failed.empty())
        {
            Store(j, 1, Load(j, 0));
            Store(j, 3, Load(j, 2));
            return Finish(j, Operations::State::Done);
        }
        const HRESULT first = results[failed.front()];
        if (!elevated && NeedsAdmin(first) && AskWindow(j, owner, Operations::Ask::Elevate, {}))
        {
            // Files' RequestAdminOperation, then the same call with FOFX_REQUIREELEVATION: the items that failed
            elevated = true;
            pending = failed;
            for (size_t i : failed)
                results[i] = S_OK;
            continue;
        }
        if (Cancelled(j))
            return Finish(j, Operations::State::Cancelled);
        j.error = Leaf(j.sources[failed.front()]) + L": " + ErrorText(first);
        return Finish(j, Operations::State::Failed);
    }
}
}

namespace FileOperations
{
void Transfer(Job& j, HWND owner, bool move)
{
    // Files' CopyItemAsync / MoveItemAsync flags, except FOF_SILENT: the user wants the system's own progress window
    // (speed graph, pause, cancel) as in Explorer, besides the status center card. FOF_ALLOWUNDO keeps the shell's undo
    // (Ctrl+Z in the view) working; name conflicts were already settled in Files' dialog, errors still come back here.
    const DWORD common = FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_ALLOWUNDO;
    RunWithRetries(j, owner, move ? 1 : 0, common | FOFX_PRESERVEFILEEXTENSIONS | FOF_RENAMEONCOLLISION, common | FOF_NOCONFIRMATION);
}

void Delete(Job& j, HWND owner, bool permanently)
{
    // Files' DeleteItemAsync: silent, no confirmation (its own dialog came first); the bin unless permanently
    DWORD flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (!permanently && Sandbox::Active())
    {
        // Test mode: "the recycle bin" is <data>\recycled (the real one is never touched); a move with new names
        j.target = Sandbox::RecycledDir();
        j.collisions.assign(j.sources.size(), Operations::Collision::NewName);
        RunWithRetries(j, owner, 1, flags | FOF_RENAMEONCOLLISION, flags);
        return;
    }
    if (!permanently)
        flags |= FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING | FOF_ALLOWUNDO;
    RunWithRetries(j, owner, 2, flags, flags);
}

void EmptyBin(Job& j)
{
    if (Sandbox::Active())
        return Finish(j, Operations::State::Done); // test mode: the real recycle bin is never emptied
    // Files' EmptyTrashBin: SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI (its own confirmation came first)
    const HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI);
    if (FAILED(hr) && hr != E_UNEXPECTED) // E_UNEXPECTED: the bin was already empty
        j.error = ErrorText(hr);
    Finish(j, FAILED(hr) && hr != E_UNEXPECTED ? Operations::State::Failed : Operations::State::Done);
}
}
