// InfoPane worker: properties (Explorer's own details list for the file type), thumbnail, and what the preview shows.
#include "InfoPane.h"
#include "Format.h"
#include "Lang.h"
#include "MediaPreview.h"
#include "SevenZip.h"
#include "fastfiles_core.h"
#include <propkey.h>
#include <propvarutil.h>
#include <thread>

struct InfoPane::Result
{
    unsigned generation = 0;
    Kind kind = Kind::None;
    std::wstring name, type, note;
    std::vector<std::pair<std::wstring, std::wstring>> props;
    HBITMAP thumb = nullptr;
    bool thumbAlpha = false;
    int icon = -1;
    std::wstring text;
    std::string rtf;        // Kind::Rich
    bool rtfFile = false;
    bool rtfWrap = true;    // Markdown / .rtf wrap; code scrolls sideways (Files' CodePreview ScrollViewer)
    std::wstring media;     // Kind::Media: the file
    std::vector<ArchiveEntry> archive;
    std::wstring archiveSummary;
    CLSID handler{};
    ~Result()
    {
        if (thumb)
            DeleteObject(thumb);
    }
};

namespace
{
struct RichStyle
{
    bool dark;
    COLORREF ink, muted, accent;
    std::wstring font;
};

const uint16_t* U16(const std::wstring& s) { return reinterpret_cast<const uint16_t*>(s.c_str()); }

std::wstring Widen(const char* data, int len, UINT cp)
{
    const int n = MultiByteToWideChar(cp, 0, data, len, nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(cp, 0, data, len, out.data(), n);
    return out;
}

std::wstring FormatBytes(ULONGLONG bytes)
{
    return Format::Size(bytes); // Files' ToSizeString
}

// True when a 32bpp DIB actually uses its alpha channel (photo thumbnails come back with alpha 0 everywhere)
bool HasAlpha(HBITMAP bmp)
{
    DIBSECTION ds{};
    if (!GetObjectW(bmp, sizeof(ds), &ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits)
        return false;
    const auto* px = static_cast<const BYTE*>(ds.dsBm.bmBits);
    const size_t count = static_cast<size_t>(ds.dsBm.bmWidth) * static_cast<size_t>(std::abs(ds.dsBm.bmHeight));
    for (size_t i = 0; i < count; ++i)
        if (px[i * 4 + 3] != 0)
            return true;
    return false;
}

// AlphaBlend(AC_SRC_ALPHA) wants premultiplied pixels. Icon-only images from the factory come back straight (a channel
// above its alpha), which draws as colored fringes on the edges: premultiply those once.
void PremultiplyIfStraight(HBITMAP bmp)
{
    DIBSECTION ds{};
    if (!bmp || !GetObjectW(bmp, sizeof(ds), &ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits)
        return;
    auto* px = static_cast<BYTE*>(ds.dsBm.bmBits);
    const size_t count = static_cast<size_t>(ds.dsBm.bmWidth) * static_cast<size_t>(std::abs(ds.dsBm.bmHeight));
    bool straight = false;
    for (size_t i = 0; i < count && !straight; ++i)
        straight = px[i * 4] > px[i * 4 + 3] || px[i * 4 + 1] > px[i * 4 + 3] || px[i * 4 + 2] > px[i * 4 + 3];
    if (!straight)
        return;
    for (size_t i = 0; i < count; ++i)
    {
        const unsigned a = px[i * 4 + 3];
        for (int c = 0; c < 3; ++c)
            px[i * 4 + c] = static_cast<BYTE>((px[i * 4 + c] * a + 127) / 255);
    }
}

HBITMAP Thumbnail(IShellItem* item, int size, SIIGBF flags)
{
    ComPtr<IShellItemImageFactory> factory;
    HBITMAP bmp = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory))) && FAILED(factory->GetImage({ size, size }, flags, &bmp)))
        bmp = nullptr;
    if (bmp && HasAlpha(bmp))
        PremultiplyIfStraight(bmp);
    return bmp;
}

