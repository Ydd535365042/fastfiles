#include "MainWindow.h"
#include "ContentDialog.h"
#include "Lang.h"
#include "Sandbox.h"

// Files' FilesystemHelpers on the status center: paste (CopyItemsFromClipboard / MoveItemsFromClipboard), delete
// (DeleteItemsAsync with its confirmation dialog), drops on the sidebar / tabs / path bar, and the conflict dialog
// (FilesystemOperationDialog) that settles name collisions before the shell copies anything.

struct DroppedFiles
{
    std::vector<std::wstring> sources;
    std::wstring destination; // empty with bin
    bool move = false;
    bool bin = false;         // dropped on the recycle bin: Files' RecycleItemsFromClipboard
};

namespace
{
std::wstring Leaf(std::wstring path)
{
    while (path.size() > 1 && path.back() == L'\\')
        path.pop_back();
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Trimmed(std::wstring path)
{
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    return path;
}

std::wstring ParentOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : Trimmed(path.substr(0, slash + 1));
}

bool InRecycleBinFolder(const std::wstring& path)
{
    return path.size() > 3 && _wcsnicmp(path.c_str() + 2, L"\\$Recycle.Bin\\", 14) == 0;
}

// The file system paths of what's in the data object; false when any item has none (virtual items: the shell's job)
bool DataPaths(IDataObject* data, std::vector<std::wstring>& out)
{
    out.clear();
    if (!data)
        return false;
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) && SUCCEEDED(items->GetCount(&count)) && count > 0)
    {
        for (DWORD i = 0; i < count; ++i)
        {
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                return false;
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    else
    {
        // Only CF_HDROP (most programs other than Explorer: editors, browsers, .NET's Clipboard)
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&fe, &medium)))
            return false;
        if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal)))
        {
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i)
            {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                if (!path.empty())
                    out.push_back(std::move(path));
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    if (out.empty())
        return false;
    // Drive roots and items still in the bin keep the shell's handling
    for (const auto& path : out)
        if (Leaf(path).empty() || path.back() == L':' || (path.size() <= 3 && path[1] == L':') || InRecycleBinFolder(path))
            return false;
    return true;
}

DWORD PreferredEffect(IDataObject* data)
{
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    FORMATETC fe{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    DWORD effect = DROPEFFECT_COPY; // Files: RequestedOperation defaults to copy
    if (SUCCEEDED(data->GetData(&fe, &medium)))
    {
        if (const auto* p = static_cast<const DWORD*>(GlobalLock(medium.hGlobal)))
        {
            effect = *p;
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    return effect;
}

void SetEffect(IDataObject* data, const wchar_t* name, DWORD effect)
{
    FORMATETC fe{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{ TYMED_HGLOBAL };
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!medium.hGlobal)
        return;
    if (auto* p = static_cast<DWORD*>(GlobalLock(medium.hGlobal)))
    {
        *p = effect;
        GlobalUnlock(medium.hGlobal);
    }
    if (FAILED(data->SetData(&fe, &medium, TRUE)))
        GlobalFree(medium.hGlobal);
}

bool IsRecycleBin(PCIDLIST_ABSOLUTE folder)
{
    PIDLIST_ABSOLUTE bin = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bin)))
        return false;
    const bool same = ILIsEqual(bin, folder) != FALSE;
    CoTaskMemFree(bin);
    return same;
}

std::wstring RecycleBinName()
{
    PIDLIST_ABSOLUTE bin = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bin)))
        return Tr(L"回收站");
    UniquePidl owned(bin);
    const std::wstring name = PidlName(bin, SIGDN_NORMALDISPLAY);
    return name.empty() ? std::wstring(Tr(L"回收站")) : name;
}
}

