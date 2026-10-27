#include "MainWindow.h"
#include "Backdrop.h"
#include "Sandbox.h"
#include "Share.h"
#include "SmoothScroll.h"
#include "resource.h"

namespace
{
constexpr wchar_t kWindowClass[] = L"FastFiles.MainWindow";
}

bool MainWindow::Create(PCIDLIST_ABSOLUTE startup, bool restoreSession, PCUITEMID_CHILD select)
{
    m_startupSelect.reset(select ? reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(select)) : nullptr);
    m_restoreSession = restoreSession;
    m_settings = LoadSettings();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!RegisterClassExW(&wc))
        return false;

    const UINT dpi = GetDpiForSystem();
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (m_hasStartPosition)
    {
        // Files (MainWindow OnActivated, "tab" with x/y): AppWindow.Move(dx - 100, dy - 16), the cursor over the new
        // window's tab row. Kept on the monitor so the title bar can still be grabbed.
        x = m_startPosition.x - 100;
        y = m_startPosition.y - 16;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromPoint(m_startPosition, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const RECT& work = mi.rcWork;
            x = std::clamp(x, static_cast<int>(work.left) - Scale(1200, dpi) + Scale(160, dpi), static_cast<int>(work.right) - Scale(160, dpi));
            y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - Scale(kTitleRow, dpi));
        }
    }
    m_hwnd = CreateWindowExW(0, kWindowClass, L"FastFiles", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, Scale(1200, dpi), Scale(760, dpi), nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return false;

    if (m_primary)
        SetPropW(m_hwnd, L"FastFiles.Primary", reinterpret_cast<HANDLE>(1));
    RestoreSession(startup);
    SetTimer(m_hwnd, kStatusTimer, 300, nullptr);
    SetTimer(m_hwnd, kUpdateTimer, 8000, nullptr); // Files: after the startup tabs loaded
    UpdateTray();
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::HitTestFrame(POINT screen)
{
    // Computed by hand: DefWindowProc still assumes the caption we removed in WM_NCCALCSIZE
    POINT pt = screen;
    ScreenToClient(m_hwnd, &pt);
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const bool zoomed = IsZoomed(m_hwnd) || m_fullScreen;
    const int frame = GetSystemMetricsForDpi(SM_CYFRAME, m_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, m_dpi);
    const int corner = frame * 2;
    if (!zoomed)
    {
        const bool left = pt.x < 0, right = pt.x >= rc.right, bottom = pt.y >= rc.bottom;
        const bool top = pt.y < frame;
        if (bottom)
            return left || pt.x < corner ? HTBOTTOMLEFT : right || pt.x >= rc.right - corner ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left)
            return pt.y < corner ? HTTOPLEFT : pt.y >= rc.bottom - corner ? HTBOTTOMLEFT : HTLEFT;
        if (right)
            return pt.y < corner ? HTTOPRIGHT : pt.y >= rc.bottom - corner ? HTBOTTOMRIGHT : HTRIGHT;
        if (top)
            return pt.x < corner ? HTTOPLEFT : pt.x >= rc.right - corner ? HTTOPRIGHT : HTTOP;
    }
    else if (pt.x < 0 || pt.x >= rc.right || pt.y >= rc.bottom)
    {
        return HTNOWHERE;
    }
    return pt.y < Scale(kTitleRow, m_dpi) ? HTCAPTION : HTCLIENT;
}

