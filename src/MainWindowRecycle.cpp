#include "MainWindow.h"
#include "Lang.h"
#include "Sandbox.h"
#include "ContentDialog.h"
#include <propsys.h>

namespace
{
// Queues one recycled item to move back to the folder it was deleted from (System.Recycle.DeletedFrom), like
// Files' StorageTrashBinService.RestoreAllTrashesInternal. A folder that no longer exists is skipped (Files does too).
bool QueueRestore(IFileOperation* op, IShellItem* item)
{
    static PROPERTYKEY deletedFrom{};
    static const bool known = SUCCEEDED(PSGetPropertyKeyFromName(L"System.Recycle.DeletedFrom", &deletedFrom));
    ComPtr<IShellItem2> item2;
    wchar_t* from = nullptr;
    if (!known || FAILED(item->QueryInterface(IID_PPV_ARGS(&item2))) || FAILED(item2->GetString(deletedFrom, &from)))
        return false;
    ComPtr<IShellItem> folder;
    const HRESULT hr = SHCreateItemFromParsingName(from, nullptr, IID_PPV_ARGS(&folder));
    CoTaskMemFree(from);
    return SUCCEEDED(hr) && SUCCEEDED(op->MoveItem(item, folder.Get(), nullptr, nullptr));
}

std::vector<ComPtr<IShellItem>> RecycledItems()
{
    std::vector<ComPtr<IShellItem>> items;
    ComPtr<IShellItem> bin;
    ComPtr<IEnumShellItems> e;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin))) ||
        FAILED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&e))))
        return items;
    for (ComPtr<IShellItem> item; e->Next(1, &item, nullptr) == S_OK; item.Reset())
        items.push_back(item);
    return items;
}
}

void MainWindow::RestoreFromRecycleBin(bool all)
{
    Tab* tab = Active();
    if (!tab || !tab->IsRecycleBin() || Sandbox::Active()) // test mode: the real recycle bin is never touched
        return;
    std::vector<ComPtr<IShellItem>> items;
    if (all)
        items = RecycledItems();
    else if (ComPtr<IShellView> view = tab->ShellView())
    {
        ComPtr<IShellItemArray> selection;
        DWORD count = 0;
        if (SUCCEEDED(view->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&selection))) && SUCCEEDED(selection->GetCount(&count)))
            for (DWORD i = 0; i < count; ++i)
                if (ComPtr<IShellItem> item; SUCCEEDED(selection->GetItemAt(i, &item)))
                    items.push_back(item);
    }
    if (items.empty())
        return; // Files: not executable while the bin (or the selection) is empty

    // Files' ConfirmRestoreBinDialog / ConfirmRestoreSelectionBinDialog: 是 / 取消, DefaultButton = Primary
    const std::wstring text = all ? Tr(L"确定要还原回收站中的所有项目吗？") : Tr(L"是否还原选中的 ") + std::to_wstring(items.size()) + Tr(L" 个项目？");
    if (!ContentDialog::Confirm(m_hwnd, &m_theme, all ? Tr(L"还原所有项目") : Tr(L"还原选定的项目"), text))
        return;

    ComPtr<IFileOperation> op;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
        SUCCEEDED(op->SetOwnerWindow(m_hwnd)) &&
        // The shell's progress and name-conflict prompts stay on: restoring never silently replaces a file that is
        // back at the original path (Files passes FOF_NO_UI, which answers "yes to all")
        SUCCEEDED(op->SetOperationFlags(FOF_NOCONFIRMMKDIR | FOFX_EARLYFAILURE));
    if (ok)
    {
        int queued = 0;
        for (const auto& item : items)
            queued += QueueRestore(op.Get(), item.Get()) ? 1 : 0;
        BOOL aborted = FALSE;
        ok = queued > 0 && SUCCEEDED(op->PerformOperations()) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted));
        if (aborted)
            return; // the user cancelled the shell's prompt
    }
    if (!ok)
    {
        Log::Warning(L"Failed to restore items from the Recycle Bin");
        // Files' RestoreAllRecycleBinAction error dialog: FailedToRestore as the title, no body text, just 确定
        ContentDialog::Notice(m_hwnd, &m_theme, Tr(L"从回收站还原项目失败"), {});
    }
}
