#include "SevenZip.h"
#include "Lang.h"
#include "fastfiles_core.h"
#include <atomic>

// The 7-Zip archive interfaces (CPP/7zip/IStream.h, IProgress.h, Archive/IArchive.h, IPassword.h). Their IIDs are
// {23170F69-40C1-278A-0000-00gg00ii0000} with gg the group and ii the interface number.
namespace
{
constexpr GUID Iid(BYTE group, BYTE id) { return { 0x23170F69, 0x40C1, 0x278A, { 0, 0, 0, group, 0, id, 0, 0 } }; }
constexpr GUID kIidSequentialIn = Iid(3, 1), kIidSequentialOut = Iid(3, 2), kIidInStream = Iid(3, 3), kIidGetSize = Iid(3, 6);
constexpr GUID kIidProgress = Iid(0, 5), kIidPassword = Iid(5, 0x10), kIidSetProperties = Iid(6, 0x03);
constexpr GUID kIidOpenCallback = Iid(6, 0x10), kIidExtractCallback = Iid(6, 0x20), kIidVolumeCallback = Iid(6, 0x30), kIidInArchive = Iid(6, 0x60);

namespace sz
{
struct ISequentialInStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Read(void* data, UINT32 size, UINT32* processed) = 0; };
struct ISequentialOutStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Write(const void* data, UINT32 size, UINT32* processed) = 0; };
struct IInStream : ISequentialInStream { virtual HRESULT STDMETHODCALLTYPE Seek(INT64 offset, UINT32 origin, UINT64* position) = 0; };
struct IStreamGetSize : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetSize(UINT64* size) = 0; };
struct IProgress : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetTotal(UINT64 total) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64* done) = 0;
};
struct IArchiveOpenCallback : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetTotal(const UINT64* files, const UINT64* bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64* files, const UINT64* bytes) = 0;
};
struct IArchiveOpenVolumeCallback : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetProperty(PROPID id, PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, IInStream** stream) = 0;
};
struct ICryptoGetTextPassword : IUnknown { virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) = 0; };
struct IArchiveExtractCallback : IProgress
{
    virtual HRESULT STDMETHODCALLTYPE GetStream(UINT32 index, ISequentialOutStream** out, INT32 askMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareOperation(INT32 askMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(INT32 result) = 0;
};
struct IInArchive : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Open(IInStream* stream, const UINT64* maxCheckStart, IArchiveOpenCallback* callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UINT32* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UINT32 index, PROPID id, PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(const UINT32* indices, UINT32 count, INT32 testMode, IArchiveExtractCallback* callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID id, PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UINT32* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UINT32 index, BSTR* name, PROPID* id, VARTYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UINT32* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UINT32 index, BSTR* name, PROPID* id, VARTYPE* type) = 0;
};
struct ISetProperties : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetProperties(const wchar_t* const* names, const PROPVARIANT* values, UINT32 count) = 0;
};
}

enum : PROPID { kpidPath = 3, kpidName = 4, kpidIsDir = 6, kpidSize = 7, kpidMTime = 12, kpidEncrypted = 15, kpidMethod = 22 };
enum : PROPID { kHandlerName = 0, kHandlerClassId = 1, kHandlerExtension = 2, kHandlerSignature = 6, kHandlerMultiSignature = 7 };
enum : INT32 { kAskExtract = 0 };
enum : INT32 { kResOk = 0, kResUnsupported, kResDataError, kResCrcError, kResUnavailable, kResUnexpectedEnd, kResDataAfterEnd,
    kResIsNotArc, kResHeadersError, kResWrongPassword };

using CreateObjectFn = HRESULT(WINAPI*)(const GUID* clsid, const GUID* iid, void** out);
using GetNumberOfFormatsFn = HRESULT(WINAPI*)(UINT32* count);
using GetHandlerProperty2Fn = HRESULT(WINAPI*)(UINT32 index, PROPID id, PROPVARIANT* value);

struct Format
{
    GUID clsid{};
    std::wstring name;
    std::vector<std::wstring> extensions; // lower case, no dot
    bool signature = false; // has magic bytes: safe to try on a file whose extension it doesn't claim
};

struct Library
{
    HMODULE dll = nullptr;
    CreateObjectFn create = nullptr;
    std::vector<Format> formats;
};

