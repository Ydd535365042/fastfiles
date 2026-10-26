#include "Ui.h"
#include <dwmapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Setup";
constexpr UINT kMsgProgress = WM_APP + 1, kMsgDone = WM_APP + 2;
constexpr UINT_PTR kTimerFrame = 1;

// GetDpiForWindow is Windows 10: looked up at run time, the system DPI on Windows 7 / 8
UINT WindowDpi(HWND hwnd)
{
    using Fn = UINT(WINAPI*)(HWND);
    static const Fn f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (f)
        return f(hwnd);
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen)
        ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96u;
}
}

int RunUi(Options& o)
{
    Ui ui(o);
    return ui.Run();
}

int Ui::Run()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(kIconId), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(kIconId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    m_previous = m_o.uninstall ? std::wstring() : InstalledVersion();
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    m_animations = animations != FALSE;
    // Test hook only: in preview mode, FASTFILES_SETUP_ANIMATIONS=0 forces the animations-off path (static frames,
    // no splash wait) without ever touching the real SPI_GETCLIENTAREAANIMATION system setting.
    if (m_o.preview)
    {
        wchar_t v[4]{};
        if (GetEnvironmentVariableW(L"FASTFILES_SETUP_ANIMATIONS", v, 4) > 0 && wcscmp(v, L"0") == 0)
            m_animations = false;
    }

    const wchar_t* caption = m_o.uninstall ? L"卸载 FastFiles" : L"FastFiles 安装";
    m_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, caption, WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd)
        return ExitInitFailed;
    // Centered on the monitor with the mouse, sized for its DPI
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    m_dpi = WindowDpi(m_hwnd);
    const RECT& wa = mi.rcWork;
    SetWindowPos(m_hwnd, nullptr, wa.left + (wa.right - wa.left - S(kW)) / 2, wa.top + (wa.bottom - wa.top - S(kH)) / 2, S(kW), S(kH),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    m_dpi = WindowDpi(m_hwnd); // another monitor's DPI once moved there
    SetWindowPos(m_hwnd, nullptr, 0, 0, S(kW), S(kH), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    m_f.Build(m_dpi);
    Theme();
    const MARGINS margins{ 0, 0, 0, 1 }; // keeps the DWM shadow on a frameless window (Windows 11)
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    m_round.SetDpi(m_dpi);
    m_round.Attach(m_hwnd, false);
    Theme();
    if (m_o.silent)
    {
        m_page = Page::Working;
        Start();
    }
    else
    {
        // Launch animation: a short splash (icon + name), then flows into Ready. Animations off: skip straight
        // there (no window is shown and then held back by a wait with SPI_GETCLIENTAREAANIMATION off).
        m_page = m_animations ? Page::Splash : Page::Ready;
        m_pageStart = GetTickCount64();
    }
    Layout();
    m_checkFill.value = m_checkFill.target = Checked() ? 1 : 0;
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(m_hwnd);
    Kick();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (m_worker.joinable())
        m_worker.join();
    if (m_old)
        DeleteObject(m_old);
    m_f.Release();
    return m_exit;
}

void Ui::Theme()
{
    // Test hook only: FASTFILES_SETUP_THEME=light|dark overrides the system theme, so screenshots can be taken in
    // either without changing the real "应用浅色/深色模式" setting. Anything else (unset, "system") reads it normally.
    wchar_t theme[16]{};
    const DWORD n = GetEnvironmentVariableW(L"FASTFILES_SETUP_THEME", theme, 16);
    const bool darkOverride = (n > 0 && CompareStringOrdinal(theme, -1, L"dark", -1, TRUE) == CSTR_EQUAL) ? true
        : (n > 0 && CompareStringOrdinal(theme, -1, L"light", -1, TRUE) == CSTR_EQUAL) ? false : SystemDark();
    m_p = MakePalette(darkOverride);
    const BOOL dark = m_p.dark;
    DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE: shadow/border tint
    m_round.SetLook({ m_p.frame, m_p.frame, m_p.frameAlpha, m_p.frameInactiveAlpha, m_p.shadowAlpha, m_p.shadowInactiveAlpha, m_p.title, m_p.footer });
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Ui::Layout()
{
    const int w = S(kW), h = S(kH), btnH = S(32), footTop = h - S(kFooter);
    for (RECT& r : m_rc)
        r = {};
    m_rc[CtlClose] = { w - S(46), 0, w, S(kTitle) };
    m_rc[CtlMin] = { w - S(92), 0, w - S(46), S(kTitle) };
    // Command area: buttons right-aligned, 8 apart (ContentDialog: 24 padding, 32 high)
    const int by = footTop + (S(kFooter) - btnH) / 2;
    if (Visible(CtlSecondary))
    {
        m_rc[CtlSecondary] = { w - S(24) - S(120), by, w - S(24), by + btnH };
        m_rc[CtlPrimary] = { m_rc[CtlSecondary].left - S(8) - S(120), by, m_rc[CtlSecondary].left - S(8), by + btnH };
    }
    else
        m_rc[CtlPrimary] = { w - S(24) - S(120), by, w - S(24), by + btnH };
    // Ready (install): folder box + 更改 at 188, desktop checkbox at 240. Done: launch checkbox at 216.
    const int left = S(24);
    if (Visible(CtlBrowse))
        m_rc[CtlBrowse] = { w - S(24) - S(96), S(188), w - S(24), S(188) + btnH };
    if (Visible(CtlCheck))
    {
        const int y = m_page == Page::Done ? S(216) : S(240);
        const std::wstring label = m_page == Page::Done ? L"启动 FastFiles" : m_o.uninstall ? L"同时删除我的设置和数据" : L"创建桌面快捷方式";
        HDC hdc = GetDC(m_hwnd);
        const int tw = TextWidth(hdc, m_f.body, label);
        ReleaseDC(m_hwnd, hdc);
        m_rc[CtlCheck] = { left, y, left + S(20) + S(8) + tw + S(4), y + S(32) };
    }
}

bool Ui::Visible(int ctl) const
{
    switch (ctl)
    {
    case CtlMin: case CtlClose: return true;
    case CtlBrowse: return m_page == Page::Ready && !m_o.uninstall;
    case CtlCheck: return m_o.uninstall ? m_page == Page::Ready : (m_page == Page::Ready || m_page == Page::Done);
    case CtlPrimary: return m_page != Page::Working && m_page != Page::Splash;
    case CtlSecondary: return m_page == Page::Ready;
    default: return false;
    }
}

bool Ui::Enabled(int ctl) const
{
    return Visible(ctl) && !(ctl == CtlClose && m_page == Page::Working);
}

int Ui::HitTest(POINT pt) const
{
    for (int i = 0; i < CtlCount; ++i)
        if (Visible(i) && PtInRect(&m_rc[i], pt))
            return i;
    return CtlNone;
}
void Ui::Activate(int ctl)
{
    if (!Enabled(ctl))
        return;
    switch (ctl)
    {
    case CtlMin:
        ShowWindow(m_hwnd, SW_MINIMIZE);
        break;
    case CtlClose:
    case CtlSecondary:
        if (m_page == Page::Ready || m_page == Page::Splash)
            Finish(ExitCancelled);
        else if (m_page == Page::Done)
            Finish(ExitOk);
        else if (m_page == Page::Failed)
            Finish(ExitFailed);
        break;
    case CtlBrowse:
        Browse();
        break;
    case CtlCheck:
        (m_o.uninstall ? m_o.deleteData : m_page == Page::Done ? m_o.launch : m_o.desktopIcon) ^= true;
        m_checkFill.target = Checked() ? 1 : 0;
        Kick();
        break;
    case CtlPrimary:
        if (m_page == Page::Ready)
        {
            Go(Page::Working);
            Start();
        }
        else
            Finish(m_page == Page::Done ? ExitOk : ExitFailed);
        break;
    }
}

void Ui::Finish(int code)
{
    // Like Inno's postinstall entry: start the app as the signed-in user, not elevated. Never in preview: it must
    // not launch the real FastFiles.exe either.
    if (code == ExitOk && !m_o.uninstall && m_o.launch && !m_o.silent && !m_o.preview)
        LaunchAsUser(Join(m_o.dir, kAppExe));
    m_exit = code;
    DestroyWindow(m_hwnd);
}

// Test hook only: FASTFILES_SETUP_PREVIEW's fake steps, timed like RunInstall/RunUninstall's real ones but never
// touching the disk, the registry or a shortcut. Runs on the worker thread exactly like the real one.
void SimulateWork(bool uninstall, const Progress& progress)
{
    const wchar_t* installSteps[] = { L"正在准备…", L"正在复制文件…", L"正在创建快捷方式…", L"正在注册…" };
    const wchar_t* uninstallSteps[] = { L"正在准备…", L"正在删除文件…", L"正在删除快捷方式…", L"正在清理…" };
    const wchar_t* const* steps = uninstall ? uninstallSteps : installSteps;
    const int percents[] = { 15, 45, 70, 90 };
    for (int i = 0; i < 4; ++i)
    {
        progress(percents[i], steps[i]);
        Sleep(380);
    }
    progress(100, L"");
}

void Ui::Start()
{
    m_target = 0;
    m_shown = 0;
    m_finished = false;
    m_status = L"正在准备…";
    m_statusFrom.clear();
    m_statusStart = GetTickCount64();
    HWND hwnd = m_hwnd;
    const Options o = m_o;
    m_worker = std::thread([hwnd, o]
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto progress = [hwnd](int percent, const std::wstring& status)
        {
            PostMessageW(hwnd, kMsgProgress, static_cast<WPARAM>(percent), reinterpret_cast<LPARAM>(new std::wstring(status)));
        };
        std::wstring error;
        bool ok;
        if (o.preview)
        {
            SimulateWork(o.uninstall, progress); // never RunInstall/RunUninstall: no real work happens
            ok = true;
        }
        else
            ok = o.uninstall ? RunUninstall(o, progress, error) : RunInstall(o, progress, error);
        PostMessageW(hwnd, kMsgDone, ok, reinterpret_cast<LPARAM>(new std::wstring(error)));
        CoUninitialize();
    });
}

void Ui::Browse()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择安装位置");
    std::wstring parent = m_o.dir;
    while (!parent.empty() && GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES && parent.find(L'\\') != std::wstring::npos)
        parent.resize(parent.find_last_of(L'\\'));
    ComPtr<IShellItem> start;
    if (!parent.empty() && SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&start))))
        dialog->SetFolder(start.Get());
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->Show(m_hwnd)) && SUCCEEDED(dialog->GetResult(&result)) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
    {
        std::wstring dir = path;
        while (dir.size() > 3 && dir.back() == L'\\')
            dir.pop_back();
        // Inno's AppendDefaultDirName: a chosen parent folder gets "\FastFiles"
        const size_t slash = dir.find_last_of(L'\\');
        const std::wstring leaf = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
        if (CompareStringOrdinal(leaf.c_str(), -1, kAppName, -1, TRUE) != CSTR_EQUAL)
            dir = Join(dir, kAppName);
        m_o.dir = dir;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    CoTaskMemFree(path);
}

