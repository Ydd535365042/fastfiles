#include "ColorPickerFlyout.h"
#include "Lang.h"
#include "DarkMode.h"

namespace ColorPickerFlyout
{
namespace
{
constexpr wchar_t kClass[] = L"FastFiles.ColorPicker";
// 96-DPI layout. WinUI ColorPicker (spectrum off): preview + hex row, then 4 sliders (R/G/B/A), each a label,
// a numeric readout and a track; Files' SettingsExpander flyout padding is 16 all around (Flyout default).
constexpr int kW = 280, kPad = 16, kPreview = 48, kGap = 12, kRowH = 20, kTrackH = 4, kSliderGap = 6, kHexH = 32;
constexpr int kButtonRow = 32;

HFONT Resize(HFONT base, int px, int weight)
{
    LOGFONTW lf{};
    GetObjectW(base, sizeof(lf), &lf);
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

// Files' color button content: a checkerboard under the color (shared look with SettingsView's DrawColorSwatch)
struct SwatchArgs
{
    const Theme* theme;
    uint32_t argb;
};

void PaintSwatch(HDC hdc, const RECT& rc, void* context)
{
    const auto& args = *static_cast<SwatchArgs*>(context);
    const Theme& t = *args.theme;
    const int h = rc.bottom - rc.top;
    const int cell = std::max(1, h / 4);
    const int a = static_cast<int>(args.argb >> 24) * 100 / 255;
    const COLORREF c = RGB((args.argb >> 16) & 0xFF, (args.argb >> 8) & 0xFF, args.argb & 0xFF);
    int y = rc.top, row = 0;
    while (y < rc.bottom)
    {
        int x = rc.left, col = row;
        while (x < rc.right)
        {
            const RECT cellRc{ x, y, std::min<LONG>(x + cell, rc.right), std::min<LONG>(y + cell, rc.bottom) };
            FillSolid(hdc, cellRc, Mix(col % 2 ? t.checkerLight : t.checkerDark, c, a));
            x += cell;
            ++col;
        }
        y += cell;
        ++row;
    }
}

// The rounded corner used to be a hard SelectClipRgn clip (jagged); FillRoundPattern composites the same checkerboard
// through GDI+ so the boundary is anti-aliased like every other rounded shape
void DrawSwatch(HDC hdc, const Theme& t, const RECT& rc, uint32_t argb, int radius)
{
    SwatchArgs args{ &t, argb };
    FillRoundPattern(hdc, rc, radius, PaintSwatch, &args);
}

struct Flyout
{
    HWND hwnd = nullptr, owner = nullptr;
    const Theme* theme = nullptr;
    HFONT label = nullptr, mono = nullptr;
    HBRUSH fieldBrush = nullptr;
    HWND hexEdit = nullptr;
    uint32_t argb = 0;          // current #AARRGGBB being edited
    uint32_t initial = 0;
    bool accepted = false, done = false, syncingHex = false;
    int dragChannel = -1;       // 0=R 1=G 2=B 3=A while a slider thumb is being dragged
    int hotChannel = -1;
    int hotButton = 0, pressed = 0; // IDOK / IDCANCEL
    // The keyboard while the flyout itself has the focus (WinUI ColorPicker: every slider is a Tab stop, the arrows
    // move it): kStopChannel + channel, IDOK, IDCANCEL; 0 = the hex box
    static constexpr int kStopChannel = 1000;
    int focusStop = 0;
    bool ring = false; // the focus visual: after Tab, not after a click

    int S(int v) const { return theme->S(v); }
    // Tab order: hex box, R, G, B, A, 确定, 取消 (round, Shift+Tab back)
    void FocusNext(bool back)
    {
        const int stops[] = { 0, kStopChannel, kStopChannel + 1, kStopChannel + 2, kStopChannel + 3, IDOK, IDCANCEL };
        constexpr int n = static_cast<int>(std::size(stops));
        int at = 0;
        for (int i = 0; i < n; ++i)
            if (stops[i] == (GetFocus() == hexEdit ? 0 : focusStop))
                at = i;
        focusStop = stops[(at + (back ? n - 1 : 1)) % n];
        ring = true;
        if (focusStop == 0)
        {
            SetFocus(hexEdit);
            SendMessageW(hexEdit, EM_SETSEL, 0, -1);
        }
        else
            SetFocus(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    bool Keyboard(int stop) const { return ring && focusStop == stop && GetFocus() == hwnd; }
    RECT Client() const { RECT rc{}; GetClientRect(hwnd, &rc); return rc; }
    RECT PreviewRect() const { const RECT c = Client(); return { S(kPad), S(kPad), c.right - S(kPad), S(kPad + kPreview) }; }
    RECT HexRect() const { const RECT c = Client(); const int top = S(kPad + kPreview + kGap); return { S(kPad), top, c.right - S(kPad), top + S(kHexH) }; }
    int ChannelTop(int i) const { return S(kPad + kPreview + kGap + kHexH + kGap) + i * S(kRowH + kTrackH + kSliderGap + 14); }
    RECT ChannelLabelRect(int i) const { const RECT c = Client(); const int top = ChannelTop(i); return { S(kPad), top, c.right - S(kPad), top + S(kRowH) }; }
    RECT TrackRect(int i) const
    {
        const RECT c = Client();
        const int top = ChannelTop(i) + S(kRowH + 2);
        return { S(kPad), top, c.right - S(kPad), top + S(kTrackH) };
    }
    RECT ButtonsRect() const { const RECT c = Client(); const int top = ChannelTop(4) + S(4); return { 0, top, c.right, top + S(kButtonRow) + S(2 * kPad) }; }
    int ContentHeight() const { return ChannelTop(4) + S(4) + S(kButtonRow) + S(2 * kPad); }

    // Channel value 0..255 (alpha is channel 3)
    int Channel(int i) const
    {
        switch (i)
        {
        case 0: return static_cast<int>((argb >> 16) & 0xFF);
        case 1: return static_cast<int>((argb >> 8) & 0xFF);
        case 2: return static_cast<int>(argb & 0xFF);
        default: return static_cast<int>((argb >> 24) & 0xFF);
        }
    }
    void SetChannel(int i, int v)
    {
        v = std::clamp(v, 0, 255);
        const uint32_t mask[4] = { 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u };
        const int shift[4] = { 16, 8, 0, 24 };
        argb = (argb & ~mask[i]) | (static_cast<uint32_t>(v) << shift[i]);
        SyncHex();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void SyncHex()
    {
        if (!hexEdit)
            return;
        wchar_t hex[16];
        swprintf_s(hex, L"#%02X%02X%02X%02X", (argb >> 24) & 0xFF, (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
        syncingHex = true;
        SetWindowTextW(hexEdit, hex);
        syncingHex = false;
    }
    void HexChanged()
    {
        if (syncingHex)
            return;
        wchar_t buf[16]{};
        GetWindowTextW(hexEdit, buf, 15);
        std::wstring s = buf;
        if (!s.empty() && s[0] == L'#')
            s.erase(0, 1);
        if (s.size() != 6 && s.size() != 8)
            return; // keep typing; Files' hex box only commits on a full value too
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(s.c_str(), &end, 16);
        if (!end || *end)
            return;
        argb = s.size() == 8 ? static_cast<uint32_t>(v) : 0xFF000000u | static_cast<uint32_t>(v);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    int ChannelAtY(int y) const
    {
        for (int i = 0; i < 4; ++i)
        {
            RECT r = TrackRect(i);
            InflateRect(&r, 0, S(8));
            if (y >= r.top && y <= r.bottom)
                return i;
        }
        return -1;
    }
    void DragTo(int channel, int x)
    {
        const RECT track = TrackRect(channel);
        const int width = std::max<int>(1, static_cast<int>(track.right - track.left));
        const int v = MulDiv(static_cast<int>(x - track.left), 255, width);
        SetChannel(channel, v);
    }
    int ButtonAt(POINT pt) const
    {
        const RECT b = ButtonsRect();
        const int w = (Client().right - 2 * S(kPad) - S(8)) / 2;
        const RECT cancel{ Client().right - S(kPad) - w, b.top + S(kPad), Client().right - S(kPad), b.top + S(kPad) + S(kButtonRow) };
        const RECT ok{ S(kPad), b.top + S(kPad), S(kPad) + w, b.top + S(kPad) + S(kButtonRow) };
        if (PtInRect(&cancel, pt)) return IDCANCEL;
        if (PtInRect(&ok, pt)) return IDOK;
        return 0;
    }
    RECT ButtonRect(int id) const
    {
        const RECT b = ButtonsRect();
        const int w = (Client().right - 2 * S(kPad) - S(8)) / 2;
        return id == IDOK ? RECT{ S(kPad), b.top + S(kPad), S(kPad) + w, b.top + S(kPad) + S(kButtonRow) }
                           : RECT{ Client().right - S(kPad) - w, b.top + S(kPad), Client().right - S(kPad), b.top + S(kPad) + S(kButtonRow) };
    }

    void Paint(HDC hdc)
    {
        const Theme& t = *theme;
        const RECT c = Client();
        FillSolid(hdc, c, t.flyoutCard);
        FrameRound(hdc, { 0, 0, c.right, c.bottom }, S(8), t.flyoutStroke);
        // Preview + hex row (Files' ColorPicker PreviewColorAndHexCodeGrid)
        const RECT prev = PreviewRect();
        DrawSwatch(hdc, t, prev, argb, S(4));
        FrameRound(hdc, prev, S(4), t.cardStroke);
        // The hex box is a real EDIT child; just frame it here
        const RECT hex = HexRect();
        FrameRound(hdc, hex, S(4), t.controlStroke);
        static const wchar_t* const chLabel[4] = { L"R", L"G", L"B", L"A" };
        for (int i = 0; i < 4; ++i)
        {
            const RECT lr = ChannelLabelRect(i);
            DrawLabel(hdc, label, chLabel[i], lr, t.ink);
            DrawLabel(hdc, label, std::to_wstring(Channel(i)), lr, t.muted, DT_RIGHT);
            const RECT track = TrackRect(i);
            FillRound(hdc, track, S(2), t.scrubTrack);
            const int v = Channel(i);
            const int x = track.left + MulDiv(v, track.right - track.left, 255);
            const COLORREF fillColor = i == 0 ? t.channelRed : i == 1 ? t.channelGreen : i == 2 ? t.channelBlue : t.accent;
            FillRound(hdc, { track.left, track.top, std::max<LONG>(track.left + S(2), static_cast<LONG>(x)), track.bottom }, S(2), fillColor);
            const int cy = (track.top + track.bottom) / 2;
            const RECT thumb{ x - S(7), cy - S(7), x + S(7), cy + S(7) };
            FillRound(hdc, thumb, S(7), t.sliderThumb);
            FrameRound(hdc, thumb, S(7), t.controlStroke);
            FillRound(hdc, { x - S(3), cy - S(3), x + S(3), cy + S(3) }, S(3), fillColor);
            if (Keyboard(kStopChannel + i))
                DrawFocusVisual(hdc, t, { track.left - S(4), cy - S(5), track.right + S(4), cy + S(5) }, S(5), t.flyoutCard, &c);
        }
        // OK / Cancel (Files' Flyout has no explicit buttons, but WinUI ColorPicker's More section has no commit
        // button either; FastFiles adds Primary/Close like its other flyouts/dialogs so the alpha choice is explicit)
        Button(hdc, IDOK, Tr(L"确定"), true);
        Button(hdc, IDCANCEL, Tr(L"取消"), false);
    }
    void Button(HDC hdc, int id, const std::wstring& text, bool primary) const
    {
        const Theme& t = *theme;
        const RECT r = ButtonRect(id);
        const bool hotNow = hotButton == id, down = hotNow && pressed == id;
        COLORREF fill = primary ? t.accent : (down ? t.controlFillPressed : hotNow ? t.controlFillHover : t.controlFill);
        if (primary && hotNow)
            fill = Mix(t.accent, t.ink, down ? 18 : 10);
        FillRound(hdc, r, S(4), fill);
        if (!primary)
            FrameRound(hdc, r, S(4), t.controlStroke);
        DrawLabel(hdc, label, text, r, primary ? t.textOnAccent : t.ink, DT_CENTER);
        if (Keyboard(id))
        {
            const RECT c = Client();
            DrawFocusVisual(hdc, t, r, S(4), t.flyoutCard, &c);
        }
    }

    bool closing = false; // DestroyWindow below re-enters WM_ACTIVATE(WA_INACTIVE) before WM_DESTROY; ignore it
    void Close(bool accept)
    {
        if (closing)
            return;
        closing = true;
        accepted = accept;
        if (owner)
            EnableWindow(owner, TRUE);
        DestroyWindow(hwnd);
    }

    static LRESULT CALLBACK HexProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<Flyout*>(ref);
        if (msg == WM_KEYDOWN)
        {
            if (wp == VK_RETURN) { self->Close(true); return 0; }
            if (wp == VK_ESCAPE) { self->Close(false); return 0; }
        }
        if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE))
            return 0;
        return DefSubclassProc(w, msg, wp, lp);
    }
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp)
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        switch (msg)
        {
        case WM_PAINT:
        {
            BufferedPaint paint(hwnd);
            Paint(paint.dc());
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        {
            ring = false; // pointer focus: no focus visual
            const int ch = ChannelAtY(pt.y);
            if (ch >= 0)
            {
                focusStop = kStopChannel + ch;
                SetFocus(hwnd);
            }
            if (ch >= 0)
            {
                dragChannel = ch;
                SetCapture(hwnd);
                DragTo(ch, pt.x);
            }
            else
            {
                pressed = ButtonAt(pt);
                if (pressed)
                    SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd };
            TrackMouseEvent(&tme);
            if (dragChannel >= 0)
                DragTo(dragChannel, pt.x);
            const int button = ButtonAt(pt);
            if (button != hotButton)
            {
                hotButton = button;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hotButton = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            if (dragChannel >= 0)
            {
                dragChannel = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
            }
            else if (pressed)
            {
                const int id = pressed;
                pressed = 0;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                if (id == ButtonAt(pt))
                    Close(id == IDOK);
            }
            return 0;
        case WM_KEYDOWN:
        {
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            const int ch = focusStop - kStopChannel;
            if (wp == VK_ESCAPE)
                Close(false);
            else if (wp == VK_TAB)
                FocusNext(shift);
            else if ((wp == VK_RETURN || wp == VK_SPACE) && (focusStop == IDOK || focusStop == IDCANCEL))
                Close(focusStop == IDOK);
            else if (wp == VK_RETURN)
                Close(true);
            else if (ch >= 0 && ch < 4)
            {
                // WinUI Slider keys: arrows by 1, Page Up / Down by 10 (Files' sliders: SmallChange 1), Home / End the ends
                int v = Channel(ch);
                switch (wp)
                {
                case VK_LEFT: case VK_DOWN: v -= 1; break;
                case VK_RIGHT: case VK_UP: v += 1; break;
                case VK_PRIOR: v += 10; break;
                case VK_NEXT: v -= 10; break;
                case VK_HOME: v = 0; break;
                case VK_END: v = 255; break;
                default: return 0;
                }
                ring = true;
                SetChannel(ch, v);
            }
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ACTIVATE:
            // Light dismiss: losing activation to anything but our own hex box closes without accepting
            if (LOWORD(wp) == WA_INACTIVE)
            {
                HWND next = reinterpret_cast<HWND>(lp);
                if (next != hexEdit)
                    Close(false);
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == hexEdit)
                HexChanged();
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), theme->ink);
            SetBkColor(reinterpret_cast<HDC>(wp), theme->field);
            return reinterpret_cast<LRESULT>(fieldBrush);
        case WM_CLOSE:
            Close(false);
            return 0;
        case WM_DESTROY:
            RemoveWindowSubclass(hexEdit, HexProc, 1);
            done = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
        {
            auto* self = static_cast<Flyout*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = w;
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto* self = reinterpret_cast<Flyout*>(GetWindowLongPtrW(w, GWLP_USERDATA)))
            return self->Handle(msg, wp, lp);
        return DefWindowProcW(w, msg, wp, lp);
    }
};
}

bool Show(HWND owner, const Theme* theme, POINT anchor, uint32_t initial, uint32_t& result)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = Flyout::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered)
        return false;

    Flyout f;
    f.owner = owner;
    f.theme = theme;
    f.argb = initial;
    f.initial = initial;
    f.label = Resize(theme->ui, theme->S(14), FW_NORMAL);
    f.mono = f.label;
    f.fieldBrush = CreateSolidBrush(theme->field);

    const int w = theme->S(kW);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP | WS_CLIPCHILDREN, anchor.x, anchor.y, w, theme->S(200),
        owner, nullptr, GetModuleHandleW(nullptr), &f);
    if (!hwnd)
    {
        DeleteObject(f.label);
        return false;
    }
    DarkMode::ApplyWindow(hwnd, theme->dark);

    const RECT hex = f.HexRect();
    f.hexEdit = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_UPPERCASE,
        hex.left + theme->S(10), hex.top + theme->S(4), hex.right - hex.left - theme->S(20), hex.bottom - hex.top - theme->S(8),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(f.hexEdit, WM_SETFONT, reinterpret_cast<WPARAM>(f.mono), FALSE);
    SendMessageW(f.hexEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetWindowSubclass(f.hexEdit, Flyout::HexProc, 1, reinterpret_cast<DWORD_PTR>(&f));
    f.SyncHex();

    const int h = f.ContentHeight();
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int x = std::clamp<int>(anchor.x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - w));
    int y = std::clamp<int>(anchor.y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h));
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, theme->S(8), theme->S(8));
    SetWindowRgn(hwnd, region, TRUE);

    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(f.hexEdit);
    SendMessageW(f.hexEdit, EM_SETSEL, 0, -1);

    MSG msg;
    while (!f.done)
    {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0)
        {
            if (IsWindow(hwnd))
                DestroyWindow(hwnd);
            break;
        }
        if (msg.hwnd == f.hexEdit || IsChild(hwnd, msg.hwnd))
        {
            if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE))
            {
                f.Close(msg.wParam == VK_RETURN);
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB)
            {
                f.FocusNext(GetKeyState(VK_SHIFT) < 0); // before IsDialogMessage swallows it
                continue;
            }
            if (!IsDialogMessageW(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    DeleteObject(f.label);
    DeleteObject(f.fieldBrush);
    result = f.argb;
    return f.accepted;
}
}
