#include "MainWindow.h"
#include "Lang.h"
#include "Sandbox.h"

// Files' customizable toolbar (UserControls/Toolbar.xaml.cs): the left part of the command bar is built from the user's
// entries per context; a context shows while one of its commands can run. The right part (filter ... more) is fixed.

namespace
{
using namespace ToolbarModel;
enum : int { MenuCustomize = 1, MenuUnpin };
}

std::wstring MainWindow::CurrentFolderPath(Tab* tab)
{
    return tab ? PidlName(tab->Location(), SIGDN_FILESYSPATH) : std::wstring();
}

void MainWindow::BuildToolbarCatalog()
{
    if (!Catalog().empty())
        return;
    // Glyphs for commands from the shortcut table (Files' RichGlyph of the same action); none = label only
    static const std::pair<int, const wchar_t*> glyphs[] = {
        { IdBack, Glyph::Back }, { IdForward, Glyph::Forward }, { IdUp, Glyph::Up }, { IdRefresh, Glyph::Refresh },
        { ActGoHome, Glyph::Home }, { ActFocusSearch, Glyph::Search }, { IdNewTab, Glyph::Add }, { ActNewWindow, L"\xE78B" },
        { ActOpenNewWindow, L"\xE8A7" }, { ActFullScreen, L"\xE740" }, { ActToggleCompact, L"\xE944" }, { ActDualPane, Glyph::DualPane },
        { ActSplitVertical, Glyph::DualPane }, { ActSplitHorizontal, L"\xE89B" }, { ActClosePane, Glyph::Close },
        { ActDetailsPane, Glyph::Pane }, { ActPreviewPane, L"\xE8FF" }, { ActNewFolder, Glyph::Folder }, { ActCopyPath, L"\xE62F" },
        { ActCopyQuoted, L"\xE62F" }, { ActPasteInto, Glyph::Paste }, { ActClassicProperties, Glyph::Properties },
        { ActEmptyBin, Glyph::Delete }, { ActRestoreAllBin, Glyph::RestoreDeleted }, { ActRestoreFromBin, Glyph::RestoreDeleted },
        { ActTerminal, L"\xE756" }, { ActTerminalAdmin, L"\xE756" },
        { ActLayoutDetails, L"\xE8FD" }, { ActLayoutList, L"\xEA37" }, { ActLayoutTiles, L"\xECA5" }, { ActLayoutLarge, L"\xE91B" },
        { ActLayoutColumns, L"\xE89A" }, { ActLayoutAdaptive, Glyph::Layout }, { ActZoomIn, L"\xE8A3" }, { ActZoomOut, L"\xE71F" },
        { ActToggleHidden, L"\xE7B3" }, { BtnFilter, Glyph::Filter }, { BtnSidebar, Glyph::Sidebar }, { ActPalette, Glyph::Keyboard },
        { ActSettings, Glyph::Settings }, { ActToggleTheme, Glyph::Palette }, { ActGitMenu, Glyph::Branch },
        { ActEditInNotepad, Glyph::Rename }, { ActRunWithPowershell, L"\xE756" }, { ActPlayAll, L"\xE768" },
        { ActInstallDriver, L"\xE9F5" }, { ActInstallCertificate, L"\xEB95" }, { ActCustomizeToolbar, L"\xE70F" },
        { ActHelp, L"\xE897" }, { ActOpenSettingsFile, L"\xE8DA" },
        { ActNewFile, L"\xE7C3" }, { ActNewShortcut, L"\xE71B" }, { ActAddItem, Glyph::Add }, { ActCreateFolderWithSelection, Glyph::Folder } };
    std::vector<Command> list;
    list.push_back({ L"separator", Tr(L"分隔符"), nullptr, nullptr, 0 });
    // The default toolbar's own buttons first (Files: groups, then its command list)
    list.push_back({ L"new", Tr(L"新建"), Glyph::Add, Tr(L"文件"), BtnNew, true });
    for (const Command& c : std::initializer_list<Command>{
             { L"cut", Tr(L"剪切"), Glyph::Cut, Tr(L"文件"), BtnCut }, { L"copy", Tr(L"复制"), Glyph::Copy, Tr(L"文件"), BtnCopy },
             { L"paste", Tr(L"粘贴"), Glyph::Paste, Tr(L"文件"), BtnPaste }, { L"rename", Tr(L"重命名"), Glyph::Rename, Tr(L"文件"), BtnRename },
             { L"share", Tr(L"共享"), Glyph::Share, Tr(L"文件"), BtnShare }, { L"delete", Tr(L"删除"), Glyph::Delete, Tr(L"文件"), BtnDelete },
             { L"properties", Tr(L"属性"), Glyph::Properties, Tr(L"文件"), BtnProperties },
             { L"runas", Tr(L"以管理员身份运行"), Glyph::Admin, Tr(L"文件"), BtnRunAs },
             { L"rotateleft", Tr(L"向左旋转"), Glyph::RotateLeft, Tr(L"文件"), BtnRotateLeft },
             { L"rotateright", Tr(L"向右旋转"), Glyph::RotateRight, Tr(L"文件"), BtnRotateRight },
             { L"wallpaper", Tr(L"设置为桌面背景"), Glyph::Wallpaper, Tr(L"文件"), BtnWallpaper },
             { L"installfont", Tr(L"安装字体"), Glyph::Font, Tr(L"文件"), BtnInstallFont } })
        list.push_back(c);
    for (const auto& def : Commands())
    {
        if (std::any_of(list.begin(), list.end(), [&](const Command& c) { return std::wstring(c.key) == def.key; }))
            continue;
        const wchar_t* glyph = nullptr;
        for (const auto& [id, g] : glyphs)
            if (id == def.id)
                glyph = g;
        list.push_back({ def.key, def.name, glyph, def.category, def.id });
    }
    SetCatalog(std::move(list));
}
MainWindow::BarState MainWindow::CurrentBarState() const
{
    BarState s;
    Tab* tab = Active();
    int items = 0;
    if (tab)
        tab->GetCounts(items, s.selected);
    s.fs = !CurrentFolderPath(tab).empty();
    s.clip = Sandbox::Active() ? Sandbox::HasPasteFiles()
        : IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    s.bin = m_inRecycleBin;
    s.binItems = m_inRecycleBin && items > 0;
    s.any = m_selKinds;
    s.all = m_selAll;
    return s;
}

