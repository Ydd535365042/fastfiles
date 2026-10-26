#include "Tab.h"
#include "Sandbox.h"

// The file view's right-click menu. Files shows its own menu over the shell's items (open in new tab, tags, compress,
// ...). The view (SHELLDLL_DefView) is subclassed and builds the same menu it would: IShellView::GetItemObject gives
// the exact IContextMenu DefView itself uses for the selection or the background (so cut, rename, New, View, Sort,
// Send to and every shell extension behave as in Explorer), FastFiles' items go around it, and picked shell items are
// invoked on it with the view as site. Header right-clicks (the column chooser) and anything unexpected fall back to
// DefView's own handling.

int TabViewMenu::VerbIndex(const wchar_t* verb) const
{
    if (!menu || !shell)
        return -1;
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i)
    {
        const UINT id = GetMenuItemID(menu, i);
        if (id == static_cast<UINT>(-1) || id < kShellFirst || id > kShellLast)
            continue;
        wchar_t text[80]{};
        if (SUCCEEDED(shell->GetCommandString(id - kShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(text), ARRAYSIZE(text))) &&
            _wcsicmp(text, verb) == 0)
            return i;
    }
    return -1;
}

void Tab::HookView()
{
    ComPtr<IShellView> view;
    if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))))
        HookViewWindow(view.Get());
}