// The job's cancel flag, written by the UI thread
bool IsSet(const bool* flag) { return std::atomic_ref<bool>(*const_cast<bool*>(flag)).load(std::memory_order_relaxed); }

std::wstring Lower(std::wstring s)
{
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

const Library* Lib()
{
    static const Library lib = []
    {
        Library l;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        PathRemoveFileSpecW(exe);
        const std::wstring path = std::wstring(exe) + L"\\7z.dll";
        // Only that file: no search path, so nothing else named 7z.dll gets loaded
        l.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!l.dll)
            return l;
        l.create = reinterpret_cast<CreateObjectFn>(GetProcAddress(l.dll, "CreateObject"));
        auto count = reinterpret_cast<GetNumberOfFormatsFn>(GetProcAddress(l.dll, "GetNumberOfFormats"));
        auto prop = reinterpret_cast<GetHandlerProperty2Fn>(GetProcAddress(l.dll, "GetHandlerProperty2"));
        UINT32 n = 0;
        if (!l.create || !count || !prop || FAILED(count(&n)))
        {
            l.create = nullptr;
            return l;
        }
        for (UINT32 i = 0; i < n; ++i)
        {
            Format f;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (FAILED(prop(i, kHandlerClassId, &v)) || v.vt != VT_BSTR || SysStringByteLen(v.bstrVal) != sizeof(GUID))
            {
                PropVariantClear(&v);
                continue;
            }
            memcpy(&f.clsid, v.bstrVal, sizeof(GUID));
            PropVariantClear(&v);
            if (SUCCEEDED(prop(i, kHandlerName, &v)) && v.vt == VT_BSTR)
                f.name = v.bstrVal;
            PropVariantClear(&v);
            if (SUCCEEDED(prop(i, kHandlerExtension, &v)) && v.vt == VT_BSTR)
            {
                const std::wstring all = Lower(v.bstrVal);
                for (size_t at = 0; at < all.size();)
                {
                    const size_t space = all.find(L' ', at);
                    const std::wstring ext = all.substr(at, space == std::wstring::npos ? std::wstring::npos : space - at);
                    if (!ext.empty())
                        f.extensions.push_back(ext);
                    at = space == std::wstring::npos ? all.size() : space + 1;
                }
            }
            PropVariantClear(&v);
            for (PROPID id : { kHandlerSignature, kHandlerMultiSignature })
            {
                if (SUCCEEDED(prop(i, id, &v)) && v.vt == VT_BSTR && SysStringByteLen(v.bstrVal) > 0)
                    f.signature = true;
                PropVariantClear(&v);
            }
            l.formats.push_back(std::move(f));
        }
        return l;
    }();
    return lib.create ? &lib : nullptr;
}

// --- COM objects handed to 7z.dll ---

template <class Self>
struct RefCounted
{
    std::atomic<ULONG> refs{ 1 };
    ULONG Add() { return ++refs; }
    ULONG Drop(Self* self)
    {
        const ULONG n = --refs;
        if (n == 0)
            delete self;
        return n;
    }
};