bool MainWindow::CanRun(int command, const BarState& s)
{
    const bool single = s.selected == 1, some = s.selected > 0;
    const bool files = some && !s.bin; // Files: not in the recycle bin
    auto every = [&](unsigned kind) { return files && (s.all & kind) != 0; };
    switch (command)
    {
    case BtnNew: case ActNewFolder: case ActNewFile: case ActNewShortcut: case ActAddItem: return s.fs && !s.bin;
    case BtnCut: case BtnCopy: case BtnDelete: case BtnRename: return some;
    case BtnShare: return every(SelShareable); // Files' ShareItemAction: every selected item shareable, not in the bin
    case BtnPaste: return s.fs && s.clip;
    case BtnRunAs: return single && every(SelRunnable);
    case BtnRotateLeft: case BtnRotateRight: return every(SelImage);
    case BtnWallpaper: return single && every(SelImage);
    case BtnInstallFont: return every(SelFont);
    case ActEditInNotepad: return every(SelBatch);
    case ActRunWithPowershell: return single && every(SelPowerShell);
    case ActPlayAll: return every(SelMedia);
    case ActInstallDriver: return single && every(SelInf);
    case ActInstallCertificate: return every(SelCertificate);
    case ActEmptyBin: return !s.bin || s.binItems;
    case ActRestoreAllBin: return s.bin && s.binItems;
    case ActRestoreFromBin: return s.bin && some;
    case ActCopyPath: case ActCopyQuoted: return some;
    case ActLaunchPreviewPopup: return single;
    }
    return true;
}

unsigned MainWindow::ActiveContexts(const BarState& s)
{
    // Files' GetActiveToolbarContexts: a context is active while any of its commands is executable
    unsigned active = 1u << Always;
    auto on = [&](int context, std::initializer_list<int> commands)
    {
        for (int c : commands)
            if (CanRun(c, s))
            {
                active |= 1u << context;
                return;
            }
    };
    on(Batch, { ActEditInNotepad });
    on(PowerShell, { ActRunWithPowershell });
    on(Image, { BtnWallpaper, BtnRotateLeft, BtnRotateRight });
    on(Media, { ActPlayAll });
    on(Font, { BtnInstallFont });
    on(Driver, { ActInstallDriver });
    on(Certificate, { ActInstallCertificate });
    if (s.bin) // the recycle bin context also needs the recycle bin page
        on(RecycleBin, { ActEmptyBin, ActRestoreAllBin, ActRestoreFromBin });
    if (active == (1u << Always))
        active |= 1u << Other;
    return active;
}

