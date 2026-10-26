#include "FileList.h"
#include "Tab.h"
#include "Lang.h"
#include "Sandbox.h"
#include <propkey.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <condition_variable>
#include <mutex>
#include <thread>

// Files' thumbnails (ShellViewModel.LoadThumbnailAsync, FileThumbnailHelper, Win32Helper.GetIcon): the size is the
// layout's icon size times the DPI snapped up to a standard one (16 24 32 48 64 96 128 256), IShellItemImageFactory with
// SIIGBF_BIGGERSIZEOK, icons only (SIIGBF_ICONONLY) without 显示缩略图 or below 48. The cards' ContextualProperty comes
// from the same pass (System.Image.Dimensions, System.Media.Duration as hh:mm:ss, System.FileVersion). One worker per
// list; the newest request first (the rows just painted), results posted to the container.

struct FileList::ThumbQueue
{
    struct Job
    {
        int item = 0;
        std::wstring name, path;
        int pixels = 0;           // asked of the shell
        int fitW = 0, fitH = 0;   // the box it's drawn in (made no larger than that)
        bool iconOnly = false, picture = true, context = false;
        bool noCache = false;     // test mode: not through the system's thumbnail cache
        unsigned generation = 0;
    };
    std::mutex lock;
    std::condition_variable wake;
    std::vector<Job> jobs;        // a stack: the back first
    bool quit = false;
    HWND to = nullptr;
};

struct FileList::ThumbResult
{
    int item = 0;
    std::wstring name;
    bool picture = false, context = false;
    std::shared_ptr<void> bitmap;
    int width = 0, height = 0;
    int contextKind = 0;          // 0 none, 1 分辨率, 2 持续时间, 3 版本
    std::wstring contextValue;
};

namespace
{
std::shared_ptr<void> OwnBitmap(HBITMAP bmp)
{
    return std::shared_ptr<void>(bmp, [](void* p) { DeleteObject(static_cast<HBITMAP>(p)); });
}

// The shell's picture (thumbnail or icon). Test mode: the file's own thumbnail handler, which skips the system's
// thumbnail cache (nothing of the machine's written), else the icon
HBITMAP ShellPicture(IShellItem* item, int pixels, bool iconOnly, bool noCache)
{
    HBITMAP source = nullptr;
    if (noCache && !iconOnly)
    {
        ComPtr<IThumbnailProvider> provider;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        if (SUCCEEDED(item->BindToHandler(nullptr, BHID_ThumbnailHandler, IID_PPV_ARGS(&provider))) &&
            SUCCEEDED(provider->GetThumbnail(static_cast<UINT>(pixels), &source, &alpha)) && source)
            return source;
        iconOnly = true;
    }
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory))))
        return nullptr;
    const SIIGBF flags = static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | (iconOnly ? SIIGBF_ICONONLY : 0));
    if (FAILED(factory->GetImage({ pixels, pixels }, flags, &source)))
        return nullptr;
    return source;
}

// The shell's picture as a premultiplied top-down 32bpp DIB no larger than fitW x fitH (scaled down with WIC's Fant)
HBITMAP MakePicture(IShellItem* item, IWICImagingFactory* wic, int pixels, bool iconOnly, bool noCache, int fitW, int fitH, int& outW, int& outH)
{
    HBITMAP source = ShellPicture(item, pixels, iconOnly, noCache);
    if (!source)
        return nullptr;
    BITMAP info{};
    GetObjectW(source, sizeof(info), &info);
    const int w = info.bmWidth, h = std::abs(info.bmHeight);
    if (w <= 0 || h <= 0)
    {
        DeleteObject(source);
        return nullptr;
    }
    // Its pixels, top-down 32bpp
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    HDC screen = GetDC(nullptr);
    const int got = GetDIBits(screen, source, 0, static_cast<UINT>(h), px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(source);
    if (got != h)
        return nullptr;
    // No alpha at all (photos' thumbnails): opaque; color above alpha somewhere: straight alpha, premultiply it
    bool anyAlpha = false, straight = false;
    for (uint32_t p : px)
    {
        const uint32_t a = p >> 24;
        anyAlpha = anyAlpha || a != 0;
        straight = straight || ((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a;
    }
    for (uint32_t& p : px)
    {
        if (!anyAlpha)
            p |= 0xFF000000u;
        else if (straight)
        {
            const uint32_t a = p >> 24;
            p = a << 24 | (((p >> 16) & 0xFF) * a / 255) << 16 | (((p >> 8) & 0xFF) * a / 255) << 8 | ((p & 0xFF) * a / 255);
        }
    }
    // Down to the box (Uniform); larger stays for the paint to scale up (it never is by much)
    const double scale = std::min({ 1.0, static_cast<double>(fitW) / w, static_cast<double>(fitH) / h });
    outW = std::max(1, static_cast<int>(w * scale + 0.5));
    outH = std::max(1, static_cast<int>(h * scale + 0.5));
    std::vector<uint32_t> scaled;
    if (outW != w || outH != h)
    {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICBitmapScaler> scaler;
        scaled.resize(static_cast<size_t>(outW) * outH);
        if (!wic || FAILED(wic->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
                        static_cast<UINT>(w) * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data()), &bitmap)) ||
            FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bitmap.Get(), static_cast<UINT>(outW), static_cast<UINT>(outH), WICBitmapInterpolationModeFant)) ||
            FAILED(scaler->CopyPixels(nullptr, static_cast<UINT>(outW) * 4, static_cast<UINT>(scaled.size() * 4), reinterpret_cast<BYTE*>(scaled.data()))))
            return nullptr;
    }
    else
        scaled = std::move(px);
    BITMAPINFO out{};
    out.bmiHeader = { sizeof(BITMAPINFOHEADER), outW, -outH, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &out, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits)
    {
        if (dib)
            DeleteObject(dib);
        return nullptr;
    }
    memcpy(bits, scaled.data(), scaled.size() * 4);
    return dib;
}

