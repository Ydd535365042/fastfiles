#include "StatusCenter.h"
#include "Format.h"
#include "Lang.h"
#include "DWriteText.h"

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace
{
constexpr wchar_t kButtonClass[] = L"FastFiles.StatusCenterButton";
constexpr wchar_t kFlyoutClass[] = L"FastFiles.StatusCenter";
constexpr UINT_PTR kTimer = 1, kTipTimer = 2;
// StatusCenter.xaml: Width 400, MinHeight 120, MaxHeight 500; header 12,8 padding; list 12 sides; card padding 8
constexpr int kWidth = 400, kMinHeight = 120, kMaxHeight = 500, kHead = 48, kSide = 12, kCardPad = 8, kCardGap = 4;

std::wstring Leaf(std::wstring path)
{
    while (path.size() > 1 && path.back() == L'\\')
        path.pop_back();
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Files' PathNormalization.GetParentDir
std::wstring Parent(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Bytes(ULONGLONG n)
{
    return Format::Size(n);
}

using Kind = Operations::Kind;
using State = Operations::State;

// Files' StatusCenter_*_Header (zh-Hans; Lang::Format brings the user's language), by kind and state
const wchar_t* HeaderPattern(Kind k, State s)
{
    static const wchar_t* const table[][4] = {
        // Running, Done, Cancelled, Failed
        { L"正在将{0, plural, other {#个项目}}压缩到“{1}”", L"已将{0, plural, other {#个项目}}压缩至“{1}”", L"已取消将 {0} 个项目压缩到“{1}”",
            L"将{0, plural, other {#个项目}}压缩到“{1}”时出错" },
        { L"正在将“{0}”解压缩到“{1}”", L"已将“{0}”解压缩到“{1}”", L"已取消将 {0} 解压缩到“{1}”", L"将“{0}”解压缩到“{1}”时出错" },
        { L"正在将{0, plural, other {#个项目}}复制到“{1}”", L"已将{0, plural, other {#个项目}}复制到“{1}”", L"已取消将{0, plural, other {#个项目}}复制到“{1}”",
            L"将{0, plural, other {#个项目}}复制到“{1}”时出错" },
        { L"正在将{0, plural, other {#个项目}}移动到“{1}”", L"已将{0, plural, other {#个项目}}移动到“{1}”", L"已取消将{0, plural, other {#个项目}}移动到“{1}”",
            L"将{0, plural, other {#个项目}}移动到“{1}”时出错" },
        // Delete and Recycle share Files' Delete strings
        { L"正在从“{1}”中删除{0, plural, other {#个项目}}", L"已从“{1}”中删除{0, plural, other {#个项目}}", L"已取消从“{1}”中删除{0, plural, other {#个项目}}",
            L"从“{1}”中删除{0, plural, other {#个项目}}时出错" },
        { L"正在从“{1}”中删除{0, plural, other {#个项目}}", L"已从“{1}”中删除{0, plural, other {#个项目}}", L"已取消从“{1}”中删除{0, plural, other {#个项目}}",
            L"从“{1}”中删除{0, plural, other {#个项目}}时出错" },
        { L"正在清空回收站", L"回收站已清空", L"已取消清空回收站", L"清空回收站出错" },
    };
    return table[static_cast<int>(k)][static_cast<int>(s)];
}

bool Archive(Kind k) { return k == Kind::Compress || k == Kind::Extract; }

// The Rust / 7z jobs report totals as they go: "counted" once a total is known, "started" once anything is done
bool Counted(const Operations::View& v) { return Archive(v.kind) ? (v.totalBytes || v.totalFiles) : v.counted; }
bool Started(const Operations::View& v) { return Archive(v.kind) ? (v.doneBytes || v.doneFiles) : v.started; }
// Files' canProvideProgress: everything but emptying the bin
bool Determinate(const Operations::View& v) { return v.kind != Kind::EmptyBin && !v.cancelRequested; }

// Files' TotalItemsCount: the walk's count once it has one, the selection before
ULONGLONG Count(const Operations::View& v) { return v.totalFiles ? v.totalFiles : v.items; }
}

std::wstring StatusCenter::Header(const Operations::View& v)
{
    const bool running = v.state == State::Running;
    std::wstring text;
    // Files' UpdateCardStrings: "已发现 N" while copy / move / delete still enumerate
    if (running && !v.counted && v.totalFiles > 0 && (v.kind == Kind::Copy || v.kind == Kind::Move || v.kind == Kind::Delete))
        text = Lang::Format(L"已发现 {0, plural, one {# item} other {# items}}", { std::to_wstring(v.totalFiles) });
    else
    {
        const wchar_t* pattern = HeaderPattern(v.kind, v.state);
        const std::wstring count = std::to_wstring(Count(v));
        const std::wstring first = v.sources.empty() ? std::wstring() : v.sources.front();
        switch (v.kind)
        {
        case Kind::Copy:
        case Kind::Move:
            // {1}: the destination's folder (Files: the parent of the first item's destination path)
            text = Lang::Format(pattern, { count, Leaf(v.target) });
            break;
        case Kind::Delete:
        case Kind::Recycle:
            text = Lang::Format(pattern, { count, v.from.empty() ? Leaf(Parent(first)) : v.from });
            break;
        case Kind::Compress:
            text = Lang::Format(pattern, { std::to_wstring(v.items), Leaf(Parent(v.archive)) });
            break;
        case Kind::Extract:
            text = Lang::Format(pattern, { Leaf(v.archive), Leaf(Parent(v.target)) });
            break;
        default:
            text = Lang::Format(pattern, {});
            break;
        }
    }
    if (running && v.cancelRequested)
        return std::wstring(Tr(L"正在取消")) + L" - " + text; // Files' ExecuteCancelCommand
    if (running && Determinate(v) && Counted(v) && Started(v) && v.percent >= 0)
        text += L" (" + std::to_wstring(v.percent) + L"%)";
    return text;
}

std::wstring StatusCenter::Message(const Operations::View& v)
{
    if (!Counted(v))
        return Tr(L"正在发现项目...");
    if (!Started(v))
        return Tr(L"正在处理项目...");
    if (v.kind == Kind::Delete || v.kind == Kind::Recycle || v.kind == Kind::Compress)
        return Lang::Format(L"已处理{0}/{1, plural, other {#个项目}}", { std::to_wstring(v.doneFiles), std::to_wstring(v.totalFiles) });
    return Lang::Format(L"已处理 {0}，共 {1}", { Bytes(v.doneBytes), Bytes(v.totalBytes) });
}

namespace
{
// StatusCenterStateToStateIconConverter: the operation while it runs (and when cancelled), a check or an error after
const wchar_t* Icon(const Operations::View& v)
{
    if (v.state == State::Done)
        return L"\xE73E";
    if (v.state == State::Failed)
        return L"\xE783";
    switch (v.kind)
    {
    case Kind::Copy: return L"\xE8C8";
    case Kind::Move: return L"\xE8DE";
    case Kind::Compress: return L"\xE7B8";
    case Kind::Extract: return L"\xE8B7";
    default: return L"\xE74D";
    }
}

// StatusCenterStateToBrushConverter (SystemFillColor*, App.Theme.FillColorAttentionBrush): circle and icon colors
void Tones(const Theme& t, const Operations::View& v, COLORREF card, COLORREF& circle, COLORREF& icon)
{
    switch (v.state)
    {
    case State::Running:
        icon = t.accent;
        circle = Mix(card, t.accent, 10); // IconBackgroundCircleBorderOpacity 0.1
        break;
    case State::Done:
        icon = t.success;
        circle = t.successBackground;
        break;
    case State::Failed:
        icon = t.critical;
        circle = t.criticalBackground;
        break;
    default:
        icon = t.muted;
        circle = Mix(card, t.muted, 22);
        break;
    }
}
}
StatusCenter::~StatusCenter()
{
    m_smooth.Reset(m_scroll); // drops it from the frame clock before the flyout goes
    if (m_flyout)
        DestroyWindow(m_flyout);
    if (m_tip && IsWindow(m_tip))
        DestroyWindow(m_tip);
    if (m_title)
        DeleteObject(m_title);
}

bool StatusCenter::Create(HWND parent, int id, const Theme* theme, Operations* ops)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ButtonProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kButtonClass;
        WNDCLASSEXW fc = wc;
        fc.style = CS_DROPSHADOW;
        fc.lpfnWndProc = FlyoutProc;
        fc.lpszClassName = kFlyoutClass;
        return (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) &&
               (RegisterClassExW(&fc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    }();
    m_parent = parent;
    m_id = id;
    m_theme = theme;
    m_ops = ops;
    StartGdiplus();
    // Hidden until Wanted(); the window text is its accessible name (Files: AutomationProperties.Name)
    CreateWindowExW(0, kButtonClass, Tr(L"状态中心"), WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, MenuId(id), GetModuleHandleW(nullptr), this);
    // Files' Flyout (BottomEdgeAlignedRight): a popup owned by the window, closed when it loses activation
    CreateWindowExW(WS_EX_TOOLWINDOW, kFlyoutClass, L"", WS_POPUP, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (!registered || !m_button || !m_flyout)
        return false;
    // The button's tooltip (Files: ToolTipService.ToolTip = 状态中心)
    if (HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, m_button, nullptr,
            GetModuleHandleW(nullptr), nullptr))
    {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = m_button;
        ti.uId = reinterpret_cast<UINT_PTR>(m_button);
        ti.lpszText = const_cast<wchar_t*>(Tr(L"状态中心"));
        SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    // A card's full header (and the error of a failed one) on hover
    m_cardTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, m_flyout, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (m_cardTip)
    {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = m_flyout;
        ti.uId = reinterpret_cast<UINT_PTR>(m_flyout);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(m_cardTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(m_cardTip, TTM_SETMAXTIPWIDTH, 0, m_theme->S(360));
    }
    Relayout();
    return true;
}

void StatusCenter::Relayout()
{
    if (m_title)
        DeleteObject(m_title);
    LOGFONTW lf{};
    GetObjectW(m_theme->ui, sizeof(lf), &lf);
    lf.lfHeight = -m_theme->S(14);
    lf.lfWeight = FW_SEMIBOLD;
    m_title = CreateFontIndirectW(&lf);
    if (m_button)
        InvalidateRect(m_button, nullptr, FALSE);
    if (Open())
        Resize();
}

LRESULT CALLBACK StatusCenter::ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<StatusCenter*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<StatusCenter*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_button = hwnd;
    }
    return self ? self->ButtonMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT StatusCenter::ButtonMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        BufferedPaint paint(hwnd);
        PaintButton(paint.dc(), paint.rect());
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (!m_hot)
        {
            m_hot = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = m_hot = m_down = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        m_down = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        const bool was = m_down;
        m_down = false;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (was && PtInRect(&rc, pt))
            Toggle();
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimer)
            Refresh();
        else if (wp == kTipTimer)
            HideTip();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimer);
        KillTimer(hwnd, kTipTimer);
        HideTip();
        m_button = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void StatusCenter::PaintButton(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.layer);
    // AddressToolbarButtonStyle: subtle fill on hover / press, kept while the flyout is open
    if (m_down || m_hot || Open())
        FillRound(hdc, rc, t.S(4), m_down ? t.subtlePressed : t.subtleHover);
    const float cx = (rc.left + rc.right) / 2.0f, cy = (rc.top + rc.bottom) / 2.0f;
    const int state = InfoState();
    const bool ring = m_ring && !m_views.empty();
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        auto color = [](COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); };
        if (!ring)
        {
            // App.ThemedIcons.StatusCenter (16px outline): a rounded panel, a divider, one row on top and two below
            const float k = t.S(16) / 16.0f, x = cx - 8 * k, y = cy - 8 * k;
            Gdiplus::Pen pen(color(t.ink), k);
            Gdiplus::SolidBrush brush(color(t.ink));
            Gdiplus::GraphicsPath panel;
            const float r = 1.5f * k, l = x + 2.5f * k, tp = y + 0.5f * k, w = 11 * k, h = 15 * k;
            panel.AddArc(l, tp, 2 * r, 2 * r, 180, 90);
            panel.AddArc(l + w - 2 * r, tp, 2 * r, 2 * r, 270, 90);
            panel.AddArc(l + w - 2 * r, tp + h - 2 * r, 2 * r, 2 * r, 0, 90);
            panel.AddArc(l, tp + h - 2 * r, 2 * r, 2 * r, 90, 90);
            panel.CloseFigure();
            g.DrawPath(&pen, &panel);
            g.DrawLine(&pen, l, y + 6.5f * k, l + w, y + 6.5f * k);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            for (float row : { 3.5f, 9.5f, 12.5f })
            {
                g.FillEllipse(&brush, x + 4 * k, y + (row - 1) * k, 2 * k, 2 * k);
                g.DrawLine(&pen, x + 7.5f * k, y + row * k, x + 11.5f * k, y + row * k);
            }
        }
        else
        {
            // Files' ProgressRing (32px, 4px stroke) under the InfoBadge; colors from the InfoBadgeState visual states
            const float d = static_cast<float>(t.S(26)), sw = static_cast<float>(std::max(2, t.S(3)));
            const Gdiplus::RectF box(cx - d / 2 + sw / 2, cy - d / 2 + sw / 2, d - sw, d - sw);
            const COLORREF track = state == 0 ? t.success : state == 3 ? t.critical : Mix(t.layer, t.ink, 12);
            Gdiplus::Pen back(color(track), sw);
            g.DrawEllipse(&back, box);
            if (state == 1 || state == 2)
            {
                Gdiplus::Pen arc(color(t.accent), sw);
                arc.SetStartCap(Gdiplus::LineCapRound);
                arc.SetEndCap(Gdiplus::LineCapRound);
                const float sweep = 360.0f * std::clamp(Average(), 0, 100) / 100.0f;
                if (sweep > 0.5f)
                    g.DrawArc(&arc, box, -90.0f, sweep);
            }
            // InfoBadge: the running count (accent), or a check (success) / an exclamation mark (critical)
            const float b = static_cast<float>(t.S(16));
            const COLORREF fill = state == 0 ? t.success : state == 3 ? t.critical : t.accent;
            Gdiplus::SolidBrush badge(color(fill));
            g.FillEllipse(&badge, cx - b / 2, cy - b / 2, b, b);
        }
    }
    if (ring)
    {
        const COLORREF on = t.textOnAccent;
        const RECT badge{ static_cast<LONG>(cx) - t.S(8), static_cast<LONG>(cy) - t.S(8), static_cast<LONG>(cx) + t.S(8), static_cast<LONG>(cy) + t.S(8) };
        const int running = Running();
        if (state == 0)
            DrawGlyph(hdc, t.iconsSmall, L"\xE73E", badge, on);
        else if (state == 3 || running == 0)
            DrawLabel(hdc, t.caption, L"!", badge, on, DT_CENTER);
        else
            DrawLabel(hdc, t.caption, std::to_wstring(running), badge, on, DT_CENTER);
    }
}
int StatusCenter::Running() const
{
    return static_cast<int>(std::count_if(m_views.begin(), m_views.end(), [](const auto& v) { return v.state == State::Running; }));
}