bool RouteFileDrop(HWND from, IDataObject* data, PCIDLIST_ABSOLUTE folder, DWORD* effect, bool rightButton)
{
    if (rightButton || !data || !folder || !effect || !(*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE)))
        return false;
    auto drop = std::make_unique<DroppedFiles>();
    drop->bin = IsRecycleBin(folder);
    if (!drop->bin)
    {
        drop->destination = PidlName(folder, SIGDN_FILESYSPATH);
        if (drop->destination.empty())
            return false;
    }
    if (!DataPaths(data, drop->sources))
        return false;
    // The effect the folder's own target chose for these keys (Ctrl copies, Shift moves, other drives copy)
    drop->move = drop->bin || (*effect & DROPEFFECT_MOVE) != 0;
    const bool move = drop->move;
    HWND root = GetAncestor(from, GA_ROOT);
    if (!root || !PostMessageW(root, WM_APP_DROPFILES, 0, reinterpret_cast<LPARAM>(drop.get())))
        return false;
    drop.release();
    // An optimized move: the move is ours, so the source must not delete anything itself
    *effect = move ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    SetEffect(data, CFSTR_PERFORMEDDROPEFFECT, *effect);
    SetEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, move ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
    return true;
}

// Files' CopyItemFromSidebar / CopyItemFromHome: Clipboard.SetContent(DataPackage with the storage item), falling back
// to the path as text when the shell can't produce a data object (IsNonCopyableLocation: tag:/Home/Settings/recycle
// bin/network/this PC already steer callers away from this, but a virtual item elsewhere could still land here).
void CopyPathsToClipboard(HWND owner, const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;
    if (Sandbox::CopyFiles(paths))
        return; // test mode: <Dir>clipboard-files.txt, the real clipboard untouched
    // SHCreateDataObject takes child IDs under one parent folder; the sidebar/Home "复制" item always acts on a
    // single path, so parse it, bind to its parent, and hand over its one child ID (Explorer's own Copy does the
    // same for a single selection). Several paths with different parents aren't a case either caller produces.
    ComPtr<IDataObject> data;
    PIDLIST_ABSOLUTE parsed = nullptr;
    if (SUCCEEDED(SHParseDisplayName(paths.front().c_str(), nullptr, &parsed, 0, nullptr)) && parsed)
    {
        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        if (SUCCEEDED(SHBindToParent(parsed, IID_PPV_ARGS(&parent), &child)))
        {
            PIDLIST_ABSOLUTE parentPidl = ILCloneFull(parsed);
            if (parentPidl)
                ILRemoveLastID(parentPidl);
            SHCreateDataObject(parentPidl, 1, &child, nullptr, IID_PPV_ARGS(&data));
            CoTaskMemFree(parentPidl);
        }
        CoTaskMemFree(parsed);
    }
    if (data)
    {
        // CFSTR_PREFERREDDROPEFFECT = DROPEFFECT_COPY, the same marker Explorer's Copy sets so a later Paste copies
        DWORD effect = DROPEFFECT_COPY;
        SetEffect(data.Get(), CFSTR_PREFERREDDROPEFFECT, effect);
        if (SUCCEEDED(OleSetClipboard(data.Get())))
            return;
    }
    // Fallback: the path(s) as plain text (Files' CopyPathFallback)
    if (OpenClipboard(owner))
    {
        EmptyClipboard();
        std::wstring text;
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (i)
                text += L"\r\n";
            text += paths[i];
        }
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
        {
            memcpy(GlobalLock(mem), text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem))
                GlobalFree(mem);
        }
        CloseClipboard();
    }
}

void MainWindow::OnDroppedFiles(DroppedFiles* raw)
{
    std::unique_ptr<DroppedFiles> drop(raw);
    if (drop->bin)
    {
        // Files: dropping on the recycle bin recycles (with the delete confirmation)
        DeleteItems(drop->sources, {}, {}, false);
        return;
    }
    StartTransfer(std::move(drop->sources), drop->destination, drop->move);
}