class FileInStream final : public sz::IInStream, public sz::IStreamGetSize
{
public:
    explicit FileInStream(HANDLE file) : m_file(file) {}
    ~FileInStream() { CloseHandle(m_file); }
    static FileInStream* Open(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return f == INVALID_HANDLE_VALUE ? nullptr : new FileInStream(f);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == IID_IUnknown || iid == kIidSequentialIn || iid == kIidInStream)
            *out = static_cast<sz::IInStream*>(this);
        else if (iid == kIidGetSize)
            *out = static_cast<sz::IStreamGetSize*>(this);
        else
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_refs.Drop(this); }
    HRESULT STDMETHODCALLTYPE Read(void* data, UINT32 size, UINT32* processed) override
    {
        DWORD read = 0;
        const BOOL ok = ReadFile(m_file, data, size, &read, nullptr);
        if (processed)
            *processed = read;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Seek(INT64 offset, UINT32 origin, UINT64* position) override
    {
        if (origin > FILE_END)
            return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER to{}, now{};
        to.QuadPart = offset;
        if (!SetFilePointerEx(m_file, to, &now, origin)) // 0/1/2 = FILE_BEGIN / CURRENT / END, as 7-Zip's
            return HRESULT_FROM_WIN32(GetLastError());
        if (position)
            *position = static_cast<UINT64>(now.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSize(UINT64* size) override
    {
        LARGE_INTEGER s{};
        if (!GetFileSizeEx(m_file, &s))
            return HRESULT_FROM_WIN32(GetLastError());
        *size = static_cast<UINT64>(s.QuadPart);
        return S_OK;
    }

private:
    HANDLE m_file;
    RefCounted<FileInStream> m_refs;
};

class FileOutStream final : public sz::ISequentialOutStream
{
public:
    FileOutStream(HANDLE file, const bool* cancel) : file(file), m_cancel(cancel) {}
    ~FileOutStream() { CloseHandle(file); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid != IID_IUnknown && iid != kIidSequentialOut)
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = static_cast<sz::ISequentialOutStream*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_refs.Drop(this); }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UINT32 size, UINT32* processed) override
    {
        if (processed)
            *processed = 0;
        if (IsSet(m_cancel))
            return E_ABORT;
        DWORD written = 0;
        if (!WriteFile(file, data, size, &written, nullptr))
        {
            error = GetLastError();
            return HRESULT_FROM_WIN32(error);
        }
        if (processed)
            *processed = written;
        return S_OK;
    }
    // Marked for deletion: goes when the last reference (ours or 7z.dll's) closes the handle
    void Discard()
    {
        FILE_DISPOSITION_INFO d{ TRUE };
        SetFileInformationByHandle(file, FileDispositionInfo, &d, sizeof(d));
    }

    HANDLE file;
    DWORD error = 0;

private:
    const bool* m_cancel;
    RefCounted<FileOutStream> m_refs;
};

std::wstring Leaf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

class OpenCallback final : public sz::IArchiveOpenCallback, public sz::IArchiveOpenVolumeCallback, public sz::ICryptoGetTextPassword
{
public:
    OpenCallback(std::wstring archive, std::wstring password) : m_archive(std::move(archive)), m_password(std::move(password)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == IID_IUnknown || iid == kIidOpenCallback)
            *out = static_cast<sz::IArchiveOpenCallback*>(this);
        else if (iid == kIidVolumeCallback)
            *out = static_cast<sz::IArchiveOpenVolumeCallback*>(this);
        else if (iid == kIidPassword)
            *out = static_cast<sz::ICryptoGetTextPassword*>(this);
        else
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_refs.Drop(this); }
    HRESULT STDMETHODCALLTYPE SetTotal(const UINT64*, const UINT64*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64*, const UINT64*) override { return S_OK; }
    // Multi-volume archives (.7z.001, .part2.rar): the other parts next to the first
    HRESULT STDMETHODCALLTYPE GetProperty(PROPID id, PROPVARIANT* value) override
    {
        PropVariantInit(value);
        if (id == kpidName)
        {
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(Leaf(m_archive).c_str());
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, sz::IInStream** stream) override
    {
        *stream = nullptr;
        const size_t slash = m_archive.find_last_of(L'\\');
        const std::wstring path = (slash == std::wstring::npos ? std::wstring() : m_archive.substr(0, slash + 1)) + Leaf(name);
        FileInStream* s = FileInStream::Open(path);
        if (!s)
            return S_FALSE;
        *stream = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) override
    {
        passwordAsked = true;
        if (m_password.empty())
            return E_ABORT; // the caller asks the user, then opens again
        *password = SysAllocString(m_password.c_str());
        return S_OK;
    }

    bool passwordAsked = false;

private:
    std::wstring m_archive, m_password;
    RefCounted<OpenCallback> m_refs;
};

// --- Archive access ---

std::wstring PropString(sz::IInArchive* a, UINT32 i, PROPID id)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(a->GetProperty(i, id, &v)) && v.vt == VT_BSTR && v.bstrVal)
        s = v.bstrVal;
    PropVariantClear(&v);
    return s;
}

bool PropBool(sz::IInArchive* a, UINT32 i, PROPID id)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    const bool b = SUCCEEDED(a->GetProperty(i, id, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    PropVariantClear(&v);
    return b;
}

ULONGLONG PropNumber(sz::IInArchive* a, UINT32 i, PROPID id)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    ULONGLONG n = 0;
    if (SUCCEEDED(a->GetProperty(i, id, &v)))
        n = v.vt == VT_UI8 ? v.uhVal.QuadPart : v.vt == VT_UI4 ? v.ulVal : v.vt == VT_UI2 ? v.uiVal : 0;
    PropVariantClear(&v);
    return n;
}