int StatusCenter::InfoState() const
{
    const bool failure = std::any_of(m_views.begin(), m_views.end(), [](const auto& v) { return v.state == State::Failed || v.state == State::Cancelled; });
    const bool running = Running() > 0;
    return failure ? (running ? 2 : 3) : (running ? 1 : 0);
}

int StatusCenter::Average() const
{
    int sum = 0, n = 0;
    for (const auto& v : m_views)
        if (v.state == State::Running)
        {
            sum += std::max(0, v.percent);
            ++n;
        }
    return n ? sum / n : 0;
}

bool StatusCenter::Wanted() const
{
    // Files' ShowStatusCenterButton: Always, or DuringOngoingFileOperations with any card (finished ones too)
    return m_always || (m_ops && m_ops->Any());
}

void StatusCenter::Refresh()
{
    if (!m_ops || !m_button)
        return;
    std::vector<unsigned> before;
    for (const auto& v : m_views)
        before.push_back(v.id);
    m_views = m_ops->List();
    const ULONGLONG now = GetTickCount64();
    bool added = false;
    for (const auto& v : m_views)
    {
        added |= std::find(before.begin(), before.end(), v.id) == before.end();
        Track& tr = m_tracks[v.id];
        if (v.state != State::Running)
            continue;
        // Files' StatusCenterItemProgressModel: speed from the change since the last sample; graph points by percent
        if (!tr.at)
        {
            tr = { tr.expanded, now, v.doneBytes, v.doneFiles };
            continue;
        }
        if (now - tr.at >= 500)
        {
            const double sec = (now - tr.at) / 1000.0;
            tr.bySize = v.totalBytes > 0;
            tr.speed = tr.bySize ? (v.doneBytes - std::min(v.doneBytes, tr.bytes)) / sec : (v.doneFiles - std::min(v.doneFiles, tr.files)) / sec;
            tr.at = now;
            tr.bytes = v.doneBytes;
            tr.files = v.doneFiles;
            const float x = static_cast<float>(std::max(0, v.percent));
            if (Started(v) && (tr.points.empty() || x - tr.points.back().first > 0.5f))
                tr.points.emplace_back(x, static_cast<float>(tr.speed));
        }
    }
    for (auto it = m_tracks.begin(); it != m_tracks.end();)
        it = std::any_of(m_views.begin(), m_views.end(), [&](const auto& v) { return v.id == it->first; }) ? std::next(it) : m_tracks.erase(it);
    const int running = Running();
    if (running > 0)
        m_ring = true; // Files' HasAnyItemInProgress
    if (m_views.empty())
        m_ring = false;
    if (added && m_tipPending)
        ShowTip();
    if (Wanted() != m_wanted)
    {
        m_wanted = Wanted();
        if (!m_wanted)
            Hide();
        SendMessageW(m_parent, WM_COMMAND, MAKEWPARAM(m_id, kVisibilityChanged), reinterpret_cast<LPARAM>(m_button));
    }
    if (running > 0)
        SetTimer(m_button, kTimer, 250, nullptr);
    else
        KillTimer(m_button, kTimer);
    UpdateTaskbar();
    UpdateText();
    InvalidateRect(m_button, nullptr, FALSE);
    if (Open())
        Resize();
}