std::wstring Extension(const std::wstring& path)
{
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    std::wstring out = ext ? ext : L"";
    CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

bool IsOneOf(const std::wstring& ext, std::initializer_list<const wchar_t*> list)
{
    return std::any_of(list.begin(), list.end(), [&](const wchar_t* e) { return ext == e; });
}

// Registered IPreviewHandler for an extension (the same lookup Explorer's preview pane does)
bool FindHandler(const std::wstring& ext, CLSID& clsid)
{
    if (ext.empty())
        return false;
    wchar_t text[64]{};
    DWORD size = ARRAYSIZE(text);
    return SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR | ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION, ext.c_str(),
               L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", text, &size)) &&
        SUCCEEDED(CLSIDFromString(text, &clsid));
}

void AddProperty(IPropertyStore* store, REFPROPERTYKEY key, std::vector<PROPERTYKEY>& seen, std::vector<std::pair<std::wstring, std::wstring>>& out)
{
    if (IsEqualPropertyKey(key, PKEY_ItemNameDisplay) ||
        std::any_of(seen.begin(), seen.end(), [&](const PROPERTYKEY& k) { return IsEqualPropertyKey(k, key); }))
        return;
    seen.push_back(key);
    PROPVARIANT value;
    PropVariantInit(&value);
    ComPtr<IPropertyDescription> desc;
    PWSTR label = nullptr, text = nullptr;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt != VT_EMPTY &&
        SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&desc))) && SUCCEEDED(desc->GetDisplayName(&label)))
    {
        // Files' details pane: sizes and dates in its own formats (size format, date format); the rest as the shell shows them
        if (IsEqualPropertyKey(key, PKEY_Size) && value.vt == VT_UI8)
            out.emplace_back(label, Format::Size(value.uhVal.QuadPart));
        else if ((IsEqualPropertyKey(key, PKEY_DateModified) || IsEqualPropertyKey(key, PKEY_DateCreated) ||
                     IsEqualPropertyKey(key, PKEY_DateAccessed)) && value.vt == VT_FILETIME)
            out.emplace_back(label, Format::DateLong(value.filetime));
        else if (SUCCEEDED(desc->FormatForDisplay(value, PDFF_DEFAULT, &text)) && text && *text)
            out.emplace_back(label, text);
    }
    CoTaskMemFree(label);
    CoTaskMemFree(text);
    PropVariantClear(&value);
}
std::wstring DecodeText(const FfBuffer& buf, uint8_t kind)
{
    const char* data = reinterpret_cast<const char*>(buf.data);
    const int len = static_cast<int>(std::min<size_t>(buf.len, INT_MAX));
    std::wstring text;
    if (kind == 1)
        text = Widen(data, len, CP_UTF8);
    else if (kind == 4)
        text = Widen(data, len, CP_ACP);
    else if (kind == 2 || kind == 3)
    {
        text.resize(buf.len / 2);
        for (size_t i = 0; i < text.size(); ++i)
        {
            const BYTE a = buf.data[i * 2], b = buf.data[i * 2 + 1];
            text[i] = kind == 2 ? static_cast<wchar_t>(a | (b << 8)) : static_cast<wchar_t>((a << 8) | b);
        }
    }
    // The edit control wants CRLF
    std::wstring out;
    out.reserve(text.size() + text.size() / 32);
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
            out += L'\r';
        out += text[i];
    }
    return out;
}

bool ListArchive(const std::wstring& path, InfoPane::Result& r);

constexpr size_t kTextLimit = 10000; // Files' Constants.PreviewPane.TextCharacterLimit

std::wstring Crlf(const std::wstring& text)
{
    // The edit control wants CRLF
    std::wstring out;
    out.reserve(text.size() + text.size() / 32);
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
            out += L'\r';
        out += text[i];
    }
    return out;
}

// The file as text (BOM / UTF-8 / UTF-16 / ANSI sniffed by the Rust core); false when it looks binary
bool ReadText(const std::wstring& path, size_t limit, std::wstring& out)
{
    uint8_t kind = 0;
    bool truncated = false;
    const FfBuffer buf = ff_text_preview(U16(path), path.size(), limit, &kind, &truncated);
    if (kind != 0)
    {
        out = DecodeText(buf, kind);
        out.erase(std::remove(out.begin(), out.end(), L'\r'), out.end());
        // Files' binary test: a run of four NULs
        if (out.find(std::wstring(4, L'\0')) != std::wstring::npos)
            kind = 0;
    }
    ff_buffer_free(buf);
    return kind != 0;
}