bool PropTime(sz::IInArchive* a, UINT32 i, PROPID id, FILETIME& out)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    const bool ok = SUCCEEDED(a->GetProperty(i, id, &v)) && v.vt == VT_FILETIME;
    if (ok)
        out = v.filetime;
    PropVariantClear(&v);
    return ok;
}

// The item name when the format stores none (a .gz / .bz2 / .xz holds one unnamed stream): 7-Zip's own rule, the
// archive name without its extension, and "x.tgz" holds "x.tar"
std::wstring DefaultName(const std::wstring& archive)
{
    const std::wstring leaf = Leaf(archive);
    const size_t dot = leaf.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0)
        return leaf + L"~";
    const std::wstring ext = Lower(leaf.substr(dot + 1));
    const bool tar = ext == L"tgz" || ext == L"tpz" || ext == L"taz" || ext == L"tbz" || ext == L"tbz2" || ext == L"txz" || ext == L"tzst";
    return leaf.substr(0, dot) + (tar ? L".tar" : L"");
}

std::wstring ItemPath(sz::IInArchive* a, UINT32 i, const std::wstring& archive)
{
    std::wstring p = PropString(a, i, kpidPath);
    return p.empty() ? DefaultName(archive) : p;
}

bool ItemEncrypted(sz::IInArchive* a, UINT32 i)
{
    if (PropBool(a, i, kpidEncrypted))
        return true;
    const std::wstring method = PropString(a, i, kpidMethod); // Files: Method contains "Crypto" or "AES"
    return method.find(L"Crypto") != std::wstring::npos || method.find(L"AES") != std::wstring::npos;
}

struct Opened
{
    ComPtr<sz::IInArchive> archive;
    SevenZip::Status status = SevenZip::Status::Unreadable;
    bool headerEncrypted = false;
};

// The formats claiming the extension first, then every other one (a .zip that is really a 7z, a renamed .rar)
Opened OpenArchive(const std::wstring& path, const std::wstring& password, UINT codepage)
{
    Opened o;
    const SevenZip::Status none = SevenZip::Status::NoLibrary;
    const Library* lib = Lib();
    if (!lib)
    {
        o.status = none;
        return o;
    }
    ComPtr<FileInStream> stream;
    stream.Attach(FileInStream::Open(path));
    if (!stream)
        return o;
    const wchar_t* dot = PathFindExtensionW(path.c_str());
    const std::wstring ext = Lower(*dot ? dot + 1 : L"");
    std::vector<const Format*> order;
    for (const Format& f : lib->formats)
        if (std::find(f.extensions.begin(), f.extensions.end(), ext) != f.extensions.end())
            order.push_back(&f);
    // Others only by their magic bytes: signature-less formats (raw LZMA, PPMd...) would take any file
    for (const Format& f : lib->formats)
        if (f.signature && std::find(order.begin(), order.end(), &f) == order.end())
            order.push_back(&f);
    const UINT64 start = 0; // the signature must be at the start: no hunting through arbitrary files
    for (const Format* f : order)
    {
        ComPtr<sz::IInArchive> a;
        if (FAILED(lib->create(&f->clsid, &kIidInArchive, reinterpret_cast<void**>(a.GetAddressOf()))) || !a)
            continue;
        if (codepage && f->name == L"zip")
        {
            ComPtr<sz::ISetProperties> props;
            if (SUCCEEDED(a->QueryInterface(kIidSetProperties, reinterpret_cast<void**>(props.GetAddressOf()))) && props)
            {
                const wchar_t* names[] = { L"cp" };
                PROPVARIANT value;
                PropVariantInit(&value);
                value.vt = VT_UI4;
                value.ulVal = codepage;
                props->SetProperties(names, &value, 1);
            }
        }
        stream->Seek(0, FILE_BEGIN, nullptr);
        ComPtr<OpenCallback> callback;
        callback.Attach(new OpenCallback(path, password));
        const HRESULT hr = a->Open(stream.Get(), &start, callback.Get());
        if (hr == S_OK)
        {
            o.archive = a;
            o.status = SevenZip::Status::Ok;
            o.headerEncrypted = callback->passwordAsked;
            return o;
        }
        a->Close();
        if (callback->passwordAsked) // it is this format, with encrypted headers
        {
            o.status = password.empty() ? SevenZip::Status::NeedPassword : SevenZip::Status::WrongPassword;
            return o;
        }
    }
    return o;
}