void MainWindow::LoadToolbarLists()
{
    BuildToolbarCatalog();
    // Before 0.9.8 buttons were switched off one by one: those become the "always visible" list without them
    if (!m_settings.toolbarHidden.empty())
    {
        if (m_settings.toolbar[Always].empty())
        {
            std::vector<Entry> entries = Defaults(Always);
            std::erase_if(entries, [&](const Entry& e) { return m_settings.Has(m_settings.toolbarHidden, e.key.c_str()); });
            m_settings.toolbar[Always] = Format(entries, Always);
        }
        m_settings.toolbarHidden.clear();
        SaveSettings(m_settings);
    }
    if (m_toolbarLoaded && m_toolbarApplied == m_settings.toolbar)
        return;
    m_toolbarLoaded = true;
    m_toolbarApplied = m_settings.toolbar;
    for (int c = 0; c < ContextCount; ++c)
    {
        m_toolbarLists[c] = Parse(m_settings.toolbar[c], c);
        if (m_toolbarLists[c].size() > static_cast<size_t>(kCustomStride))
            m_toolbarLists[c].resize(kCustomStride);
    }
    m_barContexts = ~0u; // rebuilt by the next UpdateCommandStates
    m_barKey = -1;
    UpdateCommandStates();
}

void MainWindow::RebuildCommandBar(unsigned contexts)
{
    using I = ButtonBar::Item;
    m_barContexts = contexts;
    auto tip = [this](const Command& c) -> std::wstring
    {
        static const std::pair<int, const wchar_t*> tips[] = {
            { BtnNew, Tr(L"新建文件夹 (Ctrl+Shift+N) 或文本文档") }, { BtnCut, Tr(L"剪切 (Ctrl+X)") }, { BtnCopy, Tr(L"复制 (Ctrl+C)") },
            { BtnPaste, Tr(L"粘贴 (Ctrl+V)") }, { BtnRename, Tr(L"重命名 (F2)") }, { BtnDelete, Tr(L"删除 (Del)，按住 Shift 永久删除") },
            { BtnProperties, Tr(L"属性 (Alt+Enter)") } };
        for (const auto& [id, text] : tips)
            if (id == c.id)
                return text;
        if (const CommandDef* def = FindCommand(c.key))
            if (const std::wstring keys = ChordsText(*def); !keys.empty())
                return std::wstring(c.name) + L" (" + keys + L")";
        return c.name;
    };
    std::vector<I> items;
    for (int c = 0; c < ContextCount; ++c)
    {
        if (!(contexts & (1u << c)) || m_toolbarLists[c].empty())
            continue;
        if (c != Always)
            items.push_back({ 0 });
        for (auto& item : BarItems(m_toolbarLists[c], kCustomBase + c * kCustomStride, tip))
            items.push_back(std::move(item));
    }
    // Fixed right side (Files' toolbar keeps these outside the customizable command bar)
    for (I item : { I{ -1 }, I{ BtnFilter, Glyph::Filter, L"", Tr(L"筛选 (Ctrl+Shift+F)") },
             I{ BtnSelect, Glyph::Select, Tr(L"选择"), Tr(L"全选、反选、取消选择"), true }, I{ BtnSort, Glyph::Sort, Tr(L"排序"), Tr(L"排序和分组"), true },
             I{ BtnLayout, Glyph::Layout, Tr(L"布局"), Tr(L"布局和显示选项 (Ctrl+Shift+1…6)"), true },
             I{ BtnDualPane, Glyph::DualPane, L"", Tr(L"双窗格 (Ctrl+Shift+S)，Alt+Shift+V 左右 / Alt+Shift+H 上下") },
             I{ BtnInfoPane, Glyph::Pane, L"", Tr(L"详情/预览窗格 (Ctrl+Alt+I / Alt+P)") }, I{ BtnMore, Glyph::More, L"", Tr(L"更多"), true } })
        items.push_back(std::move(item));
    m_cmd.SetItems(std::move(items));
    UpdateToggleButtons();
    InvalidateRect(m_cmd.Hwnd(), nullptr, FALSE);
}