bool ReadBytes(const std::wstring& path, size_t limit, std::string& out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    DWORD read = 0;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && static_cast<ULONGLONG>(size.QuadPart) <= limit;
    if (ok)
    {
        out.resize(static_cast<size_t>(size.QuadPart));
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Everything below may block (network shares, slow thumbnail extractors): worker thread only
void Collect(PCIDLIST_ABSOLUTE pidl, InfoPane::Page page, int thumbSize, const RichStyle& style, InfoPane::Result& r)
{
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item))))
        return;
    r.name = PidlName(pidl, SIGDN_NORMALDISPLAY);
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(reinterpret_cast<PCWSTR>(pidl), 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_TYPENAME | SHGFI_SYSICONINDEX))
    {
        r.type = sfi.szTypeName;
        r.icon = sfi.iIcon;
    }
    SFGAOF attrs = 0;
    item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_FILESYSTEM, &attrs);
    const bool folder = (attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM);
    const std::wstring path = PidlName(pidl, SIGDN_FILESYSPATH);
    const std::wstring ext = folder ? std::wstring() : Extension(path.empty() ? r.name : path);

    if (page == InfoPane::Page::Details)
    {
        r.thumb = Thumbnail(item.Get(), thumbSize, SIIGBF_RESIZETOFIT);
        // Same property list Explorer's details pane shows for this type, after a common header
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(item->GetPropertyStore(GPS_DEFAULT, IID_PPV_ARGS(&store))) ||
            SUCCEEDED(item->GetPropertyStore(GPS_BESTEFFORT, IID_PPV_ARGS(&store))))
        {
            std::vector<PROPERTYKEY> seen;
            for (const PROPERTYKEY& key : { PKEY_ItemTypeText, PKEY_Size, PKEY_DateModified, PKEY_DateCreated, PKEY_ItemFolderPathDisplay })
                AddProperty(store.Get(), key, seen, r.props);
            PWSTR list = nullptr;
            ComPtr<IPropertyDescriptionList> descs;
            UINT count = 0;
            if (SUCCEEDED(item->GetString(PKEY_PropList_PreviewDetails, &list)) &&
                SUCCEEDED(PSGetPropertyDescriptionListFromString(list, IID_PPV_ARGS(&descs))) && SUCCEEDED(descs->GetCount(&count)))
                for (UINT i = 0; i < count && r.props.size() < 40; ++i)
                {
                    ComPtr<IPropertyDescription> desc;
                    PROPERTYKEY key{};
                    if (SUCCEEDED(descs->GetAt(i, IID_PPV_ARGS(&desc))) && SUCCEEDED(desc->GetPropertyKey(&key)))
                        AddProperty(store.Get(), key, seen, r.props);
                }
            CoTaskMemFree(list);
        }
        if (r.thumb)
        {
            r.thumbAlpha = HasAlpha(r.thumb);
            r.kind = InfoPane::Kind::Image;
        }
        return;
    }

    // Preview page: zip (Rust) > images and folders (thumbnail) > system preview handler > text/code (Rust) > nothing
    // Files' IsBrowsableZipFile
    if (!path.empty() && IsOneOf(ext, { L".zip", L".7z", L".rar", L".tar", L".gz", L".lzh", L".mrpack", L".jar" }) && ListArchive(path, r))
    {
        r.kind = InfoPane::Kind::Archive;
        return;
    }
    const bool image = IsOneOf(ext, { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".ico", L".tif", L".tiff", L".heic", L".jfif", L".avif" });
    if (folder || image)
    {
        r.thumb = Thumbnail(item.Get(), thumbSize, image ? SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK : SIIGBF_RESIZETOFIT);
        if (r.thumb)
        {
            r.thumbAlpha = HasAlpha(r.thumb);
            r.kind = InfoPane::Kind::Image;
            return;
        }
    }
    if (!folder && !path.empty())
    {
        // Files' order after archives and folders: media, Markdown, images (above), .txt, .rtf, code, the shell's
        // handler (not fonts or programs), then any small file that reads as text
        if (IsOneOf(ext, { L".mp3", L".m4a", L".ogg", L".oga", L".wav", L".wma", L".aac", L".adt", L".adts", L".cda", L".flac", L".avi",
                L".mp4", L".webm", L".mov", L".qt", L".m4v", L".mp4v", L".3g2", L".3gp2", L".3gp", L".3gpp", L".mkv" }))
        {
            r.media = path;
            r.kind = InfoPane::Kind::Media;
            return;
        }
        const bool markdown = IsOneOf(ext, { L".md", L".markdown" });
        const uint8_t language = markdown ? 0 : ff_code_language(U16(ext), ext.size());
        if (markdown || language || ext == L".txt")
        {
            std::wstring text;
            if (ReadText(path, 10 * 1024 * 1024, text))
            {
                // Files' TextCharacterLimit
                if (text.size() > kTextLimit)
                    text.resize(kTextLimit);
                if (ext == L".txt")
                {
                    r.text = Crlf(text);
                    r.kind = InfoPane::Kind::Text;
                    return;
                }
                const FfBuffer rtf = ff_rich_preview(U16(text), text.size(), markdown ? 1 : 2, language, style.dark, style.ink, style.muted,
                    style.accent, U16(style.font), style.font.size());
                r.rtf.assign(reinterpret_cast<const char*>(rtf.data), rtf.len);
                ff_buffer_free(rtf);
                r.rtfWrap = markdown;
                r.kind = InfoPane::Kind::Rich;
                return;
            }
        }
        if (ext == L".rtf")
        {
            if (ReadBytes(path, 16 * 1024 * 1024, r.rtf))
            {
                r.rtfFile = true;
                r.kind = InfoPane::Kind::Rich;
                return;
            }
        }
        if (!IsOneOf(ext, { L".fon", L".otf", L".ttc", L".ttf", L".exe", L".bat", L".cmd", L".ahk" }) && FindHandler(ext, r.handler))
        {
            // Windows' TXT previewer ignores IPreviewHandlerVisuals (black on white in the dark theme): its files get
            // the theme's own text preview instead, like a .txt (Files' TextPreview)
            static constexpr CLSID kTxtPreviewer = { 0x1531d583, 0x8375, 0x4d3f, { 0xb5, 0xfb, 0xd2, 0x3b, 0xbd, 0x16, 0x9f, 0x22 } };
            if (!IsEqualCLSID(r.handler, kTxtPreviewer))
            {
                r.kind = InfoPane::Kind::Handler;
                return;
            }
            r.handler = CLSID{};
            std::wstring text;
            if (ReadText(path, 10 * 1024 * 1024, text))
            {
                if (text.size() > kTextLimit)
                    text.resize(kTextLimit);
                r.text = Crlf(text);
                r.kind = InfoPane::Kind::Text;
                return;
            }
        }
        // Files' TryLoadAsTextAsync: not .iso, not empty, at most 500 000 bytes, no run of four NULs
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const ULONGLONG size = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
            ? (static_cast<ULONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow : 0;
        std::wstring text;
        if (ext != L".iso" && size > 0 && size <= 500000 && ReadText(path, 500000, text))
        {
            if (text.size() > kTextLimit)
                text.resize(kTextLimit);
            r.text = Crlf(text);
            r.kind = InfoPane::Kind::Text;
            return;
        }
    }
    r.thumb = Thumbnail(item.Get(), thumbSize, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY);
    r.thumbAlpha = r.thumb && HasAlpha(r.thumb);
    r.note = folder ? L"" : Tr(L"此文件没有可用的预览");
}

