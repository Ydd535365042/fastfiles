#include "MediaPreview.h"
#include "Lang.h"
#include <mfapi.h>
#include <mfplay.h>

#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.MediaPreview", kVideoClass[] = L"FastFiles.MediaVideo";
constexpr UINT_PTR kTickTimer = 1;
constexpr int kBar = 48; // the compact transport bar (96 DPI)

std::wstring Clock(ULONGLONG hns)
{
    const ULONGLONG s = hns / 10000000;
    wchar_t buf[32];
    if (s >= 3600)
        swprintf_s(buf, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else
        swprintf_s(buf, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

// The video surface: MFPlay paints it; nothing to erase underneath
LRESULT CALLBACK VideoProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        BeginPaint(w, &ps);
        if (auto* player = reinterpret_cast<IMFPMediaPlayer*>(GetWindowLongPtrW(w, GWLP_USERDATA)))
            player->UpdateVideo();
        else if (const auto* theme = static_cast<const Theme*>(GetPropW(w, L"FastFiles.Theme")))
            FillSolid(ps.hdc, ps.rcPaint, theme->videoBackground);
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONUP || msg == WM_KEYDOWN)
        return SendMessageW(GetParent(w), msg, wp, lp); // click on the picture: play / pause, as Files' player
    return DefWindowProcW(w, msg, wp, lp);
}
}

// MFPlay calls back on the thread that created the player (through its own window): straight into the control
struct MediaPreview::Callback final : IMFPMediaPlayerCallback
{
    MediaPreview* owner;
    explicit Callback(MediaPreview* o) : owner(o) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback))
        {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs;
        if (n == 0)
            delete this;
        return n;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override
    {
        if (owner && e)
            owner->OnEvent(e->eEventType, e->hrEvent);
    }
    ULONG refs = 1;
};

MediaPreview::~MediaPreview()
{
    Unload();
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}

bool MediaPreview::Create(HWND parent, const Theme* theme)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        WNDCLASSEXW vc{ sizeof(vc) };
        vc.lpfnWndProc = VideoProc;
        vc.hInstance = GetModuleHandleW(nullptr);
        vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        vc.lpszClassName = kVideoClass;
        const bool a = RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        const bool b = RegisterClassExW(&vc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        // MFPlay starts Media Foundation itself; this keeps it up for the life of the process
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        return a && b;
    }();
    m_theme = theme;
    if (!registered)
        return false;
    m_hwnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd)
        return false;
    m_video = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (m_video)
        SetPropW(m_video, L"FastFiles.Theme", const_cast<Theme*>(theme)); // VideoProc's letterbox color
    return true;
}