// Files' ContextualProperty sources, in its order
void ReadContext(IShellItem* item, int& kind, std::wstring& value)
{
    ComPtr<IShellItem2> item2;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2))))
        return;
    PWSTR text = nullptr;
    if (SUCCEEDED(item2->GetString(PKEY_Image_Dimensions, &text)) && text && *text)
    {
        kind = 1;
        value = text;
        CoTaskMemFree(text);
        return;
    }
    CoTaskMemFree(text);
    text = nullptr;
    ULONGLONG duration = 0; // 100 ns units; TimeSpan.ToString(@"hh\:mm\:ss")
    if (SUCCEEDED(item2->GetUInt64(PKEY_Media_Duration, &duration)) && duration > 0)
    {
        const ULONGLONG seconds = duration / 10000000ull;
        wchar_t buffer[32];
        swprintf_s(buffer, L"%02llu:%02llu:%02llu", (seconds / 3600) % 24, (seconds / 60) % 60, seconds % 60);
        kind = 2;
        value = buffer;
        return;
    }
    if (SUCCEEDED(item2->GetString(PKEY_FileVersion, &text)) && text && *text)
    {
        kind = 3;
        value = text;
    }
    CoTaskMemFree(text);
}
}

void FileList::ThumbWorker(std::shared_ptr<ThumbQueue> queue)
{
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;)
    {
        ThumbQueue::Job job;
        HWND to = nullptr;
        {
            std::unique_lock<std::mutex> hold(queue->lock);
            queue->wake.wait(hold, [&] { return queue->quit || !queue->jobs.empty(); });
            if (queue->quit)
                break;
            job = std::move(queue->jobs.back());
            queue->jobs.pop_back();
            to = queue->to;
        }
        auto result = std::make_unique<ThumbResult>();
        result->item = job.item;
        result->name = job.name;
        result->picture = job.picture;
        result->context = job.context;
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(job.path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
            if (job.picture)
                if (HBITMAP bmp = MakePicture(item.Get(), wic.Get(), job.pixels, job.iconOnly, job.noCache, job.fitW, job.fitH, result->width, result->height))
                    result->bitmap = OwnBitmap(bmp);
            if (job.context)
                ReadContext(item.Get(), result->contextKind, result->contextValue);
        }
        {
            std::lock_guard<std::mutex> hold(queue->lock);
            if (queue->quit)
                break;
        }
        if (PostMessageW(to, kMsgThumb, job.generation, reinterpret_cast<LPARAM>(result.get())))
            result.release();
    }
    wic.Reset();
    if (SUCCEEDED(co))
        CoUninitialize();
}
void FileList::RequestThumbs()
{
    if (m_wantThumbs.empty() || !m_theme || m_path.empty())
    {
        m_wantThumbs.clear();
        return;
    }
    if (!m_thumbs)
    {
        m_thumbs = std::make_shared<ThumbQueue>();
        m_thumbs->to = m_hwnd;
        std::thread(&FileList::ThumbWorker, m_thumbs).detach();
    }
    const Theme& t = *m_theme;
    // Files: the icon size times the DPI, snapped up to a standard size; icons only without 显示缩略图 or below 48
    constexpr int kStandard[] = { 16, 24, 32, 48, 64, 96, 128, 256 };
    const int icon = IconSize(), scaled = t.S(icon);
    int pixels = kStandard[std::size(kStandard) - 1];
    for (int s : kStandard)
        if (s >= scaled)
        {
            pixels = s;
            break;
        }
    SHELLSTATEW state{};
    SHGetSetSettings(&state, SSF_ICONSONLY, FALSE);
    if ((state.fIconsOnly != FALSE) != m_iconsOnly)
    {
        // 显示缩略图 changed: what was made goes, the next paint asks again
        m_iconsOnly = state.fIconsOnly != FALSE;
        ResetThumbs();
        InvalidateRect(m_list, nullptr, FALSE);
        return;
    }
    const bool iconOnly = m_iconsOnly || icon < 48;
    // The box the picture is fitted into
    int fitW = scaled, fitH = scaled;
    if (m_mode == Mode::Cards)
    {
        constexpr int kBoxWidth[4] = { 104, 240, 280, 320 };
        fitW = t.S(kBoxWidth[Size() - 1]);
    }
    else if (m_mode == Mode::Grid)
        fitW = fitH = t.S(60 + 20 * Size()) - t.S(24);
    const std::wstring base = m_path.back() == L'\\' ? m_path : m_path + L'\\';
    std::vector<ThumbQueue::Job> jobs;
    // Painted top to bottom: pushed last to first, so the worker (newest first) takes the top one first
    for (auto it = m_wantThumbs.rbegin(); it != m_wantThumbs.rend(); ++it)
    {
        const int row = *it;
        if (row < 0 || row >= Count())
            continue;
        Item& item = m_items[m_order[row]];
        const bool picture = !item.thumbAsked;
        const bool context = m_mode == Mode::Cards && !item.contextAsked && !(item.attrs & FILE_ATTRIBUTE_DIRECTORY);
        item.thumbAsked = true;
        if (m_mode == Mode::Cards)
            item.contextAsked = true;
        if (!picture && !context)
            continue;
        ThumbQueue::Job job;
        job.item = m_order[row];
        job.name = item.name;
        job.path = base + item.name;
        job.pixels = pixels;
        job.fitW = fitW;
        job.fitH = fitH;
        job.iconOnly = iconOnly;
        job.picture = picture;
        job.context = context;
        job.noCache = Sandbox::Active();
        job.generation = m_thumbGen;
        jobs.push_back(std::move(job));
    }
    m_wantThumbs.clear();
    if (jobs.empty())
        return;
    {
        std::lock_guard<std::mutex> hold(m_thumbs->lock);
        for (ThumbQueue::Job& job : jobs)
            m_thumbs->jobs.push_back(std::move(job));
        // Scrolled far past them: the oldest waiting ones go (asked again when painted again)
        constexpr size_t kMaxWaiting = 400;
        if (m_thumbs->jobs.size() > kMaxWaiting)
        {
            const size_t drop = m_thumbs->jobs.size() - kMaxWaiting;
            for (size_t i = 0; i < drop; ++i)
            {
                const ThumbQueue::Job& old = m_thumbs->jobs[i];
                if (old.generation == m_thumbGen && old.item >= 0 && old.item < static_cast<int>(m_items.size()) &&
                    m_items[static_cast<size_t>(old.item)].name == old.name)
                {
                    Item& item = m_items[static_cast<size_t>(old.item)];
                    item.thumbAsked = item.thumbAsked && !old.picture;
                    item.contextAsked = item.contextAsked && !old.context;
                }
            }
            m_thumbs->jobs.erase(m_thumbs->jobs.begin(), m_thumbs->jobs.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }
    m_thumbs->wake.notify_one();
}

void FileList::StopThumbs()
{
    if (!m_thumbs)
        return;
    {
        std::lock_guard<std::mutex> hold(m_thumbs->lock);
        m_thumbs->quit = true;
        m_thumbs->jobs.clear();
    }
    m_thumbs->wake.notify_all();
    m_thumbs.reset();
}

void FileList::ResetThumbs()
{
    ++m_thumbGen;
    if (m_thumbs)
    {
        std::lock_guard<std::mutex> hold(m_thumbs->lock);
        m_thumbs->jobs.clear();
    }
    for (Item& item : m_items)
    {
        item.thumb.reset();
        item.thumbW = item.thumbH = 0;
        item.thumbAsked = false;
        if (item.context.empty())
            item.contextAsked = false; // (asked in another layout and nothing there: asked again only if cards)
    }
    m_wantThumbs.clear();
}

void FileList::DropThumb(LPARAM raw)
{
    delete reinterpret_cast<ThumbResult*>(raw);
}

void FileList::TakeThumb(LPARAM raw, unsigned generation)
{
    std::unique_ptr<ThumbResult> result(reinterpret_cast<ThumbResult*>(raw));
    if (result->item < 0 || result->item >= static_cast<int>(m_items.size()))
        return;
    Item& item = m_items[static_cast<size_t>(result->item)];
    if (item.name != result->name)
        return; // listed again since
    bool changed = false;
    if (result->context && result->contextKind > 0)
    {
        static const wchar_t* const kLabels[] = { L"", L"分辨率", L"持续时间", L"版本" };
        item.context = std::wstring(Tr(kLabels[result->contextKind])) + L": " + result->contextValue;
        changed = true;
    }
    if (result->picture && generation == m_thumbGen && result->bitmap)
    {
        item.thumb = std::move(result->bitmap);
        item.thumbW = result->width;
        item.thumbH = result->height;
        changed = true;
    }
    if (changed)
        InvalidateRect(m_list, nullptr, FALSE);
}