void MainWindow::RepaintMaterial()
{
    InvalidateRect(m_hwnd, nullptr, FALSE);
    EnumChildWindows(m_hwnd, [](HWND child, LPARAM) -> BOOL
        {
            wchar_t cls[32]{};
            GetClassNameW(child, cls, 32);
            if (wcsncmp(cls, L"FastFiles.", 10) == 0 && IsWindowVisible(child))
                InvalidateRect(child, nullptr, FALSE);
            return TRUE;
        }, 0);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    if (LRESULT result = 0; HandleTrayMessage(msg, wp, lp, result))
        return result;
    if (msg == Tags::ChangedMessage())
    {
        OnTagsChanged();
        return 0;
    }
    if (msg == SettingsChangedMessage())
    {
        if (wp == kShellFlagsChanged)
        {
            // Another FastFiles window changed an Explorer setting (no system broadcast reaches us)
            ShellSettingsChanged();
            ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
            return 0;
        }
        // Another FastFiles window saved settings.txt
        m_settings = LoadSettings();
        ApplySettings();
        ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
        return 0;
    }
    if (msg == DebugFrameCountMessage())
        // Test diagnostics only: how many frames SmoothFrame has paced out, process-wide, since startup. A test
        // samples this at high frequency during a wheel scroll to count the distinct frames per second actually
        // delivered (SmoothFrame::DebugFrameCount), instead of assuming the nominal refresh rate was hit.
        return Sandbox::Active() ? static_cast<LRESULT>(SmoothFrame::DebugFrameCount()) : 0;
    // The system New menu is owner-drawn and fills its submenu lazily; it needs these while tracked
    if (m_menuHandler && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR))
    {
        LRESULT result = 0;
        ComPtr<IContextMenu3> menu3;
        if (SUCCEEDED(m_menuHandler.As(&menu3)) ? SUCCEEDED(menu3->HandleMenuMsg2(msg, wp, lp, &result))
                                                 : SUCCEEDED(m_menuHandler->HandleMenuMsg(msg, wp, lp)))
            return msg == WM_MENUCHAR ? result : (msg == WM_INITMENUPOPUP ? 0 : TRUE);
    }
    switch (msg)
    {
    case WM_CREATE:
        m_dpi = GetDpiForWindow(m_hwnd);
        m_round.SetDpi(m_dpi);
        m_round.Attach(m_hwnd, true);
        CreateChildren();
        // Re-run WM_NCCALCSIZE now that the client area extends over the caption
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_NCCALCSIZE:
        if (wp)
        {
            // Keep the standard frame on three sides; the caption and top border become client area (Files/Chrome style)
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            // Rounded by a region (Windows 8 / 10): no frame at all, RoundFrame's window outside takes the resize band
            if (m_round.Custom() && !IsZoomed(m_hwnd) && !m_fullScreen)
                return 0;
            const int top = params->rgrc[0].top;
            const LRESULT r = DefWindowProcW(m_hwnd, msg, wp, lp);
            params->rgrc[0].top = top;
            if (IsZoomed(m_hwnd) && !m_fullScreen)
            {
                const UINT dpi = GetDpiForWindow(m_hwnd);
                params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            }
            return r;
        }
        break;
    case WM_NCHITTEST:
        return HitTestFrame({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
    case WM_WINDOWPOSCHANGED:
        m_round.Update();
        break;
    case WM_NCACTIVATE:
    {
        const bool active = wp != FALSE || m_center.KeepsWindowActive();
        m_strip.SetWindowActive(active);
        m_round.SetActive(active);
        // lParam -1 stops DefWindowProc from repainting a caption we no longer have
        return DefWindowProcW(m_hwnd, msg, active, -1);
    }
    case WM_SETTEXT:
    case WM_SETICON:
        // Rounded by a region (Windows 8 / 10) DWM no longer draws the frame, and DefWindowProc paints the classic caption
        // (title, icon, buttons) straight over the title bar when the text or icon changes: hidden while it updates them
        if (m_round.Custom())
        {
            const LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
            if (style & WS_VISIBLE)
                SetWindowLongPtrW(m_hwnd, GWL_STYLE, style & ~WS_VISIBLE);
            const LRESULT r = DefWindowProcW(m_hwnd, msg, wp, lp);
            if (style & WS_VISIBLE)
            {
                SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
                // (hidden, the system may not have told the taskbar and screen readers about the new title)
                if (msg == WM_SETTEXT)
                    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, m_hwnd, OBJID_WINDOW, CHILDID_SELF);
            }
            return r;
        }
        break;
    case 0x00AE: // WM_NCUAHDRAWCAPTION: uxtheme's caption drawing (same classic caption)
    case 0x00AF: // WM_NCUAHDRAWFRAME
        if (m_round.Custom())
            return 0;
        break;
    case WM_SIZE:
        m_strip.SetMaximized(wp == SIZE_MAXIMIZED || m_fullScreen);
        Layout();
        return 0;
    case WM_DPICHANGED:
    {
        m_dpi = HIWORD(wp);
        m_round.SetDpi(m_dpi);
        UpdateFonts();
        const RECT* rc = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(m_hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO:
        // The compact overlay is 400x350 physical pixels (Files' AppWindow.Resize), below the normal minimum
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = m_compact ? POINT{ Scale(200, m_dpi), Scale(150, m_dpi) }
                                                                      : POINT{ Scale(560, m_dpi), Scale(360, m_dpi) };
        return 0;
    case WM_SYSCOMMAND:
        // A compact overlay presenter can't be maximized (caption double-click and the maximize button included)
        if (m_compact && (wp & 0xFFF0) == SC_MAXIMIZE)
            return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        PaintChrome(paint.dc(), paint.rect());
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), m_theme.ink);
        SetBkColor(reinterpret_cast<HDC>(wp), m_theme.field);
        return reinterpret_cast<LRESULT>(m_contentBrush); // the omnibar/search/filter field color
    case WM_COMMAND:
    {
        const HWND from = reinterpret_cast<HWND>(lp);
        const int code = HIWORD(wp);
        if (from == m_filter && code == EN_CHANGE)
        {
            // Debounced: re-filtering re-enumerates the folder
            SetTimer(m_hwnd, kFilterTimer, 180, nullptr);
        }
        else if (from == m_address && code == EN_CHANGE)
        {
            UpdatePalette(); // ">" turns the omnibar into the command palette
        }
        else if (from == m_search && code == EN_CHANGE && GetFocus() == m_search)
        {
            UpdateSearchSuggest(); // Files' Omnibar_TextChanged (user input only)
        }
        else if ((from == m_address || from == m_search || from == m_filter) && (code == EN_SETFOCUS || code == EN_KILLFOCUS))
        {
            // Focus ring on the omnibar / search / filter frame
            InvalidateRect(m_hwnd, from == m_search ? &m_searchFrame : from == m_filter ? &m_filterFrame : &m_omni, FALSE);
            // Files' search mode: the suggestions open with the focus (previous searches) and close without it
            if (from == m_search)
            {
                if (code == EN_SETFOCUS)
                    UpdateSearchSuggest();
                else
                    m_suggest.Hide();
            }
            if (from == m_address && code == EN_KILLFOCUS)
            {
                if (m_paletteOpen)
                    ClosePalette(false);
                SetEditMode(false);
                UpdateChrome();
            }
        }
        else if (LOWORD(wp) == IdStatus && code == StatusBar::kBranchClick)
        {
            ShowGitMenu();
        }
        else if (LOWORD(wp) == IdStatus && code == StatusBar::kSyncClick)
        {
            ShowGitSyncMenu();
        }
        else if (LOWORD(wp) == IdStatusCenter && from == m_center.Button() && code == StatusCenter::kVisibilityChanged)
        {
            Layout();
        }
        else if (LOWORD(wp) == IdStatusCenter && from == m_center.Button() && code == StatusCenter::kWindowInactive)
        {
            // The flyout lost the activation to another program: the look it kept goes now
            SetLooksActive(false);
            SendMessageW(m_hwnd, WM_NCACTIVATE, FALSE, 0);
        }
        else if (LOWORD(wp) == IdStatusCenter && from == m_center.Button() && code == StatusCenter::kTipShown)
        {
            // Files' ShowStatusCenterTeachingTip: once
            m_settings.statusCenterTip = false;
            SaveAndBroadcast();
        }
        else if (code == BN_CLICKED && lp)
        {
            OnCommand(LOWORD(wp));
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kStatusTimer)
            UpdateStatus();
        else if (wp == kUpdateTimer)
        {
            SetTimer(m_hwnd, kUpdateTimer, 5 * 60 * 60 * 1000, nullptr); // Files' PeriodicallyCheckForUpdatesAsync: 5 hours
            AutoCheckForUpdates();
        }
        else if (wp == kInfoTimer)
            UpdateInfoPane();
        else if (wp == kMoveTimer)
        {
            // Held still while dragging: the material for where it is now
            KillTimer(m_hwnd, kMoveTimer);
            if (Backdrop::Live())
                RepaintMaterial();
        }
        else if (wp == kFilterTimer)
        {
            KillTimer(m_hwnd, kFilterTimer);
            std::wstring text(static_cast<size_t>(GetWindowTextLengthW(m_filter)) + 1, L'\0');
            text.resize(GetWindowTextW(m_filter, text.data(), static_cast<int>(text.size())));
            if (Tab* tab = Active())
                tab->SetFilter(text);
        }
        return 0;
    case Operations::kMsgDone:
        OnOperationDone(static_cast<unsigned>(wp));
        return 0;
    case Operations::kMsgAsk:
        OnOperationAsk(static_cast<unsigned>(wp));
        return 0;
    case WM_APP_DROPFILES:
        OnDroppedFiles(reinterpret_cast<DroppedFiles*>(lp));
        return 0;
    case kMsgGit:
        OnGitResult(reinterpret_cast<GitResult*>(lp));
        return 0;
    case kMsgGitDone:
        OnGitDone(reinterpret_cast<GitRunResult*>(lp));
        return 0;
    case Update::kMsgUpdate:
        OnUpdateResult(reinterpret_cast<Update::Result*>(lp));
        return 0;
    case Share::kMsgReady:
        Share::OnReady(m_hwnd, &m_theme, wp, lp);
        return 0;
    case SearchSuggest::kMsgItems:
        OnSuggestItems(static_cast<unsigned>(wp), reinterpret_cast<std::vector<SearchSuggest::Entry>*>(lp));
        return 0;
    case SearchSuggest::kMsgPick:
        PickSuggestion(static_cast<int>(wp));
        return 0;
    case WM_APP_NAVIGATE:
    {
        UniquePidl pidl(reinterpret_cast<ITEMIDLIST_ABSOLUTE*>(lp));
        switch (static_cast<OpenMode>(wp & 0xFF))
        {
        case OpenMode::NewTab: OpenInNewTab(pidl.get(), (wp & kNavigateActivate) != 0); break;
        case OpenMode::NewWindow: OpenNewWindow(pidl.get()); break;
        case OpenMode::OtherPane: OpenInOtherPane(pidl.get()); break;
        default:
            if (Tab* tab = Active())
                tab->Navigate(pidl.get());
            break;
        }
        return 0;
    }
    case kMsgRecent:
        OnRecentScanned(reinterpret_cast<std::vector<HomeItem>*>(lp));
        return 0;
    case kMsgTagged:
        OnTaggedScanned(reinterpret_cast<std::vector<HomeTagGroup>*>(lp));
        return 0;
    case CommandPalette::kMsgRun:
    {
        const int id = static_cast<int>(wp);
        ClosePalette(true);
        if (id)
            RunAction(id);
        return 0;
    }
    case Backdrop::kMsgReady:
        if (Backdrop::OnReady(wp, lp))
            ApplyTheme(false);
        return 0;
    case Backdrop::kMsgImage:
        if (Backdrop::OnImage(wp, lp))
            ApplyTheme(true);
        return 0;
    case WM_DISPLAYCHANGE:
        Backdrop::Reload(); // monitors moved: the material follows the wallpaper per monitor
        SmoothFrame::NotifyDisplayChange(); // the refresh rate a wheel animation paces to may have changed too
        break;
    case WM_EXITSIZEMOVE:
        m_moving = false;
        KillTimer(m_hwnd, kMoveTimer);
        if (Backdrop::Live())
        {
            ApplyTheme(false); // the palette follows the material's average where the window now is
            RepaintMaterial(); // (and the material exactly where it was dropped)
        }
        break;
    case WM_SETTINGCHANGE:
        // A new wallpaper: the material is made from it
        if (wp == SPI_SETDESKWALLPAPER)
            Backdrop::Reload();
        // Windows switched between light and dark apps (Settings > Personalization > Colors)
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && m_settings.theme == AppSettings::ThemeMode::System)
            ApplyTheme(false);
        // Explorer's hidden-items switch moved: our filter is needed when ours now differs from it
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ShellState") == 0)
            ApplyHiddenSettings();
        break;
    case WM_MOVE:
    case WM_ENTERSIZEMOVE:
        if (m_paletteOpen)
            ClosePalette(false);
        if (msg == WM_ENTERSIZEMOVE)
            m_moving = true;
        // The material is the wallpaper behind the window: our own surfaces repaint for where it now is (a background
        // image moves with the window: nothing to repaint). While it's dragged a repaint costs more than a frame and
        // the blurred material barely changes over a few pixels: the picture moves with the window, and is redone
        // once the window rests a moment (WM_TIMER) or is dropped (WM_EXITSIZEMOVE)
        if (msg == WM_MOVE && Backdrop::Live())
        {
            if (m_moving)
                SetTimer(m_hwnd, kMoveTimer, 120, nullptr); // (restarted by every move)
            else
                RepaintMaterial(); // moved some other way (SetWindowPos, snap, another monitor)
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (OverPaneSplitter(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
        {
            if (TabSlot* slot = ActiveSlot())
                slot->ratio = 500;
            PlacePanes();
            return 0;
        }
        [[fallthrough]];
    case WM_LBUTTONDOWN:
        if (OverPaneSplitter(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
        {
            m_paneDragging = true;
            SetCapture(m_hwnd);
        }
        else if (OverSplitter(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
        {
            m_dragging = true;
            SetCapture(m_hwnd);
        }
        else if (OverInfoSplitter(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
        {
            m_infoDragging = true;
            SetCapture(m_hwnd);
        }
        else if (PtInRect(&m_searchFrame, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }))
        {
            FocusSearch();
        }
        else if (m_filterVisible && PtInRect(&m_filterFrame, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }))
        {
            SetFocus(m_filter);
        }
        else if (PtInRect(&m_omni, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }) && !m_editing)
        {
            FocusAddress();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (m_paneDragging)
        {
            if (TabSlot* slot = ActiveSlot())
            {
                const int gap = Scale(kPaneGap, m_dpi);
                const int span = slot->horizontal ? m_viewRect.bottom - m_viewRect.top - gap : m_viewRect.right - m_viewRect.left - gap;
                const int pos = slot->horizontal ? GET_Y_LPARAM(lp) - m_viewRect.top : GET_X_LPARAM(lp) - m_viewRect.left;
                if (span > 0)
                {
                    slot->ratio = std::clamp(MulDiv(pos - gap / 2, 1000, span), 150, 850);
                    PlacePanes();
                }
            }
        }
        else if (m_dragging)
        {
            m_sidebarWidth = std::clamp(MulDiv(GET_X_LPARAM(lp), 96, static_cast<int>(m_dpi)), 160, 600);
            Layout();
        }
        else if (m_infoDragging)
        {
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            if (m_infoBottom)
                m_infoHeight = std::clamp(MulDiv(rc.bottom - Scale(kCardMargin, m_dpi) - GET_Y_LPARAM(lp), 96, static_cast<int>(m_dpi)), 140, 900);
            else
                m_infoWidth = std::clamp(MulDiv(rc.right - Scale(kCardMargin, m_dpi) - GET_X_LPARAM(lp), 96, static_cast<int>(m_dpi)), 200, 900);
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_dragging || m_infoDragging || m_paneDragging)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        m_dragging = false;
        m_infoDragging = false;
        m_paneDragging = false;
        return 0;
    case WM_SETCURSOR:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(m_hwnd, &pt);
        if (reinterpret_cast<HWND>(wp) == m_hwnd && LOWORD(lp) == HTCLIENT)
        {
            if (OverPaneSplitter(pt.x, pt.y))
            {
                const TabSlot* slot = ActiveSlot();
                SetCursor(LoadCursorW(nullptr, slot && slot->horizontal ? IDC_SIZENS : IDC_SIZEWE));
                return TRUE;
            }
            if (OverSplitter(pt.x, pt.y) || OverInfoSplitter(pt.x, pt.y))
            {
                SetCursor(LoadCursorW(nullptr, OverInfoSplitter(pt.x, pt.y) && m_infoBottom ? IDC_SIZENS : IDC_SIZEWE));
                return TRUE;
            }
            if (PtInRect(&m_searchFrame, pt) || PtInRect(&m_omni, pt) || (m_filterVisible && PtInRect(&m_filterFrame, pt)))
            {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
        }
        break;
    }
    case WM_ACTIVATE:
        // The status center's flyout taking the activation leaves the window's look as it is (Files' flyout is in the window)
        SetLooksActive(LOWORD(wp) != WA_INACTIVE || m_center.KeepsWindowActive());
        if (LOWORD(wp) == WA_INACTIVE && m_paletteOpen)
            ClosePalette(false);
        if (LOWORD(wp) == WA_INACTIVE)
            m_suggest.Hide();
        if (LOWORD(wp) == WA_INACTIVE)
            m_lastFocus = GetFocus();
        else if (m_lastFocus && IsWindow(m_lastFocus) && IsChild(m_hwnd, m_lastFocus))
            SetFocus(m_lastFocus);
        else if (Tab* tab = Active())
            tab->Focus();
        if (LOWORD(wp) != WA_INACTIVE)
        {
            if (Tab* tab = Active(); tab && tab->OnHome())
                EnsureHomeFresh(); // files opened elsewhere show up in Recent
            UpdateGit(true);       // the branch may have been switched in a terminal
        }
        return 0;
    case WM_CLOSE:
        if (!ConfirmCloseWithOperations())
            return 0;
        // Background mode: the primary window goes to the tray instead (tray > 退出 closes for real)
        if (m_primary && m_settings.background && !m_quitting)
        {
            HideToTray();
            return 0;
        }
        // Extra windows (Ctrl+N) don't own the session; only the main one saves it
        if (m_restoreSession)
            SaveSessionState();
        DestroyWindow(m_hwnd);
        return 0;
    case WM_DESTROY:
        m_round.Detach();
        OnDestroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}