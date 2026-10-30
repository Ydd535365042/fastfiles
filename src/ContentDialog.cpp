#include "ContentDialog.h"
#include "Lang.h"
#include "DarkMode.h"
#include "SmoothScroll.h"
#include "ScrollOverlay.h"

namespace ContentDialog
{
namespace
{
constexpr wchar_t kClass[] = L"FastFiles.ContentDialog", kSmokeClass[] = L"FastFiles.Smoke";
// 96-DPI layout from DefaultContentDialogStyle: 24 padding, 20px semibold title, 12 between parts, 80px button band
constexpr int kW = 448, kPad = 24, kTitle = 28, kGap = 12, kRow = 50, kMaxRows = 7, kBox = 32, kLabel = 20, kError = 18, kFoot = 80;
constexpr int kBrowseW = 72, kCheck = 32, kCheckBox = 20;
constexpr UINT_PTR kFadeTimer = 1, kPollTimer = 2, kBarTimer = 3;
constexpr int kComboId = 300, kCheckId = 400; // hit ids next to IDOK / IDCANCEL / 100 + field (浏览)
constexpr int kListStop = 500;                // the item list as a keyboard stop
constexpr int kSmokeAlpha = 77; // SmokeFillColorDefault: black at 30%

HFONT Resize(HFONT base, int px, int weight)
{
    LOGFONTW lf{};
    GetObjectW(base, sizeof(lf), &lf);
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

std::vector<std::wstring> Wrap(HDC hdc, HFONT font, const std::wstring& text, int width)
{
    std::vector<std::wstring> lines;
    std::wstring line;
    for (wchar_t c : text)
    {
        if (c == L'\n')
        {
            lines.push_back(line);
            line.clear();
            continue;
        }
        std::wstring next = line + c;
        if (!line.empty() && TextWidth(hdc, font, next) > width)
        {
            // Break at the last space when the line has one (Latin text), else at the character (CJK)
            const size_t space = line.rfind(L' ');
            if (space != std::wstring::npos && space > 0 && c != L' ')
            {
                lines.push_back(line.substr(0, space));
                line = line.substr(space + 1) + c;
            }
            else
            {
                lines.push_back(line);
                line = c == L' ' ? std::wstring() : std::wstring(1, c);
            }
        }
        else
            line = std::move(next);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

struct Dialog
{
    HWND hwnd = nullptr, owner = nullptr, smoke = nullptr;
    const Theme* theme = nullptr;
    Spec* spec = nullptr;
    HFONT title = nullptr, glyphs = nullptr;
    HBRUSH fieldBrush = nullptr, restBrush = nullptr; // text box background focused / not
    std::vector<HWND> edits;
    std::vector<std::wstring> textLines;
    Result result = Closed;
    bool done = false, syncing = false;
    // The drawn stop with the keyboard while the window itself has the focus (WinUI ContentDialog: Tab reaches every
    // control, the buttons too): kListStop, kCheckId, IDOK, IDCANCEL, 100 + field (浏览), kComboId + field. 0 = a text box.
    int focusStop = 0;
    bool ring = false; // the focus visual: after Tab, not after a click (WinUI keyboard focus)
    int hot = -1, focusRow = -1, scroll = 0;
    SmoothScroll smoothScroll; // the item list's wheel animation
    ScrollOverlay bar;         // the ListView's ScrollViewer bar (the file list's WinUI ScrollBar)
    int hotButton = 0, pressed = 0; // IDOK / IDCANCEL / 100 + field (浏览)
    ULONGLONG fadeStart = 0;

    int S(int v) const { return theme->S(v); }
    int Rows() const { return static_cast<int>(spec->items.size()); }
    int ListHeight() const { return std::min(Rows(), kMaxRows) * S(kRow); }
    int TextTop() const { return S(kPad + kTitle + kGap); }
    int TextHeight() const { return static_cast<int>(textLines.size()) * S(20); }
    int BodyTop() const { return TextTop() + (textLines.empty() ? 0 : TextHeight() + S(kGap)); }
    int FieldTop(int i) const { return BodyTop() + i * S(kLabel + 4 + kBox + kError + 6); }
    int ContentHeight() const
    {
        int h = BodyTop();
        if (!spec->items.empty())
            h += ListHeight() + S(kGap);
        if (!spec->fields.empty())
            h = FieldTop(static_cast<int>(spec->fields.size())) + S(6);
        if (!spec->check.empty())
            h = CheckTop() + S(kCheck) + S(kGap);
        return std::max(h, S(kPad + kTitle + kGap)) + S(kPad - kGap);
    }
    int CheckTop() const { return spec->fields.empty() ? BodyTop() : FieldTop(static_cast<int>(spec->fields.size())); }
    RECT CheckRect() const
    {
        const RECT c = Client();
        return { S(kPad), CheckTop(), std::min<LONG>(c.right - S(kPad), S(kPad + kCheckBox + 8) + TextWidthOf(spec->check) + S(8)), CheckTop() + S(kCheck) };
    }
    int TextWidthOf(const std::wstring& s) const
    {
        HDC hdc = GetDC(hwnd);
        const int w = TextWidth(hdc, theme->ui, s);
        ReleaseDC(hwnd, hdc);
        return w;
    }
    bool IsCombo(int i) const { return !spec->fields[i].choices.empty(); }
    RECT Client() const { RECT rc{}; GetClientRect(hwnd, &rc); return rc; }
    RECT ListRect() const { const RECT c = Client(); return { S(kPad) - S(4), BodyTop(), c.right - S(kPad) + S(4), BodyTop() + ListHeight() }; }
    RECT BoxRect(int i) const
    {
        const RECT c = Client();
        const int top = FieldTop(i) + S(kLabel + 4);
        const int right = c.right - S(kPad) - (spec->fields[i].browse ? S(kBrowseW + 8) : 0);
        return { S(kPad), top, right, top + S(kBox) };
    }
    RECT BrowseRect(int i) const { const RECT b = BoxRect(i); const RECT c = Client(); return { c.right - S(kPad + kBrowseW), b.top, c.right - S(kPad), b.bottom }; }
    RECT FootRect() const { const RECT c = Client(); return { 0, c.bottom - S(kFoot), c.right, c.bottom }; }
    RECT ButtonRect(int id) const
    {
        const RECT f = FootRect();
        const int y = f.top + S(kPad), h = S(32);
        const int inner = f.right - f.left - 2 * S(kPad);
        if (spec->primary.empty()) // one button takes half, on the right (WinUI's single CloseButton column)
            return { f.right - S(kPad) - (inner - S(8)) / 2, y, f.right - S(kPad), y + h };
        const int w = (inner - S(8)) / 2;
        return id == IDOK ? RECT{ f.left + S(kPad), y, f.left + S(kPad) + w, y + h } : RECT{ f.right - S(kPad) - w, y, f.right - S(kPad), y + h };
    }
    void Button(HDC hdc, int id, const std::wstring& text, bool primary) const
    {
        const Theme& t = *theme;
        const RECT r = ButtonRect(id);
        const bool enabled = id != IDOK || spec->primaryEnabled;
        const bool hotNow = enabled && hotButton == id, down = hotNow && pressed == id;
        COLORREF fill = primary ? (enabled ? t.accent : Mix(t.accent, t.layer, 55)) : (down ? t.controlFillPressed : hotNow ? t.controlFillHover : t.controlFill);
        if (primary && enabled && hotNow)
            fill = Mix(t.accent, t.ink, down ? 18 : 10);
        FillRound(hdc, r, S(4), fill);
        if (!primary)
            FrameRound(hdc, r, S(4), t.controlStroke);
        const COLORREF ink = primary ? t.textOnAccent : t.ink;
        DrawLabel(hdc, t.ui, text, r, enabled ? ink : Mix(ink, fill, 45), DT_CENTER);
        FocusRing(hdc, id, r, t.dialogFooter);
    }
    bool Keyboard(int stop) const { return ring && focusStop == stop && GetFocus() == hwnd; }
    void FocusRing(HDC hdc, int stop, const RECT& r, COLORREF under) const
    {
        if (Keyboard(stop))
        {
            const RECT c = Client();
            DrawFocusVisual(hdc, *theme, r, S(4), under, &c);
        }
    }
    void Paint(HDC hdc, const RECT& rc)
    {
        const Theme& t = *theme;
        const RECT c = Client();
        FillSolid(hdc, rc, t.layer);
        FillSolid(hdc, FootRect(), t.dialogFooter);
        FrameRound(hdc, { 0, 0, c.right, c.bottom }, S(8), t.cardStroke);
        DrawLabel(hdc, title, spec->title, { S(kPad), S(kPad), c.right - S(kPad), S(kPad + kTitle) }, t.ink, DT_END_ELLIPSIS);
        for (size_t i = 0; i < textLines.size(); ++i)
        {
            const int y = TextTop() + static_cast<int>(i) * S(20);
            DrawLabel(hdc, t.ui, textLines[i], { S(kPad), y, c.right - S(kPad), y + S(20) }, t.ink);
        }
        // AddItemDialog's ListView: 50px rows, a 24px icon, header over a gray subheader
        if (!spec->items.empty())
        {
            const RECT list = ListRect();
            const int saved = SaveDC(hdc);
            IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
            for (int i = 0; i < Rows(); ++i)
            {
                const RECT r{ list.left, list.top + i * S(kRow) - scroll, list.right - S(6), list.top + (i + 1) * S(kRow) - scroll };
                if (r.bottom < list.top || r.top > list.bottom)
                    continue;
                const Item& item = spec->items[i];
                const bool listFocus = GetFocus() == hwnd && focusStop == kListStop;
                if (i == hot || i == focusRow)
                    FillRound(hdc, { r.left, r.top + S(2), r.right, r.bottom - S(2) }, S(4), i == focusRow && listFocus ? t.selected : t.hover);
                if (i == focusRow && listFocus)
                    FillRound(hdc, { r.left, r.top + S(16), r.left + S(3), r.bottom - S(16) }, S(2), t.accent);
                const RECT icon{ r.left + S(12), (r.top + r.bottom) / 2 - S(12), r.left + S(36), (r.top + r.bottom) / 2 + S(12) };
                if (item.icon)
                    DrawIconEx(hdc, icon.left, icon.top, item.icon, S(24), S(24), 0, nullptr, DI_NORMAL);
                else if (item.glyph)
                    DrawGlyph(hdc, glyphs, item.glyph, icon, t.ink);
                const int x = icon.right + S(10);
                DrawLabel(hdc, t.ui, item.header, { x, r.top + S(6), r.right - S(8), r.top + S(26) }, t.ink, DT_END_ELLIPSIS);
                DrawLabel(hdc, t.caption, item.subheader, { x, r.top + S(26), r.right - S(8), r.bottom - S(6) }, t.muted, DT_END_ELLIPSIS);
            }
            RestoreDC(hdc, saved);
            bar.Paint(hdc, rc);
        }
        // Text boxes: label above, WinUI TextBox look (accent underline while focused), error line under it
        for (int i = 0; i < static_cast<int>(spec->fields.size()); ++i)
        {
            const Field& f = spec->fields[i];
            const int top = FieldTop(i);
            if (!f.label.empty())
                DrawLabel(hdc, t.ui, f.label, { S(kPad), top, c.right - S(kPad), top + S(kLabel) }, t.ink);
            const RECT box = BoxRect(i);
            if (IsCombo(i))
            {
                // WinUI ComboBox: a button-like box, the selected item and a chevron
                const int id = kComboId + i;
                FillRound(hdc, box, S(4), pressed == id ? t.controlFillPressed : hotButton == id ? t.controlFillHover : t.controlFill);
                FrameRound(hdc, box, S(4), t.controlStroke);
                const int chosen = std::clamp(f.choice, 0, static_cast<int>(f.choices.size()) - 1);
                DrawLabel(hdc, t.ui, f.choices[chosen], { box.left + S(11), box.top, box.right - S(36), box.bottom }, t.ink, DT_END_ELLIPSIS);
                DrawGlyph(hdc, t.iconsSmall, L"\xE70D", { box.right - S(32), box.top, box.right - S(8), box.bottom }, t.muted);
                FocusRing(hdc, id, box, t.layer);
                continue;
            }
            const bool focused = GetFocus() == edits[i];
            FillRound(hdc, box, S(4), focused ? t.field : t.controlFill);
            FrameRound(hdc, box, S(4), f.error.empty() ? t.controlStroke : t.errorStroke);
            if (focused)
                FillRound(hdc, { box.left + S(1), box.bottom - S(2), box.right - S(1), box.bottom }, S(1), f.error.empty() ? t.accent : t.errorStroke);
            if (!f.error.empty())
                DrawLabel(hdc, t.caption, f.error, { S(kPad), box.bottom + S(2), c.right - S(kPad), box.bottom + S(kError) },
                    t.critical, DT_END_ELLIPSIS);
            if (f.browse)
            {
                const RECT b = BrowseRect(i);
                const int id = 100 + i;
                FillRound(hdc, b, S(4), pressed == id ? t.controlFillPressed : hotButton == id ? t.controlFillHover : t.controlFill);
                FrameRound(hdc, b, S(4), t.controlStroke);
                DrawLabel(hdc, t.ui, Tr(L"浏览"), b, t.ink, DT_CENTER);
                FocusRing(hdc, id, b, t.layer);
            }
        }
        // WinUI CheckBox: 20px rounded box (accent with a check mark when on), the label beside it
        if (!spec->check.empty())
        {
            const RECT r = CheckRect();
            const RECT box{ r.left, (r.top + r.bottom) / 2 - S(kCheckBox) / 2, r.left + S(kCheckBox), (r.top + r.bottom) / 2 + S(kCheckBox) / 2 };
            const bool hotNow = spec->checkEnabled && hotButton == kCheckId, down = hotNow && pressed == kCheckId;
            if (spec->checked)
            {
                const COLORREF on = spec->checkEnabled ? t.accent : Mix(t.muted, t.layer, 45); // AccentFillColorDisabled
                FillRound(hdc, box, S(4), down ? Mix(on, t.layer, 20) : hotNow ? Mix(on, t.ink, 10) : on);
                DrawGlyph(hdc, t.iconsMid, L"\xE73E", box, t.textOnAccent);
            }
            else
            {
                FillRound(hdc, box, S(4), down ? t.controlFillPressed : hotNow ? t.controlFillHover : t.controlFill);
                FrameRound(hdc, box, S(4), Mix(t.muted, t.layer, 20));
            }
            DrawLabel(hdc, t.ui, spec->check, { box.right + S(8), r.top, r.right, r.bottom }, spec->checkEnabled ? t.ink : Mix(t.ink, t.layer, 55),
                DT_END_ELLIPSIS);
            if (GetFocus() == hwnd && focusStop == kCheckId)
                FrameRound(hdc, { r.left - S(3), r.top + S(2), r.right, r.bottom - S(2) }, S(4), t.ink);
        }
        if (!spec->primary.empty())
            Button(hdc, IDOK, spec->primary, true);
        Button(hdc, IDCANCEL, spec->close, false);
    }
    // --- Layout ---
    void Measure()
    {
        HDC hdc = GetDC(hwnd);
        textLines = spec->text.empty() ? std::vector<std::wstring>() : Wrap(hdc, theme->ui, spec->text, S(kW - 2 * kPad));
        ReleaseDC(hwnd, hdc);
    }
    void Place(bool center)
    {
        const int w = S(kW), h = ContentHeight() + S(kFoot);
        RECT frame{};
        GetWindowRect(owner, &frame);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
        RECT now{};
        GetWindowRect(hwnd, &now);
        int x = center ? (frame.left + frame.right - w) / 2 : now.left;
        int y = center ? (frame.top + frame.bottom - h) / 2 : now.top + ((now.bottom - now.top) - h) / 2;
        x = std::clamp(x, static_cast<int>(mi.rcWork.left), std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - w));
        y = std::clamp(y, static_cast<int>(mi.rcWork.top), std::max(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - h));
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, S(16), S(16));
        SetWindowRgn(hwnd, region, TRUE);
        for (int i = 0; i < static_cast<int>(edits.size()); ++i)
        {
            if (!edits[i])
                continue; // drop-down: drawn, no child window
            const RECT b = BoxRect(i);
            const int eh = S(20);
            SetWindowPos(edits[i], nullptr, b.left + S(10), (b.top + b.bottom - eh) / 2, b.right - b.left - S(20), eh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ScrollTo(scroll); // the list's rows may have changed (poll): clamped, the bar follows
    }
    // --- State ---
    std::wstring EditText(HWND edit) const
    {
        std::wstring s(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
        s.resize(GetWindowTextW(edit, s.data(), static_cast<int>(s.size())));
        return s;
    }
    void Changed(int field)
    {
        if (syncing)
            return;
        if (field >= 0 && edits[field])
            spec->fields[field].text = EditText(edits[field]);
        if (spec->changed)
            spec->changed(*spec, field);
        // The callback may have filled other boxes (the shortcut's name from its target)
        syncing = true;
        for (int i = 0; i < static_cast<int>(edits.size()); ++i)
            if (i != field && edits[i] && EditText(edits[i]) != spec->fields[i].text)
                SetWindowTextW(edits[i], spec->fields[i].text.c_str());
        syncing = false;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void Close(Result r)
    {
        if (r == Primary && !spec->primaryEnabled)
            return;
        for (int i = 0; i < static_cast<int>(edits.size()); ++i)
            if (edits[i])
                spec->fields[i].text = EditText(edits[i]);
        result = r;
        if (owner)
            EnableWindow(owner, TRUE); // before the window goes, so activation returns to the owner
        DestroyWindow(hwnd);
    }
    void Pick(int row)
    {
        if (row < 0 || row >= Rows())
            return;
        spec->selected = row;
        Close(Picked);
    }
    void Press(int id)
    {
        if (id == IDOK)
            Close(Primary);
        else if (id == IDCANCEL)
            Close(Closed);
        else if (id == kCheckId && spec->checkEnabled)
        {
            spec->checked = !spec->checked;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (id >= kComboId && id < kCheckId)
            Drop(id - kComboId);
        else if (id >= 100 && id < 200 && spec->browse)
        {
            const int field = id - 100;
            spec->browse(*spec, field);
            SetWindowTextW(edits[field], spec->fields[field].text.c_str()); // EN_CHANGE -> Changed
            SetFocus(edits[field]);
        }
    }
    // The ComboBox's drop-down: the items in a menu under the box, the selected one checked
    void Drop(int field)
    {
        Field& f = spec->fields[field];
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < static_cast<int>(f.choices.size()); ++i)
            AppendMenuW(menu, MF_STRING | (i == f.choice ? MF_CHECKED : 0), static_cast<UINT_PTR>(i) + 1, f.choices[i].c_str());
        RECT box = BoxRect(field);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&box), 2);
        const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, box.left, box.bottom, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd)
        {
            f.choice = static_cast<int>(cmd) - 1;
            Changed(field);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    // Tab order (WinUI ContentDialog: the content, then the buttons, primary first): the item list, each field (its
    // text box or ComboBox, then its 浏览), the check box, the primary button, the close button. Disabled ones skipped.
    struct Stop { HWND edit; int id; };
    std::vector<Stop> Stops() const
    {
        std::vector<Stop> stops;
        if (!spec->items.empty())
            stops.push_back({ nullptr, kListStop });
        for (int i = 0; i < static_cast<int>(spec->fields.size()); ++i)
        {
            if (IsCombo(i))
                stops.push_back({ nullptr, kComboId + i });
            else if (edits[i])
                stops.push_back({ edits[i], 0 });
            if (spec->fields[i].browse)
                stops.push_back({ nullptr, 100 + i });
        }
        if (!spec->check.empty() && spec->checkEnabled)
            stops.push_back({ nullptr, kCheckId });
        if (!spec->primary.empty() && spec->primaryEnabled)
            stops.push_back({ nullptr, IDOK });
        stops.push_back({ nullptr, IDCANCEL });
        return stops;
    }
    void FocusNext(HWND from, bool back)
    {
        const std::vector<Stop> stops = Stops();
        const int n = static_cast<int>(stops.size());
        int at = -1;
        for (int i = 0; i < n; ++i)
            if (from != hwnd ? stops[i].edit == from : (!stops[i].edit && stops[i].id == focusStop))
                at = i;
        const Stop& next = stops[at < 0 ? (back ? n - 1 : 0) : (at + (back ? n - 1 : 1)) % n];
        ring = true;
        focusStop = next.edit ? 0 : next.id;
        if (next.edit)
        {
            SetFocus(next.edit);
            SendMessageW(next.edit, EM_SETSEL, 0, -1);
        }
        else
            SetFocus(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void EnsureVisible(int row)
    {
        const int h = ListHeight();
        int y = scroll;
        if (row * S(kRow) < scroll)
            y = row * S(kRow);
        else if ((row + 1) * S(kRow) > scroll + h)
            y = (row + 1) * S(kRow) - h;
        if (y == scroll)
            return;
        smoothScroll.Reset(y); // a key jump, not the wheel: goes straight there
        ScrollTo(y);
    }
    void ScrollTo(int y)
    {
        scroll = std::clamp(y, 0, std::max(0, Rows() * S(kRow) - ListHeight()));
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateBar();
    }
    void UpdateBar()
    {
        const RECT list = ListRect();
        const RECT strip{ list.right - S(12), list.top, list.right, list.bottom };
        const int content = Rows() * S(kRow);
        bar.Set(!spec->items.empty() && content > ListHeight() ? content : 0, ListHeight(), scroll, strip, S(kRow));
    }
    // --- Hit testing ---
    HWND FirstEdit() const
    {
        for (HWND e : edits)
            if (e)
                return e;
        return nullptr;
    }
    int RowAt(POINT pt) const
    {
        if (spec->items.empty())
            return -1;
        const RECT list = ListRect();
        if (!PtInRect(&list, pt))
            return -1;
        const int i = (pt.y - list.top + scroll) / S(kRow);
        return i < Rows() ? i : -1;
    }
    int ButtonAt(POINT pt) const
    {
        RECT r = ButtonRect(IDCANCEL);
        if (PtInRect(&r, pt))
            return IDCANCEL;
        r = ButtonRect(IDOK);
        if (!spec->primary.empty() && spec->primaryEnabled && PtInRect(&r, pt))
            return IDOK;
        for (int i = 0; i < static_cast<int>(spec->fields.size()); ++i)
        {
            if (spec->fields[i].browse && (r = BrowseRect(i), PtInRect(&r, pt)))
                return 100 + i;
            if (IsCombo(i) && (r = BoxRect(i), PtInRect(&r, pt)))
                return kComboId + i;
        }
        if (!spec->check.empty() && spec->checkEnabled && (r = CheckRect(), PtInRect(&r, pt)))
            return kCheckId;
        return 0;
    }

    static LRESULT CALLBACK EditProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<Dialog*>(ref);
        if (msg == WM_KEYDOWN)
        {
            if (wp == VK_RETURN) { self->Press(self->spec->defaultClose ? IDCANCEL : IDOK); return 0; }
            if (wp == VK_ESCAPE) { self->Press(IDCANCEL); return 0; }
            if (wp == VK_TAB)
            {
                // Between the boxes and the check box (Shift+Tab backwards); the buttons answer Enter / Esc
                self->FocusNext(w, GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
        }
        if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB))
            return 0; // no beep
        if (msg == WM_SETFOCUS)
            self->focusStop = 0;
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
            InvalidateRect(self->hwnd, nullptr, FALSE);
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
            Paint(paint.dc(), paint.rect());
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        // Title + body as one string: there's no child "static" control to read them from like a MessageBox's, so
        // accessibility tools (and tests) ask the window directly. The title bar itself is never drawn (WS_POPUP).
        case WM_GETTEXTLENGTH:
            return static_cast<LRESULT>(spec->title.size() + (spec->text.empty() ? 0 : 1 + spec->text.size()));
        case WM_GETTEXT:
        {
            const std::wstring all = spec->text.empty() ? spec->title : spec->title + L"\n" + spec->text;
            const size_t n = std::min(static_cast<size_t>(wp), all.size() + 1);
            if (n == 0)
                return 0;
            wcsncpy_s(reinterpret_cast<wchar_t*>(lp), static_cast<size_t>(wp), all.c_str(), n - 1);
            return static_cast<LRESULT>(n - 1);
        }
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd };
            TrackMouseEvent(&tme);
            const bool held = bar.Captured();
            const bool onBar = !pressed && (bar.OnMouseMove(pt, (wp & MK_LBUTTON) != 0) || held);
            const int row = onBar ? -1 : RowAt(pt), button = onBar ? 0 : ButtonAt(pt);
            if (row != hot || button != hotButton)
            {
                hot = row;
                hotButton = button;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            bar.OnMouseLeave();
            hot = -1;
            hotButton = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CAPTURECHANGED:
            bar.OnCaptureLost();
            return 0;
        case WM_LBUTTONDOWN:
            if (bar.Over(pt))
            {
                bar.OnButtonDown(pt);
                return 0;
            }
            pressed = ButtonAt(pt);
            ring = false; // pointer focus: no focus visual
            if (pressed == kCheckId)
            {
                focusStop = kCheckId;
                SetFocus(hwnd);
            }
            if (pressed)
                SetCapture(hwnd);
            else if (const int row = RowAt(pt); row >= 0)
                Pick(row);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        {
            if (bar.OnButtonUp())
                return 0;
            const int id = pressed;
            pressed = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (id && id == ButtonAt(pt))
                Press(id);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (!spec->items.empty())
            {
                const int content = Rows() * S(kRow);
                ScrollTo(smoothScroll.OnWheel(hwnd, GET_WHEEL_DELTA_WPARAM(wp), S(kRow) * 2, std::max(0, content - ListHeight())));
            }
            return 0;
        case SmoothFrame::kMsgTick:
            if (smoothScroll.Animating())
                ScrollTo(smoothScroll.Tick());
            return 0;
        case WM_KEYDOWN: // the window has the keyboard for the list, the check box, the buttons, 浏览 and the ComboBoxes
        {
            const bool onList = focusStop == kListStop || (focusStop == 0 && Rows() > 0);
            if (wp == VK_ESCAPE)
                Press(IDCANCEL);
            else if (wp == VK_TAB)
                FocusNext(hwnd, GetKeyState(VK_SHIFT) < 0);
            else if ((wp == VK_SPACE || wp == VK_RETURN) && (focusStop == IDOK || focusStop == IDCANCEL ||
                         (focusStop >= 100 && focusStop < kCheckId)))
                Press(focusStop); // Button / 浏览 / ComboBox (Enter and Space press it, F4 below opens a ComboBox too)
            else if (wp == VK_F4 && focusStop >= kComboId && focusStop < kCheckId)
                Press(focusStop);
            else if (wp == VK_SPACE && focusStop == kCheckId)
                Press(kCheckId);
            else if (wp == VK_RETURN)
                onList && focusRow >= 0 ? Pick(focusRow) : Press(spec->defaultClose ? IDCANCEL : IDOK);
            else if ((wp == VK_UP || wp == VK_DOWN || wp == VK_HOME || wp == VK_END) && Rows() > 0 && onList)
            {
                focusRow = wp == VK_HOME ? 0 : wp == VK_END ? Rows() - 1 : std::clamp(focusRow + (wp == VK_UP ? -1 : 1), 0, Rows() - 1);
                EnsureVisible(focusRow);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE)
                for (int i = 0; i < static_cast<int>(edits.size()); ++i)
                    if (reinterpret_cast<HWND>(lp) == edits[i])
                        Changed(i);
            if (HIWORD(wp) == 0 && lp == 0 && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
                Press(LOWORD(wp));
            return 0;
        case WM_TIMER:
            if (wp == kBarTimer)
                bar.OnTimer(wp);
            else if (wp == kFadeTimer && smoke)
            {
                const double p = AnimProgress(fadeStart, 160);
                SetLayeredWindowAttributes(smoke, 0, static_cast<BYTE>(kSmokeAlpha * p), LWA_ALPHA);
                if (p >= 1.0)
                    KillTimer(hwnd, kFadeTimer);
            }
            else if (wp == kPollTimer && spec->poll && spec->poll(*spec))
            {
                hot = -1;
                Place(false);
            }
            return 0;
        case WM_CTLCOLOREDIT:
        {
            const bool focused = GetFocus() == reinterpret_cast<HWND>(lp);
            SetTextColor(reinterpret_cast<HDC>(wp), theme->ink);
            SetBkColor(reinterpret_cast<HDC>(wp), focused ? theme->field : theme->controlFill);
            return reinterpret_cast<LRESULT>(focused ? fieldBrush : restBrush);
        }
        case WM_SETFOCUS:
            // Activation (no stop of its own yet): the first text box, else the list
            if (focusStop == 0)
            {
                if (HWND first = FirstEdit())
                    SetFocus(first);
                else if (Rows() > 0)
                    focusStop = kListStop;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            Press(IDCANCEL);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kFadeTimer);
            KillTimer(hwnd, kPollTimer);
            for (HWND e : edits)
                if (e)
                    RemoveWindowSubclass(e, EditProc, 1);
            if (smoke)
                DestroyWindow(smoke);
            smoke = nullptr;
            done = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
        {
            auto* self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = w;
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(w, GWLP_USERDATA)))
            return self->Handle(msg, wp, lp);
        return DefWindowProcW(w, msg, wp, lp);
    }
};

// The dimmed owner (Files' SmokeLayer): eats clicks, never activates
LRESULT CALLBACK SmokeProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATEANDEAT;
    if (msg == WM_ERASEBKGND)
    {
        RECT rc{};
        GetClientRect(w, &rc);
        FillSolid(reinterpret_cast<HDC>(wp), rc, static_cast<COLORREF>(GetWindowLongPtrW(w, GWLP_USERDATA))); // Theme::smoke
        return 1;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

HWND g_open = nullptr;
}

Result Show(HWND owner, const Theme* theme, Spec& spec)
{
    if (g_open)
    {
        SetForegroundWindow(g_open);
        return Closed;
    }
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = Dialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        const bool a = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        WNDCLASSEXW sc{ sizeof(sc) };
        sc.lpfnWndProc = SmokeProc;
        sc.hInstance = GetModuleHandleW(nullptr);
        sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        sc.lpszClassName = kSmokeClass;
        const bool b = RegisterClassExW(&sc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        return a && b;
    }();
    if (!registered)
        return Closed;

    Dialog d;
    d.owner = owner;
    d.theme = theme;
    d.spec = &spec;
    d.title = Resize(theme->ui, theme->S(20), FW_SEMIBOLD);
    d.glyphs = Resize(theme->icons, theme->S(20), FW_NORMAL);
    d.fieldBrush = CreateSolidBrush(theme->field);
    d.restBrush = CreateSolidBrush(theme->controlFill);

    // The dimmed owner first, then the dialog above it
    RECT frame{};
    if (owner && !IsIconic(owner) && GetClientRect(owner, &frame))
    {
        MapWindowPoints(owner, nullptr, reinterpret_cast<POINT*>(&frame), 2);
        d.smoke = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kSmokeClass, L"", WS_POPUP, frame.left, frame.top,
            frame.right - frame.left, frame.bottom - frame.top, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (d.smoke)
        {
            SetWindowLongPtrW(d.smoke, GWLP_USERDATA, static_cast<LONG_PTR>(theme->smoke)); // its color (SmokeProc)
            InvalidateRect(d.smoke, nullptr, TRUE);
            const bool fade = AnimationsEnabled();
            SetLayeredWindowAttributes(d.smoke, 0, fade ? 0 : kSmokeAlpha, LWA_ALPHA);
            ShowWindow(d.smoke, SW_SHOWNOACTIVATE);
        }
    }
    HWND hwnd = CreateWindowExW(0, kClass, spec.title.c_str(), WS_POPUP | WS_CLIPCHILDREN, 0, 0, theme->S(kW), theme->S(200),
        owner, nullptr, GetModuleHandleW(nullptr), &d);
    if (!hwnd)
    {
        if (d.smoke)
            DestroyWindow(d.smoke);
        DeleteObject(d.title);
        DeleteObject(d.glyphs);
        return Closed;
    }
    g_open = hwnd;
    d.bar.Attach(hwnd, true, kBarTimer, theme, [&d](int y) { d.smoothScroll.Reset(y); d.ScrollTo(y); });
    DarkMode::ApplyWindow(hwnd, theme->dark);
    for (int i = 0; i < static_cast<int>(spec.fields.size()); ++i)
    {
        if (!spec.fields[i].choices.empty())
        {
            d.edits.push_back(nullptr); // drop-down: drawn by the dialog
            continue;
        }
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (spec.fields[i].password ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, WC_EDITW, spec.fields[i].text.c_str(), style, 0, 0, 0, 0,
            hwnd, MenuId(200 + i), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(theme->ui), FALSE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        if (!spec.fields[i].placeholder.empty())
            SendMessageW(e, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(spec.fields[i].placeholder.c_str()));
        SetWindowSubclass(e, Dialog::EditProc, 1, reinterpret_cast<DWORD_PTR>(&d));
        d.edits.push_back(e);
    }
    d.Measure();
    d.Changed(-1); // initial validation (Files' UpdateDialogState)
    d.Place(true);
    if (spec.poll)
        SetTimer(hwnd, kPollTimer, 100, nullptr);

    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    if (d.smoke && AnimationsEnabled())
    {
        d.fadeStart = GetTickCount64();
        SetTimer(hwnd, kFadeTimer, 15, nullptr);
    }
    if (HWND first = d.FirstEdit())
    {
        SetFocus(first);
        SendMessageW(first, EM_SETSEL, 0, -1); // Files selects the prefilled name
    }
    else
        SetFocus(hwnd);

    MSG msg;
    bool quit = false;
    int quitCode = 0;
    while (!d.done)
    {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0)
        {
            quit = got == 0;
            quitCode = static_cast<int>(msg.wParam);
            if (IsWindow(hwnd))
                DestroyWindow(hwnd);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (IsWindow(hwnd))
        DestroyWindow(hwnd);
    g_open = nullptr;
    DeleteObject(d.title);
    DeleteObject(d.glyphs);
    DeleteObject(d.fieldBrush);
    DeleteObject(d.restBrush);
    if (quit)
        PostQuitMessage(quitCode);
    return d.result;
}

bool Confirm(HWND owner, const Theme* theme, const std::wstring& title, const std::wstring& text, const std::wstring& primary, const std::wstring& close,
    bool defaultClose)
{
    Spec spec;
    spec.title = title;
    spec.text = text;
    spec.primary = primary;
    spec.close = close;
    spec.defaultClose = defaultClose;
    return Show(owner, theme, spec) == Primary;
}

void Notice(HWND owner, const Theme* theme, const std::wstring& title, const std::wstring& text, const std::wstring& close)
{
    Spec spec; // primary left empty: Show() draws just the one (close) button, like Files' OK-only error ContentDialogs
    spec.title = title;
    spec.text = text;
    spec.close = close;
    Show(owner, theme, spec);
}
}
