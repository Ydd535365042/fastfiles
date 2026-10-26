#include "Share.h"
#include "ContentDialog.h"
#include "Lang.h"
#include "Log.h"
#include "Sandbox.h"
#include "Settings.h"
#include <roapi.h>
#include <winstring.h>
#include <wrl/implements.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.applicationmodel.datatransfer.h>
#include <windows.storage.h>
#include <thread>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
namespace DT = ABI::Windows::ApplicationModel::DataTransfer;
namespace St = ABI::Windows::Storage;
namespace Fd = ABI::Windows::Foundation;

namespace Share
{
namespace
{
// One share: the paths checked on a worker, the StorageFiles made once the sheet asks for them (Files'
// Manager_DataRequested: GetDeferral, ToStorageItem per item, SetStorageItems, Complete)
struct Job
{
    std::vector<std::wstring> paths;
    std::wstring link;  // a .url's address: shared as a web link instead
    std::wstring title; // the first item's name
    size_t next = 0;    // Resolve: the path being turned into a StorageFile
    std::vector<ComPtr<St::IStorageItem>> items;
    ComPtr<DT::IDataRequest> request;   // the sheet's request (empty: a test's dry run)
    ComPtr<DT::IDataRequestDeferral> deferral;
};

// Test mode: <data>\share-log.txt; failures also go to FastFiles' debug log, in every mode
void Trace(const std::wstring& line, bool failure = false)
{
    if (failure)
        Log::Warning(L"Share: " + line);
    if (!Sandbox::Active())
        return;
    const std::string text = ToUtf8(line + L"\r\n");
    HANDLE f = CreateFileW((Sandbox::Dir() + L"share-log.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(f);
}

std::wstring Hr(HRESULT hr)
{
    wchar_t text[16];
    swprintf_s(text, L"0x%08X", static_cast<unsigned>(hr));
    return text;
}

// IIterable<IStorageItem*> over the prepared items (DataPackage.SetStorageItems takes one)
class ItemIterator : public RuntimeClass<RuntimeClassFlags<WinRt>, Fd::Collections::IIterator<St::IStorageItem*>>
{
    InspectableClass(L"FastFiles.ShareItemIterator", BaseTrust)
public:
    explicit ItemIterator(std::shared_ptr<std::vector<ComPtr<St::IStorageItem>>> items) : m_items(std::move(items)) {}
    IFACEMETHODIMP get_Current(St::IStorageItem** current) override
    {
        if (m_at >= m_items->size())
            return E_BOUNDS;
        return (*m_items)[m_at].CopyTo(current);
    }
    IFACEMETHODIMP get_HasCurrent(boolean* has) override
    {
        *has = m_at < m_items->size();
        return S_OK;
    }
    IFACEMETHODIMP MoveNext(boolean* has) override
    {
        if (m_at < m_items->size())
            ++m_at;
        *has = m_at < m_items->size();
        return S_OK;
    }
    IFACEMETHODIMP GetMany(unsigned capacity, St::IStorageItem** out, unsigned* actual) override
    {
        *actual = 0;
        while (*actual < capacity && m_at < m_items->size())
            (*m_items)[m_at++].CopyTo(&out[(*actual)++]);
        return S_OK;
    }
private:
    std::shared_ptr<std::vector<ComPtr<St::IStorageItem>>> m_items;
    size_t m_at = 0;
};

class ItemList : public RuntimeClass<RuntimeClassFlags<WinRt>, Fd::Collections::IIterable<St::IStorageItem*>>
{
    InspectableClass(L"FastFiles.ShareItemList", BaseTrust)
public:
    explicit ItemList(std::vector<ComPtr<St::IStorageItem>> items) : m_items(std::make_shared<std::vector<ComPtr<St::IStorageItem>>>(std::move(items))) {}
    IFACEMETHODIMP First(Fd::Collections::IIterator<St::IStorageItem*>** first) override
    {
        ComPtr<ItemIterator> it = Make<ItemIterator>(m_items);
        return it ? it.CopyTo(first) : E_OUTOFMEMORY;
    }
private:
    std::shared_ptr<std::vector<ComPtr<St::IStorageItem>>> m_items;
};

// The URL= line of an Internet shortcut
std::wstring UrlOf(const std::wstring& path)
{
    wchar_t url[2048]{};
    GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, ARRAYSIZE(url), path.c_str());
    return url;
}

bool HasExt(const std::wstring& path, const wchar_t* ext)
{
    return _wcsicmp(PathFindExtensionW(path.c_str()), ext) == 0;
}

// StorageFile.GetFileFromPathAsync for each path in turn (no thread of ours waits: the completions run on the
// runtime's thread pool), then the job goes back to the UI thread (kMsgReady, wParam 1) to fill the request
void Resolve(HWND owner, std::shared_ptr<Job> job)
{
    ComPtr<St::IStorageFileStatics> statics;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_StorageFile).Get(), IID_PPV_ARGS(&statics));
    if (FAILED(hr))
        Trace(L"StorageFile factory " + Hr(hr), true);
    while (statics && job->next < job->paths.size())
    {
        const std::wstring path = job->paths[job->next++];
        ComPtr<Fd::IAsyncOperation<St::StorageFile*>> op;
        hr = statics->GetFileFromPathAsync(HStringReference(path.c_str()).Get(), &op);
        if (SUCCEEDED(hr))
            hr = op->put_Completed(Callback<Fd::IAsyncOperationCompletedHandler<St::StorageFile*>>(
                [owner, job, path](Fd::IAsyncOperation<St::StorageFile*>* done, Fd::AsyncStatus status)
                {
                    ComPtr<St::IStorageFile> file;
                    ComPtr<St::IStorageItem> item;
                    const HRESULT r = status == Fd::AsyncStatus::Completed ? done->GetResults(&file) : E_FAIL;
                    if (SUCCEEDED(r) && file && SUCCEEDED(file.As(&item)))
                        job->items.push_back(item);
                    else
                        Trace(L"GetFileFromPathAsync " + path + L" status " + std::to_wstring(static_cast<int>(status)) + L" " + Hr(r), true);
                    Resolve(owner, job);
                    return S_OK;
                }).Get());
        if (SUCCEEDED(hr))
            return; // the completion goes on
        Trace(L"GetFileFromPathAsync " + path + L" " + Hr(hr), true);
    }
    auto* held = new std::shared_ptr<Job>(job);
    if (!PostMessageW(owner, kMsgReady, 1, reinterpret_cast<LPARAM>(held)))
        delete held;
}

EventRegistrationToken g_token{};
ComPtr<DT::IDataTransferManager> g_manager; // the one the pending handler is on (the sheet may be cancelled unseen)

// Files: a link item shares its target as a web link (filled at once, nothing to wait for)
bool FillLink(DT::IDataRequest* request, const Job& job)
{
    ComPtr<DT::IDataPackage> data;
    ComPtr<DT::IDataPackagePropertySet> props;
    ComPtr<Fd::IUriRuntimeClassFactory> uris;
    ComPtr<Fd::IUriRuntimeClass> uri;
    HRESULT hr = request->get_Data(&data);
    if (SUCCEEDED(hr))
        hr = data->get_Properties(&props);
    if (SUCCEEDED(hr))
        hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(), IID_PPV_ARGS(&uris));
    if (SUCCEEDED(hr))
        hr = uris->CreateUri(HStringReference(job.link.c_str()).Get(), &uri);
    if (FAILED(hr))
    {
        Trace(L"link " + job.link + L" " + Hr(hr), true);
        return false;
    }
    const std::wstring title = Lang::Format(L"正在共享 {0}", { job.title });
    props->put_Title(HStringReference(title.c_str()).Get());
    props->put_Description(HStringReference(Tr(L"将共享已选项目")).Get());
    ComPtr<DT::IDataPackage2> data2;
    if (SUCCEEDED(data.As(&data2)))
        hr = data2->SetWebLink(uri.Get());
    else
    {
#pragma warning(suppress : 4996) // Windows 8.0 has no IDataPackage2 (SetWebLink): SetUri is its way to share a link
        hr = data->SetUri(uri.Get());
    }
    Trace(L"request link " + job.link + L" " + Hr(hr), FAILED(hr));
    return SUCCEEDED(hr);
}