bool MainWindow::PasteTo(const std::wstring& destination)
{
    if (destination.empty())
        return false;
    std::vector<std::wstring> testFiles;
    bool testMove = false;
    if (Sandbox::PasteFiles(testFiles, testMove))
    {
        // Test mode: the files "on the clipboard" come from <data>\clipboard-files.txt; the real clipboard isn't read
        if (testFiles.empty())
            return true;
        StartTransfer(std::move(testFiles), destination, testMove);
        return true;
    }
    ComPtr<IDataObject> data;
    if (FAILED(OleGetClipboard(&data)) || !data)
        return false;
    std::vector<std::wstring> sources;
    if (!DataPaths(data.Get(), sources))
        return false;
    // Files' PerformOperationTypeAsync: Move when the clipboard was cut, Copy otherwise
    StartTransfer(std::move(sources), destination, (PreferredEffect(data.Get()) & DROPEFFECT_MOVE) != 0);
    return true;
}

void MainWindow::PasteHere()
{
    Tab* tab = Active();
    if (!tab || tab->OnHome())
        return;
    if (!PasteTo(CurrentFolderPath(tab)))
        tab->InvokeVerb("paste", true); // virtual folders, or things that aren't files (an image, text...)
}

void MainWindow::PasteIntoSelection()
{
    Tab* tab = Active();
    UniquePidl folder;
    if (!tab)
        return;
    // Files' PasteItemToSelectionAction: into the selected folder, otherwise here
    if (const PCIDLIST_ABSOLUTE target = tab->SingleSelectedFolder(folder))
    {
        if (PasteTo(PidlName(target, SIGDN_FILESYSPATH)) || tab->InvokeVerb("paste", false))
            return;
    }
    PasteHere();
}

void MainWindow::StartTransfer(std::vector<std::wstring> sources, const std::wstring& destination, bool move)
{
    const std::wstring dest = Trimmed(destination);
    std::vector<std::wstring> items;
    std::vector<bool> conflict;
    for (auto& s : sources)
    {
        const std::wstring parent = ParentOf(s);
        // Moving into the folder it's already in does nothing (Files: Source == Destination)
        if (move && _wcsicmp(parent.c_str(), dest.c_str()) == 0)
            continue;
        const std::wstring target = (dest.back() == L'\\' ? dest : dest + L"\\") + Leaf(s);
        // Files' GetCollisions: the same name exists there (the item itself, copied into its own folder, gets a new name)
        conflict.push_back(_wcsicmp(target.c_str(), s.c_str()) != 0 && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES);
        items.push_back(std::move(s));
    }
    if (items.empty())
        return;
    std::vector<Operations::Collision> collisions(items.size(), Operations::Collision::NewName);
    if (std::any_of(conflict.begin(), conflict.end(), [](bool c) { return c; }) && !ResolveConflicts(items, conflict, collisions))
        return;
    if (std::all_of(collisions.begin(), collisions.end(), [](auto c) { return c == Operations::Collision::Skip; }))
        return;
    m_ops.Transfer(m_hwnd, move, std::move(items), std::move(collisions), dest);
    m_center.Refresh();
}