void AddArchiveEntry(InfoPane::Result& r, std::wstring name, bool folder, ULONGLONG size)
{
    InfoPane::ArchiveEntry e;
    e.name = std::move(name);
    e.folder = folder;
    e.size = size;
    SHFILEINFOW sfi{};
    SHGetFileInfoW(e.name.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    e.icon = sfi.iIcon;
    r.archive.push_back(std::move(e));
}

// Files' ArchivePreviewViewModel: "{n} 个项目 ({files} 个文件, {folders} 个文件夹)" and the uncompressed size
std::wstring ArchiveSummary(ULONGLONG items, ULONGLONG files, ULONGLONG folders, ULONGLONG bytes, size_t listed)
{
    std::wstring s = std::to_wstring(items) + Tr(L" 个项目 (") + std::to_wstring(files) + Tr(L" 个文件, ") + std::to_wstring(folders) +
                     Tr(L" 个文件夹)");
    if (items > listed)
        s += Tr(L"（列出前 ") + std::to_wstring(listed) + Tr(L" 个）");
    s += Tr(L"\n解压缩大小 ") + FormatBytes(bytes);
    return s;
}

bool ListArchive(const std::wstring& path, InfoPane::Result& r)
{
    uint64_t total = 0;
    const FfBuffer buf = ff_zip_list(U16(path), path.size(), 5000, &total);
    if (!buf.data && total == 0)
    {
        // Not a zip the Rust reader knows: 7z.dll (7z, rar, tar, gz, lzh...); encrypted headers show no list, as in Files
        const SevenZip::Listing l = SevenZip::List(path, L"", 5000);
        if (l.status != SevenZip::Status::Ok)
            return false;
        for (const SevenZip::Entry& e : l.entries)
            AddArchiveEntry(r, e.path, e.folder, e.size);
        r.archiveSummary = ArchiveSummary(l.items, l.files, l.folders, l.bytes, r.archive.size());
        return true;
    }
    ULONGLONG bytes = 0, files = 0;
    for (size_t i = 0; i + 11 <= buf.len;)
    {
        const bool utf8 = buf.data[i] != 0;
        ULONGLONG size = 0;
        memcpy(&size, buf.data + i + 1, 8);
        uint16_t len = 0;
        memcpy(&len, buf.data + i + 9, 2);
        i += 11;
        if (i + len > buf.len)
            break;
        std::wstring name = Widen(reinterpret_cast<const char*>(buf.data + i), len, utf8 ? CP_UTF8 : CP_OEMCP);
        i += len;
        std::replace(name.begin(), name.end(), L'/', L'\\');
        const bool folder = !name.empty() && name.back() == L'\\';
        if (folder)
            name.pop_back();
        else
            ++files;
        bytes += size;
        AddArchiveEntry(r, std::move(name), folder, size);
    }
    ff_buffer_free(buf);
    // Counted over the listed entries; past the list limit the rest count as files
    files += total > r.archive.size() ? total - r.archive.size() : 0;
    r.archiveSummary = ArchiveSummary(total, files, total - std::min<ULONGLONG>(files, total), bytes, r.archive.size());
    return true;
}
}