// The files are StorageFiles now: into the request, which the deferral then hands to the sheet
void FinishItems(Job& job)
{
    if (!job.request)
        return;
    ComPtr<DT::IDataPackage> data;
    ComPtr<DT::IDataPackagePropertySet> props;
    HRESULT hr = job.request->get_Data(&data);
    if (SUCCEEDED(hr))
        hr = data->get_Properties(&props);
    if (FAILED(hr) || job.items.empty())
    {
        if (FAILED(hr))
            Trace(L"request data " + Hr(hr), true);
        job.request->FailWithDisplayText(HStringReference(Tr(L"目前没有可共享的内容。")).Get());
    }
    else
    {
        const std::wstring title = job.items.size() == 1 ? Lang::Format(L"正在共享 {0}", { job.title })
            : Lang::Format(L"正在共享 {0} {1}", { std::to_wstring(job.items.size()), Tr(L"项") });
        props->put_Title(HStringReference(title.c_str()).Get());
        props->put_Description(HStringReference(job.items.size() == 1 ? Tr(L"将共享已选项目") : Tr(L"将共享你选择的项目")).Get());
        hr = data->SetStorageItems(Make<ItemList>(job.items).Get(), false);
        Trace(L"request items " + std::to_wstring(job.items.size()) + L" " + Hr(hr), FAILED(hr));
    }
    if (job.deferral)
    {
        hr = job.deferral->Complete();
        Trace(L"deferral complete " + Hr(hr), FAILED(hr));
    }
    job.request.Reset();
    job.deferral.Reset();
}

