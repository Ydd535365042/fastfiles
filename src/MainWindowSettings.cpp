#include "MainWindow.h"
#include "Lang.h"
#include "DarkMode.h"
#include "Session.h"
#include "Sandbox.h"
#include "Backdrop.h"
#include "Format.h"
#include "ColorPickerFlyout.h"
#include "fastfiles_core.h"
#include "ContentDialog.h"

// Settings: what each page shows (Files' Views/Settings/*), how a change is applied, and keeping other windows in sync.

#ifndef FASTFILES_VERSION
#define FASTFILES_VERSION L"dev"
#endif

namespace
{
constexpr wchar_t kMainClass[] = L"FastFiles.MainWindow";

const wchar_t* const kHomeWidgets[] = { L"quick", L"drives", L"network", L"recent" }; // HomeView::Section order

bool CopyText(HWND owner, const std::wstring& text)
{
    if (!text.empty() && Sandbox::CopyText(text))
        return true;
    if (text.empty() || !OpenClipboard(owner))
        return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool ok = false;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok)
            GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::wstring PickFile(HWND owner, bool save, const wchar_t* title)
{
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};
    static const COMDLG_FILTERSPEC types[] = { { Tr(L"FastFiles 设置 (*.txt)"), L"*.txt" }, { Tr(L"所有文件"), L"*.*" } };
    dialog->SetFileTypes(ARRAYSIZE(types), types);
    dialog->SetDefaultExtension(L"txt");
    dialog->SetTitle(title);
    if (save)
        dialog->SetFileName(L"FastFiles-settings.txt");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring PickFolder(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Tr(L"启动时打开的文件夹"));
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}

bool ShellFlag(DWORD mask)
{
    SHELLSTATEW s{};
    SHGetSetSettings(&s, mask, FALSE);
    switch (mask)
    {
    case SSF_SHOWALLOBJECTS: return s.fShowAllObjects;
    case SSF_SHOWEXTENSIONS: return s.fShowExtensions;
    case SSF_SHOWSUPERHIDDEN: return s.fShowSuperHidden;
    case SSF_DOUBLECLICKINWEBVIEW: return s.fDoubleClickInWebView;
    case SSF_NOCONFIRMRECYCLE: return s.fNoConfirmRecycle;
    case SSF_ICONSONLY: return s.fIconsOnly;
    }
    return false;
}

std::wstring WindowsBuild()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{ sizeof(info) };
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&info);
    return std::to_wstring(info.dwMajorVersion) + L"." + std::to_wstring(info.dwMinorVersion) + L"." + std::to_wstring(info.dwBuildNumber);
}
}

UINT MainWindow::SettingsChangedMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"FastFiles.SettingsChanged");
    return msg;
}

UINT MainWindow::DebugFrameCountMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"FastFiles.DebugFrameCount");
    return msg;
}

namespace
{
constexpr wchar_t kExplorerKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
constexpr wchar_t kAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

bool WriteDword(const wchar_t* key, const wchar_t* name, DWORD value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

// The flag's bit in SHELLSTATE's first bitfield word
template <typename Set> DWORD ShellStateBit(Set set)
{
    SHELLSTATEW s{};
    set(s);
    DWORD bits;
    memcpy(&bits, &s, sizeof(bits));
    return bits;
}

// ShellState is a DWORD cbSize followed by SHELLSTATE; SHGetSetSettings keeps these flags in it
bool WriteShellStateBit(DWORD bit, bool on)
{
    BYTE data[64]{};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kExplorerKey, L"ShellState", RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS ||
        size < 8)
        return false;
    DWORD flags;
    memcpy(&flags, data + 4, sizeof(flags));
    flags = on ? flags | bit : flags & ~bit;
    memcpy(data + 4, &flags, sizeof(flags));
    return RegSetKeyValueW(HKEY_CURRENT_USER, kExplorerKey, L"ShellState", REG_BINARY, data, size) == ERROR_SUCCESS;
}
}