int MainWindow::CustomCommand(int barId) const
{
    const int offset = barId - kCustomBase;
    if (offset < 0 || offset >= ContextCount * kCustomStride)
        return 0;
    const auto& list = m_toolbarLists[offset / kCustomStride];
    const int index = offset % kCustomStride;
    if (index >= static_cast<int>(list.size()))
        return 0;
    const Command* c = Find(list[index].key);
    return c ? c->id : 0;
}

void MainWindow::UpdateToggleButtons()
{
    // Files' AppBarToggleButton: IsChecked follows the command's IsOn, on the fixed buttons and on pinned copies alike
    const TabSlot* slot = m_active >= 0 && m_active < static_cast<int>(m_tabList.size()) ? &m_tabList[m_active] : nullptr;
    auto isOn = [&](int command) -> int
    {
        switch (command)
        {
        case BtnFilter: return m_filterVisible;
        case BtnDualPane: case ActDualPane: return slot && slot->Split();
        case BtnInfoPane: return m_infoVisible;
        case BtnSidebar: return m_sidebarVisible;
        case ActToggleHidden: return m_settings.showHidden;
        case ActToggleDotFiles: return m_settings.showDotFiles;
        case ActToggleCheckboxes: return m_prefs.checkboxes;
        case ActToggleCompact: return m_compact;
        case ActFullScreen: return m_fullScreen;
        }
        return -1; // not a toggle
    };
    for (int id : { BtnFilter, BtnDualPane, BtnInfoPane })
        m_cmd.SetChecked(id, isOn(id) == 1);
    for (int c = 0; c < ContextCount; ++c)
        for (int i = 0; i < static_cast<int>(m_toolbarLists[c].size()); ++i)
        {
            const int barId = kCustomBase + c * kCustomStride + i;
            if (const int on = isOn(CustomCommand(barId)); on >= 0)
                m_cmd.SetChecked(barId, on == 1);
        }
}

void MainWindow::CustomizeToolbar()
{
    LoadToolbarLists();
    Lists lists = m_toolbarLists;
    if (!ShowToolbarDialog(m_hwnd, &m_theme, lists, Always))
        return;
    for (int c = 0; c < ContextCount; ++c)
        m_settings.toolbar[c] = Format(lists[c], c);
    SaveAndBroadcast();
    LoadToolbarLists();
}

void MainWindow::UnpinToolbarItem(int barId)
{
    const int offset = barId - kCustomBase;
    if (offset < 0 || offset >= ContextCount * kCustomStride)
        return;
    const int c = offset / kCustomStride, index = offset % kCustomStride;
    auto list = m_toolbarLists[c];
    if (index >= static_cast<int>(list.size()))
        return;
    list.erase(list.begin() + index);
    m_settings.toolbar[c] = Format(list, c);
    SaveAndBroadcast();
    LoadToolbarLists();
}

void MainWindow::OnBarContext(int id, const RECT& screen)
{
    if (id == IdBack || id == IdForward)
    {
        if (m_nav.IsEnabled(id))
            ShowHistoryMenu(id == IdBack, screen);
        return;
    }
    // Files: the toolbar's empty part offers 自定义工具栏; a pinned button adds 取消固定
    POINT pt{ screen.left, screen.top };
    RECT bar{};
    GetWindowRect(m_cmd.Hwnd(), &bar);
    if (!PtInRect(&bar, pt))
        return; // another bar (navigation, settings button)
    if (!IsRectEmpty(&screen))
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        pt = PtInRect(&screen, cursor) ? cursor : POINT{ screen.left, screen.bottom };
    }
    const bool custom = CustomCommand(id) != 0;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MenuCustomize, Tr(L"自定义工具栏"));
    if (custom)
        AppendMenuW(menu, MF_STRING, MenuUnpin, Tr(L"取消固定"));
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == MenuCustomize)
        CustomizeToolbar();
    else if (cmd == MenuUnpin)
        UnpinToolbarItem(id);
}