void StatusCenter::Toggle()
{
    if (Open())
    {
        Hide();
        return;
    }
    if (GetTickCount64() - m_hiddenAt < 250)
        return; // this click is what closed it
    HideTip();
    Refresh();
    // Files' OnStatusCenterFlyoutOpened: the ring stays only while something runs or a failure is listed
    m_ring = Running() > 0 || InfoState() == 3;
    m_scroll = 0;
    Resize();
    ShowWindow(m_flyout, SW_SHOW);
    SetFocus(m_flyout);
    InvalidateRect(m_button, nullptr, FALSE);
}

void StatusCenter::Hide()
{
    if (!Open())
        return;
    m_smooth.Reset(m_scroll); // stop ticking a hidden flyout
    ShowWindow(m_flyout, SW_HIDE);
    m_hiddenAt = GetTickCount64();
    if (m_cardTip)
        SendMessageW(m_cardTip, TTM_POP, 0, 0);
    if (m_button)
        InvalidateRect(m_button, nullptr, FALSE);
}

RECT StatusCenter::ClearRect() const
{
    const Theme& t = *m_theme;
    RECT rc{};
    GetClientRect(m_flyout, &rc);
    HDC hdc = GetDC(m_flyout);
    const int w = TextWidth(hdc, t.caption, Tr(L"清除已完成的条目")) + t.S(16);
    ReleaseDC(m_flyout, hdc);
    const int mid = t.S(kHead) / 2;
    return { rc.right - t.S(kSide) - w, mid - t.S(12), rc.right - t.S(kSide), mid + t.S(12) };
}