// The Rust extractor's rules (core/src/zip.rs safe_relative): no "..", drive or stream names, control or wildcard
// characters, reserved device names; trailing dots / spaces trimmed. Empty when the name is unsafe.
std::wstring SafeRelative(const std::wstring& name)
{
    static const wchar_t* const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
        L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    std::wstring out;
    for (size_t i = 0; i <= name.size();)
    {
        size_t j = name.find_first_of(L"\\/", i);
        if (j == std::wstring::npos)
            j = name.size();
        std::wstring part = name.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == L".")
            continue;
        if (part == L"..")
            return {};
        for (wchar_t c : part)
            if (c < 32 || wcschr(L"<>\"|?*:", c))
                return {};
        while (!part.empty() && (part.back() == L' ' || part.back() == L'.'))
            part.pop_back();
        if (part.empty())
            return {};
        std::wstring stem = part.substr(0, part.find(L'.'));
        CharUpperBuffW(stem.data(), static_cast<DWORD>(stem.size()));
        for (const wchar_t* r : reserved)
            if (stem == r)
                return {};
        if (!out.empty())
            out += L'\\';
        out += part;
    }
    return out;
}

// Files' ValidateAndGetSafeExtractionPath: an existing junction / symlink on the way would lead out of the target
bool CrossesReparsePoint(const std::wstring& target, const std::wstring& relative)
{
    std::wstring path = target;
    for (size_t i = 0; i < relative.size();)
    {
        size_t j = relative.find(L'\\', i);
        if (j == std::wstring::npos)
            j = relative.size();
        path += L'\\';
        path.append(relative, i, j - i);
        i = j + 1;
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return false; // the rest doesn't exist yet: we create it
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            return true;
    }
    return false;
}

bool MakeDirs(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
        return (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (GetLastError() != ERROR_PATH_NOT_FOUND)
        return false;
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 3 || !MakeDirs(path.substr(0, slash)))
        return false;
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring UniquePath(const std::wstring& path)
{
    const FfBuffer b = ff_unique_path(reinterpret_cast<const uint16_t*>(path.c_str()), path.size());
    std::wstring out(reinterpret_cast<const wchar_t*>(b.data), b.len / sizeof(wchar_t));
    ff_buffer_free(b);
    return out.empty() ? path : out;
}

std::wstring SystemMessage(DWORD code)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring s = text ? text : Tr(L"错误 ") + std::to_wstring(code);
    LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' '))
        s.pop_back();
    return s;
}