// Test mode shows the real sheet only when a test asks for it (FASTFILES_TEST_SHARESHEET=1): it's the system's window
bool RealSheet()
{
    if (!Sandbox::Active())
        return true;
    wchar_t v[4]{};
    return GetEnvironmentVariableW(L"FASTFILES_TEST_SHARESHEET", v, ARRAYSIZE(v)) > 0 && v[0] == L'1';
}
}

namespace
{
// ntshrui's "Shell extensions for sharing" (HKCR\Directory\shellex\ContextMenuHandlers\Sharing): Explorer's
// 授予访问权限 submenu, whose last item (特定用户...) opens the 网络访问 window
constexpr CLSID kSharingHandler = { 0xf81e9010, 0x6ea4, 0x11ce, { 0xa7, 0xff, 0x00, 0xaa, 0x00, 0x3c, 0xa9, 0xf6 } };
constexpr UINT kGiveFirst = 1, kGiveLast = 0x7FFF;

struct GiveMenu
{
    ComPtr<IContextMenu> handler;
    HMENU menu = nullptr;
    std::wstring text; // 授予访问权限
    UINT pick = 0;     // 特定用户...
    ~GiveMenu()
    {
        if (menu)
            DestroyMenu(menu);
    }
};

// The handler on the folders (one parent), its submenu filled as when it opens (WM_INITMENUPOPUP)
bool BuildGive(HWND owner, const std::vector<std::wstring>& folders, GiveMenu& g)
{
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const std::wstring& path : folders)
        if (PIDLIST_ABSOLUTE pidl = nullptr; SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)))
            pidls.push_back(pidl);
    ComPtr<IShellItemArray> items;
    ComPtr<IDataObject> data;
    ComPtr<IShellExtInit> init;
    PIDLIST_ABSOLUTE parent = pidls.size() == folders.size() && !pidls.empty() ? ILClone(pidls[0]) : nullptr;
    HRESULT hr = parent && ILRemoveLastID(parent) ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items);
    if (SUCCEEDED(hr))
        hr = items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(kSharingHandler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init));
    if (SUCCEEDED(hr))
        hr = init->Initialize(parent, data.Get(), nullptr);
    if (SUCCEEDED(hr))
        hr = init.As(&g.handler);
    if (SUCCEEDED(hr) && (g.menu = CreatePopupMenu()) != nullptr)
        hr = g.handler->QueryContextMenu(g.menu, 0, kGiveFirst, kGiveLast, CMF_NORMAL | CMF_EXPLORE);
    for (PIDLIST_ABSOLUTE pidl : pidls)
        CoTaskMemFree(pidl);
    CoTaskMemFree(parent);
    if (FAILED(hr) || !g.menu)
    {
        Trace(L"give access handler " + Hr(hr), true);
        return false;
    }
    ComPtr<IContextMenu3> menu3;
    ComPtr<IContextMenu2> menu2;
    g.handler.As(&menu3);
    g.handler.As(&menu2);
    for (int i = 0; i < GetMenuItemCount(g.menu) && !g.pick; ++i)
    {
        HMENU sub = GetSubMenu(g.menu, i);
        if (!sub)
            continue;
        LRESULT result = 0;
        if (menu3)
            menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(i, FALSE), &result);
        else if (menu2)
            menu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(i, FALSE));
        for (int j = GetMenuItemCount(sub) - 1; j >= 0 && !g.pick; --j)
        {
            const UINT id = GetMenuItemID(sub, j);
            if (id >= kGiveFirst && id <= kGiveLast && !(GetMenuState(sub, j, MF_BYPOSITION) & (MF_GRAYED | MF_DISABLED)))
                g.pick = id;
        }
        wchar_t text[128]{};
        GetMenuStringW(g.menu, i, text, ARRAYSIZE(text), MF_BYPOSITION);
        g.text = text;
    }
    if (!g.pick)
        Trace(L"give access: no item", true);
    return g.pick != 0;
}
}