RECT StatusCenter::ListRect() const
{
    RECT rc{};
    GetClientRect(m_flyout, &rc);
    return { m_theme->S(kSide), m_theme->S(kHead), rc.right - m_theme->S(kSide), rc.bottom - m_theme->S(kSide) };
}

StatusCenter::Layout StatusCenter::LayoutCard(const Operations::View& v, int width) const
{
    const Theme& t = *m_theme;
    Layout l;
    const bool running = v.state == State::Running;
    const bool determinate = Determinate(v);
    const auto it = m_tracks.find(v.id);
    const bool expanded = running && determinate && it != m_tracks.end() && it->second.expanded;
    const int pad = t.S(kCardPad), button = t.S(32);
    // Header row: 32px circle, the header (2 lines at most), the dismiss X (finished) or the chevron (running)
    const bool side = !running || determinate;
    const int textLeft = pad + t.S(32) + t.S(12), textRight = width - pad - (side ? button + t.S(12) : 0);
    const std::wstring header = Header(v);
    const int measureWidth = std::max(1, textRight - textLeft);
    int lineH, measuredHeight;
    if (DWriteText::Available())
    {
        lineH = std::max(1, DWriteText::LineHeight(t.ui));
        measuredHeight = DWriteText::WrappedHeight(nullptr, t.ui, header, measureWidth);
    }
    else
    {
        HDC hdc = GetDC(m_flyout);
        HGDIOBJ old = SelectObject(hdc, t.ui);
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        RECT measure{ 0, 0, measureWidth, 0 };
        DrawTextW(hdc, header.c_str(), static_cast<int>(header.size()), &measure, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        SelectObject(hdc, old);
        ReleaseDC(m_flyout, hdc);
        lineH = std::max(1, static_cast<int>(tm.tmHeight));
        measuredHeight = measure.bottom;
    }
    l.headerLines = std::clamp((measuredHeight + lineH - 1) / lineH, 1, 2);
    const int rowH = std::max(button, l.headerLines * lineH);
    l.circle = { pad, pad + (rowH - t.S(32)) / 2, pad + t.S(32), pad + (rowH - t.S(32)) / 2 + t.S(32) };
    l.header = { textLeft, pad + (rowH - l.headerLines * lineH) / 2, textRight, pad + (rowH + l.headerLines * lineH) / 2 };
    const RECT sideButton{ width - pad - button, pad + (rowH - button) / 2, width - pad, pad + (rowH + button) / 2 };
    if (!running)
        l.dismiss = sideButton;
    else if (determinate)
        l.expand = sideButton;
    int y = pad + rowH;
    if (running)
    {
        // The progress grid (margin 0,8): bar or graph, then the message; the ... button beside both
        l.progress = true;
        y += t.S(8);
        const int top = y, right = width - pad - (v.kind != Kind::EmptyBin ? button + t.S(4) : 0);
        if (expanded)
        {
            l.graphShown = true;
            l.graph = { pad + t.S(4), y + t.S(4), right - t.S(4), y + t.S(4) + t.S(88) };
            y = l.graph.bottom + t.S(12);
            l.name = { pad + t.S(4), y, right - t.S(4), y + t.S(20) };
            y += t.S(22);
            l.message = { pad + t.S(4), y, right - t.S(4), y + t.S(16) };
            y += t.S(16);
        }
        else
        {
            l.bar = { pad + t.S(4), y + t.S(4), right - t.S(4), y + t.S(8) };
            y += t.S(12);
            if (determinate)
            {
                l.message = { pad + t.S(4), y, right - t.S(4), y + t.S(16) };
                y += t.S(16);
            }
        }
        if (v.kind != Kind::EmptyBin && !v.cancelRequested)
            l.more = { width - pad - button, (top + y) / 2 - button / 2, width - pad, (top + y) / 2 + button / 2 };
        y += t.S(8);
    }
    l.height = y + pad;
    return l;
}

int StatusCenter::ContentHeight() const
{
    // The cards' width is fixed (the flyout is always kWidth), so this doesn't depend on the window's current size
    const int width = m_theme->S(kWidth) - 2 * m_theme->S(kSide);
    int h = 0;
    for (const auto& v : m_views)
        h += LayoutCard(v, width).height + m_theme->S(kCardGap);
    return h;
}

void StatusCenter::Resize()
{
    const Theme& t = *m_theme;
    RECT button{};
    GetWindowRect(m_button, &button);
    const int w = t.S(kWidth);
    const int content = m_views.empty() ? t.S(64) : ContentHeight();
    const int h = std::clamp(t.S(kHead) + content + t.S(kSide), t.S(kMinHeight), t.S(kMaxHeight));
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(m_parent, MONITOR_DEFAULTTONEAREST), &mi);
    int x = button.right - w, y = button.bottom + t.S(4);
    x = std::clamp(x, static_cast<int>(mi.rcWork.left), std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - w));
    y = std::min(y, std::max(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - h));
    SetWindowPos(m_flyout, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
    // OverlayCornerRadius 8
    SetWindowRgn(m_flyout, CreateRoundRectRgn(0, 0, w + 1, h + 1, t.S(16), t.S(16)), TRUE);
    const RECT list = ListRect();
    const int maxScroll = std::max(0, content - static_cast<int>(list.bottom - list.top));
    m_smooth.ClampTarget(maxScroll);
    m_scroll = std::clamp(m_scroll, 0, maxScroll);
    InvalidateRect(m_flyout, nullptr, FALSE);
}