class ExtractCallback final : public sz::IArchiveExtractCallback, public sz::ICryptoGetTextPassword
{
public:
    ExtractCallback(sz::IInArchive* archive, std::wstring archivePath, std::wstring target, std::wstring password, uint64_t* progress,
        const bool* cancel)
        : m_archive(archive), m_archivePath(std::move(archivePath)), m_target(std::move(target)), m_password(std::move(password)),
          m_progress(progress), m_cancel(cancel)
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == IID_IUnknown || iid == kIidProgress || iid == kIidExtractCallback)
            *out = static_cast<sz::IArchiveExtractCallback*>(this);
        else if (iid == kIidPassword)
            *out = static_cast<sz::ICryptoGetTextPassword*>(this);
        else
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_refs.Drop(this); }

    HRESULT STDMETHODCALLTYPE SetTotal(UINT64 total) override
    {
        if (Counter(0).load(std::memory_order_relaxed) == 0)
            Counter(0).store(total, std::memory_order_relaxed);
        return Cancelled() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64* done) override
    {
        if (done)
            Counter(1).store(std::min<uint64_t>(*done, Counter(0).load(std::memory_order_relaxed)), std::memory_order_relaxed);
        return Cancelled() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UINT32 index, sz::ISequentialOutStream** out, INT32 askMode) override
    {
        *out = nullptr;
        m_file = false;
        if (askMode != kAskExtract)
            return S_OK;
        if (Cancelled())
            return E_ABORT;
        m_name = ItemPath(m_archive, index, m_archivePath);
        const bool folder = PropBool(m_archive, index, kpidIsDir);
        m_file = !folder;
        m_encrypted = ItemEncrypted(m_archive, index);
        const std::wstring relative = SafeRelative(m_name);
        if (relative.empty() || CrossesReparsePoint(m_target, relative))
        {
            ++skipped; // no stream: 7z.dll skips the data
            return S_OK;
        }
        std::wstring dest = m_target + L"\\" + relative;
        if (folder)
        {
            if (!MakeDirs(dest))
                return Fail(GetLastError(), dest);
            return S_OK;
        }
        const size_t slash = dest.find_last_of(L'\\');
        if (!MakeDirs(dest.substr(0, slash)))
            return Fail(GetLastError(), dest);
        dest = UniquePath(dest); // never over an existing file
        HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return Fail(GetLastError(), dest);
        m_current.Attach(new FileOutStream(f, m_cancel));
        m_hasTime = PropTime(m_archive, index, kpidMTime, m_time);
        *out = m_current.Get();
        m_current->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(INT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(INT32 result) override
    {
        if (m_current)
        {
            if (result == kResOk && !m_current->error)
            {
                if (m_hasTime)
                    SetFileTime(m_current->file, nullptr, nullptr, &m_time);
            }
            else
                m_current->Discard();
            m_current.Reset();
        }
        if (result != kResOk && status == SevenZip::Status::Ok)
            Record(result);
        if (m_file)
            Counter(3).fetch_add(1, std::memory_order_relaxed);
        m_file = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) override
    {
        passwordAsked = true;
        if (m_password.empty())
            return E_ABORT;
        *password = SysAllocString(m_password.c_str());
        return S_OK;
    }
    // After Extract returns: a file still open was cut short (cancelled, or an error stopped 7z.dll)
    void Finish()
    {
        if (m_current)
        {
            m_current->Discard();
            m_current.Reset();
        }
    }

    SevenZip::Status status = SevenZip::Status::Ok;
    std::wstring error;
    uint64_t skipped = 0;
    bool passwordAsked = false;

private:
    std::atomic_ref<uint64_t> Counter(int i) const { return std::atomic_ref<uint64_t>(m_progress[i]); }
    bool Cancelled() const { return IsSet(m_cancel); }
    HRESULT Fail(DWORD code, const std::wstring& path)
    {
        if (status == SevenZip::Status::Ok)
        {
            status = SevenZip::Status::Failed;
            error = Tr(L"无法写入 ") + path + L"\n" + SystemMessage(code);
        }
        return HRESULT_FROM_WIN32(code ? code : ERROR_WRITE_FAULT);
    }
    void Record(INT32 result)
    {
        const std::wstring name = L"“" + Leaf(m_name) + L"”";
        status = SevenZip::Status::Failed;
        if ((result == kResDataError || result == kResCrcError || result == kResWrongPassword) && (m_encrypted || result == kResWrongPassword))
        {
            status = SevenZip::Status::WrongPassword;
            error = SevenZip::Message(status);
        }
        else if (result == kResUnsupported)
            error = name + Tr(L"使用了不支持的压缩方式");
        else if (result == kResCrcError)
            error = name + Tr(L"校验失败，压缩文件可能已损坏");
        else if (result == kResDataError || result == kResHeadersError)
            error = name + Tr(L"的数据已损坏");
        else if (result == kResUnexpectedEnd)
            error = Tr(L"压缩文件不完整，") + name + Tr(L"缺少数据");
        else if (result == kResUnavailable)
            error = Tr(L"缺少分卷，无法解压") + name;
        else
            error = Tr(L"解压") + name + Tr(L"失败");
    }

    sz::IInArchive* m_archive;
    std::wstring m_archivePath, m_target, m_password, m_name;
    uint64_t* m_progress;
    const bool* m_cancel;
    ComPtr<FileOutStream> m_current;
    FILETIME m_time{};
    bool m_hasTime = false, m_file = false, m_encrypted = false;
    RefCounted<ExtractCallback> m_refs;
};
}