LRESULT CALLBACK Tab::ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* self = reinterpret_cast<Tab*>(data);
    switch (msg)
    {
    case WM_CONTEXTMENU:
        if (self->ShowViewMenu(hwnd, reinterpret_cast<HWND>(wp), { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }))
            return 0;
        break;
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR:
        // Owner-drawn shell submenus (Send to, Open with, New) while our menu is up
        if (self->m_viewMenu && (msg != WM_DRAWITEM || reinterpret_cast<DRAWITEMSTRUCT*>(lp)->CtlType == ODT_MENU) &&
            (msg != WM_MEASUREITEM || reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->CtlType == ODT_MENU))
        {
            LRESULT result = 0;
            ComPtr<IContextMenu3> menu3;
            ComPtr<IContextMenu2> menu2;
            if (SUCCEEDED(self->m_viewMenu.As(&menu3)) ? SUCCEEDED(menu3->HandleMenuMsg2(msg, wp, lp, &result))
                : SUCCEEDED(self->m_viewMenu.As(&menu2)) && SUCCEEDED(menu2->HandleMenuMsg(msg, wp, lp)))
                return msg == WM_MENUCHAR ? result : (msg == WM_INITMENUPOPUP ? 0 : TRUE);
        }
        break;
    case WM_ERASEBKGND:
        // Paint the palette's background: the default erase is white, which flashes in dark mode while a folder loads
        if (const Theme* t = self->m_prefs ? self->m_prefs->theme : nullptr)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillSolid(reinterpret_cast<HDC>(wp), rc, t->content);
            return 1;
        }
        break;
    case WM_NOTIFY:
    {
        // Vertical lines between details columns (Settings > 布局, off by default). DefView draws its own faint blue
        // ones in its custom draw whatever the theme class; after it painted, each boundary gets either our line in the
        // palette's colors or the pixels just left of it (keeps selection, hover and the sorted column's tint).
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        if (cd->nmcd.hdr.code == NM_DBLCLK && reinterpret_cast<NMITEMACTIVATE*>(lp)->iItem < 0 && self->m_prefs && self->m_prefs->doubleClickUp)
        {
            // Files' DoubleClickToGoUp: a double click on empty space goes up (posted: going up replaces this view)
            wchar_t cls[32]{};
            GetClassNameW(cd->nmcd.hdr.hwndFrom, cls, ARRAYSIZE(cls));
            if (wcscmp(cls, WC_LISTVIEWW) == 0)
                PostMessageW(self->m_host, kMsgUp, 0, 0);
            break;
        }
        if (cd->nmcd.hdr.code != NM_CUSTOMDRAW || !self->m_prefs || !self->m_prefs->theme)
            break;
        const HWND list = cd->nmcd.hdr.hwndFrom;
        wchar_t cls[32]{};
        GetClassNameW(list, cls, ARRAYSIZE(cls));
        if (wcscmp(cls, WC_LISTVIEWW) != 0 || ListView_GetView(list) != LV_VIEW_DETAILS)
            break;
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
            return result | CDRF_NOTIFYPOSTPAINT;
        if (cd->nmcd.dwDrawStage == CDDS_POSTPAINT)
        {
            const Theme& t = *self->m_prefs->theme;
            const HWND header = ListView_GetHeader(list);
            const int count = header ? Header_GetItemCount(header) : 0;
            RECT client{}, hrc{};
            GetClientRect(list, &client);
            if (header && IsWindowVisible(header))
            {
                GetWindowRect(header, &hrc);
                MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&hrc), 2);
                client.top = hrc.bottom;
            }
            const bool lines = self->m_prefs->columnLines;
            const COLORREF color = Mix(t.content, t.ink, t.dark ? 12 : 9);
            for (int i = 0; i < count; ++i)
            {
                RECT item{};
                if (!Header_GetItemRect(header, Header_OrderToIndex(header, i), &item))
                    continue;
                const int x = item.right - 1 + hrc.left; // header coordinates follow the horizontal scroll
                if (x <= client.left || x >= client.right)
                    continue;
                if (lines)
                    FillSolid(cd->nmcd.hdc, { x, client.top, x + 1, client.bottom }, color);
                else
                    for (int dx : { -1, 0 }) // DefView's edge is 2px; both get the pixel left of it
                        BitBlt(cd->nmcd.hdc, x + dx, client.top, 1, client.bottom - client.top, cd->nmcd.hdc, x - 2, client.top, SRCCOPY);
            }
        }
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ViewProc, kSubclassId);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool Tab::ShowViewMenu(HWND viewWindow, HWND from, POINT screen)
{
    if (m_viewMenu)
        return false; // already tracking one
    // The folder view (the column view's columns are FastFiles' own lists: TabList.cpp)
    ComPtr<IShellView> view = ViewForWindow(viewWindow);
    ComPtr<IFolderView> folderView;
    if (!view || FAILED(view.As(&folderView)))
        return false;
    const HWND list = FindWindowExW(viewWindow, nullptr, WC_LISTVIEWW, nullptr);
    if (!list)
        return false;
    // The column header's own menu (choose columns, size to fit) stays DefView's
    if (const HWND header = ListView_GetHeader(list); header && (from == header || IsChild(header, from)))
        return false;
    if (from != list && from != viewWindow)
        return false;

    int selected = 0;
    folderView->ItemCount(SVGIO_SELECTION, &selected);
    const bool keyboard = screen.x == -1 && screen.y == -1;
    if (keyboard)
    {
        // Apps key / Shift+F10: at the focused item, else the top left of the view
        RECT rc{};
        const int focused = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
        if (selected && focused >= 0 && ListView_GetItemRect(list, focused, &rc, LVIR_ICON))
            screen = { rc.left + (rc.right - rc.left) / 2, rc.bottom };
        else
            screen = { 8, 8 };
        ClientToScreen(list, &screen);
    }
    const bool background = selected == 0;

    ComPtr<IContextMenu> shell;
    if (FAILED(view->GetItemObject(background ? SVGIO_BACKGROUND : SVGIO_SELECTION, IID_PPV_ARGS(&shell))))
        return false;
    IUnknown_SetSite(shell.Get(), view.Get());
    HMENU menu = CreatePopupMenu();
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const UINT flags = CMF_NORMAL | CMF_EXPLORE | (background ? 0 : CMF_CANRENAME | CMF_ITEMMENU) | (shift ? CMF_EXTENDEDVERBS : 0);
    if (!menu || FAILED(shell->QueryContextMenu(menu, 0, TabViewMenu::kShellFirst, TabViewMenu::kShellLast, flags)))
    {
        if (menu)
            DestroyMenu(menu);
        IUnknown_SetSite(shell.Get(), nullptr);
        return false;
    }
    TabViewMenu info{ menu, background, shell.Get() };
    m_events->OnTabViewMenu(this, info);

    // The menu loop pumps messages: keep this tab alive even if it's closed meanwhile
    ComPtr<Tab> keep(this);
    m_viewMenu = shell;
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, viewWindow, nullptr);
    m_viewMenu.Reset();
    const bool alive = m_host && IsWindow(viewWindow);

    bool handled = false;
    wchar_t verb[64]{};
    if (alive && cmd >= TabViewMenu::kShellFirst && cmd <= TabViewMenu::kShellLast &&
        FAILED(shell->GetCommandString(cmd - TabViewMenu::kShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
        verb[0] = L'\0';
    // "打开" on one folder: navigate here (like a double click), whatever the folder's open verb points at
    if (alive && !background && (_wcsicmp(verb, L"open") == 0 || _wcsicmp(verb, L"explore") == 0))
        handled = OnDefaultCommand(view.Get()) == S_OK;
    // 粘贴 / 删除: Files runs them itself (status center, its own dialogs)
    if (alive && !handled && (_wcsicmp(verb, L"paste") == 0 || _wcsicmp(verb, L"delete") == 0))
        handled = m_events->OnTabVerb(this, verb, background);
    // Test mode: shell items that would change the system (send to, open with, extensions, delete to the bin...) don't run
    if (alive && !handled && cmd >= TabViewMenu::kShellFirst && cmd <= TabViewMenu::kShellLast && Sandbox::VerbAllowed(verb))
    {
        CMINVOKECOMMANDINFOEX invoke{ sizeof(invoke) };
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
            ((GetKeyState(VK_CONTROL) & 0x8000) ? CMIC_MASK_CONTROL_DOWN : 0) | (shift ? CMIC_MASK_SHIFT_DOWN : 0);
        invoke.hwnd = viewWindow;
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - TabViewMenu::kShellFirst);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - TabViewMenu::kShellFirst);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screen;
        shell->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
    }
    DestroyMenu(menu); // after InvokeCommand (see TabList.cpp's OnListMenu)
    IUnknown_SetSite(shell.Get(), nullptr);
    // Ours run after the shell menu is gone (they may navigate this tab); 0 lets the window free what it attached
    m_events->OnTabViewCommand(this, alive && cmd >= TabViewMenu::kAppFirst && cmd <= TabViewMenu::kAppLast ? cmd : 0);
    return true;
}