void StatusCenter::UpdateText()
{
    // "state<TAB>header<TAB>message" per card, newest first (the flyout window's text; tests read it)
    static const wchar_t* const names[] = { L"running", L"done", L"cancelled", L"failed" };
    std::wstring text;
    for (const auto& v : m_views)
    {
        text += std::wstring(names[static_cast<int>(v.state)]) + L"\t" + Header(v) + L"\t" +
                (v.state == State::Running ? (Determinate(v) ? Message(v) : L"") : v.error) + L"\n";
    }
    if (text != m_text)
    {
        m_text = text;
        SetWindowTextW(m_flyout, text.c_str());
    }
}

void StatusCenter::UpdateTaskbar()
{
    // Files' StatusCenterViewModel: TBPF_NORMAL with the average while operations run, TBPF_NOPROGRESS otherwise
    const int running = Running();
    const int state = running ? TBPF_NORMAL : TBPF_NOPROGRESS, value = running ? Average() : 0;
    if (state == m_taskbarState && value == m_taskbarValue)
        return;
    if (!m_taskbar && running)
    {
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_taskbar))))
            m_taskbar->HrInit();
    }
    if (!m_taskbar)
        return;
    m_taskbarState = state;
    m_taskbarValue = value;
    m_taskbar->SetProgressState(m_parent, static_cast<TBPFLAG>(state));
    if (running)
        m_taskbar->SetProgressValue(m_parent, static_cast<ULONGLONG>(value), 100);
}