namespace SevenZip
{
bool Available() { return Lib() != nullptr; }

std::wstring Message(Status status)
{
    switch (status)
    {
    case Status::NeedPassword: return Tr(L"此压缩文件有密码");
    case Status::WrongPassword: return Tr(L"密码错误，或者压缩文件已损坏");
    case Status::Unreadable: return Tr(L"无法打开此压缩文件：格式不受支持，或者文件已损坏");
    case Status::NoLibrary: return Tr(L"找不到 7z.dll，无法处理此格式。请重新安装 FastFiles。");
    case Status::Failed: return Tr(L"解压失败");
    default: return {};
    }
}

Listing List(const std::wstring& archive, const std::wstring& password, size_t max, UINT codepage)
{
    Listing out;
    Opened o = OpenArchive(archive, password, codepage);
    out.status = o.status;
    if (!o.archive)
        return out;
    out.encrypted = o.headerEncrypted;
    UINT32 n = 0;
    o.archive->GetNumberOfItems(&n);
    for (UINT32 i = 0; i < n; ++i)
    {
        Entry e;
        e.path = ItemPath(o.archive.Get(), i, archive);
        std::replace(e.path.begin(), e.path.end(), L'/', L'\\');
        e.folder = PropBool(o.archive.Get(), i, kpidIsDir);
        e.size = e.folder ? 0 : PropNumber(o.archive.Get(), i, kpidSize);
        ++out.items;
        if (e.folder)
            ++out.folders;
        else
        {
            ++out.files;
            out.bytes += e.size;
        }
        if (!out.encrypted && ItemEncrypted(o.archive.Get(), i))
            out.encrypted = true;
        if (out.entries.size() < max)
            out.entries.push_back(std::move(e));
    }
    o.archive->Close();
    return out;
}

int TopLevel(const std::wstring& archive)
{
    Opened o = OpenArchive(archive, L"", 0);
    if (!o.archive)
        return -1;
    UINT32 n = 0;
    o.archive->GetNumberOfItems(&n);
    std::wstring first;
    int result = 0;
    for (UINT32 i = 0; i < n && result < 2; ++i)
    {
        // Files' GetFirstMeaningfulSegment: skip separators, "." and ".."
        const std::wstring path = ItemPath(o.archive.Get(), i, archive);
        std::wstring segment;
        for (size_t at = 0; at < path.size();)
        {
            size_t end = path.find_first_of(L"\\/", at);
            if (end == std::wstring::npos)
                end = path.size();
            std::wstring s = path.substr(at, end - at);
            at = end + 1;
            if (!s.empty() && s != L"." && s != L"..")
            {
                segment = std::move(s);
                break;
            }
        }
        if (segment.empty())
            continue;
        if (first.empty())
        {
            first = segment;
            result = 1;
        }
        else if (segment != first)
            result = 2;
    }
    o.archive->Close();
    return result;
}

Status Extract(const std::wstring& archive, const std::wstring& target, const std::wstring& password, UINT codepage, uint64_t* progress,
    const bool* cancel, uint64_t* skipped, std::wstring& error)
{
    Opened o = OpenArchive(archive, password, codepage);
    if (!o.archive)
    {
        error = Message(o.status);
        return o.status;
    }
    UINT32 n = 0;
    o.archive->GetNumberOfItems(&n);
    uint64_t bytes = 0, files = 0;
    for (UINT32 i = 0; i < n; ++i)
        if (!PropBool(o.archive.Get(), i, kpidIsDir))
        {
            ++files;
            bytes += PropNumber(o.archive.Get(), i, kpidSize);
        }
    std::atomic_ref<uint64_t>(progress[0]).store(bytes, std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(progress[2]).store(files, std::memory_order_relaxed);
    if (!MakeDirs(target))
    {
        error = Tr(L"无法创建文件夹 ") + target + L"\n" + SystemMessage(GetLastError());
        o.archive->Close();
        return Status::Failed;
    }
    ComPtr<ExtractCallback> callback;
    callback.Attach(new ExtractCallback(o.archive.Get(), archive, target, password, progress, cancel));
    const HRESULT hr = o.archive->Extract(nullptr, static_cast<UINT32>(-1), FALSE, callback.Get());
    callback->Finish();
    o.archive->Close();
    if (skipped)
        *skipped = callback->skipped;
    if (IsSet(cancel))
        return Status::Cancelled;
    if (callback->passwordAsked && password.empty())
    {
        error = Message(Status::NeedPassword);
        return Status::NeedPassword;
    }
    if (callback->status != Status::Ok)
    {
        error = callback->error;
        return callback->status;
    }
    if (FAILED(hr))
    {
        error = Tr(L"解压失败：") + SystemMessage(static_cast<DWORD>(hr));
        return Status::Failed;
    }
    return Status::Ok;
}
}