void Ui::Go(Page page)
{
    Snapshot();
    m_page = page;
    m_pageStart = GetTickCount64();
    m_hot = m_pressed = CtlNone;
    m_focus = (page == Page::Ready || page == Page::Splash) ? CtlNone : CtlPrimary;
    m_checkFill.value = m_checkFill.target = Checked() ? 1 : 0;
    for (Anim& a : m_hover)
        a.value = a.target = 0;
    Layout();
    Kick();
}

void Ui::SetHot(int ctl)
{
    if (ctl == m_hot)
        return;
    m_hot = ctl;
    for (int i = 0; i < CtlCount; ++i)
        m_hover[i].target = i == ctl && Enabled(i) ? 1 : 0;
    Kick();
}

void Ui::Kick()
{
    if (!m_animating)
    {
        m_animating = true;
        SetTimer(m_hwnd, kTimerFrame, 15, nullptr);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Ui::Animate()
{
    bool busy = false;
    auto ease = [&](double& v, double target, double rate)
    {
        if (!m_animations || std::fabs(target - v) < 0.004)
            v = target;
        else
        {
            v += (target - v) * rate;
            busy = true;
        }
    };
    for (Anim& a : m_hover)
        ease(a.value, a.target, 0.28);
    ease(m_checkFill.value, m_checkFill.target, 0.3);
    const ULONGLONG age = GetTickCount64() - m_pageStart;
    busy |= m_animations && age < kSlideMs + 700; // page slide, badge
    if (m_page == Page::Splash)
    {
        busy = true;
        if (age >= kSplashMs)
            Go(Page::Ready);
    }
    if (m_page == Page::Working)
    {
        ease(m_shown, m_target, 0.12);
        busy = true; // shimmer
        if (m_finished && m_ok && m_shown >= 99.5)
        {
            if (m_o.silent)
            {
                Finish(ExitOk);
                return;
            }
            Go(Page::Done);
        }
    }
    if (!busy)
    {
        KillTimer(m_hwnd, kTimerFrame);
        m_animating = false;
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}
LRESULT CALLBACK Ui::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Ui::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCCALCSIZE:
        return 0; // no system frame: the whole window is client area (DWM still draws the shadow)
    case WM_NCHITTEST:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(m_hwnd, &pt);
        return pt.y < S(kTitle) && HitTest(pt) == CtlNone ? HTCAPTION : HTCLIENT;
    }
    case WM_NCACTIVATE:
        InvalidateRect(m_hwnd, nullptr, FALSE); // title text dims when inactive
        m_round.SetActive(wp != FALSE);
        return DefWindowProcW(m_hwnd, msg, wp, -1);
    case WM_WINDOWPOSCHANGED:
        m_round.Update();
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwnd, &ps);
        Paint(hdc);
        EndPaint(m_hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerFrame)
            Animate();
        return 0;
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        SetHot(HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }));
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHot(CtlNone);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        m_keyboard = false;
        m_pressed = HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (m_pressed != CtlNone)
            SetCapture(m_hwnd);
        Kick();
        return 0;
    case WM_LBUTTONUP:
    {
        const int pressed = m_pressed;
        m_pressed = CtlNone;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        Kick();
        if (pressed != CtlNone && HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }) == pressed)
            Activate(pressed);
        return 0;
    }
    case WM_KEYDOWN:
    {
        // Tab between the page's controls, Enter = focused (or the primary) button, Esc = cancel/close
        const int order[] = { CtlBrowse, CtlCheck, CtlPrimary, CtlSecondary };
        if (wp == VK_TAB)
        {
            const bool back = GetKeyState(VK_SHIFT) < 0;
            int at = -1;
            for (int i = 0; i < 4; ++i)
                if (order[i] == m_focus)
                    at = i;
            for (int step = 0; step < 4; ++step)
            {
                at = at < 0 ? (back ? 3 : 0) : (at + (back ? 3 : 1)) % 4;
                if (Enabled(order[at]))
                {
                    m_focus = order[at];
                    break;
                }
            }
            m_keyboard = true;
            Kick();
        }
        else if (wp == VK_RETURN)
            Activate(m_focus != CtlNone && Enabled(m_focus) ? m_focus : CtlPrimary);
        else if (wp == VK_SPACE && m_focus != CtlNone)
            Activate(m_focus);
        else if (wp == VK_ESCAPE)
            Activate(CtlClose);
        return 0;
    }
    case WM_CLOSE:
        Activate(CtlClose); // Alt+F4, taskbar close: ignored while working
        return 0;
    case kMsgProgress:
    {
        std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lp));
        if (static_cast<int>(wp) >= 0)
            m_target = std::max(m_target, static_cast<int>(wp));
        if (!status->empty() && *status != m_status)
        {
            m_statusFrom = m_status; // cross-fades to the new step's text instead of popping
            m_status = *status;
            m_statusStart = GetTickCount64();
        }
        Kick();
        return 0;
    }
    case kMsgDone:
    {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lp));
        if (m_worker.joinable())
            m_worker.join();
        m_finished = true;
        m_ok = wp != 0;
        m_error = *error;
        if (m_ok)
            m_target = 100; // Animate moves on once the bar has filled
        else
            Go(Page::Failed);
        Kick();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0)
            Theme();
        return 0;
    case WM_DPICHANGED:
    {
        m_dpi = HIWORD(wp);
        m_round.SetDpi(m_dpi);
        m_f.Build(m_dpi);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, S(kW), S(kH), SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        m_round.Detach();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