void StatusCenter::ShowTip()
{
    // Files' OngoingTasksTeachingTip: once, on the first card, pointing at the button
    if (!m_button || !IsWindowVisible(m_button) || !IsWindowVisible(m_parent))
        return; // stays pending
    m_tipPending = false;
    if (!m_tip)
    {
        m_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_BALLOON | TTS_CLOSE, 0, 0, 0, 0, m_parent,
            nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_tip)
            return;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
        ti.hwnd = m_parent;
        ti.uId = reinterpret_cast<UINT_PTR>(m_button);
        ti.lpszText = const_cast<wchar_t*>(Tr(L"在此处检查文件操作状态"));
        SendMessageW(m_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(m_tip, TTM_SETTITLEW, TTI_INFO, reinterpret_cast<LPARAM>(Tr(L"状态中心")));
        SendMessageW(m_tip, TTM_SETMAXTIPWIDTH, 0, m_theme->S(300));
    }
    RECT rc{};
    GetWindowRect(m_button, &rc);
    SendMessageW(m_tip, TTM_TRACKPOSITION, 0, MAKELPARAM((rc.left + rc.right) / 2, rc.bottom));
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = m_parent;
    ti.uId = reinterpret_cast<UINT_PTR>(m_button);
    SendMessageW(m_tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    SetTimer(m_button, kTipTimer, 8000, nullptr);
    SendMessageW(m_parent, WM_COMMAND, MAKEWPARAM(m_id, kTipShown), reinterpret_cast<LPARAM>(m_button));
}

void StatusCenter::HideTip()
{
    if (!m_tip || !IsWindow(m_tip))
        return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = m_parent;
    ti.uId = reinterpret_cast<UINT_PTR>(m_button);
    SendMessageW(m_tip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    if (m_button)
        KillTimer(m_button, kTipTimer);
}
void StatusCenter::PaintFlyout(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    // AcrylicInAppFillColorDefault without the blur (Windows 10), the flyout's stroke
    const COLORREF back = t.flyout;
    FillSolid(hdc, rc, back);
    RECT client{};
    GetClientRect(m_flyout, &client);
    FrameRound(hdc, client, t.S(8), t.flyoutStroke);
    // Header: 状态中心, 清除已完成的条目 on the right
    DrawLabel(hdc, m_title, Tr(L"状态中心"), { t.S(kSide + 4), 0, client.right - t.S(kSide), t.S(kHead) }, t.ink);
    const RECT clear = ClearRect();
    const bool hot = m_hotHit == HitClear;
    FillRound(hdc, clear, t.S(4), hot ? Mix(back, t.ink, 6) : Mix(back, t.fillWhite, t.dark ? 6 : 70));
    FrameRound(hdc, clear, t.S(4), Mix(back, t.ink, 12));
    DrawLabel(hdc, t.caption, Tr(L"清除已完成的条目"), clear, t.ink, DT_CENTER);
    const RECT list = ListRect();
    if (m_views.empty())
    {
        DrawLabel(hdc, t.ui, Tr(L"没有正在进行的文件操作"), { list.left, list.top, list.right, list.top + t.S(48) }, t.muted, DT_CENTER);
        return;
    }
    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
    int y = list.top - m_scroll;
    for (const auto& v : m_views)
    {
        const Layout l = LayoutCard(v, list.right - list.left);
        const RECT card{ list.left, y + t.S(2), list.right, y + t.S(2) + l.height };
        if (card.bottom >= list.top && card.top <= list.bottom)
            PaintCard(hdc, v, card, l);
        y += l.height + t.S(kCardGap);
    }
    RestoreDC(hdc, saved);
    const int content = ContentHeight(), h = list.bottom - list.top;
    if (content > h && h > 0)
    {
        const int th = std::max(t.S(24), MulDiv(h, h, content));
        const int top = list.top + MulDiv(h - th, m_scroll, content - h);
        FillRound(hdc, { client.right - t.S(6), top, client.right - t.S(3), top + th }, t.S(2), Mix(t.muted, back, 40));
    }
}

void StatusCenter::PaintCard(HDC hdc, const Operations::View& v, const RECT& card, const Layout& l)
{
    const Theme& t = *m_theme;
    auto at = [&card](const RECT& r) { return RECT{ card.left + r.left, card.top + r.top, card.left + r.right, card.top + r.bottom }; };
    // CardBackgroundFillColorDefault over the flyout, OverlayCornerRadius
    const COLORREF fill = t.flyoutCard;
    FillRound(hdc, card, t.S(8), fill);
    COLORREF circle = 0, icon = 0;
    Tones(t, v, fill, circle, icon);
    const RECT c = at(l.circle);
    FillRound(hdc, c, t.S(16), circle);
    DrawGlyph(hdc, t.icons, Icon(v), c, icon);
    // The header, wrapped to two lines
    {
        RECT h = at(l.header);
        const std::wstring text = Header(v);
        if (DWriteText::Available())
            DWriteText::DrawWrapped(hdc, t.ui, text, h, t.ink, DT_LEFT | DT_END_ELLIPSIS);
        else
        {
            HGDIOBJ old = SelectObject(hdc, t.ui);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, t.ink);
            DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &h, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, old);
        }
    }
    auto button = [&](const RECT& r, Hit hit, const wchar_t* glyph)
    {
        if (IsRectEmpty(&r))
            return;
        const RECT b = at(r);
        if (m_hotHit == hit && m_hotId == v.id)
            FillRound(hdc, b, t.S(4), Mix(fill, t.ink, 6));
        DrawGlyph(hdc, t.icons, glyph, b, t.ink);
    };
    button(l.dismiss, HitDismiss, L"\xE711");
    const auto tr = m_tracks.find(v.id);
    const bool expanded = tr != m_tracks.end() && tr->second.expanded;
    button(l.expand, HitExpand, expanded ? L"\xE70E" : L"\xE70D");
    button(l.more, HitMore, L"\xE712");
    if (!l.progress)
        return;
    const COLORREF track = Mix(fill, t.ink, 30);
    if (!IsRectEmpty(&l.bar))
    {
        // WinUI ProgressBar: a 1px track, the 3px rounded indicator; indeterminate: a sliding segment
        const RECT b = at(l.bar);
        const int mid = (b.top + b.bottom) / 2;
        FillSolid(hdc, { b.left, mid, b.right, mid + std::max(1, t.S(1)) }, track);
        const int w = b.right - b.left, ih = std::max(2, t.S(3));
        if (Determinate(v) && v.percent >= 0)
        {
            const int fillW = w * std::clamp(v.percent, 0, 100) / 100;
            if (fillW > 0)
                FillRound(hdc, { b.left, mid - ih / 2, b.left + fillW, mid - ih / 2 + ih }, ih / 2, t.accent);
        }
        else
        {
            const int seg = w * 2 / 5, span = w + seg;
            const int x = b.left - seg + static_cast<int>((GetTickCount64() / 4) % static_cast<ULONGLONG>(std::max(1, span)));
            const int saved = SaveDC(hdc);
            IntersectClipRect(hdc, b.left, b.top - t.S(4), b.right, b.bottom + t.S(4));
            FillRound(hdc, { x, mid - ih / 2, x + seg, mid - ih / 2 + ih }, ih / 2, t.accent);
            RestoreDC(hdc, saved);
        }
    }
    if (l.graphShown && tr != m_tracks.end())
    {
        // SpeedGraph: speed over percent, a gradient area under the line (accent), "速度：" and the speed on top
        const RECT g = at(l.graph);
        FillRound(hdc, g, t.S(4), Mix(fill, t.ink, 4));
        const auto& pts = tr->second.points;
        if (pts.size() >= 2)
        {
            float top = 0;
            for (const auto& p : pts)
                top = std::max(top, p.second);
            top = top > 0 ? top * 1.15f : 1.0f;
            const float gx = static_cast<float>(g.left), gy = static_cast<float>(g.top), gw = static_cast<float>(g.right - g.left),
                        gh = static_cast<float>(g.bottom - g.top);
            std::vector<Gdiplus::PointF> line;
            for (const auto& p : pts)
                line.emplace_back(gx + gw * std::clamp(p.first, 0.0f, 100.0f) / 100.0f, gy + gh - gh * p.second / top);
            std::vector<Gdiplus::PointF> area = line;
            area.emplace_back(line.back().X, gy + gh);
            area.emplace_back(line.front().X, gy + gh);
            Gdiplus::Graphics gr(hdc);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath path;
            path.AddPolygon(area.data(), static_cast<INT>(area.size()));
            const Gdiplus::Color accent(255, GetRValue(t.accent), GetGValue(t.accent), GetBValue(t.accent));
            const Gdiplus::Color clearAccent(30, GetRValue(t.accent), GetGValue(t.accent), GetBValue(t.accent));
            Gdiplus::LinearGradientBrush brush(Gdiplus::PointF(gx, gy), Gdiplus::PointF(gx, gy + gh), Gdiplus::Color(150, accent.GetR(), accent.GetG(),
                accent.GetB()), clearAccent);
            gr.FillPath(&brush, &path);
            Gdiplus::Pen pen(accent, static_cast<float>(std::max(1, t.S(1))) * 1.5f);
            gr.DrawLines(&pen, line.data(), static_cast<INT>(line.size()));
        }
        const Track& k = tr->second;
        // Files' SpeedText: "<size>/s", "<n> items/s" without a size, N/A before anything moved
        std::wstring speed = !Started(v) ? L"N/A" : k.bySize ? Bytes(static_cast<ULONGLONG>(k.speed)) + L"/s" : std::to_wstring(static_cast<long long>(k.speed + 0.5)) + L" items/s";
        DrawLabel(hdc, t.caption, Tr(L"速度："), { g.left + t.S(8), g.top + t.S(4), g.right - t.S(8), g.top + t.S(18) }, t.muted);
        DrawLabel(hdc, m_title, speed, { g.left + t.S(8), g.top + t.S(18), g.right - t.S(8), g.top + t.S(38) }, t.ink);
        DrawLabel(hdc, t.ui, std::wstring(Tr(L"名称：")) + v.current, at(l.name), t.ink, DT_END_ELLIPSIS);
    }
    if (!IsRectEmpty(&l.message))
        DrawLabel(hdc, t.caption, Message(v), at(l.message), t.muted, DT_END_ELLIPSIS);
}

