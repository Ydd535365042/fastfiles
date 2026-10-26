#include "MainWindow.h"
#include "ContentDialog.h"
#include "Format.h"
#include "Lang.h"
#include "Sandbox.h"
#include "fastfiles_core.h"
#include <regex>
#include <thread>

// Status bar extras: folder size of the selection (Rust walk) and the Git branch of the current folder (Rust reads
// .git; commands go to git.exe on a worker). Both are polled/refreshed from the status timer and navigation.

namespace
{
const uint16_t* U16(const std::wstring& s) { return reinterpret_cast<const uint16_t*>(s.c_str()); }

// File system folder that's worth walking: not a drive root (that's the whole disk) and not a virtual folder
std::wstring SizablePath(PCIDLIST_ABSOLUTE pidl)
{
    if (!pidl || IsPagePidl(pidl))
        return {};
    ComPtr<IShellItem> item;
    SFGAOF attrs = 0;
    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item))) ||
        FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_FILESYSTEM, &attrs)) ||
        (attrs & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM) || (attrs & SFGAO_STREAM))
        return {};
    std::wstring path = PidlName(pidl, SIGDN_FILESYSPATH);
    if (path.empty() || PathIsRootW(path.c_str()))
        return {};
    return path;
}

bool CopyText(HWND owner, const std::wstring& text)
{
    if (!text.empty() && Sandbox::CopyText(text))
        return true;
    if (text.empty() || !OpenClipboard(owner))
        return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool ok = false;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok)
            GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::wstring Count(ULONGLONG n)
{
    std::wstring digits = std::to_wstring(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), 1, L',');
    return digits;
}

}

std::wstring FindGitExe()
{
    wchar_t path[MAX_PATH]{};
    if (SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, path, nullptr))
        return path;
    for (const wchar_t* guess : { L"%ProgramFiles%\\Git\\cmd\\git.exe", L"%LocalAppData%\\Programs\\Git\\cmd\\git.exe" })
        if (ExpandEnvironmentStringsW(guess, path, MAX_PATH) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            return path;
    return {};
}

namespace
{
struct GitRun
{
    std::wstring label;  // "拉取" ...
    std::wstring output; // stdout + stderr
    DWORD exitCode = 1;
    bool started = false;
};

// git <args> in dir, no console window, output captured; credential helpers can still show their own dialogs.
// background: below-normal priority, and give up after a minute (a hung remote must not keep a process around)
void RunGit(const std::wstring& git, const std::wstring& dir, const std::wstring& args, GitRun& run, bool background = false)
{
    if (git.empty())
        return;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0))
        return;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write;
    si.hStdError = write;
    si.hStdInput = nullptr;
    std::wstring cmd = L"\"" + git + L"\" " + args;
    PROCESS_INFORMATION pi{};
    // No terminal prompt to answer (it would hang); GUI credential managers still work
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");
    run.started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | (background ? BELOW_NORMAL_PRIORITY_CLASS : 0), nullptr, dir.c_str(), &si, &pi) != FALSE;
    CloseHandle(write);
    if (run.started)
    {
        std::thread watchdog;
        HANDLE done = nullptr;
        if (background)
        {
            done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            watchdog = std::thread([process = pi.hProcess, done]
            {
                if (WaitForSingleObject(done, 60 * 1000) == WAIT_TIMEOUT)
                    TerminateProcess(process, 1); // closes the pipe, so ReadFile below returns
            });
        }
        std::string bytes;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(read, buf, sizeof(buf), &got, nullptr) && got > 0)
            if (bytes.size() < 64 * 1024)
                bytes.append(buf, got);
        if (watchdog.joinable())
        {
            SetEvent(done);
            watchdog.join();
            CloseHandle(done);
        }
        WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
        GetExitCodeProcess(pi.hProcess, &run.exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        run.output = FromUtf8(bytes.data(), bytes.size());
        while (!run.output.empty() && iswspace(run.output.back()))
            run.output.pop_back();
    }
    CloseHandle(read);
}