bool MainWindow::ResolveConflicts(const std::vector<std::wstring>& sources, const std::vector<bool>& conflict,
    std::vector<Operations::Collision>& out)
{
    // Files' FilesystemOperationDialog (ConflictsExist): per item 生成新名称 / 替换已有的项目 / 跳过, and one choice for all
    std::vector<size_t> conflicts;
    for (size_t i = 0; i < conflict.size(); ++i)
        if (conflict[i])
            conflicts.push_back(i);
    const size_t clean = sources.size() - conflicts.size();
    const std::wstring n = std::to_wstring(conflicts.size()), m = std::to_wstring(clean);
    ContentDialog::Spec spec;
    spec.title = Lang::Format(L"{0, plural, other {文件名冲突}}", { n });
    spec.text = clean ? Lang::Format(L"{0, plural, other {有 # 个文件名冲突}}和 {1, plural, other {# 个无冲突的项目}}", { n, m })
                      : Lang::Format(L"{0, plural, other {有 # 个文件名冲突}}", { n });
    const std::vector<std::wstring> options = { Tr(L"生成新名称"), Tr(L"替换已有的项目"), Tr(L"跳过") };
    // The dialog lists the first conflicts; the rest follow 将此操作应用到所有冲突项
    constexpr size_t kShown = 5;
    const size_t shown = std::min(conflicts.size(), kShown);
    ContentDialog::Field all;
    all.label = Tr(L"将此操作应用到所有冲突项");
    all.choices = { Tr(L"自定义"), options[0], options[1], options[2] };
    all.choice = 1;
    spec.fields.push_back(all);
    for (size_t k = 0; k < shown; ++k)
    {
        ContentDialog::Field f;
        f.label = Leaf(sources[conflicts[k]]);
        f.choices = options;
        spec.fields.push_back(f);
    }
    if (conflicts.size() > shown)
        spec.text += L"\n" + Lang::Format(L"（只显示前 {0} 个）", { std::to_wstring(shown) });
    spec.changed = [shown, total = conflicts.size()](ContentDialog::Spec& s, int field)
    {
        if (field == 0 && s.fields[0].choice > 0)
        {
            for (size_t k = 1; k <= shown; ++k)
                s.fields[k].choice = s.fields[0].choice - 1;
        }
        else if (field > 0)
        {
            // Files' AggregatedResolveOption: one choice shared by every item (the unlisted ones keep it), else 自定义
            const int first = s.fields[1].choice;
            bool same = total <= shown || first == s.fields[0].choice - 1;
            for (size_t k = 1; k <= shown; ++k)
                same &= s.fields[k].choice == first;
            s.fields[0].choice = same ? first + 1 : 0;
        }
    };
    spec.primary = Tr(L"继续");
    spec.close = Tr(L"取消");
    if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary)
        return false;
    static const Operations::Collision kinds[] = { Operations::Collision::NewName, Operations::Collision::Replace, Operations::Collision::Skip };
    const int aggregate = spec.fields[0].choice;
    for (size_t k = 0; k < conflicts.size(); ++k)
        out[conflicts[k]] = k < shown ? kinds[std::clamp(spec.fields[k + 1].choice, 0, 2)] : aggregate > 0 ? kinds[aggregate - 1] : kinds[0];
    return true;
}

bool MainWindow::DeleteSelectionOf(Tab* tab, bool permanently)
{
    std::vector<std::wstring> paths, names;
    std::vector<UniquePidl> pidls;
    if (!tab || tab->OnHome() || !tab->SelectedFileSystemItems(paths, pidls, names))
        return false;
    DeleteItems(std::move(paths), std::move(pidls), names, permanently);
    return true;
}

void MainWindow::DeleteSelection(bool permanently)
{
    Tab* tab = Active();
    if (!tab || tab->OnHome() || tab->SelectedCount() == 0)
        return;
    if (!DeleteSelectionOf(tab, permanently))
        tab->InvokeVerb("delete", false); // not file system items (a library, a phone...): the shell's own delete
}