bool FolderShareable(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !PathIsRootW(path.c_str());
}

std::wstring GiveAccessText(const std::vector<std::wstring>& folders)
{
    static std::wstring text; // the system's string: the same every time
    if (text.empty() && !folders.empty())
    {
        GiveMenu g;
        if (BuildGive(nullptr, folders, g))
            text = g.text;
    }
    return text;
}

bool GiveAccess(HWND owner, const std::vector<std::wstring>& folders)
{
    GiveMenu g;
    if (!BuildGive(owner, folders, g))
    {
        // No sharing handler here (sharing turned off): the folder's property sheet, whose 共享 tab does the same
        if (folders.size() != 1)
            return false;
        Trace(L"give access: property sheet");
        if (!RealSheet())
            return true;
        return SHObjectProperties(owner, SHOP_FILEPATH, folders[0].c_str(), nullptr) != FALSE;
    }
    if (!RealSheet())
    {
        Trace(L"[dry-run] give access " + std::to_wstring(folders.size()) + L" folders " + g.text);
        return true;
    }
    CMINVOKECOMMANDINFOEX info{ sizeof(info) };
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = owner;
    info.lpVerb = MAKEINTRESOURCEA(g.pick - kGiveFirst);
    info.lpVerbW = MAKEINTRESOURCEW(g.pick - kGiveFirst);
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr = g.handler->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)); // the 网络访问 window (modal)
    Trace(L"give access " + Hr(hr), FAILED(hr));
    return SUCCEEDED(hr);
}

bool Shareable(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) && !HasExt(path, L".lnk");
}

