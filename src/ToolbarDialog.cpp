// Files' ToolbarCustomizationPage (Views/Settings/ToolbarCustomizationPage.xaml) as a self-drawn window: the toolbar
// context to edit, the available items (by category, searchable), the added items (drag to add / reorder, icon and label
// switches, remove), a live preview, and 恢复默认 / 保存 / 取消.
#include "Toolbar.h"
#include "Lang.h"
#include "DarkMode.h"
#include "SmoothScroll.h"

namespace
{
using namespace ToolbarModel;

constexpr wchar_t kClass[] = L"FastFiles.ToolbarDialog";
// 96-DPI layout: Files sizes the window 760 x 560, the available list 240 wide
constexpr int kW = 760, kH = 560, kPad = 16, kLeftW = 240, kTop = 52, kRowL = 32, kRowR = 36, kHead = 30, kPreview = 52, kFoot = 56;
enum : int { IdSearch = 10, IdPreview = 11, IdReset = 20, IdContextBase = 100 };
constexpr UINT_PTR kSearchTimer = 1;
constexpr wchar_t kGripper[] = L"\xE76F", kAccept[] = L"\xE73E";

struct LeftRow
{
    std::wstring category; // header row when command is null
    const Command* command = nullptr;
};

struct Dialog final : ButtonBarEvents
{
    HWND hwnd = nullptr, owner = nullptr, search = nullptr;
    const Theme* theme = nullptr;
    HFONT bold = nullptr;
    HBRUSH fieldBrush = nullptr;
    ButtonBar preview;
    Lists work, saved;
    int context = Always;
    bool result = false, done = false;

    std::vector<LeftRow> left;
    std::vector<std::wstring> collapsed;
    int leftScroll = 0, rightScroll = 0;
    SmoothScroll leftSmooth, rightSmooth; // each panel's wheel animation
    int leftHot = -1, leftSel = -1, rightHot = -1, rightSel = -1, rightHotPart = 0; // part: 1 icon, 2 label, 3 remove
    int pressed = 0, hotButton = 0;   // IdReset / IDOK / IDCANCEL / IdContextBase (the context picker)
    bool focusRight = false;
    // Dragging: from the available list (dragLeft) or within the added list
    bool dragArmed = false, dragging = false, dragLeft = false;
    int dragIndex = -1, dropIndex = -1;
    POINT dragFrom{}, dragAt{};

    void OnBarDropdown(int, const RECT&) override {} // the preview doesn't open menus
    int S(int v) const { return theme->S(v); }
    // --- Geometry (client coordinates) ---
    RECT Client() const { RECT rc{}; GetClientRect(hwnd, &rc); return rc; }
    RECT ContextBox() const { return { S(kPad) + S(220), S(12), S(kPad) + S(220 + 240), S(12 + 32) }; }
    RECT LeftPanel() const { const RECT c = Client(); return { S(kPad), S(kTop), S(kPad + kLeftW), c.bottom - S(kFoot + kPreview + 8) }; }
    RECT RightPanel() const { const RECT c = Client(); return { S(kPad + kLeftW + 12), S(kTop), c.right - S(kPad), c.bottom - S(kFoot + kPreview + 8) }; }
    RECT SearchRect() const { RECT p = LeftPanel(); return { p.left + S(8), p.top + S(kHead + 2), p.right - S(8), p.top + S(kHead + 34) }; }
    RECT LeftList() const { RECT p = LeftPanel(); return { p.left + 1, p.top + S(kHead + 40), p.right - 1, p.bottom - 1 }; }
    RECT RightList() const { RECT p = RightPanel(); return { p.left + 1, p.top + S(kHead + 30), p.right - 1, p.bottom - 1 }; }
    RECT PreviewRect() const { const RECT c = Client(); return { S(kPad), c.bottom - S(kFoot + kPreview), c.right - S(kPad), c.bottom - S(kFoot) }; }
    RECT ButtonRect(int id) const
    {
        const RECT c = Client();
        const int y = c.bottom - S(kFoot) + S(12), h = S(32);
        switch (id)
        {
        case IdReset: return { S(kPad), y, S(kPad + 96), y + h };
        case IDOK: return { c.right - S(kPad + 96 + 8 + 96), y, c.right - S(kPad + 96 + 8), y + h };
        default: return { c.right - S(kPad + 96), y, c.right - S(kPad), y + h };
        }
    }
    // Added list columns: gripper | name | 图标 | 文字 | remove
    int ColIcon() const { return RightList().right - S(36 + 64 + 64); }
    int ColLabel() const { return RightList().right - S(36 + 64); }
    int ColRemove() const { return RightList().right - S(36); }