void MainWindow::DeleteItems(std::vector<std::wstring> paths, std::vector<UniquePidl> pidls, const std::vector<std::wstring>& names, bool permanently)
{
    if (paths.empty())
        return;
    // Files' DeleteItemsAsync: items already in the bin go for good; elsewhere only where the drive has a bin
    const bool fromBin = std::all_of(paths.begin(), paths.end(), InRecycleBinFolder);
    bool canRecycle = false;
    if (!fromBin)
    {
        wchar_t root[MAX_PATH]{};
        wcsncpy_s(root, paths.front().c_str(), _TRUNCATE);
        PathStripToRootW(root);
        SHQUERYRBINFO info{ sizeof(info) };
        canRecycle = SUCCEEDED(SHQueryRecycleBinW(root, &info));
    }
    const auto policy = m_settings.deleteConfirm;
    const bool permanent = permanently || !canRecycle;
    if (policy == AppSettings::DeleteConfirm::Always || (policy == AppSettings::DeleteConfirm::Permanent && permanent))
    {
        const std::wstring count = std::to_wstring(paths.size());
        ContentDialog::Spec spec;
        spec.title = Lang::Format(L"删除{0, plural, other {项目}}", { count });
        spec.text = Lang::Format(L"{0, plural, other {将删除 # 个项目}}", { count });
        // The items being deleted (Files lists them; the bin shows their original names)
        constexpr size_t kListed = 5;
        for (size_t i = 0; i < paths.size() && i < kListed; ++i)
            spec.text += L"\n" + (i < names.size() && !names[i].empty() ? names[i] : Leaf(paths[i]));
        if (paths.size() > kListed)
            spec.text += L"\n…";
        spec.check = Tr(L"永久删除");
        spec.checked = permanent;
        spec.checkEnabled = canRecycle; // Files' IsDeletePermanentlyEnabled
        spec.primary = Tr(L"删除");
        spec.close = Tr(L"取消");
        if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary)
            return;
        permanently = spec.checked || !canRecycle;
    }
    else
        permanently = permanent;
    m_ops.Delete(m_hwnd, std::move(paths), std::move(pidls), permanently, fromBin ? RecycleBinName() : std::wstring());
    m_center.Refresh();
}

void MainWindow::EmptyRecycleBin()
{
    // Files' EmptyRecycleBinAction: 清空回收站 / 确定要永久删除所有项目吗？ (unless DeleteConfirmationPolicy is Never)
    if (m_settings.deleteConfirm != AppSettings::DeleteConfirm::Never)
    {
        ContentDialog::Spec spec;
        spec.title = Tr(L"清空回收站");
        spec.text = Tr(L"确定要永久删除所有项目吗？");
        spec.primary = Tr(L"是");
        spec.close = Tr(L"取消");
        if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary)
            return;
    }
    m_ops.EmptyBin(m_hwnd);
    m_center.Refresh();
}

void MainWindow::OnOperationAsk(unsigned id)
{
    Operations::Ask ask = Operations::Ask::None;
    std::wstring detail;
    if (!m_ops.Question(id, ask, detail))
        return;
    ContentDialog::Spec spec;
    if (ask == Operations::Ask::Elevate)
    {
        // Files' ElevateConfirmDialog
        spec.title = Tr(L"此操作需要管理员权限");
        spec.text = Tr(L"要以管理员身份继续吗？");
        spec.primary = Tr(L"是");
        spec.close = Tr(L"否");
    }
    else
    {
        // Files' FileInUseDialog: who holds it when Restart Manager knows, the file, 重试 / 取消
        const size_t nl = detail.find(L'\n');
        const std::wstring file = detail.substr(0, nl), apps = nl == std::wstring::npos ? std::wstring() : detail.substr(nl + 1);
        spec.title = Tr(L"文件正在使用中");
        spec.text = (apps.empty() ? std::wstring(Tr(L"另一程序正在使用您尝试访问的文件")) : Lang::Format(L"{0}正在使用您尝试访问的文件", { apps })) +
                    L"\n" + file;
        spec.primary = Tr(L"重试");
        spec.close = Tr(L"取消");
    }
    m_ops.Answer(id, ContentDialog::Show(m_hwnd, &m_theme, spec) == ContentDialog::Primary);
}

bool MainWindow::OnTabVerb(Tab* tab, const wchar_t* verb, bool background)
{
    if (_wcsicmp(verb, L"paste") == 0)
    {
        if (background)
            return PasteTo(CurrentFolderPath(tab));
        UniquePidl holder;
        const PCIDLIST_ABSOLUTE folder = tab->SingleSelectedFolder(holder);
        return folder && PasteTo(PidlName(folder, SIGDN_FILESYSPATH));
    }
    if (_wcsicmp(verb, L"delete") == 0 && !background)
        return DeleteSelectionOf(tab, GetKeyState(VK_SHIFT) < 0);
    return false;
}