void MediaPreview::Load(const std::wstring& path)
{
    Unload();
    m_path = path;
    m_callback = new Callback(this);
    // Not started: Files' MediaPlayerElement waits for the play button
    if (FAILED(MFPCreateMediaPlayer(path.c_str(), FALSE, MFP_OPTION_NONE, m_callback, m_video, &m_player)))
    {
        m_player = nullptr;
        m_failed = true;
    }
    SetWindowLongPtrW(m_video, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m_player));
    UpdateTitle();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MediaPreview::Unload()
{
    if (m_hwnd)
        KillTimer(m_hwnd, kTickTimer);
    if (m_video)
    {
        SetWindowLongPtrW(m_video, GWLP_USERDATA, 0);
        ShowWindow(m_video, SW_HIDE);
    }
    if (m_player)
    {
        m_player->Shutdown();
        m_player->Release();
        m_player = nullptr;
    }
    if (m_callback)
    {
        m_callback->owner = nullptr;
        m_callback->Release();
        m_callback = nullptr;
    }
    m_path.clear();
    m_duration = 0;
    m_hasVideo = m_playing = m_ready = m_failed = m_seeking = false;
    if (m_hwnd)
    {
        UpdateTitle();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void MediaPreview::Move(const RECT& rc)
{
    if (m_hwnd)
        SetWindowPos(m_hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MediaPreview::Show(bool show)
{
    if (m_hwnd)
        ShowWindow(m_hwnd, show ? SW_SHOWNA : SW_HIDE);
}

void MediaPreview::OnEvent(int type, HRESULT hr)
{
    switch (type)
    {
    case MFP_EVENT_TYPE_MEDIAITEM_SET:
        if (FAILED(hr))
        {
            m_failed = true;
            break;
        }
        {
            IMFPMediaItem* item = nullptr;
            if (m_player && SUCCEEDED(m_player->GetMediaItem(&item)) && item)
            {
                BOOL has = FALSE, selected = FALSE;
                m_hasVideo = SUCCEEDED(item->HasVideo(&has, &selected)) && has && selected;
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(item->GetDuration(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_UI8)
                    m_duration = v.uhVal.QuadPart;
                PropVariantClear(&v);
                item->Release();
            }
            m_ready = true;
            if (m_player)
                m_player->SetMute(m_muted);
            ShowWindow(m_video, m_hasVideo ? SW_SHOWNA : SW_HIDE);
            Layout();
        }
        break;
    case MFP_EVENT_TYPE_PLAY:
        m_playing = SUCCEEDED(hr);
        if (m_playing)
            SetTimer(m_hwnd, kTickTimer, 250, nullptr);
        break;
    case MFP_EVENT_TYPE_PAUSE:
    case MFP_EVENT_TYPE_STOP:
        m_playing = false;
        KillTimer(m_hwnd, kTickTimer);
        break;
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:
    {
        // Back to the start, paused (MediaPlayerElement's end of playback)
        m_playing = false;
        KillTimer(m_hwnd, kTickTimer);
        PROPVARIANT zero;
        PropVariantInit(&zero);
        zero.vt = VT_I8;
        if (m_player)
        {
            m_player->Pause();
            m_player->SetPosition(MFP_POSITIONTYPE_100NS, &zero);
        }
        break;
    }
    case MFP_EVENT_TYPE_ERROR:
        m_failed = true;
        m_playing = false;
        KillTimer(m_hwnd, kTickTimer);
        break;
    default:
        break;
    }
    UpdateTitle();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

ULONGLONG MediaPreview::Position() const
{
    if (!m_player || !m_ready)
        return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    ULONGLONG pos = 0;
    if (SUCCEEDED(m_player->GetPosition(MFP_POSITIONTYPE_100NS, &v)) && (v.vt == VT_I8 || v.vt == VT_UI8))
        pos = v.uhVal.QuadPart;
    PropVariantClear(&v);
    return std::min(pos, m_duration ? m_duration : pos);
}

void MediaPreview::TogglePlay()
{
    if (!m_player || !m_ready || m_failed)
        return;
    if (m_playing)
        m_player->Pause();
    else
        m_player->Play();
}

void MediaPreview::UpdateTitle()
{
    if (!m_hwnd)
        return;
    std::wstring t;
    if (m_failed)
        t = Tr(L"无法播放此文件");
    else if (!m_path.empty())
        t = std::wstring(m_playing ? Tr(L"正在播放 ") : Tr(L"已暂停 ")) + Clock(Position()) + L" / " + Clock(m_duration);
    SetWindowTextW(m_hwnd, t.c_str());
}

// --- Layout: video (or the audio glyph) above, the bar at the bottom ---

RECT MediaPreview::BarRect() const
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    return { rc.left, std::max<LONG>(rc.top, rc.bottom - m_theme->S(kBar)), rc.right, rc.bottom };
}

RECT MediaPreview::PlayRect() const
{
    const RECT b = BarRect();
    const int s = m_theme->S(32), y = (b.top + b.bottom - s) / 2;
    return { b.left + m_theme->S(8), y, b.left + m_theme->S(8) + s, y + s };
}

RECT MediaPreview::MuteRect() const
{
    const RECT b = BarRect();
    const int s = m_theme->S(32), y = (b.top + b.bottom - s) / 2;
    return { b.right - m_theme->S(8) - s, y, b.right - m_theme->S(8), y + s };
}

RECT MediaPreview::TrackRect() const
{
    // Between the play button and the time text
    const RECT p = PlayRect(), m = MuteRect();
    const int time = m_theme->S(m_duration >= 36000000000ull ? 110 : 84);
    const int y = (p.top + p.bottom) / 2;
    return { p.right + m_theme->S(10), y - m_theme->S(8), std::max<LONG>(p.right + m_theme->S(20), m.left - time - m_theme->S(6)), y + m_theme->S(8) };
}

int MediaPreview::Hit(POINT pt) const
{
    RECT r = PlayRect();
    if (PtInRect(&r, pt))
        return 1;
    r = TrackRect();
    if (PtInRect(&r, pt))
        return 2;
    r = MuteRect();
    if (PtInRect(&r, pt))
        return 3;
    return 0;
}

void MediaPreview::Layout()
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const RECT bar = BarRect();
    SetWindowPos(m_video, nullptr, rc.left, rc.top, rc.right - rc.left, std::max<LONG>(0, bar.top - rc.top - m_theme->S(4)), SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_player && m_hasVideo)
        m_player->UpdateVideo();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MediaPreview::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.content);
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const RECT bar = BarRect();
    if (!m_hasVideo)
    {
        // Audio (or not loaded yet): the media glyph where the picture would be
        const RECT art{ client.left, client.top, client.right, bar.top };
        LOGFONTW lf{};
        GetObjectW(t.icons, sizeof(lf), &lf); // the icon font, larger
        lf.lfHeight = -t.S(48);
        HFONT glyph = CreateFontIndirectW(&lf);
        DrawGlyph(hdc, glyph, m_failed ? L"\xE783" : L"\xE8D6", art, t.muted);
        DeleteObject(glyph);
        if (m_failed)
            DrawLabel(hdc, t.caption, Tr(L"无法播放此文件"), { art.left, (art.top + art.bottom) / 2 + t.S(34), art.right, (art.top + art.bottom) / 2 + t.S(54) },
                t.muted, DT_CENTER);
    }
    FillRound(hdc, bar, t.S(6), t.scrubTrack);
    const bool enabled = m_ready && !m_failed;
    const COLORREF ink = enabled ? t.ink : Mix(t.ink, t.content, 55);
    const RECT play = PlayRect(), mute = MuteRect(), track = TrackRect();
    if (m_hot == 1 && enabled)
        FillRound(hdc, play, t.S(4), t.subtleHover);
    DrawGlyph(hdc, t.icons, m_playing ? L"\xE769" : L"\xE768", play, ink); // Pause / Play
    if (m_hot == 3 && enabled)
        FillRound(hdc, mute, t.S(4), t.subtleHover);
    DrawGlyph(hdc, t.icons, m_muted ? L"\xE74F" : L"\xE767", mute, ink); // Mute / Volume
    // Slider: rail, filled part, thumb
    const int cy = (track.top + track.bottom) / 2;
    const RECT rail{ track.left, cy - t.S(2), track.right, cy + t.S(2) };
    FillRound(hdc, rail, t.S(2), Mix(t.muted, t.content, 50));
    const ULONGLONG pos = Position();
    const int x = m_duration ? track.left + static_cast<int>((track.right - track.left) * (static_cast<double>(pos) / m_duration)) : track.left;
    if (enabled)
    {
        FillRound(hdc, { rail.left, rail.top, std::max<LONG>(rail.left + t.S(2), x), rail.bottom }, t.S(2), t.accent);
        const RECT thumb{ x - t.S(7), cy - t.S(7), x + t.S(7), cy + t.S(7) };
        FillRound(hdc, thumb, t.S(7), t.sliderThumb);
        FrameRound(hdc, thumb, t.S(7), t.controlStroke);
        FillRound(hdc, { x - t.S(3), cy - t.S(3), x + t.S(3), cy + t.S(3) }, t.S(3), t.accent);
    }
    const std::wstring time = Clock(pos) + L" / " + Clock(m_duration);
    DrawLabel(hdc, t.caption, time, { track.right + t.S(6), bar.top, mute.left - t.S(2), bar.bottom }, t.muted, DT_RIGHT);
}

LRESULT MediaPreview::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        Paint(paint.dc(), paint.rect());
        return 0;
    }
    case WM_TIMER:
        if (wp == kTickTimer)
        {
            UpdateTitle();
            const RECT bar = BarRect();
            InvalidateRect(m_hwnd, &bar, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd };
        TrackMouseEvent(&tme);
        const int hit = Hit(pt);
        if (hit != m_hot)
        {
            m_hot = hit;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        if (!m_seeking)
            return 0;
        [[fallthrough]];
    }
    case WM_LBUTTONDOWN:
    {
        if (msg == WM_LBUTTONDOWN)
        {
            SetFocus(m_hwnd);
            if (Hit(pt) != 2)
                return 0;
            m_seeking = true;
            SetCapture(m_hwnd);
        }
        const RECT track = TrackRect();
        if (m_player && m_ready && m_duration && track.right > track.left)
        {
            const double f = std::clamp((pt.x - track.left) / static_cast<double>(track.right - track.left), 0.0, 1.0);
            PROPVARIANT v;
            PropVariantInit(&v);
            v.vt = VT_I8;
            v.hVal.QuadPart = static_cast<LONGLONG>(f * m_duration);
            m_player->SetPosition(MFP_POSITIONTYPE_100NS, &v);
            UpdateTitle();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (m_seeking)
        {
            m_seeking = false;
            ReleaseCapture();
            return 0;
        }
        if (Hit(pt) == 1 || pt.y < BarRect().top)
            TogglePlay();
        else if (Hit(pt) == 3 && m_player)
        {
            m_muted = !m_muted;
            m_player->SetMute(m_muted);
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        m_hot = 0;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE) // Files: Alt+Space toggles playback; Space while the player has the keyboard does too
        {
            TogglePlay();
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_SPACE)
        {
            TogglePlay();
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_DESTROY:
        Unload();
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

LRESULT CALLBACK MediaPreview::WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = static_cast<MediaPreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (auto* self = reinterpret_cast<MediaPreview*>(GetWindowLongPtrW(w, GWLP_USERDATA)))
        return self->Handle(msg, wp, lp);
    return DefWindowProcW(w, msg, wp, lp);
}