StatusCenter::Hit StatusCenter::HitTest(POINT pt, unsigned& id) const
{
    id = 0;
    const RECT clear = ClearRect();
    if (PtInRect(&clear, pt))
        return HitClear;
    const RECT list = ListRect();
    if (!PtInRect(&list, pt))
        return HitNone;
    int y = list.top - m_scroll;
    for (const auto& v : m_views)
    {
        const Layout l = LayoutCard(v, list.right - list.left);
        const RECT card{ list.left, y + m_theme->S(2), list.right, y + m_theme->S(2) + l.height };
        if (PtInRect(&card, pt))
        {
            id = v.id;
            const POINT local{ pt.x - card.left, pt.y - card.top };
            if (PtInRect(&l.dismiss, local))
                return HitDismiss;
            if (PtInRect(&l.expand, local))
                return HitExpand;
            if (PtInRect(&l.more, local))
                return HitMore;
            return HitNone;
        }
        y += l.height + m_theme->S(kCardGap);
    }
    return HitNone;
}

LRESULT CALLBACK StatusCenter::FlyoutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<StatusCenter*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<StatusCenter*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_flyout = hwnd;
    }
    return self ? self->FlyoutMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT StatusCenter::FlyoutMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        BufferedPaint paint(hwnd);
        PaintFlyout(paint.dc(), paint.rect());
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            Hide(); // Files' light-dismiss flyout
        break;
    case WM_MOUSEMOVE:
    {
        if (!m_flyTracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            m_flyTracking = TrackMouseEvent(&tme) != FALSE;
        }
        unsigned id = 0;
        const Hit hit = HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, id);
        if (id != m_tipId && m_cardTip)
            SendMessageW(m_cardTip, TTM_POP, 0, 0); // the next card's text on the next hover
        m_tipId = id;
        if (hit != m_hotHit || id != m_hotId)
        {
            m_hotHit = hit;
            m_hotId = id;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_flyTracking = false;
        m_hotHit = HitNone;
        m_hotId = m_tipId = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        unsigned id = 0;
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        switch (HitTest(pt, id))
        {
        case HitClear:
            m_ops->ForgetFinished(); // Files' RemoveAllCompletedItems
            Refresh();
            break;
        case HitDismiss:
            m_ops->Forget(id);
            Refresh();
            break;
        case HitExpand:
            m_tracks[id].expanded = !m_tracks[id].expanded;
            Resize();
            break;
        case HitMore:
        {
            // Files' MoreOptions flyout: 取消
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, Tr(L"取消"));
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1)
            {
                m_ops->Cancel(id);
                Refresh();
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        const RECT list = ListRect();
        const int maxScroll = std::max(0, ContentHeight() - static_cast<int>(list.bottom - list.top));
        m_scroll = m_smooth.OnWheel(hwnd, GET_WHEEL_DELTA_WPARAM(wp), m_theme->S(48), maxScroll);
        InvalidateRect(m_flyout, nullptr, FALSE);
        return 0;
    }
    case SmoothFrame::kMsgTick:
        if (m_smooth.Animating())
        {
            m_scroll = m_smooth.Tick();
            InvalidateRect(m_flyout, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            Hide();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == kClearCompleted)
        {
            m_ops->ForgetFinished();
            Refresh();
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW)
        {
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp);
            m_cardTipText.clear();
            for (const auto& v : m_views)
                if (v.id == m_tipId)
                    m_cardTipText = Header(v) + (v.state == State::Failed && !v.error.empty() ? L"\n" + v.error : L"");
            info->lpszText = const_cast<wchar_t*>(m_cardTipText.c_str());
            return 0;
        }
        break;
    case WM_DESTROY:
        m_flyout = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