void Show(HWND owner, std::vector<std::wstring> paths)
{
    std::thread([owner, paths = std::move(paths)]
    {
        // Only the file checks here (they may touch a slow disk); the StorageFiles come when the sheet asks
        auto job = std::make_unique<Job>();
        for (const std::wstring& path : paths)
        {
            if (!Shareable(path))
                continue;
            if (job->title.empty())
                job->title = PathFindFileNameW(path.c_str());
            if (HasExt(path, L".url"))
                if (std::wstring url = UrlOf(path); !url.empty())
                {
                    job->link = url; // Files: the first link item wins
                    job->paths.clear();
                    break;
                }
            job->paths.push_back(path);
        }
        auto* held = new std::shared_ptr<Job>(std::move(job));
        if (!PostMessageW(owner, kMsgReady, 0, reinterpret_cast<LPARAM>(held)))
            delete held;
    }).detach();
}

void OnReady(HWND owner, const Theme* theme, WPARAM stage, LPARAM data)
{
    std::unique_ptr<std::shared_ptr<Job>> held(reinterpret_cast<std::shared_ptr<Job>*>(data));
    std::shared_ptr<Job> job = *held;
    if (stage == 1)
    {
        // The StorageFiles are made: the sheet's request gets them, or a test logs what it would have got
        if (!job->request)
            Trace(L"[dry-run] share " + std::to_wstring(job->items.size()) + L" items title " + job->title);
        FinishItems(*job);
        return;
    }
    if (!RealSheet())
    {
        if (!job->link.empty())
            Trace(L"[dry-run] share 0 items link " + job->link + L" title " + job->title);
        else
            Resolve(owner, job); // the same StorageFiles the sheet would get
        return;
    }
    ComPtr<IDataTransferManagerInterop> interop;
    ComPtr<DT::IDataTransferManager> manager;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager).Get(), IID_PPV_ARGS(&interop));
    if (SUCCEEDED(hr))
        hr = interop->GetForWindow(owner, IID_PPV_ARGS(&manager));
    Trace(L"GetForWindow " + Hr(hr), FAILED(hr));
    if (FAILED(hr))
    {
        ContentDialog::Notice(owner, theme, Tr(L"共享项目失败"), Tr(L"此版本的 Windows 不支持共享。"));
        return;
    }
    // A sheet that was cancelled before asking leaves its handler behind: only the latest one answers
    if (g_manager)
        g_manager->remove_DataRequested(g_token);
    g_manager = manager;
    manager->add_DataRequested(Callback<Fd::ITypedEventHandler<DT::DataTransferManager*, DT::DataRequestedEventArgs*>>(
        [owner, job](DT::IDataTransferManager* sender, DT::IDataRequestedEventArgs* args)
        {
            sender->remove_DataRequested(g_token);
            g_manager.Reset();
            ComPtr<DT::IDataRequest> request;
            HRESULT r = args->get_Request(&request);
            Trace(L"DataRequested " + Hr(r), FAILED(r));
            if (FAILED(r))
                return S_OK;
            if (!job->link.empty())
            {
                if (!FillLink(request.Get(), *job))
                    request->FailWithDisplayText(HStringReference(Tr(L"目前没有可共享的内容。")).Get());
                return S_OK;
            }
            // Files: GetDeferral, then the StorageFiles, SetStorageItems and Complete once they're made
            r = request->GetDeferral(&job->deferral);
            Trace(L"GetDeferral " + Hr(r), FAILED(r));
            job->request = request;
            Resolve(owner, job);
            return S_OK;
        }).Get(), &g_token);
    hr = interop->ShowShareUIForWindow(owner);
    Trace(L"ShowShareUIForWindow " + Hr(hr), FAILED(hr));
    if (FAILED(hr))
    {
        manager->remove_DataRequested(g_token);
        g_manager.Reset();
        ContentDialog::Notice(owner, theme, Tr(L"共享项目失败"), Tr(L"无法打开共享窗口。"));
    }
}
}