void InfoPane::Start()
{
    const unsigned generation = ++m_generation;
    KillTimer(m_hwnd, kHandlerTimer);
    if (!m_pidl)
        return;
    // Request the box size (RESIZETOFIT keeps the aspect ratio); DrawFit only ever scales down
    const RECT body = BodyRect();
    const RECT box = ThumbRect();
    const int thumbSize = m_page == Page::Details
        ? std::max<int>(48, std::max(box.right - box.left, box.bottom - box.top) - m_theme->S(16))
        : std::max<int>(64, std::max(body.right - body.left, body.bottom - body.top - m_theme->S(52)));
    // Colors and the monospace face for generated RTF (code / Markdown follow the theme)
    RichStyle style{ m_theme->dark, m_theme->ink, m_theme->muted, m_theme->accent, {} };
    LOGFONTW mono{};
    if (m_mono && GetObjectW(m_mono, sizeof(mono), &mono))
        style.font = mono.lfFaceName;
    std::thread([target = m_hwnd, generation, page = m_page, thumbSize, style, pidl = ClonePidl(m_pidl.get())]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        auto result = new Result;
        result->generation = generation;
        Collect(pidl.get(), page, thumbSize, style, *result);
        if (!PostMessageW(target, kMsgResult, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void InfoPane::OnResult(Result* raw)
{
    std::unique_ptr<Result> r(raw);
    if (r->generation != m_generation)
        return;
    ClearThumb();
    m_kind = r->kind;
    m_name = std::move(r->name);
    m_type = std::move(r->type);
    m_note = std::move(r->note);
    m_props = std::move(r->props);
    m_thumb = r->thumb;
    r->thumb = nullptr;
    m_thumbAlpha = r->thumbAlpha;
    m_icon = r->icon;
    m_archive = std::move(r->archive);
    m_archiveSummary = std::move(r->archiveSummary);
    m_handlerClsid = r->handler;
    if (m_kind == Kind::Text && m_text)
        SetWindowTextW(m_text, r->text.c_str());
    if (m_kind == Kind::Rich)
    {
        m_rtf = std::move(r->rtf); // ASCII RTF (non-ASCII is \u-escaped), or a .rtf file's bytes
        m_rtfFile = r->rtfFile;
        m_rtfWrap = r->rtfWrap;
        SetRich();
    }
    if (m_kind != Kind::Media)
        m_mediaPath.clear();
    else if (r->media != m_mediaPath)
    {
        m_mediaPath = std::move(r->media);
        if (m_media)
            m_media->Load(m_mediaPath);
    }
    ApplyPreview();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}