    std::vector<Entry>& Items() { return work[context]; }
    bool Changed() const { return work != saved; }

    // --- Available items: Files' tree by category, flat while searching ---
    std::wstring Query() const
    {
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(search)) + 1, L'\0');
        text.resize(GetWindowTextW(search, text.data(), static_cast<int>(text.size())));
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    }
    static std::wstring DisplayName(const Command& c) { return c.group ? std::wstring(c.name) + L" ..." : std::wstring(c.name); }
    void BuildLeft()
    {
        left.clear();
        const std::wstring q = Query();
        std::vector<std::wstring> categories;
        for (const Command& c : Catalog())
            if (c.category && std::find(categories.begin(), categories.end(), c.category) == categories.end())
                categories.push_back(c.category);
        for (const Command& c : Catalog())
            if (!c.category)
                if (q.empty() || [&] { std::wstring n = c.name; CharLowerBuffW(n.data(), static_cast<DWORD>(n.size())); return n.find(q) != std::wstring::npos; }())
                    left.push_back({ L"", &c });
        for (const auto& cat : categories)
        {
            const bool open = std::find(collapsed.begin(), collapsed.end(), cat) == collapsed.end();
            const size_t header = left.size();
            if (q.empty())
                left.push_back({ cat, nullptr });
            for (const Command& c : Catalog())
            {
                if (!c.category || cat != c.category)
                    continue;
                std::wstring n = c.name;
                CharLowerBuffW(n.data(), static_cast<DWORD>(n.size()));
                if (!q.empty() ? n.find(q) != std::wstring::npos : open)
                    left.push_back({ L"", &c });
            }
            (void)header;
        }
        leftSel = left.empty() ? -1 : std::clamp(leftSel, -1, static_cast<int>(left.size()) - 1);
        leftScroll = 0;
        leftSmooth.Reset(0);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    // --- Editing the added list ---
    void Touch(bool previewToo)
    {
        if (previewToo)
            UpdatePreview();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    // Files' AddItem: a copy at the drop position, shown with its label (separators have neither)
    void Add(const Command& c, int at)
    {
        Entry e{ c.key, c.glyph != nullptr, true };
        if (IsSeparator(e.key))
            e.icon = e.label = false;
        auto& items = Items();
        at = std::clamp(at, 0, static_cast<int>(items.size()));
        items.insert(items.begin() + at, e);
        rightSel = at;
        focusRight = true;
        EnsureRightVisible(at);
        Touch(true);
    }
    void Remove(int index)
    {
        auto& items = Items();
        if (index < 0 || index >= static_cast<int>(items.size()))
            return;
        items.erase(items.begin() + index);
        rightSel = items.empty() ? -1 : std::min(index, static_cast<int>(items.size()) - 1);
        Touch(true);
    }
    void Move(int from, int to) // to: insertion index in the list before the move
    {
        auto& items = Items();
        if (from < 0 || from >= static_cast<int>(items.size()))
            return;
        to = std::clamp(to, 0, static_cast<int>(items.size()));
        if (to == from || to == from + 1)
            return;
        Entry e = items[from];
        items.erase(items.begin() + from);
        if (to > from)
            --to;
        items.insert(items.begin() + to, e);
        rightSel = to;
        EnsureRightVisible(to);
        Touch(true);
    }
    void SetContext(int c)
    {
        context = std::clamp(c, 0, ContextCount - 1);
        rightSel = rightHot = -1;
        rightScroll = 0;
        rightSmooth.Reset(0);
        Touch(true);
    }
    void UpdatePreview()
    {
        // Files previews the selected context; "always" items lead like on the real bar
        std::vector<ButtonBar::Item> items = BarItems(work[Always], 1000, nullptr);
        if (context != Always)
        {
            if (!items.empty())
                items.push_back({ 0 });
            for (auto& item : BarItems(work[context], 2000, nullptr))
                items.push_back(std::move(item));
        }
        preview.SetItems(std::move(items));
    }
    void EnsureRightVisible(int index)
    {
        const RECT list = RightList();
        const int h = list.bottom - list.top, top = index * S(kRowR);
        if (top < rightScroll)
            rightScroll = top;
        else if (top + S(kRowR) > rightScroll + h)
            rightScroll = top + S(kRowR) - h;
        rightSmooth.Reset(rightScroll); // a key / drop jump, not the wheel: goes straight there
    }
    void EnsureLeftVisible(int index)
    {
        const RECT list = LeftList();
        const int h = list.bottom - list.top, top = index * S(kRowL);
        if (top < leftScroll)
            leftScroll = top;
        else if (top + S(kRowL) > leftScroll + h)
            leftScroll = top + S(kRowL) - h;
        leftSmooth.Reset(leftScroll);
    }
    void ScrollBy(bool right, int delta)
    {
        const RECT list = right ? RightList() : LeftList();
        const int content = (right ? static_cast<int>(Items().size()) * S(kRowR) : static_cast<int>(left.size()) * S(kRowL));
        int& scroll = right ? rightScroll : leftScroll;
        scroll = std::clamp(scroll + delta, 0, std::max(0, content - static_cast<int>(list.bottom - list.top)));
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    // The wheel: Files' ScrollViewer animation (ScrollBy itself stays immediate: drag auto-scroll, re-clamps)
    void ScrollWheel(bool right, int wheelDelta)
    {
        const RECT list = right ? RightList() : LeftList();
        const int content = (right ? static_cast<int>(Items().size()) * S(kRowR) : static_cast<int>(left.size()) * S(kRowL));
        const int max = std::max(0, content - static_cast<int>(list.bottom - list.top));
        int& scroll = right ? rightScroll : leftScroll;
        scroll = (right ? rightSmooth : leftSmooth).OnWheel(hwnd, wheelDelta, S(kRowR) * 3, max);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void TickScroll()
    {
        if (leftSmooth.Animating())
        {
            leftScroll = leftSmooth.Tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (rightSmooth.Animating())
        {
            rightScroll = rightSmooth.Tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    // --- Painting ---
    void Check(HDC hdc, RECT box, bool on, bool enabled, bool hot) const
    {
        const Theme& t = *theme;
        const int d = S(18);
        const RECT r{ (box.left + box.right - d) / 2, (box.top + box.bottom - d) / 2, (box.left + box.right + d) / 2, (box.top + box.bottom + d) / 2 };
        const COLORREF accent = enabled ? t.accent : Mix(t.muted, t.card, 50);
        if (on)
        {
            FillRound(hdc, r, S(4), hot ? Mix(accent, t.ink, 12) : accent);
            DrawGlyph(hdc, t.iconsSmall, kAccept, r, t.textOnAccent);
        }
        else
        {
            FillRound(hdc, r, S(4), hot ? t.hover : t.card);
            FrameRound(hdc, r, S(4), enabled ? t.muted : Mix(t.muted, t.card, 50));
        }
    }
    void Button(HDC hdc, int id, const wchar_t* text, bool primary) const
    {
        const Theme& t = *theme;
        const RECT r = ButtonRect(id);
        const bool enabled = id != IDOK || Changed();
        const bool hot = enabled && hotButton == id, down = enabled && pressed == id && hot;
        COLORREF fill = primary ? (enabled ? t.accent : Mix(t.accent, t.layer, 55)) : (down ? t.controlFillPressed : hot ? t.controlFillHover : t.controlFill);
        if (primary && enabled && (hot || down))
            fill = Mix(t.accent, t.ink, down ? 18 : 10);
        FillRound(hdc, r, S(4), fill);
        if (!primary)
            FrameRound(hdc, r, S(4), t.controlStroke);
        const COLORREF ink = primary ? t.textOnAccent : enabled ? t.ink : t.muted;
        DrawLabel(hdc, t.ui, text, r, ink, DT_CENTER);
    }
    void Paint(HDC hdc, const RECT& rc)
    {
        const Theme& t = *theme;
        FillSolid(hdc, rc, t.layer);
        // Context picker (Files' ComboBox under 选择希望自定义的工具栏模式)
        DrawLabel(hdc, t.ui, Tr(L"选择希望自定义的工具栏模式"), { S(kPad), S(12), S(kPad + 212), S(44) }, t.ink);
        const RECT box = ContextBox();
        FillRound(hdc, box, S(4), pressed == IdContextBase ? t.controlFillPressed : hotButton == IdContextBase ? t.controlFillHover : t.controlFill);
        FrameRound(hdc, box, S(4), t.controlStroke);
        DrawLabel(hdc, t.ui, ContextName(context), { box.left + S(12), box.top, box.right - S(32), box.bottom }, t.ink);
        DrawGlyph(hdc, t.iconsSmall, Glyph::ChevronDown, { box.right - S(30), box.top, box.right - S(10), box.bottom }, t.muted);

        PaintLeft(hdc);
        PaintRight(hdc);

        // Preview card
        const RECT pv = PreviewRect();
        FillRound(hdc, pv, S(8), t.toolbar);
        FrameRound(hdc, pv, S(8), t.cardEdge);

        Button(hdc, IdReset, Tr(L"恢复默认"), false);
        Button(hdc, IDOK, Tr(L"保存"), true);
        Button(hdc, IDCANCEL, Tr(L"取消"), false);

        // Drag ghost
        if (dragging && dragIndex >= 0)
        {
            const Command* c = nullptr;
            if (dragLeft && dragIndex < static_cast<int>(left.size()))
                c = left[dragIndex].command;
            else if (!dragLeft && dragIndex < static_cast<int>(Items().size()))
                c = Find(Items()[dragIndex].key);
            if (c)
            {
                const std::wstring name = IsSeparator(c->key) ? Tr(L"分隔符") : DisplayName(*c);
                const int w = TextWidth(hdc, t.ui, name) + S(c->glyph ? 48 : 24);
                const RECT g{ dragAt.x + S(10), dragAt.y - S(14), dragAt.x + S(10) + w, dragAt.y + S(14) };
                FillRound(hdc, g, S(6), t.card);
                FrameRound(hdc, g, S(6), t.accent);
                int x = g.left + S(12);
                if (c->glyph)
                {
                    DrawGlyph(hdc, t.icons, c->glyph, { x, g.top, x + S(16), g.bottom }, t.ink);
                    x += S(24);
                }
                DrawLabel(hdc, t.ui, name, { x, g.top, g.right - S(8), g.bottom }, t.ink);
            }
        }
    }
    void Panel(HDC hdc, const RECT& p, const wchar_t* title) const
    {
        const Theme& t = *theme;
        FillRound(hdc, p, S(8), t.card);
        FrameRound(hdc, p, S(8), t.cardStroke);
        DrawLabel(hdc, bold, title, { p.left + S(12), p.top, p.right - S(12), p.top + S(kHead) + S(4) }, t.ink);
    }
    void Thumb(HDC hdc, const RECT& list, int content, int scroll) const
    {
        const int h = list.bottom - list.top;
        if (content <= h || h <= 0)
            return;
        const int th = std::max(S(24), MulDiv(h, h, content));
        const int top = list.top + MulDiv(h - th, scroll, content - h);
        FillRound(hdc, { list.right - S(5), top, list.right - S(2), top + th }, S(2), Mix(theme->muted, theme->card, 40));
    }
    void PaintLeft(HDC hdc)
    {
        const Theme& t = *theme;
        Panel(hdc, LeftPanel(), Tr(L"可用项目"));
        const RECT sr = SearchRect();
        FillRound(hdc, sr, S(4), t.field);
        FrameRound(hdc, sr, S(4), GetFocus() == search ? t.accent : t.fieldStroke);
        const RECT list = LeftList();
        const int dc = SaveDC(hdc);
        IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
        const bool searching = !Query().empty();
        for (int i = 0; i < static_cast<int>(left.size()); ++i)
        {
            const RECT r{ list.left + S(4), list.top + i * S(kRowL) - leftScroll, list.right - S(6), list.top + (i + 1) * S(kRowL) - leftScroll };
            if (r.bottom < list.top || r.top > list.bottom)
                continue;
            const LeftRow& row = left[i];
            const bool sel = i == leftSel && !focusRight;
            if (sel || i == leftHot)
                FillRound(hdc, { r.left, r.top + S(2), r.right, r.bottom - S(2) }, S(4), sel ? (GetFocus() == hwnd ? t.selected : t.hover) : t.hover);
            if (!row.command)
            {
                const bool open = std::find(collapsed.begin(), collapsed.end(), row.category) == collapsed.end();
                DrawGlyph(hdc, t.iconsSmall, open ? Glyph::ChevronDown : Glyph::ChevronRight, { r.left + S(4), r.top, r.left + S(20), r.bottom }, t.muted);
                DrawLabel(hdc, bold, row.category, { r.left + S(24), r.top, r.right - S(4), r.bottom }, t.ink);
                continue;
            }
            const int indent = row.command->category && !searching ? S(20) : S(4);
            int x = r.left + indent;
            if (row.command->glyph)
                DrawGlyph(hdc, t.icons, row.command->glyph, { x, r.top, x + S(16), r.bottom }, t.ink);
            x += S(26);
            DrawLabel(hdc, t.ui, IsSeparator(row.command->key) ? std::wstring(Tr(L"分隔符")) : DisplayName(*row.command), { x, r.top, r.right - S(4), r.bottom }, t.ink);
        }
        RestoreDC(hdc, dc);
        Thumb(hdc, list, static_cast<int>(left.size()) * S(kRowL), leftScroll);
    }
    void PaintRight(HDC hdc)
    {
        const Theme& t = *theme;
        const RECT panel = RightPanel();
        Panel(hdc, panel, Tr(L"已添加的项目"));
        // Column headers (Files: 操作 / 图标 / 文字)
        const int hy = panel.top + S(kHead);
        DrawLabel(hdc, t.caption, Tr(L"操作"), { panel.left + S(40), hy, ColIcon(), hy + S(26) }, t.muted);
        DrawLabel(hdc, t.caption, Tr(L"图标"), { ColIcon(), hy, ColLabel(), hy + S(26) }, t.muted, DT_CENTER);
        DrawLabel(hdc, t.caption, Tr(L"文字"), { ColLabel(), hy, ColRemove(), hy + S(26) }, t.muted, DT_CENTER);
        FillSolid(hdc, { panel.left + 1, hy + S(28), panel.right - 1, hy + S(28) + std::max(1, S(1)) }, t.cardStroke);
        const RECT list = RightList();
        const auto& items = Items();
        const int dc = SaveDC(hdc);
        IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
        {
            const RECT r{ list.left + S(4), list.top + i * S(kRowR) - rightScroll, list.right - S(6), list.top + (i + 1) * S(kRowR) - rightScroll };
            if (r.bottom < list.top || r.top > list.bottom)
                continue;
            const Entry& e = items[i];
            const Command* c = Find(e.key);
            const bool sel = i == rightSel && focusRight;
            if (sel || (i == rightHot && rightHotPart == 0))
                FillRound(hdc, { r.left, r.top + S(2), r.right, r.bottom - S(2) }, S(4), sel ? (GetFocus() == hwnd ? t.selected : t.hover) : t.hover);
            DrawGlyph(hdc, t.iconsSmall, kGripper, { r.left + S(6), r.top, r.left + S(22), r.bottom }, t.muted);
            int x = r.left + S(30);
            if (c && c->glyph && !IsSeparator(e.key))
            {
                DrawGlyph(hdc, t.icons, c->glyph, { x, r.top, x + S(16), r.bottom }, t.ink);
                x += S(26);
            }
            const std::wstring name = IsSeparator(e.key) ? std::wstring(Tr(L"分隔符")) : c ? DisplayName(*c) : e.key;
            DrawLabel(hdc, t.ui, name, { x, r.top, ColIcon() - S(4), r.bottom }, t.ink, DT_END_ELLIPSIS);
            if (!IsSeparator(e.key))
            {
                const bool hasIcon = c && c->glyph;
                Check(hdc, { ColIcon(), r.top, ColLabel(), r.bottom }, e.icon && hasIcon, hasIcon, i == rightHot && rightHotPart == 1);
                Check(hdc, { ColLabel(), r.top, ColRemove(), r.bottom }, e.label, true, i == rightHot && rightHotPart == 2);
            }
            const RECT rm{ ColRemove() + S(4), r.top + S(6), ColRemove() + S(30), r.bottom - S(6) };
            if (i == rightHot && rightHotPart == 3)
                FillRound(hdc, rm, S(4), t.hover);
            DrawGlyph(hdc, t.iconsSmall, Glyph::Close, rm, t.muted);
        }
        // Insertion line while dragging
        if (dragging && dropIndex >= 0)
        {
            const int y = list.top + dropIndex * S(kRowR) - rightScroll;
            FillRound(hdc, { list.left + S(8), y - S(1), list.right - S(10), y + S(2) }, S(1), t.accent);
        }
        RestoreDC(hdc, dc);
        Thumb(hdc, list, static_cast<int>(items.size()) * S(kRowR), rightScroll);
    }
    // --- Hit testing ---
    int LeftAt(POINT pt) const
    {
        const RECT list = LeftList();
        if (!PtInRect(&list, pt))
            return -1;
        const int i = (pt.y - list.top + leftScroll) / S(kRowL);
        return i < static_cast<int>(left.size()) ? i : -1;
    }
    int RightAt(POINT pt, int* part) const
    {
        const RECT list = RightList();
        *part = 0;
        if (!PtInRect(&list, pt))
            return -1;
        const int i = (pt.y - list.top + rightScroll) / S(kRowR);
        if (i >= static_cast<int>(work[context].size()))
            return -1;
        const bool separator = IsSeparator(work[context][i].key);
        if (pt.x >= ColRemove())
            *part = 3;
        else if (!separator && pt.x >= ColLabel())
            *part = 2;
        else if (!separator && pt.x >= ColIcon())
            *part = 1;
        return i;
    }
    // Files' AddedItemsList_Drop: before the first row whose middle is below the pointer
    int DropAt(POINT pt) const
    {
        const RECT list = RightList();
        const int n = static_cast<int>(work[context].size());
        for (int i = 0; i < n; ++i)
            if (pt.y < list.top + i * S(kRowR) - rightScroll + S(kRowR) / 2)
                return i;
        return n;
    }
    int ButtonAt(POINT pt) const
    {
        for (int id : { static_cast<int>(IdReset), static_cast<int>(IDOK), static_cast<int>(IDCANCEL) })
        {
            const RECT r = ButtonRect(id);
            if (PtInRect(&r, pt))
                return id;
        }
        const RECT box = ContextBox();
        return PtInRect(&box, pt) ? IdContextBase : 0;
    }
    void Toggle(int index, int part)
    {
        auto& items = Items();
        if (index < 0 || index >= static_cast<int>(items.size()))
            return;
        Entry& e = items[index];
        const Command* c = Find(e.key);
        if (part == 1 && c && c->glyph)
            e.icon = !e.icon;
        else if (part == 2)
            e.label = !e.label;
        Touch(true);
    }
    void ShowContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        for (int c = 0; c < ContextCount; ++c)
            AppendMenuW(menu, MF_STRING | (c == context ? MF_CHECKED : 0), static_cast<UINT_PTR>(IdContextBase) + c, ContextName(c));
        const RECT box = ContextBox();
        POINT pt{ box.left, box.bottom + S(2) };
        ClientToScreen(hwnd, &pt);
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd >= IdContextBase && cmd < IdContextBase + static_cast<int>(ContextCount))
            SetContext(cmd - IdContextBase);
    }
    // Re-enable the owner first so activation goes back to it instead of another application
    void Close()
    {
        if (owner)
            EnableWindow(owner, TRUE);
        DestroyWindow(hwnd);
    }
    void OnButton(int id)
    {
        switch (id)
        {
        case IdReset: // Files: every context back to its defaults; nothing is saved until 保存
            for (int c = 0; c < ContextCount; ++c)
                work[c] = Defaults(c);
            rightSel = -1;
            Touch(true);
            break;
        case IDOK:
            if (!Changed())
                break;
            result = true;
            Close();
            break;
        case IDCANCEL:
            Close();
            break;
        case IdContextBase:
            ShowContextMenu();
            break;
        }
    }
    // First match while searching (Enter in the search box), else the selected row
    const ToolbarModel::Command* LeftCommand(int index) const
    {
        if (index >= 0 && index < static_cast<int>(left.size()))
            return left[index].command;
        return nullptr;
    }
    // --- Mouse ---
    void MouseMove(POINT pt)
    {
        if (dragArmed && !dragging && (std::abs(pt.x - dragFrom.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - dragFrom.y) > GetSystemMetrics(SM_CYDRAG)))
            dragging = true;
        if (dragging)
        {
            dragAt = pt;
            const RECT list = RightList();
            RECT wide = list;
            InflateRect(&wide, S(8), S(8));
            dropIndex = PtInRect(&wide, pt) ? DropAt(pt) : -1;
            // Auto-scroll at the edges of the added list
            if (dropIndex >= 0 && pt.y < list.top + S(12))
                ScrollBy(true, -S(8));
            else if (dropIndex >= 0 && pt.y > list.bottom - S(12))
                ScrollBy(true, S(8));
            if (dropIndex >= 0)
                rightSmooth.Reset(rightScroll); // drag auto-scroll: no wheel animation to fight it
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        int part = 0;
        const int lh = LeftAt(pt), rh = RightAt(pt, &part), bh = ButtonAt(pt);
        if (lh != leftHot || rh != rightHot || part != rightHotPart || bh != hotButton)
        {
            leftHot = lh;
            rightHot = rh;
            rightHotPart = part;
            hotButton = bh;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    void MouseDown(POINT pt)
    {
        SetFocus(hwnd);
        if (const int b = ButtonAt(pt))
        {
            pressed = b;
            if (b == IdContextBase)
            {
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateWindow(hwnd);
                ShowContextMenu();
                pressed = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (const int l = LeftAt(pt); l >= 0)
        {
            focusRight = false;
            leftSel = l;
            if (!left[l].command)
            {
                // Category node: expand / collapse (Files' tree item click)
                auto it = std::find(collapsed.begin(), collapsed.end(), left[l].category);
                if (it == collapsed.end())
                    collapsed.push_back(left[l].category);
                else
                    collapsed.erase(it);
                const int keep = leftScroll;
                BuildLeft();
                leftSel = std::min(l, static_cast<int>(left.size()) - 1);
                leftScroll = keep;
                ScrollBy(false, 0);
                leftSmooth.Reset(leftScroll); // BuildLeft() reset it to 0; this restores the kept position
                return;
            }
            dragArmed = true;
            dragLeft = true;
            dragIndex = l;
            dragFrom = pt;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        int part = 0;
        if (const int r = RightAt(pt, &part); r >= 0)
        {
            focusRight = true;
            rightSel = r;
            if (part == 3)
                Remove(r);
            else if (part == 1 || part == 2)
                Toggle(r, part);
            else
            {
                dragArmed = true;
                dragLeft = false;
                dragIndex = r;
                dragFrom = pt;
                SetCapture(hwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    void MouseUp(POINT pt)
    {
        const int b = pressed;
        const bool wasDragging = dragging;
        const int from = dragIndex, to = dropIndex;
        const bool fromLeft = dragLeft;
        pressed = 0;
        dragArmed = dragging = false;
        dragIndex = dropIndex = -1;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (wasDragging && to >= 0)
        {
            if (fromLeft)
            {
                if (const ToolbarModel::Command* c = LeftCommand(from))
                    Add(*c, to);
            }
            else
                Move(from, to);
            return;
        }
        if (b && b == ButtonAt(pt))
            OnButton(b);
    }
    void DoubleClick(POINT pt)
    {
        if (const int l = LeftAt(pt); l >= 0 && left[l].command)
            Add(*left[l].command, static_cast<int>(Items().size()));
        else if (l >= 0)
            MouseDown(pt); // category: toggles again like a second click
    }
    // --- Keyboard: search box -> available list -> added list -> buttons (Tab), Files' list keys ---
    void Key(WPARAM vk)
    {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (vk == VK_TAB)
        {
            if (GetKeyState(VK_SHIFT) < 0 ? !focusRight : focusRight)
                SetFocus(search);
            else
            {
                focusRight = !focusRight;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return;
        }
        if (vk == VK_ESCAPE)
        {
            OnButton(IDCANCEL);
            return;
        }
        if (focusRight)
        {
            const int n = static_cast<int>(Items().size());
            if ((vk == VK_UP || vk == VK_DOWN) && ctrl && rightSel >= 0)
                Move(rightSel, vk == VK_UP ? rightSel - 1 : rightSel + 2);
            else if ((vk == VK_UP || vk == VK_DOWN) && n > 0)
            {
                rightSel = std::clamp(rightSel + (vk == VK_UP ? -1 : 1), 0, n - 1);
                EnsureRightVisible(rightSel);
            }
            else if (vk == VK_DELETE || vk == VK_RETURN)
                Remove(rightSel);
            else if (vk == VK_SPACE)
                Toggle(rightSel, 2);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        const int n = static_cast<int>(left.size());
        if ((vk == VK_UP || vk == VK_DOWN) && n > 0)
        {
            leftSel = std::clamp(leftSel + (vk == VK_UP ? -1 : 1), 0, n - 1);
            EnsureLeftVisible(leftSel);
        }
        else if ((vk == VK_LEFT || vk == VK_RIGHT) && leftSel >= 0 && !left[leftSel].command)
        {
            const bool isCollapsed = std::find(collapsed.begin(), collapsed.end(), left[leftSel].category) != collapsed.end();
            if (isCollapsed == (vk == VK_RIGHT))
            {
                const std::wstring cat = left[leftSel].category;
                if (isCollapsed)
                    collapsed.erase(std::find(collapsed.begin(), collapsed.end(), cat));
                else
                    collapsed.push_back(cat);
                const int keep = leftSel, scroll = leftScroll;
                BuildLeft();
                leftSel = keep;
                leftScroll = scroll;
            }
        }
        else if (vk == VK_RETURN)
        {
            if (const Command* c = LeftCommand(leftSel))
                Add(*c, static_cast<int>(Items().size()));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void Layout()
    {
        if (!search || !preview.Hwnd()) // WM_SIZE during CreateWindow, before the children exist
            return;
        const RECT sr = SearchRect();
        const int h = S(20);
        SetWindowPos(search, nullptr, sr.left + S(10), (sr.top + sr.bottom - h) / 2, sr.right - sr.left - S(20), h, SWP_NOZORDER | SWP_NOACTIVATE);
        const RECT pv = PreviewRect();
        SetWindowPos(preview.Hwnd(), nullptr, pv.left + S(6), pv.top + S(6), pv.right - pv.left - S(12), pv.bottom - pv.top - S(12),
            SWP_NOZORDER | SWP_NOACTIVATE);
        preview.Relayout();
    }

    static LRESULT CALLBACK SearchProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<Dialog*>(ref);
        if (msg == WM_KEYDOWN)
            switch (wp)
            {
            case VK_RETURN: // Files' search box: the first match goes in
                KillTimer(self->hwnd, kSearchTimer);
                self->BuildLeft();
                for (const LeftRow& row : self->left)
                    if (row.command)
                    {
                        self->Add(*row.command, static_cast<int>(self->Items().size()));
                        break;
                    }
                return 0;
            case VK_DOWN:
            case VK_TAB:
                self->focusRight = GetKeyState(VK_SHIFT) < 0 && wp == VK_TAB;
                if (self->leftSel < 0 && !self->left.empty())
                    self->leftSel = 0;
                SetFocus(self->hwnd);
                return 0;
            case VK_ESCAPE:
                self->OnButton(IDCANCEL);
                return 0;
            }
        if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_TAB || wp == VK_ESCAPE))
            return 0; // no beep
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
        case WM_SIZE:
            Layout();
            return 0;
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd };
            TrackMouseEvent(&tme);
            MouseMove(pt);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!dragging && (leftHot >= 0 || rightHot >= 0 || hotButton))
            {
                leftHot = rightHot = -1;
                rightHotPart = hotButton = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            MouseDown(pt);
            return 0;
        case WM_LBUTTONDBLCLK:
            DoubleClick(pt);
            return 0;
        case WM_LBUTTONUP:
            MouseUp(pt);
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd && (dragArmed || pressed))
            {
                dragArmed = dragging = false;
                dragIndex = dropIndex = -1;
                pressed = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL:
        {
            POINT p = pt;
            ScreenToClient(hwnd, &p);
            const RECT rp = RightPanel();
            ScrollWheel(PtInRect(&rp, p) != FALSE, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case SmoothFrame::kMsgTick:
            TickScroll();
            return 0;
        case WM_KEYDOWN:
            Key(wp);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IdSearch && HIWORD(wp) == EN_CHANGE)
                SetTimer(hwnd, kSearchTimer, 250, nullptr); // Files' 250 ms debounce
            else if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL || LOWORD(wp) == IdReset)
                OnButton(LOWORD(wp));
            return 0; // preview buttons do nothing
        case WM_TIMER:
            if (wp == kSearchTimer)
            {
                KillTimer(hwnd, kSearchTimer);
                BuildLeft();
            }
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), theme->ink);
            SetBkColor(reinterpret_cast<HDC>(wp), theme->field);
            return reinterpret_cast<LRESULT>(fieldBrush);
        case WM_CLOSE:
            OnButton(IDCANCEL);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kSearchTimer);
            RemoveWindowSubclass(search, SearchProc, 1);
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

HWND g_open = nullptr; // one customization window at a time
}

bool ShowToolbarDialog(HWND owner, const Theme* theme, ToolbarModel::Lists& lists, int context)
{
    if (g_open)
    {
        SetForegroundWindow(g_open);
        return false;
    }
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Dialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered)
        return false;

    Dialog d;
    d.owner = owner;
    d.theme = theme;
    d.work = d.saved = lists;
    d.context = std::clamp(context, 0, ContextCount - 1);
    LOGFONTW lf{};
    GetObjectW(theme->ui, sizeof(lf), &lf);
    lf.lfWeight = FW_SEMIBOLD;
    d.bold = CreateFontIndirectW(&lf);
    d.fieldBrush = CreateSolidBrush(theme->field);

    // Files opens the window at the pointer; keep it inside the monitor's work area
    const int w = theme->S(kW), h = theme->S(kH);
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    RECT frame{ 0, 0, w, h };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, theme->dpi);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    const int x = std::clamp(static_cast<int>(cursor.x - fw / 2), static_cast<int>(mi.rcWork.left), static_cast<int>(std::max(mi.rcWork.left, mi.rcWork.right - fw)));
    const int y = std::clamp(static_cast<int>(cursor.y - theme->S(40)), static_cast<int>(mi.rcWork.top), static_cast<int>(std::max(mi.rcWork.top, mi.rcWork.bottom - fh)));

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Tr(L"自定义工具栏"), style, x, y, fw, fh, owner, nullptr, GetModuleHandleW(nullptr), &d);
    if (!hwnd)
    {
        DeleteObject(d.bold);
        DeleteObject(d.fieldBrush);
        return false;
    }
    g_open = hwnd;
    DarkMode::ApplyWindow(hwnd, theme->dark);
    d.search = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, MenuId(IdSearch),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(d.search, WM_SETFONT, reinterpret_cast<WPARAM>(theme->ui), FALSE);
    SendMessageW(d.search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(d.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Tr(L"搜索")));
    SetWindowSubclass(d.search, Dialog::SearchProc, 1, reinterpret_cast<DWORD_PTR>(&d));
    d.preview.Create(hwnd, IdPreview, theme, &d, {}, &theme->toolbar);
    d.BuildLeft();
    d.Layout();
    d.UpdatePreview();

    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(d.search);
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
    // Re-enable before the window goes away so activation returns to the owner (no flash to another app)
    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (IsWindow(hwnd))
        DestroyWindow(hwnd);
    g_open = nullptr;
    DeleteObject(d.bold);
    DeleteObject(d.fieldBrush);
    if (quit)
        PostQuitMessage(quitCode);
    if (d.result)
        lists = d.work;
    return d.result;
}
