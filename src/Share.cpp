#include "Share.h"
#include "ContentDialog.h"
#include "Lang.h"
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
// What the worker found (StorageFile objects are agile: usable on the UI thread)
struct Prepared
{
    std::vector<ComPtr<St::IStorageItem>> items;
    std::wstring link;  // a .url's address: shared as a web link instead
    std::wstring title; // the first item's name
    size_t count = 0;
};

void Log(const std::wstring& line)
{
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

// StorageFile.GetFileFromPathAsync, waited for (worker thread, multithreaded apartment)
ComPtr<St::IStorageItem> FileFor(St::IStorageFileStatics* statics, const std::wstring& path)
{
    ComPtr<Fd::IAsyncOperation<St::StorageFile*>> op;
    if (FAILED(statics->GetFileFromPathAsync(HStringReference(path.c_str()).Get(), &op)))
        return nullptr;
    Event done(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
    HRESULT hr = op->put_Completed(Callback<Fd::IAsyncOperationCompletedHandler<St::StorageFile*>>(
        [&done](Fd::IAsyncOperation<St::StorageFile*>*, Fd::AsyncStatus) { SetEvent(done.Get()); return S_OK; }).Get());
    if (FAILED(hr) || WaitForSingleObject(done.Get(), 15000) != WAIT_OBJECT_0)
        return nullptr;
    ComPtr<St::IStorageFile> file;
    ComPtr<St::IStorageItem> item;
    if (FAILED(op->GetResults(&file)) || !file || FAILED(file.As(&item)))
        return nullptr;
    return item;
}

EventRegistrationToken g_token{};
ComPtr<DT::IDataTransferManager> g_manager; // the one the pending handler is on (the sheet may be cancelled unseen)

void Fill(DT::IDataRequest* request, const Prepared& p)
{
    ComPtr<DT::IDataPackage> data;
    ComPtr<DT::IDataPackagePropertySet> props;
    if (FAILED(request->get_Data(&data)) || FAILED(data->get_Properties(&props)))
        return;
    if (!p.link.empty())
    {
        // Files: a link item shares its target as a web link
        ComPtr<Fd::IUriRuntimeClassFactory> uris;
        ComPtr<Fd::IUriRuntimeClass> uri;
        ComPtr<DT::IDataPackage2> data2;
        if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(), IID_PPV_ARGS(&uris))) &&
            SUCCEEDED(uris->CreateUri(HStringReference(p.link.c_str()).Get(), &uri)))
        {
            props->put_Title(HStringReference(Lang::Format(L"正在共享 {0}", { p.title }).c_str()).Get());
            props->put_Description(HStringReference(Tr(L"将共享已选项目")).Get());
            if (SUCCEEDED(data.As(&data2)))
                data2->SetWebLink(uri.Get());
            else
            {
#pragma warning(suppress : 4996) // Windows 8.0 has no IDataPackage2 (SetWebLink): SetUri is its way to share a link
                data->SetUri(uri.Get());
            }
            return;
        }
    }
    if (p.items.empty())
    {
        request->FailWithDisplayText(HStringReference(Tr(L"目前没有可共享的内容。")).Get());
        return;
    }
    const std::wstring title = p.items.size() == 1 ? Lang::Format(L"正在共享 {0}", { p.title })
        : Lang::Format(L"正在共享 {0} {1}", { std::to_wstring(p.items.size()), Tr(L"项") });
    props->put_Title(HStringReference(title.c_str()).Get());
    props->put_Description(HStringReference(p.items.size() == 1 ? Tr(L"将共享已选项目") : Tr(L"将共享你选择的项目")).Get());
    data->SetStorageItems(Make<ItemList>(p.items).Get(), false);
}
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
        RoInitializeWrapper ro(RO_INIT_MULTITHREADED);
        auto p = std::make_unique<Prepared>();
        p->count = paths.size();
        ComPtr<St::IStorageFileStatics> statics;
        RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_StorageFile).Get(), IID_PPV_ARGS(&statics));
        for (const std::wstring& path : paths)
        {
            if (!Shareable(path))
                continue;
            if (p->title.empty())
                p->title = PathFindFileNameW(path.c_str());
            if (HasExt(path, L".url"))
                if (std::wstring url = UrlOf(path); !url.empty())
                {
                    p->link = url;
                    p->items.clear();
                    break;
                }
            if (statics)
                if (ComPtr<St::IStorageItem> item = FileFor(statics.Get(), path))
                    p->items.push_back(item);
        }
        Prepared* raw = p.release();
        if (!PostMessageW(owner, kMsgReady, 0, reinterpret_cast<LPARAM>(raw)))
            delete raw;
    }).detach();
}

void OnReady(HWND owner, const Theme* theme, LPARAM data)
{
    auto p = std::shared_ptr<Prepared>(reinterpret_cast<Prepared*>(data));
    if (Sandbox::Active())
    {
        Log(L"[dry-run] share " + std::to_wstring(p->items.size()) + L" items" + (p->link.empty() ? L"" : L" link " + p->link) + L" title " + p->title);
        return;
    }
    ComPtr<IDataTransferManagerInterop> interop;
    ComPtr<DT::IDataTransferManager> manager;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager).Get(), IID_PPV_ARGS(&interop))) ||
        FAILED(interop->GetForWindow(owner, IID_PPV_ARGS(&manager))))
    {
        ContentDialog::Notice(owner, theme, Tr(L"共享项目失败"), Tr(L"此版本的 Windows 不支持共享。"));
        return;
    }
    // A sheet that was cancelled before asking leaves its handler behind: only the latest one answers
    if (g_manager)
        g_manager->remove_DataRequested(g_token);
    g_manager = manager;
    manager->add_DataRequested(Callback<Fd::ITypedEventHandler<DT::DataTransferManager*, DT::DataRequestedEventArgs*>>(
        [p](DT::IDataTransferManager* sender, DT::IDataRequestedEventArgs* args)
        {
            sender->remove_DataRequested(g_token);
            g_manager.Reset();
            ComPtr<DT::IDataRequest> request;
            if (SUCCEEDED(args->get_Request(&request)))
                Fill(request.Get(), *p);
            return S_OK;
        }).Get(), &g_token);
    const HRESULT hr = interop->ShowShareUIForWindow(owner);
    if (FAILED(hr))
    {
        manager->remove_DataRequested(g_token);
        g_manager.Reset();
        ContentDialog::Notice(owner, theme, Tr(L"共享项目失败"), Tr(L"无法打开共享窗口。"));
    }
}
}