// Commits on HEAD not on its upstream, and the other way (Files' TrackingDetails.AheadBy / BehindBy).
// No upstream: 0 / 0, like Files. No git.exe: -1 (the sync chip stays hidden; pull/push need git.exe anyway).
void AheadBehind(const std::wstring& git, const std::wstring& root, int& ahead, int& behind)
{
    ahead = behind = -1;
    if (git.empty())
        return;
    ahead = behind = 0;
    GitRun run;
    RunGit(git, root, L"rev-list --left-right --count HEAD...@{upstream}", run, true);
    if (run.started && run.exitCode == 0)
        swscanf_s(run.output.c_str(), L"%d %d", &ahead, &behind);
}

std::vector<std::wstring> Lines(const FfBuffer& buf)
{
    std::vector<std::wstring> out;
    const std::wstring text = FromUtf8(reinterpret_cast<const char*>(buf.data), buf.len);
    for (size_t start = 0; start <= text.size();)
    {
        const size_t end = std::min(text.find(L'\n', start), text.size());
        if (end > start)
            out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
}

// Called from the status/info timers: picks the folder to size and starts (or keeps) the walk
void MainWindow::UpdateFolderSize()
{
    Tab* tab = Active();
    std::wstring target;
    UniquePidl targetPidl;
    if (m_settings.folderSizes && tab && !tab->OnHome())
    {
        const int selected = tab->SelectedCount();
        UniquePidl holder;
        PCIDLIST_ABSOLUTE pidl = selected == 1 ? tab->SingleSelectedItem(holder) : selected == 0 && m_infoVisible ? tab->Location() : nullptr;
        // Only re-resolve when the selection changed (cheap key, like the info pane's)
        const long long key = (static_cast<long long>(m_navSerial) << 40) ^ (static_cast<long long>(selected) << 20) ^
            static_cast<unsigned>(tab->FocusedIndex() & 0xFFFFF) ^ (static_cast<long long>(m_active) << 56) ^ (m_infoVisible ? 1ll << 62 : 0);
        if (key != m_folderKey)
        {
            m_folderKey = key;
            m_folderTarget = pidl ? SizablePath(pidl) : std::wstring();
            m_folderPidl = pidl && !m_folderTarget.empty() ? ClonePidl(pidl) : nullptr;
            m_folderSince = GetTickCount64();
        }
        target = m_folderTarget;
    }
    if (target.empty())
    {
        m_sizer.Cancel();
        return;
    }
    // Arrowing through a list shouldn't start a walk per folder: wait until the selection rests
    if (target != m_sizer.Path() && GetTickCount64() - m_folderSince >= 350)
        m_sizer.Start(target);
    if (m_info && m_infoVisible && m_sizer.Active() && m_sizer.Path() == target && m_folderPidl)
    {
        const FolderSizer::Snapshot s = m_sizer.Read();
        std::vector<std::pair<std::wstring, std::wstring>> rows;
        rows.emplace_back(Tr(L"大小"), s.done ? FormatSizeWithBytes(s.bytes) : FormatSizeWithBytes(s.bytes) + L" …");
        rows.emplace_back(Tr(L"包含"), Count(s.files) + Tr(L" 个文件，") + Count(s.folders) + Tr(L" 个文件夹"));
        if (s.skipped)
            rows.emplace_back(Tr(L"无法读取"), Count(s.skipped) + Tr(L" 个文件夹（没有权限）"));
        m_info->SetExtra(m_folderPidl.get(), std::move(rows));
    }
}

// "，12.4 GB" / "，正在计算 3.1 GB…" for the status bar when the selected folder is being sized
std::wstring MainWindow::FolderSizeText() const
{
    if (!m_sizer.Active() || m_sizer.Path() != m_folderTarget || m_folderTarget.empty())
        return {};
    const FolderSizer::Snapshot s = m_sizer.Read();
    wchar_t buf[32]{};
    wcscpy_s(buf, Format::Size(s.bytes).c_str());
    return s.done ? std::wstring(buf) + L"（" + Count(s.files) + Tr(L" 个文件）") : Tr(L"正在计算 ") + std::wstring(buf) + L"…";
}

void MainWindow::UpdateGit(bool force)
{
    Tab* tab = Active();
    std::wstring dir = tab && !tab->OnHome() ? PidlName(tab->Location(), SIGDN_FILESYSPATH) : std::wstring();
    if (dir.empty())
    {
        m_gitDir.clear();
        m_gitRoot.clear();
        m_gitHead.clear();
        m_gitAhead = m_gitBehind = -1;
        m_status.SetBranch(L"");
        m_status.SetSync(L"", L"");
        return;
    }
    if (dir == m_gitDir && !force)
        return;
    m_gitDir = dir;
    const unsigned serial = ++m_gitSerial;
    // .git lookups walk up the parents: off the UI thread (network folders can be slow to answer)
    std::thread([target = m_hwnd, serial, dir]
    {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        bool detached = false;
        const FfBuffer buf = ff_git_repo(U16(dir), dir.size(), &detached);
        auto* result = new GitResult;
        result->serial = serial;
        result->detached = detached;
        const std::vector<std::wstring> lines = Lines(buf);
        ff_buffer_free(buf);
        if (lines.size() == 2)
        {
            result->root = lines[0];
            result->head = lines[1];
            if (!detached)
                AheadBehind(FindGitExe(), result->root, result->ahead, result->behind);
        }
        if (!PostMessageW(target, kMsgGit, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

void MainWindow::OnGitResult(GitResult* raw)
{
    std::unique_ptr<GitResult> r(raw);
    if (r->serial != m_gitSerial)
        return;
    m_gitRoot = r->root;
    m_gitHead = r->head;
    m_gitDetached = r->detached;
    m_gitAhead = r->ahead;
    m_gitBehind = r->behind;
    m_status.SetBranch(m_gitHead.empty() ? L"" : m_gitDetached ? m_gitHead + Tr(L"（分离）") : m_gitHead);
    // Files' StatusInfo "{ahead} / {behind}" and ExtendedStatusInfo
    if (m_gitHead.empty() || m_gitAhead < 0)
        m_status.SetSync(L"", L"");
    else
        m_status.SetSync(std::to_wstring(m_gitAhead) + L" / " + std::to_wstring(m_gitBehind),
            std::to_wstring(m_gitAhead) + Tr(L" 个传出提交 / ") + std::to_wstring(m_gitBehind) + Tr(L" 个传入提交"));

    // Entering a repository fetches origin in the background (Files' BaseShellPage), so "incoming" is current
    if (!r->root.empty() && r->ahead >= 0 && !m_gitBusy &&
        (r->root != m_gitFetchedRoot || GetTickCount64() - m_gitFetchedAt > 60 * 1000))
    {
        m_gitFetchedRoot = r->root;
        m_gitFetchedAt = GetTickCount64();
        std::thread([target = m_hwnd, serial = m_gitSerial, root = r->root, head = r->head, detached = r->detached]
        {
            const std::wstring git = FindGitExe();
            GitRun run;
            // Never asks for credentials in the background (a private remote just stays unfetched)
            RunGit(git, root, L"-c credential.interactive=never fetch --quiet --no-progress", run, true);
            auto* result = new GitResult;
            result->serial = serial;
            result->root = root;
            result->head = head;
            result->detached = detached;
            AheadBehind(git, root, result->ahead, result->behind);
            if (!PostMessageW(target, kMsgGit, 1, reinterpret_cast<LPARAM>(result)))
                delete result;
        }).detach();
    }
}

void MainWindow::ShowGitMenu()
{
    if (m_gitRoot.empty())
        return;
    enum : UINT { GitPull = 1, GitPush, GitFetch, GitCopy, GitOpenRoot, GitTerminal, GitCreate, GitBranchBase = 100, GitDeleteBase = 300 };
    const std::wstring git = FindGitExe();
    const UINT gitFlags = !git.empty() && !m_gitBusy ? MF_STRING : MF_STRING | MF_GRAYED;
    const FfBuffer buf = ff_git_branches(U16(m_gitRoot), m_gitRoot.size());
    std::vector<std::wstring> branches = Lines(buf);
    ff_buffer_free(buf);
    if (branches.size() > 60)
        branches.resize(60);

    HMENU menu = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    for (size_t i = 0; i < branches.size(); ++i)
        AppendMenuW(sub, gitFlags | (!m_gitDetached && branches[i] == m_gitHead ? MF_CHECKED : 0), GitBranchBase + static_cast<UINT>(i), branches[i].c_str());
    if (branches.empty())
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, Tr(L"（没有本地分支）"));
    const std::wstring title = (m_gitDetached ? Tr(L"分离的 HEAD：") : Tr(L"分支：")) + m_gitHead;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP | (branches.empty() ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(sub), Tr(L"切换分支"));
    // Files' branches flyout: 创建分支 (AddBranchDialog) and 删除 Git 分支 (any branch but the current one)
    AppendMenuW(menu, gitFlags, GitCreate, Tr(L"创建分支"));
    HMENU del = CreatePopupMenu();
    int deletable = 0;
    for (size_t i = 0; i < branches.size(); ++i)
        if (m_gitDetached || branches[i] != m_gitHead)
        {
            AppendMenuW(del, gitFlags, GitDeleteBase + static_cast<UINT>(i), branches[i].c_str());
            ++deletable;
        }
    AppendMenuW(menu, MF_POPUP | (deletable && !git.empty() && !m_gitBusy ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(del), Tr(L"删除 Git 分支"));
    AppendMenuW(menu, gitFlags, GitPull, Tr(L"拉取 (pull)"));
    AppendMenuW(menu, gitFlags | (m_gitDetached ? MF_GRAYED : 0), GitPush, Tr(L"推送 (push)"));
    AppendMenuW(menu, gitFlags, GitFetch, Tr(L"获取 (fetch)"));
    if (git.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Tr(L"没有找到 git.exe，安装 Git 后可用"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, GitCopy, m_gitDetached ? Tr(L"复制提交") : Tr(L"复制分支名"));
    AppendMenuW(menu, MF_STRING, GitOpenRoot, Tr(L"打开仓库根目录"));
    AppendMenuW(menu, MF_STRING, GitTerminal, Tr(L"在仓库根目录打开终端"));
    RECT chip = m_status.BranchScreenRect();
    if (!m_statusShown || chip.right <= chip.left)
    {
        // Status bar hidden (command palette route): open at the pointer
        POINT pt{};
        GetCursorPos(&pt);
        chip = { pt.x, pt.y, pt.x, pt.y };
    }
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, chip.right, chip.top, m_hwnd, nullptr);
    DestroyMenu(menu);

    std::wstring args, label;
    if (cmd >= GitBranchBase && cmd - GitBranchBase < branches.size())
    {
        const std::wstring& branch = branches[cmd - GitBranchBase];
        if (!m_gitDetached && branch == m_gitHead)
            return;
        args = L"switch \"" + branch + L"\"";
        label = Tr(L"切换到 ") + branch;
    }
    if (cmd >= GitDeleteBase && cmd - GitDeleteBase < branches.size())
    {
        // Files' GitDeleteBranch: confirm, then delete it (forced, as LibGit2Sharp's Branches.Remove)
        const std::wstring& branch = branches[cmd - GitDeleteBase];
        ContentDialog::Spec spec;
        spec.title = Tr(L"删除 Git 分支");
        spec.text = Lang::Format(L"确定要永久删除分支 \"{0}\" 吗?", { branch });
        spec.primary = Tr(L"删除");
        spec.close = Tr(L"取消");
        if (ContentDialog::Show(m_hwnd, &m_theme, spec) == ContentDialog::Primary)
            RunGitCommand({ L"branch -D \"" + branch + L"\"" }, Tr(L"删除 Git 分支"));
        return;
    }
    if (cmd == GitCreate)
    {
        // Files' AddBranchDialog: 名称 (valid, not taken), 基于 (the current branch), 切换到新分支 (on)
        ContentDialog::Spec spec;
        spec.title = Tr(L"创建分支");
        spec.primary = Tr(L"创建");
        spec.close = Tr(L"取消");
        ContentDialog::Field name;
        name.label = Tr(L"名称");
        name.placeholder = Tr(L"输入名称");
        ContentDialog::Field basedOn;
        basedOn.label = Tr(L"基于");
        basedOn.choices = branches;
        for (size_t i = 0; i < branches.size(); ++i)
            if (branches[i] == m_gitHead)
                basedOn.choice = static_cast<int>(i);
        spec.fields = { name, basedOn };
        spec.check = Tr(L"切换到新分支");
        spec.checked = true;
        spec.primaryEnabled = false;
        spec.changed = [branches](ContentDialog::Spec& s, int)
        {
            const std::wstring& n = s.fields[0].text;
            // git check-ref-format rules (LibGit2Sharp's Reference.IsValidName), and not an existing branch
            bool ok = !n.empty() && n.front() != L'-' && n.front() != L'/' && n.back() != L'/' && n.back() != L'.' &&
                n.find(L"..") == std::wstring::npos && n.find(L"@{") == std::wstring::npos && n.find(L"//") == std::wstring::npos &&
                (n.size() < 5 || n.compare(n.size() - 5, 5, L".lock") != 0) && n != L"@" &&
                std::find(branches.begin(), branches.end(), n) == branches.end();
            for (wchar_t c : n)
                ok = ok && c > L' ' && c != 0x7F && wcschr(L"~^:?*[\\", c) == nullptr;
            s.primaryEnabled = ok;
            s.fields[0].error = !n.empty() && !ok ? Tr(L"分支名称无效") : L"";
        };
        if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary || branches.empty())
            return;
        const std::wstring branch = spec.fields[0].text;
        const std::wstring base = branches[std::clamp(spec.fields[1].choice, 0, static_cast<int>(branches.size()) - 1)];
        RunGitCommand({ spec.checked ? L"switch -c \"" + branch + L"\" \"" + base + L"\"" : L"branch \"" + branch + L"\" \"" + base + L"\"" }, Tr(L"创建分支"));
        return;
    }
    switch (cmd)
    {
    case GitPull: args = L"pull --ff-only"; label = Tr(L"拉取"); break;
    case GitPush: args = L"push"; label = Tr(L"推送"); break;
    case GitFetch: args = L"fetch --all --prune"; label = Tr(L"获取"); break;
    case GitCopy:
    {
        CopyText(m_hwnd, m_gitHead);
        return;
    }
    case GitOpenRoot:
    case GitTerminal:
    {
        if (cmd == GitTerminal)
        {
            OpenTerminal(false, m_gitRoot);
            return;
        }
        PIDLIST_ABSOLUTE raw = nullptr;
        if (SUCCEEDED(SHParseDisplayName(m_gitRoot.c_str(), nullptr, &raw, 0, nullptr)))
        {
            UniquePidl pidl(raw);
            if (Tab* tab = Active())
                tab->Navigate(pidl.get());
        }
        return;
    }
    }
    if (!args.empty())
        RunGitCommand({ args }, label);
}

std::wstring MainWindow::GitFolder() const
{
    Tab* tab = Active();
    if (!tab || tab->OnHome() || tab->InResults() || !m_gitRoot.empty())
        return {};
    return PidlName(tab->Location(), SIGDN_FILESYSPATH);
}

void MainWindow::GitInit()
{
    // Files' GitInitAction: a folder that isn't a drive root and isn't in a repository yet
    const std::wstring folder = GitFolder();
    if (folder.empty() || folder.size() <= 3)
        return;
    if (FindGitExe().empty())
    {
        SetStatusText(Tr(L"没有找到 git.exe，安装 Git 后可用"), true);
        return;
    }
    RunGitCommand({ L"init" }, Tr(L"初始化存储库"), folder);
}

void MainWindow::GitClone()
{
    // Files' CloneRepoDialog: 存储库 URL (GitHub / GitLab, GitHelpers.GetRepoInfo), 克隆 into <current folder>\<repo>
    const std::wstring folder = GitFolder();
    if (folder.empty())
        return;
    if (FindGitExe().empty())
    {
        SetStatusText(Tr(L"没有找到 git.exe，安装 Git 后可用"), true);
        return;
    }
    static const std::wregex repo(LR"(^(?:https?://)?(?:www\.)?(github|gitlab)\.com/([^/]+)/([^/]+?)(?=\.git|/|$)(?:\.git)?/?)", std::regex::icase);
    ContentDialog::Spec spec;
    spec.title = Tr(L"克隆存储库");
    spec.primary = Tr(L"克隆");
    spec.close = Tr(L"取消");
    ContentDialog::Field url;
    url.label = Tr(L"存储库 URL");
    url.placeholder = L"https://github.com/files-community/Files";
    spec.fields.push_back(url);
    spec.primaryEnabled = false;
    spec.changed = [](ContentDialog::Spec& s, int) { s.primaryEnabled = std::regex_search(s.fields[0].text, repo); };
    if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary)
        return;
    std::wsmatch m;
    const std::wstring text = spec.fields[0].text;
    if (!std::regex_search(text, m, repo))
        return;
    const std::wstring repoUrl = L"https://" + m[1].str() + L".com/" + m[2].str() + L"/" + m[3].str();
    RunGitCommand({ L"clone \"" + repoUrl + L"\" \"" + m[3].str() + L"\"" }, Tr(L"克隆存储库"), folder);
}

// Files' status bar GitNetworkActions flyout (拉取 / 推送 / 同步); the chip shows outgoing / incoming commits
void MainWindow::ShowGitSyncMenu()
{
    if (m_gitRoot.empty())
        return;
    enum : UINT { SyncPull = 1, SyncPush, SyncBoth };
    const UINT flags = !m_gitBusy ? MF_STRING : MF_STRING | MF_GRAYED;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, flags, SyncPull, Tr(L"拉取"));
    AppendMenuW(menu, flags | (m_gitDetached ? MF_GRAYED : 0), SyncPush, Tr(L"推送"));
    AppendMenuW(menu, flags | (m_gitDetached ? MF_GRAYED : 0), SyncBoth, Tr(L"同步"));
    RECT chip = m_status.SyncScreenRect();
    if (!m_statusShown || chip.right <= chip.left)
    {
        POINT pt{};
        GetCursorPos(&pt);
        chip = { pt.x, pt.y, pt.x, pt.y };
    }
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, chip.left, chip.top, m_hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd)
    {
    case SyncPull: RunGitCommand({ L"pull --ff-only" }, Tr(L"拉取")); break;
    case SyncPush: RunGitCommand({ L"push" }, Tr(L"推送")); break;
    case SyncBoth: RunGitCommand({ L"pull --ff-only", L"push" }, Tr(L"同步")); break; // Files' SyncOriginAsync: pull, then push
    }
}

// Runs git commands one after another in the repository root (stops at the first failure); OnGitDone reports
void MainWindow::RunGitCommand(std::vector<std::wstring> commands, const std::wstring& label, const std::wstring& in)
{
    const std::wstring git = FindGitExe();
    const std::wstring where = in.empty() ? m_gitRoot : in;
    if (commands.empty() || git.empty() || where.empty() || m_gitBusy)
        return;
    m_gitBusy = true;
    SetStatusText(Tr(L"Git：正在") + label + L"…", true);
    std::thread([target = m_hwnd, git, dir = where, commands = std::move(commands), label]
    {
        auto* run = new GitRunResult;
        run->label = label;
        run->ok = true;
        for (const std::wstring& args : commands)
        {
            GitRun r;
            r.label = label;
            RunGit(git, dir, args, r);
            if (!run->output.empty() && !r.output.empty())
                run->output += L"\n";
            run->output += r.started ? r.output : Tr(L"无法启动 git.exe");
            if (!r.started || r.exitCode != 0)
            {
                run->ok = false;
                break;
            }
        }
        if (!PostMessageW(target, kMsgGitDone, 0, reinterpret_cast<LPARAM>(run)))
            delete run;
    }).detach();
}

void MainWindow::OnGitDone(GitRunResult* raw)
{
    std::unique_ptr<GitRunResult> r(raw);
    m_gitBusy = false;
    if (r->ok)
    {
        // No success notice (the chips update); the item count comes back
        m_statusHoldUntil = 0;
        m_statusText.clear();
        UpdateStatus();
    }
    else
    {
        SetStatusText(L"Git：" + r->label + Tr(L"失败"), true);
        Log::Warning(L"Git " + r->label + L" failed: " + (r->output.size() > 2000 ? r->output.substr(r->output.size() - 2000) : r->output));
        // Files' HandlePullResultAsync shows a close-only DynamicDialog titled GitError ("Git 错误"), CloseButtonText
        // "关闭" (Strings.Close), with the git output as content; FastFiles runs more than pull, so the body keeps
        // naming which action failed.
        std::wstring text = L"Git " + r->label + Tr(L"失败。\n\n") + (r->output.size() > 2000 ? r->output.substr(r->output.size() - 2000) : r->output);
        ContentDialog::Notice(m_hwnd, &m_theme, Tr(L"Git 错误"), text, Tr(L"关闭"));
    }
    UpdateGit(true);
    if (Tab* tab = Active())
        tab->Refresh();
}