// Writes Explorer's value straight to the registry and re-reads it in this process only. SHGetSetSettings(set)
// would broadcast WM_SETTINGCHANGE, and every Explorer window and the desktop redraw on it (the user saw the desktop
// flash). Explorer reads the new value the next time it loads its settings.
void MainWindow::SetShellFlag(DWORD mask, bool on)
{
    if (Sandbox::Active())
        return; // test mode: Explorer's settings stay as they are
    bool written = false;
    switch (mask)
    {
    case SSF_SHOWEXTENSIONS: written = WriteDword(kAdvancedKey, L"HideFileExt", on ? 0 : 1); break;
    case SSF_ICONSONLY: written = WriteDword(kAdvancedKey, L"IconsOnly", on ? 1 : 0); break;
    // The BOOL bitfields are signed 1-bit: -1 is the set bit
    case SSF_NOCONFIRMRECYCLE: written = WriteShellStateBit(ShellStateBit([](SHELLSTATEW& s) { s.fNoConfirmRecycle = -1; }), on); break;
    case SSF_DOUBLECLICKINWEBVIEW: written = WriteShellStateBit(ShellStateBit([](SHELLSTATEW& s) { s.fDoubleClickInWebView = -1; }), on); break;
    default: return;
    }
    SHGetSetSettings(nullptr, 0, TRUE);
    if (!written || ShellFlag(mask) != on)
    {
        // Unexpected registry layout: let the shell write it (this path broadcasts)
        SHELLSTATEW s{};
        SHGetSetSettings(&s, mask, FALSE);
        switch (mask)
        {
        case SSF_SHOWEXTENSIONS: s.fShowExtensions = on; break;
        case SSF_ICONSONLY: s.fIconsOnly = on; break;
        case SSF_NOCONFIRMRECYCLE: s.fNoConfirmRecycle = on; break;
        case SSF_DOUBLECLICKINWEBVIEW: s.fDoubleClickInWebView = on; break;
        }
        SHGetSetSettings(&s, mask, TRUE);
    }
    ShellSettingsChanged();
    // Our other windows (Ctrl+N is a separate process) re-read it too
    struct Ctx { HWND self; UINT msg; } ctx{ m_hwnd, SettingsChangedMessage() };
    EnumWindows([](HWND w, LPARAM lp) -> BOOL
    {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64]{};
        if (w != c->self && GetClassNameW(w, cls, 64) && wcscmp(cls, kMainClass) == 0)
            PostMessageW(w, c->msg, kShellFlagsChanged, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void MainWindow::ShellSettingsChanged()
{
    SHGetSetSettings(nullptr, 0, TRUE); // SHRefreshSettings(): drop this process' cached SHELLSTATE, no broadcast
    RefreshShellViews();
}

// A shell setting changed: our own views re-read it. Nothing is broadcast to the rest of the system (a broadcast makes
// Explorer redraw the desktop and every window; Explorer picks the value up on its own).
void MainWindow::RefreshShellViews()
{
    ForEachPane([](Tab* tab)
    {
        if (tab->OnHome())
            return;
        if (ComPtr<IShellView> view = tab->ShellView())
        {
            HWND hwnd = nullptr;
            if (SUCCEEDED(view->GetWindow(&hwnd)) && hwnd)
                SendMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ShellState"));
        }
        tab->Refresh();
    });
}

// FastFiles' own hidden-item settings vs the system's: views list everything and filter only when they differ
void MainWindow::ApplyHiddenSettings()
{
    SHELLSTATEW s{};
    SHGetSetSettings(&s, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
    const bool systemProtected = s.fShowAllObjects && s.fShowSuperHidden;
    const bool own = m_settings.showHidden != (s.fShowAllObjects != FALSE) ||
        (m_settings.showHidden && m_settings.showProtected != systemProtected);
    const bool changed = m_prefs.showHidden != m_settings.showHidden || m_prefs.showProtected != m_settings.showProtected ||
        m_prefs.ownHiddenFilter != own || m_prefs.showDotFiles != m_settings.showDotFiles;
    m_prefs.showHidden = m_settings.showHidden;
    m_prefs.showProtected = m_settings.showProtected;
    m_prefs.ownHiddenFilter = own;
    m_prefs.showDotFiles = m_settings.showDotFiles; // Files' ShowDotFiles (the views and the sidebar tree)
    if (m_sidebar)
        m_sidebar->SetHiddenFilter(m_settings.showHidden, m_settings.showProtected, m_settings.showDotFiles);
    if (changed)
        ForEachPane([](Tab* tab) { if (!tab->OnHome()) tab->Reload(); });
}

// --- Applying ---

void MainWindow::ApplyTheme(bool force)
{
    bool dark = m_settings.theme == AppSettings::ThemeMode::Dark;
    if (m_settings.theme == AppSettings::ThemeMode::System)
        dark = DarkMode::SystemPrefersDark();
    // Files' AppThemeBackgroundColor ("#AARRGGBB") and AppThemeFontFamily
    const std::wstring& bg = m_settings.themeBackground;
    const uint32_t tint = bg.size() == 9 && bg[0] == L'#' ? static_cast<uint32_t>(wcstoul(bg.c_str() + 1, nullptr, 16)) : 0;
    const bool fontChanged = m_theme.face != m_settings.themeFont;
    // Files' AppThemeBackdropMaterial: the material's average over this window feeds the palette while it is live
    Backdrop::Configure(m_hwnd, m_settings.themeBackdrop, dark, tint);
    Backdrop::SetImage(m_settings.bgImage, m_settings.bgImageFit, m_settings.bgImageVertical, m_settings.bgImageHorizontal, m_settings.bgImageOpacity);
    COLORREF backdrop = CLR_INVALID;
    if (Backdrop::Live())
    {
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        backdrop = Backdrop::Mean(wr);
    }
    if (Backdrop::HasImage())
    {
        // Files' background image is under the translucent layers: the solid page color (or the material) under it
        Theme solid = m_theme;
        solid.tint = tint;
        solid.backdrop = CLR_INVALID;
        solid.SetPalette(dark);
        Backdrop::SetBase(solid.chrome);
        backdrop = Mix(backdrop == CLR_INVALID ? solid.chrome : backdrop, Backdrop::ImageMean(), m_settings.bgImageOpacity);
    }
    // The image's place (fit / alignments) changes no color: still a repaint
    static std::wstring placedAs;
    const std::wstring placement = m_settings.bgImage + L"|" + std::to_wstring(m_settings.bgImageFit) + std::to_wstring(m_settings.bgImageVertical) +
        std::to_wstring(m_settings.bgImageHorizontal) + L"|" + std::to_wstring(Backdrop::HasImage());
    const bool moved = placement != placedAs;
    placedAs = placement;
    if (!force && !moved && m_themeApplied == static_cast<int>(dark) && tint == m_theme.tint && !fontChanged && backdrop == m_theme.backdrop)
        return;
    m_themeApplied = dark;
    m_theme.tint = tint;
    m_theme.backdrop = backdrop;
    if (fontChanged)
    {
        m_theme.face = m_settings.themeFont;
        if (m_theme.ui)
            UpdateFonts();
    }
    DarkMode::SetApp(dark);
    m_theme.SetPalette(dark);
    // The surfaces BufferedPaint swaps for the material: the tab row, the nav row / sidebar, the toolbar card
    Backdrop::SetLayers({ { m_theme.chrome, {}, {} },
        { m_theme.layer, { m_theme.layerFill, 0 }, { m_theme.layerPct, 0 } },
        { m_theme.toolbar, { m_theme.layerFill, m_theme.toolbarFill }, { m_theme.layerPct, m_theme.toolbarPct } } });
    DarkMode::ApplyWindow(m_hwnd, dark);
    const auto alpha = [](int pct) { return pct * 255 / 100; };
    m_round.SetLook({ m_theme.windowBorder, m_theme.windowBorder, alpha(m_theme.windowBorderPct), alpha(m_theme.windowBorderInactivePct),
        alpha(m_theme.windowShadowPct), alpha(m_theme.windowShadowInactivePct), m_theme.chrome, m_theme.layer });
    // DarkMode_CFD: the dark edit theme whose placeholder (cue banner) is light; DarkMode_Explorer keeps it dark gray
    for (HWND w : { m_address, m_search, m_filter })
        DarkMode::ApplyControl(w, dark, L"", L"DarkMode_CFD");
    if (m_contentBrush)
        DeleteObject(m_contentBrush);
    m_contentBrush = CreateSolidBrush(m_theme.field);
    if (m_info)
        m_info->Relayout();
    m_palette.Relayout();
    ForEachPane([](Tab* tab) { tab->ApplyTheme(); tab->RelayoutHome(); });
    // Everything custom-drawn reads the palette when it paints
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_ERASE);
}

void MainWindow::ApplySettings()
{
    ApplyTheme(false);
    ApplyHiddenSettings();
    m_prefs.openFoldersNewTab = m_settings.openFoldersNewTab;
    m_prefs.doubleClickUp = m_settings.doubleClickUp;
    m_prefs.selectPrevious = m_settings.selectPrevious;
    m_prefs.hoverSelect = m_settings.hoverSelect;
    if (m_prefs.sortPriority != m_settings.sortPriority)
    {
        m_prefs.sortPriority = m_settings.sortPriority;
        ForEachPane([](Tab* tab) { tab->ForEachList([](FileList* list) { list->ApplyPrefs(); }); }); // Files re-sorts open folders
    }
    // Files' default sort / grouping: folders opened from now on (synced: every open one too, as Files' live preferences)
    {
        ListSort s;
        s.key = std::clamp(m_settings.defaultSort, 0, static_cast<int>(SortKey::Created));
        s.descending = m_settings.defaultSortDesc;
        s.group = std::clamp(m_settings.defaultGroup, FileList::kGroupNone, FileList::kGroupTag);
        s.unit = std::clamp(m_settings.defaultGroupUnit, 0, 2);
        s.groupDescending = m_settings.defaultGroupDesc;
        if (s != m_prefs.defaultSort)
        {
            m_prefs.defaultSort = s;
            ForEachPane([](Tab* tab) { tab->ApplySortDefault(); });
        }
    }
    // Files' layout sizes: every open list follows
    if (const int sizes[FileList::kModeCount] = { m_settings.sizeDetails, m_settings.sizeList, m_settings.sizeCards, m_settings.sizeGrid,
            m_settings.sizeColumns };
        !std::equal(std::begin(sizes), std::end(sizes), std::begin(m_prefs.layoutSize)))
    {
        std::copy(std::begin(sizes), std::end(sizes), std::begin(m_prefs.layoutSize));
        ForEachPane([](Tab* tab) { tab->ForEachList([](FileList* list) { list->ApplyLayout(); }); });
    }
    if (const unsigned hidden = FileList::ColumnBits(m_settings.detailsHidden); hidden != m_prefs.hiddenColumns)
    {
        // Files' Show*Column: every open details list follows
        m_prefs.hiddenColumns = hidden;
        ForEachPane([hidden](Tab* tab) { tab->SetListColumns(hidden); });
    }
    Format::SetSizeDecimal(m_settings.sizeDecimal);
    Format::SetDateFormat(m_settings.dateFormat);
    m_sizeKey = -1; // the status bar's size text in the new format
    MenuOptions::g_hidden = 0;
    for (int i = 0; i < MenuOptions::Count; ++i)
        if (m_settings.Has(m_settings.menuHidden, MenuOptions::kKeys[i]))
            MenuOptions::g_hidden |= 1u << i;
    // Files' StatusCenterVisibility / ShowStatusCenterTeachingTip (Refresh lays the address bar out if it changed)
    m_center.SetAlways(m_settings.statusCenterAlways);
    m_center.SetTipPending(m_settings.statusCenterTip);
    m_center.Refresh();
    if (m_prefs.columnLines != m_settings.columnLines)
    {
        m_prefs.columnLines = m_settings.columnLines;
        ForEachPane([](Tab* tab) { tab->ApplyTheme(); }); // repaints the lists
    }
    LoadToolbarLists(); // the customized command bar
    UpdateToggleButtons();

    unsigned hidden = 0;
    for (int s = 0; s < static_cast<int>(ARRAYSIZE(kHomeWidgets)); ++s)
        if (m_settings.Has(m_settings.homeHidden, kHomeWidgets[s]))
            hidden |= 1u << s;
    if (!m_settings.homeTags)
        hidden |= 1u << HomeView::SecTags;
    const bool layoutChanged = m_prefs.defaultLayout != m_settings.defaultLayout || m_prefs.syncLayout != m_settings.syncLayout;
    const bool homeChanged = m_prefs.homeHidden != hidden;
    m_prefs.defaultLayout = m_settings.defaultLayout;
    m_prefs.syncLayout = m_settings.syncLayout;
    m_prefs.homeHidden = hidden;
    if (layoutChanged)
        ApplyPrefsToTabs();
    if (homeChanged)
    {
        ForEachPane([](Tab* tab) { tab->ApplyHome(); });
        EnsureHomeFresh();
    }
    if (m_statusVisible != m_settings.statusBar)
    {
        m_statusVisible = m_settings.statusBar;
        Layout();
    }
    RebuildKeys();
    m_strip.SetActionsVisible(m_settings.tabActions);
    if (const CommandDef* def = FindCommand(L"newtab"))
    {
        // Files: "+" tooltip is Commands.NewTab.LabelWithHotKey, "label (hotkey)"
        const auto chords = EffectiveChords(*def);
        m_strip.SetNewTabTip(chords.empty() ? std::wstring(Tr(L"新建标签页")) : Tr(L"新建标签页 (") + FormatShortcut(chords.front()) + L")");
    }
    if (m_hwnd)
        UpdateTray();
    if (!m_settings.folderSizes)
        m_sizer.Cancel();
    m_folderKey = -1;
    Layout();
}

void MainWindow::SaveAndBroadcast()
{
    SaveSettings(m_settings);
    if (Sandbox::Active())
        return; // test mode: its settings file is its own; the user's FastFiles windows aren't told
    // Ctrl+N windows are separate processes; they re-read the file
    struct Ctx { HWND self; UINT msg; } ctx{ m_hwnd, SettingsChangedMessage() };
    EnumWindows([](HWND w, LPARAM lp) -> BOOL
    {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64]{};
        if (w != c->self && GetClassNameW(w, cls, 64) && wcscmp(cls, kMainClass) == 0)
            PostMessageW(w, c->msg, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void MainWindow::OpenSettings(int page)
{
    // Files' OpenSettingsAction: Settings opens in a new tab (OpenPathInNewTab("Settings"))
    const int index = NewTab(SettingsPidl(), true);
    if (index >= 0 && index < static_cast<int>(m_tabList.size()))
        m_tabList[index].panes[0]->ShowSettingsPage(page);
}

void MainWindow::SetDefaultLayout(int layout)
{
    m_settings.defaultLayout = std::clamp(layout, 0, static_cast<int>(ViewLayout::Adaptive));
    // Files: the adaptive layout isn't available while every folder shares one layout
    if (m_settings.syncLayout && m_settings.defaultLayout == static_cast<int>(ViewLayout::Adaptive))
        m_settings.defaultLayout = static_cast<int>(ViewLayout::Details);
    SaveAndBroadcast();
    ApplySettings();
}

// Layout picked from the menu / Ctrl+Shift+1..6. The folder remembers it (Files' layout preferences); with
// "same layout everywhere" it becomes the layout of every folder.
// Files' SetDetailsColumnsAsDefault: the header menu's 设为默认 makes these the columns of every folder
void MainWindow::OnTabColumnsDefault(Tab*, unsigned hidden)
{
    m_settings.detailsHidden = FileList::ColumnNames(hidden);
    SaveAndBroadcast();
    ApplySettings();
}

// Files' SetLayoutPreferencesForPath while SyncFolderPreferencesAcrossDirectories is on: the sort / grouping picked in any
// folder becomes LayoutSettingsService's default (DefaultSortOption ... DefaultSortFilesFirst), so every folder shows it
void MainWindow::OnTabSortDefault(Tab*, const ListSort& s)
{
    m_settings.defaultSort = s.key;
    m_settings.defaultSortDesc = s.descending;
    m_settings.defaultGroup = s.group;
    m_settings.defaultGroupUnit = s.unit;
    m_settings.defaultGroupDesc = s.groupDescending;
    if (s.priority >= 0)
        m_settings.sortPriority = s.priority;
    SaveAndBroadcast();
    ApplySettings();
}

// Files' LayoutSettingsService.DetailsViewSize / ListViewSize / CardsViewSize / GridViewSize: one value for every folder
void MainWindow::OnTabLayoutSize(Tab*, int mode, int size)
{
    if (mode < 0 || mode >= FileList::kModeCount)
        return;
    int* sizes[FileList::kModeCount] = { &m_settings.sizeDetails, &m_settings.sizeList, &m_settings.sizeCards, &m_settings.sizeGrid,
        &m_settings.sizeColumns };
    size = std::clamp(size, 1, FileList::MaxSize(static_cast<FileList::Mode>(mode)));
    if (*sizes[mode] == size)
        return;
    *sizes[mode] = size;
    m_prefs.layoutSize[mode] = size;
    SaveAndBroadcast();
    ForEachPane([mode](Tab* tab)
    {
        tab->ForEachList([mode](FileList* list)
        {
            if (static_cast<int>(list->GetMode()) == mode)
                list->ApplyLayout();
        });
        if (mode == static_cast<int>(FileList::Mode::Column))
            tab->ColumnsResized(); // the columns' widths follow the icon box
    });
}

void MainWindow::OnTabCycleLayout(Tab* tab, ViewLayout layout)
{
    if (tab != Active())
    {
        // (Ctrl+wheel over the other pane: that pane's folder)
        tab->ChooseLayout(layout);
        return;
    }
    SetLayoutFromUser(layout);
}

void MainWindow::SetLayoutFromUser(ViewLayout layout)
{
    Tab* tab = Active();
    if (!tab)
        return;
    if (layout == ViewLayout::Adaptive)
    {
        // Files' LayoutAdaptiveAction is a toggle: off keeps what's shown as the folder's own choice
        if (m_settings.syncLayout)
            return;
        tab->ChooseLayout(tab->AdaptiveOn() ? tab->DisplayedLayout() : ViewLayout::Adaptive);
        return;
    }
    if (layout == ViewLayout::Columns && !tab->CanColumns())
        return;
    tab->ChooseLayout(layout);
    if (m_settings.syncLayout && m_settings.defaultLayout != static_cast<int>(layout))
        SetDefaultLayout(static_cast<int>(layout));
}

// --- Shortcuts ---

namespace
{
// Override for one command; an override equal to the defaults is dropped (the row then shows no "restore default")
void SetOverride(AppSettings& s, const std::wstring& command, const std::vector<uint32_t>* chords, const std::vector<uint32_t>& defaults)
{
    auto& list = s.shortcuts;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& e) { return e.first == command; }), list.end());
    if (chords && *chords != defaults)
        list.emplace_back(command, *chords);
}
}

std::wstring MainWindow::SetShortcut(const std::wstring& command, uint32_t code)
{
    const CommandDef* def = FindCommand(command);
    if (!def)
        return Tr(L"未知命令");
    for (const auto& other : Commands())
    {
        if (&other == def)
            continue;
        auto chords = EffectiveChords(other);
        const auto it = std::find(chords.begin(), chords.end(), code);
        if (it == chords.end())
            continue;
        // Files' KeyBindingEditorDialog just blocks a conflicting binding (KeybindingAlreadyUsedNotification teaching
        // tip, no reassign option); FastFiles offers to reassign instead, so there's no Files dialog text to match.
        // MB_DEFBUTTON2 in the old MessageBox: Enter must not steal the other command's shortcut, so close stays default
        const std::wstring text = FormatShortcut(code) + Tr(L" 已经分配给“") + other.name + Tr(L"”。\n\n改为分配给“") + def->name + Tr(L"”吗？");
        if (!ContentDialog::Confirm(m_hwnd, &m_theme, Tr(L"快捷键冲突"), text, Tr(L"是"), Tr(L"取消"), true))
            return Tr(L"已取消，") + FormatShortcut(code) + Tr(L" 仍属于“") + other.name + L"”";
        chords.erase(it);
        SetOverride(m_settings, other.key, &chords, DefaultChords(other));
    }
    const std::vector<uint32_t> chords{ code };
    SetOverride(m_settings, command, &chords, DefaultChords(*def));
    SaveAndBroadcast();
    RebuildKeys();
    return {};
}

void MainWindow::ResetShortcut(const std::wstring& command)
{
    SetOverride(m_settings, command, nullptr, {});
    SaveAndBroadcast();
    RebuildKeys();
}

void MainWindow::ClearShortcut(const std::wstring& command)
{
    const CommandDef* def = FindCommand(command);
    const std::vector<uint32_t> none;
    SetOverride(m_settings, command, &none, def ? DefaultChords(*def) : std::vector<uint32_t>{ 0 });
    SaveAndBroadcast();
    RebuildKeys();
}

// --- Pages (Files 4.3: General, Appearance, Layout, Files & folders, Shortcuts, Tags, Dev tools, Advanced, About) ---

std::vector<SettingsPage> MainWindow::BuildSettingsPages()
{
    using K = SettingsRow::Kind;
    auto changed = [this] { SaveAndBroadcast(); ApplySettings(); };
    // A row's `detail` is its explanation: shown in a bubble from the ⓘ next to the label, not as text on the page
    auto header = [](std::wstring text, std::wstring detail = {})
    {
        SettingsRow r;
        r.kind = K::Header;
        r.label = std::move(text);
        r.detail = std::move(detail);
        return r;
    };
    auto toggle = [](std::wstring label, std::wstring detail, std::function<bool()> get, std::function<void(bool)> set)
    {
        SettingsRow r;
        r.kind = K::Toggle;
        r.label = std::move(label);
        r.detail = std::move(detail);
        r.isOn = std::move(get);
        r.setOn = std::move(set);
        return r;
    };
    auto choice = [](std::wstring label, std::wstring detail, std::vector<std::wstring> options, std::function<int()> get, std::function<void(int)> set)
    {
        SettingsRow r;
        r.kind = K::Choice;
        r.label = std::move(label);
        r.detail = std::move(detail);
        r.options = std::move(options);
        r.selected = std::move(get);
        r.select = std::move(set);
        return r;
    };
    auto button = [](std::wstring label, std::wstring detail, std::wstring text, std::function<void()> press)
    {
        SettingsRow r;
        r.kind = K::Button;
        r.label = std::move(label);
        r.detail = std::move(detail);
        r.button = std::move(text);
        r.press = std::move(press);
        return r;
    };
    auto value = [](std::wstring label, std::function<std::wstring()> get, std::wstring text = {}, std::function<void()> press = {})
    {
        SettingsRow r;
        r.kind = K::Value;
        r.label = std::move(label);
        r.value = std::move(get);
        r.button = std::move(text);
        r.press = std::move(press);
        return r;
    };
    const HWND owner = m_hwnd;
    std::vector<SettingsPage> pages;

    // 常规
    {
        SettingsPage p{ Glyph::Settings, Tr(L"常规"), {} };
        // Files' GeneralPage 语言 (AppLanguageHelper): 使用系统设置 first; a change asks to restart
        static const std::vector<Lang::Language> languages = Lang::Available();
        std::vector<std::wstring> languageNames;
        for (const auto& l : languages)
            languageNames.push_back(l.name);
        p.rows.push_back(choice(Tr(L"语言"), L"", languageNames, [this]
            {
                for (int i = 0; i < static_cast<int>(languages.size()); ++i)
                    if (_wcsicmp(languages[i].code.c_str(), m_settings.language.c_str()) == 0)
                        return i;
                return 0;
            },
            [this, owner](int i)
            {
                if (i < 0 || i >= static_cast<int>(languages.size()) || languages[i].code == m_settings.language)
                    return;
                m_settings.language = languages[i].code;
                SaveAndBroadcast();
                // Files' RestartNotificationText teaching tip: 是 restarts now, 否 keeps going until the next start
                if (ContentDialog::Confirm(owner, &m_theme, L"FastFiles", Tr(L"若要应用这些设置，需要重新启动应用程序。是否立即重启？"), Tr(L"是"), Tr(L"否")))
                    RestartApp();
            }));
        // Files' DateFormat card (应用 / 系统 / 通用) with today's date as its sample
        p.rows.push_back(choice(Tr(L"日期格式"), Tr(L"用于详细信息窗格和主页的最近使用的文件；文件列表的日期列由 Windows 显示。"),
            { Tr(L"应用"), Tr(L"系统"), Tr(L"通用") }, [this] { return m_settings.dateFormat; },
            [this, changed](int i) { m_settings.dateFormat = std::clamp(i, 0, 2); changed(); }));
        p.rows.back().icon = Glyph::Calendar;
        p.rows.push_back(header(Tr(L"启动")));
        p.rows.push_back(choice(Tr(L"启动时"), Tr(L"从命令行或资源管理器直接打开某个文件夹时，以那个文件夹为准。"),
            { Tr(L"恢复上次的标签页"), Tr(L"打开主页"), Tr(L"打开指定文件夹") }, [this] { return static_cast<int>(m_settings.startup); },
            [this, owner, changed](int i)
            {
                if (i == static_cast<int>(AppSettings::Startup::Path) && m_settings.startupPath.empty())
                {
                    const std::wstring folder = PickFolder(owner);
                    if (folder.empty())
                        return;
                    m_settings.startupPath = folder;
                }
                m_settings.startup = static_cast<AppSettings::Startup>(i);
                changed();
            }));
        if (m_settings.startup == AppSettings::Startup::Path)
            p.rows.push_back(value(Tr(L"启动文件夹"), [this] { return m_settings.startupPath; }, Tr(L"选择…"), [this, owner, changed]
            {
                const std::wstring folder = PickFolder(owner);
                if (!folder.empty())
                {
                    m_settings.startupPath = folder;
                    changed();
                }
            }));
        p.rows.push_back(choice(Tr(L"新建标签页时打开"), Tr(L"按 Ctrl+T 或点击标签栏上的 + 时打开的位置。"), { Tr(L"主页"), Tr(L"当前文件夹") },
            [this] { return static_cast<int>(m_settings.newTab); },
            [this, changed](int i) { m_settings.newTab = static_cast<AppSettings::NewTab>(i); changed(); }));
        p.rows.push_back(toggle(Tr(L"总是切换到新创建的标签页"), L"", [this] { return m_settings.switchNewTab; },
            [this, changed](bool on) { m_settings.switchNewTab = on; changed(); }));
        p.rows.push_back(header(Tr(L"主页小组件")));
        // Files' GeneralPage widget order: quick access, drives, network locations, file tags, recent files
        static const wchar_t* const widgetNames[] = { Tr(L"快速访问"), Tr(L"驱动器"), Tr(L"网络位置"), Tr(L"最近使用的文件") };
        for (int s : { HomeView::SecQuick, HomeView::SecDrives, HomeView::SecNetwork, HomeView::SecTags, HomeView::SecRecent })
        {
            if (s == HomeView::SecTags)
            {
                // Files' ShowFileTagsWidget is off by default
                p.rows.push_back(toggle(Tr(L"标签"), L"", [this] { return m_settings.homeTags; },
                    [this, changed](bool on) { m_settings.homeTags = on; changed(); }));
                continue;
            }
            p.rows.push_back(toggle(widgetNames[s], s == HomeView::SecRecent ? Tr(L"关闭后不再读取最近使用的文件列表。") : L"",
                [this, s] { return !m_settings.Has(m_settings.homeHidden, kHomeWidgets[s]); },
                [this, s, changed](bool on) { AppSettings::Toggle(m_settings.homeHidden, kHomeWidgets[s], !on); changed(); }));
        }
        // Files' GeneralPage 双面板模式: AlwaysOpenDualPaneInNewTab, ShellPaneArrangementOption (垂直 = side by side)
        p.rows.push_back(header(Tr(L"双面板模式")));
        p.rows.push_back(toggle(Tr(L"以双窗格模式打开标签页"), L"", [this] { return m_settings.dualPaneNewTab; },
            [this, changed](bool on) { m_settings.dualPaneNewTab = on; changed(); }));
        p.rows.push_back(choice(Tr(L"默认双面板分隔方向"), L"", { Tr(L"垂直"), Tr(L"水平") }, [this] { return m_settings.paneHorizontal ? 1 : 0; },
            [this, changed](int i) { m_settings.paneHorizontal = i == 1; changed(); }));
        // Files' 右键菜单选项 (the ones FastFiles' menus have; no alternate data streams, flatten or shell extension submenu)
        p.rows.push_back(header(Tr(L"右键菜单选项")));
        // MenuOptions::Compress (显示压缩选项) removed at the user's request: FastFiles has no built-in compress /
        // extract menu item any more, so there is nothing left for this row to show or hide. The bit itself (128,
        // "compress" in settings.txt) stays reserved in Common.h so an old settings file still parses.
        static const std::pair<unsigned, const wchar_t*> menuRows[] = { { MenuOptions::NewTab, Tr(L"显示“在新标签页中打开”") },
            { MenuOptions::NewWindow, Tr(L"显示“在新窗口中打开”") }, { MenuOptions::NewPane, Tr(L"显示“在新面板中打开”选项") },
            { MenuOptions::CopyPath, Tr(L"显示“复制路径”选项") }, { MenuOptions::FolderWithSelection, Tr(L"显示“用选中的项目创建文件夹”选项") },
            { MenuOptions::Shortcut, Tr(L"显示“创建快捷方式”选项") }, { MenuOptions::Pin, Tr(L"显示“固定到侧边栏”选项") },
            { MenuOptions::SendTo, Tr(L"显示“发送到”菜单") },
            { MenuOptions::Terminal, Tr(L"显示“在 Windows 终端中打开”") }, { MenuOptions::Tags, Tr(L"显示“编辑文件标签”") },
            { MenuOptions::PinStart, Tr(L"显示“固定到开始菜单”选项") } };
        for (const auto& [option, label] : menuRows)
        {
            const wchar_t* key = L"";
            for (int i = 0; i < MenuOptions::Count; ++i)
                if (option == 1u << i)
                    key = MenuOptions::kKeys[i];
            p.rows.push_back(toggle(label, L"", [this, key] { return !m_settings.Has(m_settings.menuHidden, key); },
                [this, key, changed](bool on) { AppSettings::Toggle(m_settings.menuHidden, key, !on); changed(); }));
        }
        // Files' GeneralPage "翻页": SelectedTabScrollDirectionIndex (0 = default, 1 = ReverseTabScrollDirection)
        p.rows.push_back(header(Tr(L"翻页")));
        p.rows.push_back(choice(Tr(L"反转标签页滚轮切换方向"), L"", { Tr(L"向下滚动切换至左侧标签页"), Tr(L"向下滚动切换至右侧标签页") },
            [this] { return m_settings.reverseTabWheel ? 1 : 0; },
            [this, changed](int i) { m_settings.reverseTabWheel = i == 1; changed(); }));
        pages.push_back(std::move(p));
    }
    // 外观
    {
        // Files' AppearancePage: flat cards with header icons (no group headers above 地址栏 / 工具栏)
        SettingsPage p{ Glyph::Palette, Tr(L"外观"), {} };
        p.rows.push_back(choice(Tr(L"主题"), Tr(L"Ctrl+Alt+T 可以在浅色和深色之间快速切换。"), { Tr(L"使用系统设置"), Tr(L"浅色"), Tr(L"深色") },
            [this] { return static_cast<int>(m_settings.theme); },
            [this](int i) { m_settings.theme = static_cast<AppSettings::ThemeMode>(i); SaveAndBroadcast(); ApplySettings(); }));
        p.rows.back().icon = Glyph::Palette;
        // Files' BackdropMaterial in Files' order (无, 云母, 云母 Alt); its 亚克力 / 薄亚克力 show the windows behind,
        // which FastFiles can't do: left out
        p.rows.push_back(choice(Tr(L"背景材质"), L"", { Tr(L"无"), Tr(L"云母"), Tr(L"云母 Alt") },
            [this] { return m_settings.themeBackdrop == Backdrop::Solid ? 0 : m_settings.themeBackdrop == Backdrop::Mica ? 1 : 2; },
            [this](int i) { m_settings.themeBackdrop = i == 0 ? Backdrop::Solid : i == 1 ? Backdrop::Mica : Backdrop::MicaAlt; SaveAndBroadcast(); ApplySettings(); }));
        p.rows.back().icon = Glyph::Backdrop;
        // Files' AppThemeResourcesFactory: the default (none) and the accent presets at alpha 0x32, then 自定义
        static const std::pair<const wchar_t*, const wchar_t*> presets[] = { { Tr(L"默认"), L"#00000000" }, { Tr(L"金黄色"), L"#32FFB900" },
            { Tr(L"浅橙色"), L"#32F7630C" }, { Tr(L"砖红色"), L"#32D13438" }, { Tr(L"鸢红色"), L"#32FF4343" }, { Tr(L"红色"), L"#32EA005E" },
            { Tr(L"玫瑰色"), L"#32EA005E" }, { Tr(L"蓝色"), L"#320078D7" }, { Tr(L"粉鸢尾花色"), L"#328764B8" }, { Tr(L"菖蒲色"), L"#32B146C2" },
            { Tr(L"浅蓝色"), L"#320099BC" }, { Tr(L"海泡石"), L"#3200B7C3" }, { Tr(L"天青色"), L"#3200B294" }, { Tr(L"灰色"), L"#327A7574" },
            { Tr(L"绿色"), L"#32107C10" }, { Tr(L"阴天"), L"#32767676" }, { Tr(L"风暴"), L"#324C4A48" }, { Tr(L"蓝灰色"), L"#3269797E" },
            { Tr(L"暗灰色"), L"#324A5459" }, { Tr(L"迷彩色"), L"#327E735F" } };
        auto argbOf = [](const std::wstring& s) { return s.size() > 1 ? static_cast<uint32_t>(wcstoul(s.c_str() + 1, nullptr, 16)) : 0u; };
        // Files' background color SettingsExpander: a preview card per preset; a color that is no preset adds a
        // 自定义 card at the end (AppearanceViewModel.AppThemeBackgroundColor)
        SettingsRow colors;
        colors.kind = SettingsRow::Kind::Colors;
        colors.label = Tr(L"背景颜色");
        colors.icon = Glyph::Color;
        for (const auto& [name, color] : presets)
        {
            colors.options.push_back(name);
            colors.colors.push_back(argbOf(color));
        }
        // Files selects the first preset with this color (RoseBright shares Red's)
        int preset = m_settings.themeBackground.empty() ? 0 : -1;
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(presets)) && preset < 0; ++i)
            if (_wcsicmp(m_settings.themeBackground.c_str(), presets[i].second) == 0)
                preset = i;
        if (preset < 0 && !m_settings.themeBackground.empty())
        {
            colors.options.push_back(Tr(L"自定义"));
            colors.colors.push_back(argbOf(m_settings.themeBackground));
        }
        const int customIndex = preset < 0 && !m_settings.themeBackground.empty() ? static_cast<int>(ARRAYSIZE(presets)) : -1;
        colors.selected = [preset, customIndex] { return preset >= 0 ? preset : customIndex; };
        colors.select = [this, changed](int i)
        {
            if (i < 0 || i >= static_cast<int>(ARRAYSIZE(presets)))
                return; // the 自定义 card is the current color already
            m_settings.themeBackground = presets[i].second;
            changed();
        };
        colors.color = [this, argbOf] { return argbOf(m_settings.themeBackground); };
        colors.press = [this, owner, changed, argbOf]
        {
            // Files' ColorPicker flyout: CommunityToolkit ColorPicker with IsAlphaEnabled="True",
            // IsColorSpectrumVisible="False" (AppearancePage.xaml) - drawn natively (ColorPickerFlyout.cpp) with a
            // preview + hex box and R/G/B/A sliders, so the alpha channel can actually be set (ChooseColorW has none)
            const uint32_t now = argbOf(m_settings.themeBackground);
            const uint32_t withAlpha = (now >> 24) ? now : (0x32u << 24) | (now & 0x00FFFFFFu);
            POINT pt{};
            GetCursorPos(&pt);
            uint32_t picked = 0;
            if (!ColorPickerFlyout::Show(owner, &m_theme, pt, withAlpha, picked))
                return;
            wchar_t hex[16];
            swprintf_s(hex, L"#%02X%02X%02X%02X", (picked >> 24) & 0xFF, (picked >> 16) & 0xFF, (picked >> 8) & 0xFF, picked & 0xFF);
            m_settings.themeBackground = hex;
            changed();
        };
        p.rows.push_back(std::move(colors));
        // Files' BackgroundImage expander: 浏览 (with 移除), 不透明度 (.1..1), 图片契合方式, 垂直 / 水平对齐方式
        p.rows.push_back(header(Tr(L"背景图片")));
        p.rows.push_back(value(Tr(L"背景图片"), [this] { return m_settings.bgImage.empty() ? std::wstring(Tr(L"无")) : m_settings.bgImage; },
            Tr(L"浏览"), [this, owner, changed]
            {
                // Files' SelectImage: 图片文件 / 位图文件
                wchar_t file[MAX_PATH]{};
                const std::wstring images = std::wstring(Tr(L"图片文件")) + L'\0' + L"*.bmp;*.dib;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.tif;*.tiff;*.png;*.heic;*.hif;*.webp" +
                    L'\0' + Tr(L"位图文件") + L'\0' + L"*.bmp;*.dib" + L'\0' + L'\0';
                OPENFILENAMEW ofn{ sizeof(ofn) };
                ofn.hwndOwner = owner;
                ofn.lpstrFilter = images.c_str();
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
                if (!GetOpenFileNameW(&ofn))
                    return;
                m_settings.bgImage = file;
                changed();
            }));
        p.rows.back().icon = Glyph::Picture;
        if (!m_settings.bgImage.empty())
        {
            // Files: the 移除 item of 浏览's split-button menu
            p.rows.push_back(button(Tr(L"移除"), L"", Tr(L"移除"), [this, changed] { m_settings.bgImage.clear(); changed(); }));
            std::vector<std::wstring> opacities;
            for (int i = 1; i <= 10; ++i)
                opacities.push_back(std::to_wstring(i * 10) + L"%");
            p.rows.push_back(choice(Tr(L"不透明度"), L"", opacities, [this] { return std::clamp(m_settings.bgImageOpacity / 10 - 1, 0, 9); },
                [this, changed](int i) { m_settings.bgImageOpacity = (i + 1) * 10; changed(); }));
            p.rows.push_back(choice(Tr(L"图片契合方式"), L"", { Tr(L"无"), Tr(L"填充"), Tr(L"均匀"), Tr(L"均匀填充") },
                [this] { return m_settings.bgImageFit; }, [this, changed](int i) { m_settings.bgImageFit = i; changed(); }));
            p.rows.push_back(choice(Tr(L"垂直对齐方式"), L"", { Tr(L"顶端对齐"), Tr(L"居中"), Tr(L"底端对齐") },
                [this] { return m_settings.bgImageVertical; }, [this, changed](int i) { m_settings.bgImageVertical = i; changed(); }));
            p.rows.push_back(choice(Tr(L"水平对齐方式"), L"", { Tr(L"左对齐"), Tr(L"居中"), Tr(L"右对齐") },
                [this] { return m_settings.bgImageHorizontal; }, [this, changed](int i) { m_settings.bgImageHorizontal = i; changed(); }));
        }
        // Files' AppThemeFontFamilyOptions: 默认 (the standard font), then the installed families
        static const std::vector<std::wstring> families = []
        {
            std::vector<std::wstring> out;
            LOGFONTW lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            HDC hdc = GetDC(nullptr);
            EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW* f, const TEXTMETRICW*, DWORD, LPARAM p)
                {
                    auto* list = reinterpret_cast<std::vector<std::wstring>*>(p);
                    if (f->lfFaceName[0] != L'@')
                        list->push_back(f->lfFaceName);
                    return 1;
                }, reinterpret_cast<LPARAM>(&out), 0);
            ReleaseDC(nullptr, hdc);
            std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }();
        std::vector<std::wstring> fontNames{ Tr(L"默认") };
        fontNames.insert(fontNames.end(), families.begin(), families.end());
        p.rows.push_back(choice(Tr(L"字体"), L"", fontNames, [this]
            {
                const auto it = std::find(families.begin(), families.end(), m_settings.themeFont);
                return m_settings.themeFont.empty() || it == families.end() ? 0 : static_cast<int>(it - families.begin()) + 1;
            },
            [this, changed](int i)
            {
                m_settings.themeFont = i <= 0 ? std::wstring() : families[std::min<size_t>(i - 1, families.size() - 1)];
                changed();
            }));
        p.rows.back().icon = Glyph::Font;
        p.rows.push_back(toggle(Tr(L"在标题栏中显示标签操作按钮"), L"", [this] { return m_settings.tabActions; },
            [this, changed](bool on) { m_settings.tabActions = on; changed(); }));
        // Files' AddressBar expander: ShowStatusCenterButton (始终 / 在文件操作进行期间)
        p.rows.push_back(header(Tr(L"地址栏")));
        p.rows.push_back(choice(Tr(L"显示状态中心按钮"), L"", { Tr(L"始终"), Tr(L"在文件操作进行期间") },
            [this] { return m_settings.statusCenterAlways ? 0 : 1; },
            [this, changed](int i) { m_settings.statusCenterAlways = i == 0; changed(); }));
        // Files' Toolbar expander: 显示工具栏 in its header, 自定义工具栏按钮 opens the customization window
        p.rows.push_back(header(Tr(L"工具栏")));
        p.rows.push_back(toggle(Tr(L"显示工具栏"), L"", [this] { return m_toolbarVisible; }, [this](bool) { ToggleToolbar(); }));
        p.rows.push_back(button(Tr(L"自定义工具栏按钮"), L"", Tr(L"自定义"), [this] { CustomizeToolbar(); }));
        p.rows.back().icon = Glyph::Customize;
        p.rows.push_back(toggle(Tr(L"显示状态栏"), L"", [this] { return m_settings.statusBar; }, [this, changed](bool on) { m_settings.statusBar = on; changed(); }));
        pages.push_back(std::move(p));
    }
    // 布局
    {
        SettingsPage p{ Glyph::Layout, Tr(L"布局"), {} };
        // Files' LayoutPage combo (FolderLayoutModes order): 详细信息, 列表, 卡片, 列视图, 网格, 自适应布局
        static const ViewLayout kChoices[] = { ViewLayout::Details, ViewLayout::List, ViewLayout::Tiles, ViewLayout::Columns,
            ViewLayout::LargeIcons, ViewLayout::Adaptive };
        p.rows.push_back(choice(Tr(L"默认布局"), Tr(L"没有打开过的文件夹使用这个布局，打开过的文件夹会记住自己的布局。"),
            { Tr(L"详细信息"), Tr(L"列表"), Tr(L"卡片"), Tr(L"列视图"), Tr(L"网格"), Tr(L"自适应布局") },
            [this]
            {
                const int d = m_settings.defaultLayout;
                const ViewLayout shown = d == static_cast<int>(ViewLayout::Columns) || d == static_cast<int>(ViewLayout::Adaptive)
                    ? static_cast<ViewLayout>(d) : FilesLayout(d);
                for (int i = 0; i < static_cast<int>(ARRAYSIZE(kChoices)); ++i)
                    if (kChoices[i] == shown)
                        return i;
                return 0;
            },
            [this](int i) { if (i >= 0 && i < static_cast<int>(ARRAYSIZE(kChoices))) SetDefaultLayout(static_cast<int>(kChoices[i])); }));
        p.rows.push_back(toggle(Tr(L"所有文件夹使用同一布局"), Tr(L"开启后，在任何文件夹里更改布局都会成为新的默认布局，自适应布局不可用。"),
            [this] { return m_settings.syncLayout; }, [this, owner, changed](bool on)
            {
                if (on == m_settings.syncLayout)
                    return;
                m_settings.syncLayout = on;
                // Files' LayoutViewModel: switching it resets every folder's own layout
                DeleteFileW(AppDataFile(L"layouts.txt").c_str());
                if (on && m_settings.defaultLayout == static_cast<int>(ViewLayout::Adaptive))
                {
                    m_settings.defaultLayout = static_cast<int>(ViewLayout::Details);
                    changed();
                    // Files' LayoutPage TeachingTip, AdaptiveLayoutDisabledNotification: same text, word for word
                    ContentDialog::Notice(owner, &m_theme, L"FastFiles",
                        Tr(L"当选项“在不同目录之间使用相同的布局和排序设置”启用时，自适应布局不可用。默认布局已更改为“详细信息”。"));
                    ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
                    return;
                }
                changed();
            }));
        p.rows.push_back(toggle(Tr(L"项目复选框"), L"", [this] { return m_prefs.checkboxes; }, [this](bool) { ToggleCheckboxes(); }));
        p.rows.push_back(toggle(Tr(L"显示列分隔线"), Tr(L"在详细信息视图的各列之间显示竖线。"), [this] { return m_settings.columnLines; },
            [this, changed](bool on) { m_settings.columnLines = on; changed(); }));
        // Files' 排序和分组 > 排序优先级 (LayoutViewModel.SelectedDefaultSortPriorityIndex); only FastFiles' own list
        // honours it (the shell's view always puts folders first), so it shows while that list is in use
        if (Tab::OwnList())
        {
            p.rows.push_back(header(Tr(L"排序和分组")));
            // Files' 排序方式 expander (LayoutViewModel.SelectedDefaultSortingIndex: 名称, 修改日期, 创建日期, 大小, 类型, 文件标签),
            // 降序排序, 排序优先级: the defaults of folders without their own choice
            static const SortKey kSorts[] = { SortKey::Name, SortKey::Modified, SortKey::Created, SortKey::Size, SortKey::Type, SortKey::Tag };
            p.rows.push_back(choice(Tr(L"排序方式"), L"", { Tr(L"名称"), Tr(L"修改日期"), Tr(L"创建日期"), Tr(L"大小"), Tr(L"类型"), Tr(L"文件标签") },
                [this]
                {
                    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kSorts)); ++i)
                        if (static_cast<int>(kSorts[i]) == m_settings.defaultSort)
                            return i;
                    return 0;
                },
                [this, changed](int i) { if (i >= 0 && i < static_cast<int>(ARRAYSIZE(kSorts))) { m_settings.defaultSort = static_cast<int>(kSorts[i]); changed(); } }));
            p.rows.back().icon = Glyph::Sort;
            p.rows.push_back(toggle(Tr(L"降序排序"), L"", [this] { return m_settings.defaultSortDesc; },
                [this, changed](bool on) { m_settings.defaultSortDesc = on; changed(); }));
            p.rows.push_back(choice(Tr(L"排序优先级"), L"", { Tr(L"先排序文件夹"), Tr(L"先排序文件"), Tr(L"将文件和文件夹一同排序") },
                [this] { return m_settings.sortPriority; }, [this, changed](int i) { m_settings.sortPriority = i; changed(); }));
            // Files' 分组依据 expander (SelectedDefaultGroupingIndex: 无, 名称, 修改日期, 创建日期, 大小, 类型, 文件标签 = GroupOption
            // order), 降序分组 (only while grouped) and 按日期分组时的单位 (only by a date); shown only when they apply here
            p.rows.push_back(choice(Tr(L"分组依据"), L"", { Tr(L"无"), Tr(L"名称"), Tr(L"修改日期"), Tr(L"创建日期"), Tr(L"大小"), Tr(L"类型"), Tr(L"文件标签") },
                [this] { return m_settings.defaultGroup; },
                [this, changed](int i)
                {
                    if (i == m_settings.defaultGroup)
                        return;
                    m_settings.defaultGroup = i;
                    changed();
                    ForEachPane([](Tab* tab) { tab->RebuildSettings(); }); // the rows below come and go
                }));
            p.rows.back().icon = Glyph::Group;
            if (m_settings.defaultGroup != FileList::kGroupNone)
                p.rows.push_back(toggle(Tr(L"降序分组"), L"", [this] { return m_settings.defaultGroupDesc; },
                    [this, changed](bool on) { m_settings.defaultGroupDesc = on; changed(); }));
            if (m_settings.defaultGroup == FileList::kGroupModified || m_settings.defaultGroup == FileList::kGroupCreated)
                p.rows.push_back(choice(Tr(L"按日期分组时的单位"), L"", { Tr(L"年"), Tr(L"月"), Tr(L"日") },
                    [this] { return m_settings.defaultGroupUnit; }, [this, changed](int i) { m_settings.defaultGroupUnit = i; changed(); }));
            // Files' 详细信息视图 > 列视图: TagColumn, SizeColumn, TypeColumn, DateColumn, DateCreatedColumn (in that order)
            p.rows.push_back(header(Tr(L"详细信息视图")));
            const struct { const wchar_t* key; const wchar_t* label; } columns[] = { { L"tag", L"“标签”列" }, { L"size", L"“大小”列" },
                { L"type", L"“类型”列" }, { L"modified", L"“日期”列" }, { L"created", L"“创建日期”列" } };
            for (const auto& c : columns)
            {
                const std::wstring key = c.key;
                p.rows.push_back(toggle(Tr(c.label), L"", [this, key] { return !m_settings.Has(m_settings.detailsHidden, key.c_str()); },
                    [this, key, changed](bool on)
                    {
                        auto& list = m_settings.detailsHidden;
                        list.erase(std::remove(list.begin(), list.end(), key), list.end());
                        if (!on)
                            list.push_back(key);
                        changed();
                    }));
            }
        }
        pages.push_back(std::move(p));
    }
    // 文件和文件夹
    {
        SettingsPage p{ Glyph::Folder, Tr(L"文件和文件夹"), {} };
        // Files' FoldersPage: its own settings, Explorer's are left alone
        p.rows.push_back(toggle(Tr(L"显示隐藏的项目"), L"", [this] { return m_settings.showHidden; },
            [this](bool on) { if (on != m_settings.showHidden) ToggleHidden(); }));
        if (m_settings.showHidden)
            p.rows.push_back(toggle(Tr(L"显示受保护的系统文件"), Tr(L"这些是 Windows 运行需要的文件，删除或修改可能导致系统无法正常工作。"),
                [this] { return m_settings.showProtected; }, [this, owner, changed](bool on)
                {
                    // Files' FoldersViewModel.ShowProtectedSystemFiles toggles straight through with no confirmation;
                    // FastFiles asks first, so there's no Files dialog text to match here.
                    // MB_DEFBUTTON2 in the old MessageBox: Enter must not reveal protected files, so close stays default
                    if (on && !ContentDialog::Confirm(owner, &m_theme, L"FastFiles",
                                  Tr(L"受保护的系统文件是 Windows 运行需要的文件，删除或修改可能导致系统无法正常工作。\n\n确定要显示吗？"),
                                  Tr(L"是"), Tr(L"取消"), true))
                        return;
                    m_settings.showProtected = on;
                    changed();
                }));
        p.rows.push_back(toggle(Tr(L"显示以英文句点 (.) 开头的文件"), L"", [this] { return m_settings.showDotFiles; },
            [this](bool on) { if (on != m_settings.showDotFiles) ToggleDotFiles(); }));
        // Files' FoldersPage 行为: OpenFoldersInNewTab, DeleteConfirmationPolicy, DoubleClickToGoUp, ScrollToPreviousFolderWhenNavigatingUp
        p.rows.push_back(toggle(Tr(L"在新标签页中打开文件夹"), L"", [this] { return m_settings.openFoldersNewTab; },
            [this, changed](bool on) { m_settings.openFoldersNewTab = on; changed(); }));
        p.rows.push_back(choice(Tr(L"删除项目时显示确认对话框"), L"", { Tr(L"始终"), Tr(L"仅在永久删除时"), Tr(L"从不") },
            [this] { return static_cast<int>(m_settings.deleteConfirm); },
            [this, changed](int i) { m_settings.deleteConfirm = static_cast<AppSettings::DeleteConfirm>(std::clamp(i, 0, 2)); changed(); }));
        p.rows.push_back(toggle(Tr(L"双击空白处以转到上级目录"), L"", [this] { return m_settings.doubleClickUp; },
            [this, changed](bool on) { m_settings.doubleClickUp = on; changed(); }));
        p.rows.push_back(toggle(Tr(L"向上导航时滚动至上一目录"), L"", [this] { return m_settings.selectPrevious; },
            [this, changed](bool on) { m_settings.selectPrevious = on; changed(); }));
        // Files' SelectFilesOnHover: the item under the mouse for a second is selected (off by default)
        p.rows.push_back(toggle(Tr(L"鼠标悬停在项目上时自动选择"), L"", [this] { return m_settings.hoverSelect; },
            [this, changed](bool on) { m_settings.hoverSelect = on; changed(); }));
        p.rows.push_back(header(Tr(L"与资源管理器共用"), Tr(L"下面这些是 Windows 的设置，和资源管理器“文件夹选项”是同一份。")));
        p.rows.push_back(toggle(Tr(L"显示文件扩展名"), L"", [] { return ShellFlag(SSF_SHOWEXTENSIONS); },
            [this](bool on) { if (on != ShellFlag(SSF_SHOWEXTENSIONS)) ToggleExtensions(); }));
        p.rows.push_back(toggle(Tr(L"显示缩略图"), Tr(L"关闭后只显示图标，图片很多的文件夹打开更快。"), [] { return !ShellFlag(SSF_ICONSONLY); },
            [this](bool on) { SetShellFlag(SSF_ICONSONLY, !on); }));
        p.rows.push_back(toggle(Tr(L"单击打开项目"), Tr(L"单击打开项目，鼠标指向时选中。"), [] { return !ShellFlag(SSF_DOUBLECLICKINWEBVIEW); },
            [this](bool on) { SetShellFlag(SSF_DOUBLECLICKINWEBVIEW, !on); }));
        p.rows.push_back(header(Tr(L"搜索")));
        p.rows.push_back(choice(Tr(L"搜索框使用"), Tr(L"FastFiles 搜索：按文件名查找当前文件夹及其子文件夹，不依赖索引；用空格分开多个词，* 和 ? 作为通配符。\n") +
                                              std::wstring(Tr(L"Windows 搜索：使用系统索引，可以搜索文件内容。")),
            { Tr(L"FastFiles 搜索（文件名）"), Tr(L"Windows 搜索") }, [this] { return static_cast<int>(m_settings.searchEngine); },
            [this, changed](int i) { m_settings.searchEngine = static_cast<AppSettings::SearchEngine>(i); changed(); }));
        // Files' SizeFormat (二进制 = 1024, the default; 十进制 = 1000): FastFiles' own views (details pane, status bar,
        // home page, status center); the file list's 大小 column is the shell's
        p.rows.push_back(choice(Tr(L"大小格式"), Tr(L"用于详细信息窗格、状态栏、主页和状态中心；文件列表的“大小”列由 Windows 显示。"),
            { Tr(L"二进制"), Tr(L"十进制") }, [this] { return m_settings.sizeDecimal ? 1 : 0; },
            [this, changed](int i) { m_settings.sizeDecimal = i == 1; changed(); }));
        p.rows.push_back(header(Tr(L"文件夹大小")));
        p.rows.push_back(toggle(Tr(L"计算选中文件夹的大小"), Tr(L"选中一个文件夹时，在状态栏和详细信息窗格中显示它的总大小。"),
            [this] { return m_settings.folderSizes; }, [this, changed](bool on) { m_settings.folderSizes = on; changed(); }));
        pages.push_back(std::move(p));
    }
    // 快捷键
    {
        SettingsPage p{ Glyph::Keyboard, Tr(L"快捷键"), {} };
        p.rows.push_back(button(Tr(L"恢复全部默认快捷键"),
            Tr(L"点击快捷键录制新的组合键，点击 ✕ 取消分配。组合键需要包含 Ctrl 或 Alt（F3–F12 可以单独使用）。"), Tr(L"恢复默认"), [this, owner]
        {
            // Files' ActionsPage RestoreDefaultsConfirmationTeachingTip: RestoreDefaultKeybindingsConfirmation, 是 / 否
            if (!ContentDialog::Confirm(owner, &m_theme, L"FastFiles", Tr(L"确定要恢复为默认按键绑定吗？此操作无法撤消。"), Tr(L"是"), Tr(L"否")))
                return;
            m_settings.shortcuts.clear();
            SaveAndBroadcast();
            RebuildKeys();
        }));
        const wchar_t* category = nullptr;
        for (const auto& def : Commands())
        {
            if (!category || wcscmp(category, def.category) != 0)
            {
                category = def.category;
                p.rows.push_back(header(category));
            }
            SettingsRow r;
            r.kind = K::Shortcut;
            r.label = def.name;
            r.command = def.key;
            r.chords = [this, d = &def] { return EffectiveChords(*d); };
            r.overridden = [this, d = &def] { return m_settings.Override(d->key) != nullptr; };
            p.rows.push_back(std::move(r));
        }
        pages.push_back(std::move(p));
    }
    // 标签
    AddTagsSettings(pages, owner);
    // (No 开发工具 page: the user asked for it to go. The Git branch chip and Ctrl+` terminal stay.)
    // 高级
    {
        SettingsPage p{ Glyph::Repair, Tr(L"高级"), {} };
        p.rows.push_back(header(Tr(L"设置文件")));
        p.rows.push_back(button(Tr(L"导出设置"), Tr(L"保存为文本文件，可以在其他电脑上导入。"), Tr(L"导出…"), [this, owner]
        {
            const std::wstring path = PickFile(owner, true, Tr(L"导出 FastFiles 设置"));
            // Files' ExportSettingsAsync is silent on success and failure (no dialog at all); FastFiles keeps its own notice.
            if (!path.empty())
                ContentDialog::Notice(owner, &m_theme, L"FastFiles", ExportSettings(path) ? Tr(L"设置已导出。") : Tr(L"导出失败。"));
        }));
        p.rows.push_back(button(Tr(L"导入设置"), Tr(L"用文件中的设置替换当前设置。"), Tr(L"导入…"), [this, owner]
        {
            const std::wstring path = PickFile(owner, false, Tr(L"导入 FastFiles 设置"));
            if (path.empty())
                return;
            const int count = ImportSettings(path);
            if (count <= 0)
            {
                // Files' AdvancedViewModel has one failure text (SettingsImportErrorTitle/Description, "此设置文件已损坏");
                // FastFiles tells read/write failures apart from a file with nothing to import, so it keeps both texts.
                ContentDialog::Notice(owner, &m_theme, L"FastFiles",
                    count < 0 ? Tr(L"读取或写入文件失败。") : Tr(L"这个文件里没有可用的 FastFiles 设置，当前设置没有改动。"));
                return;
            }
            m_settings = LoadSettings();
            SaveAndBroadcast();
            ApplySettings();
            // Files shows nothing on import success either; FastFiles keeps its own notice with the count.
            ContentDialog::Notice(owner, &m_theme, L"FastFiles", Tr(L"已导入 ") + std::to_wstring(count) + Tr(L" 项设置。"));
        }));
        p.rows.push_back(button(Tr(L"用记事本编辑设置文件"), Tr(L"编辑完成后点击“重新加载设置文件”。"), Tr(L"打开"), []
        {
            SaveSettings(LoadSettings()); // make sure the file exists
            ShellExecuteW(nullptr, nullptr, L"notepad.exe", (L"\"" + SettingsPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }));
        p.rows.push_back(button(Tr(L"重新加载设置文件"), L"", Tr(L"重新加载"), [this]
        {
            m_settings = LoadSettings();
            SaveAndBroadcast();
            ApplySettings();
        }));
        p.rows.push_back(button(Tr(L"恢复全部默认设置"), Tr(L"主题、布局、工具栏按钮和快捷键恢复默认，固定的文件夹和标签页不受影响。"), Tr(L"恢复默认"), [this, owner]
        {
            // Files has no single "restore all settings" action to match; FastFiles keeps its own confirmation text.
            // MB_DEFBUTTON2 in the old MessageBox: Enter must not wipe every setting, so close stays default
            if (!ContentDialog::Confirm(owner, &m_theme, L"FastFiles", Tr(L"把所有设置恢复成默认值？"), Tr(L"是"), Tr(L"取消"), true))
                return;
            m_settings = AppSettings{};
            SaveAndBroadcast();
            ApplySettings();
        }));
        p.rows.push_back(header(Tr(L"数据")));
        p.rows.push_back(value(Tr(L"数据文件夹"), [] { std::wstring dir = AppDataFile(L""); if (!dir.empty()) dir.pop_back(); return dir; }, Tr(L"打开"), [this]
        {
            std::wstring dir = AppDataFile(L"");
            PIDLIST_ABSOLUTE raw = nullptr;
            if (!dir.empty() && SUCCEEDED(SHParseDisplayName(dir.c_str(), nullptr, &raw, 0, nullptr)))
            {
                UniquePidl pidl(raw);
                NewTab(pidl.get(), true);
            }
        }));
        p.rows.push_back(header(Tr(L"后台和系统集成")));
        p.rows.push_back(toggle(Tr(L"在已打开的窗口中打开文件夹"), Tr(L"再次启动 FastFiles 时，在已有窗口中打开新标签页，而不是新开一个窗口。"),
            [this] { return m_settings.singleInstance; }, [this, changed](bool on) { m_settings.singleInstance = on; changed(); }));
        p.rows.push_back(toggle(Tr(L"关闭窗口后继续在后台运行"), Tr(L"关闭窗口后保留在通知区域，再次打开更快。右键单击托盘图标并选择“退出”才会完全关闭。"),
            [this] { return m_settings.background; }, [this, changed](bool on)
            {
                m_settings.background = on;
                if (!on && AutoStartEnabled())
                    SetAutoStart(false); // starting hidden with no tray icon would leave no way back in
                changed();
            }));
        // Files' ShowSystemTrayIcon (on): the icon while FastFiles runs; it stays while the window is in the background
        p.rows.push_back(toggle(Tr(L"在系统托盘中显示 FastFiles 图标"), L"", [this] { return m_settings.trayIcon; },
            [this, changed](bool on) { m_settings.trayIcon = on; changed(); }));
        p.rows.push_back(toggle(Tr(L"开机时启动"), Tr(L"登录 Windows 后在后台启动，同时开启“关闭窗口后继续在后台运行”。"),
            [] { return AutoStartEnabled(); }, [this, owner, changed](bool on)
            {
                // Files is a packaged UWP app (its own startup task registration rarely fails this way), so there's
                // no matching Files error dialog; FastFiles keeps its own text for this Win32-only failure.
                if (!SetAutoStart(on))
                {
                    ContentDialog::Notice(owner, &m_theme, L"FastFiles", Tr(L"写入启动项失败。"));
                    return;
                }
                if (on && !m_settings.background)
                    m_settings.background = true;
                changed();
            }));
        // Files: 将 Files 设为默认文件管理器 (reads the registry, so the toggle shows what is actually in effect)
        p.rows.push_back(toggle(Tr(L"将 FastFiles 设为默认文件管理器"), Tr(L"此选项会修改系统注册表，并可能对你的设备产生意想不到的副作用。继续即代表您自行承担风险。"),
            [] { return DefaultFileManagerEnabled(); }, [this, owner](bool on)
            {
                // Same here: no Files counterpart for a registry-write failure, so FastFiles' own text stays.
                if (!SetDefaultFileManager(on))
                    ContentDialog::Notice(owner, &m_theme, L"FastFiles", Tr(L"修改注册表失败。"));
            }));
        pages.push_back(std::move(p));
    }
    // 关于
    {
        // Files' AboutPage: the app card (name, version), then 打开 GitHub 存储库. The update check is Files'
        // update service (its toolbar button); here it sits on the app card's page, as the user asked.
        SettingsPage p{ Glyph::Info, Tr(L"关于"), {} };
        p.rows.push_back(value(L"FastFiles", [] { return Tr(L"版本 ") + std::wstring(FASTFILES_VERSION); }));
        p.rows.back().icon = Glyph::Info;
        const bool busy = m_updateStage == UpdateStage::Checking || m_updateStage == UpdateStage::Downloading;
        p.rows.push_back(value(Tr(L"检查更新"), [this] { return UpdateCheckText(); },
            busy ? std::wstring() : m_updateStage == UpdateStage::Available ? Tr(L"立即更新") : Tr(L"检查更新"),
            [this] { m_updateStage == UpdateStage::Available ? StartUpdate() : CheckForUpdates(); }));
        p.rows.back().icon = Glyph::Sync;
        p.rows.push_back(value(Tr(L"GitHub 仓库"), [] { return std::wstring(L"github.com/") + Update::kRepo; }, Tr(L"打开"), [] { Update::OpenRepo(); }));
        p.rows.back().icon = Glyph::Globe;
        pages.push_back(std::move(p));
    }
    return pages;
}

// --- 检查更新 (Files' IUpdateService: check, download with progress, then the app restarts) ---

std::wstring MainWindow::UpdateCheckText() const
{
    switch (m_updateStage)
    {
    case UpdateStage::Checking: return Tr(L"正在检查更新…");
    case UpdateStage::Latest: return Tr(L"已是最新版本");
    case UpdateStage::Available: return Tr(L"发现新版本 ") + m_updateRelease.version;
    case UpdateStage::Downloading: return Tr(L"正在下载更新… ") + std::to_wstring(m_updatePercent) + L"%";
    case UpdateStage::Failed: return m_updateError;
    default: return {};
    }
}

void MainWindow::CheckForUpdates()
{
    if (m_updateStage == UpdateStage::Checking || m_updateStage == UpdateStage::Downloading)
        return;
    m_updateStage = UpdateStage::Checking;
    Update::TestLog(L"stage checking");
    Update::CheckAsync(m_hwnd);
    ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
}

void MainWindow::StartUpdate()
{
    if (m_updateStage != UpdateStage::Available)
        return;
    m_updateStage = UpdateStage::Downloading;
    m_updatePercent = 0;
    Update::TestLog(L"stage downloading " + m_updateRelease.version);
    Update::DownloadAsync(m_hwnd, m_updateRelease);
    ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
}

void MainWindow::OnUpdateResult(Update::Result* result)
{
    std::unique_ptr<Update::Result> r(result);
    switch (r->kind)
    {
    case Update::Result::Kind::Checked:
        m_updateRelease = r->release;
        m_updateStage = r->release.version.empty() ? UpdateStage::Latest : UpdateStage::Available;
        break;
    case Update::Result::Kind::Progress:
        if (m_updateStage != UpdateStage::Downloading || r->percent == m_updatePercent)
            return;
        m_updatePercent = r->percent;
        break;
    case Update::Result::Kind::Downloaded:
    {
        // The update closes FastFiles: running copies / moves are asked about first (as closing the window)
        m_quitting = true;
        std::wstring error;
        if (!ConfirmCloseWithOperations())
        {
            DeleteFileW(r->file.c_str());
            m_updateStage = UpdateStage::Available;
            break;
        }
        if (Update::Apply(m_hwnd, r->file, error))
        {
            Update::TestLog(L"stage applied");
            ExitApp();
            return;
        }
        m_quitting = false;
        DeleteFileW(r->file.c_str());
        m_updateError = error;
        m_updateStage = UpdateStage::Failed;
        break;
    }
    case Update::Result::Kind::Failed:
        m_updateError = r->error;
        m_updateStage = UpdateStage::Failed;
        break;
    }
    Update::TestLog(L"status " + UpdateCheckText());
    ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
}