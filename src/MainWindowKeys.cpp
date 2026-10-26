#include "MainWindow.h"
#include "Lang.h"
#include "ContentDialog.h"

// Every command a shortcut or the command palette can run (Files' Actions/*: ~150 there, these are the ones FastFiles has).
// Defaults follow Files' keyboard shortcuts page; users rebind them in Settings > 快捷键 (key.<key>= in settings.txt).

const std::vector<MainWindow::CommandDef>& MainWindow::Commands()
{
    static const std::vector<CommandDef> list = {
        { IdBack, L"back", Tr(L"后退"), Tr(L"导航"), L"Alt+Left", 0 },
        { IdForward, L"forward", Tr(L"前进"), Tr(L"导航"), L"Alt+Right", 0 },
        { IdUp, L"up", Tr(L"上一级"), Tr(L"导航"), L"Alt+Up", 0 },
        { IdRefresh, L"refresh", Tr(L"刷新"), Tr(L"导航"), L"F5,Ctrl+R", 0 },
        { ActGoHome, L"home", Tr(L"打开主页"), Tr(L"导航"), L"Alt+Home", 0 },
        { ActFocusAddress, L"address", Tr(L"编辑路径"), Tr(L"导航"), L"Ctrl+L,Alt+D,F4", 0 },
        { ActFocusSearch, L"search", Tr(L"搜索"), Tr(L"导航"), L"Ctrl+F,F3", 0 },

        { IdNewTab, L"newtab", Tr(L"新建标签页"), Tr(L"标签页和窗口"), L"Ctrl+T", 0 },
        { ActCloseTab, L"closetab", Tr(L"关闭标签页"), Tr(L"标签页和窗口"), L"Ctrl+W,Ctrl+F4", 0 },
        { ActReopenTab, L"reopentab", Tr(L"重新打开关闭的标签页"), Tr(L"标签页和窗口"), L"Ctrl+Shift+T", 0 },
        { ActDuplicateTab, L"duplicatetab", Tr(L"复制标签页"), Tr(L"标签页和窗口"), L"Ctrl+Shift+K", 0 },
        { ActNextTab, L"nexttab", Tr(L"下一个标签页"), Tr(L"标签页和窗口"), L"Ctrl+Tab", 0 },
        { ActPrevTab, L"prevtab", Tr(L"上一个标签页"), Tr(L"标签页和窗口"), L"Ctrl+Shift+Tab", 0 },
        { ActNewWindow, L"newwindow", Tr(L"新建窗口"), Tr(L"标签页和窗口"), L"Ctrl+N", 0 },
        { ActCloseWindow, L"closewindow", Tr(L"关闭所有标签页"), Tr(L"标签页和窗口"), L"Ctrl+Shift+W", 0 },
        { ActOpenNewWindow, L"openinnewwindow", Tr(L"在新窗口中打开"), Tr(L"标签页和窗口"), L"Ctrl+Alt+Enter", 0 },
        { ActFullScreen, L"fullscreen", Tr(L"全屏"), Tr(L"标签页和窗口"), L"F11", 0 },
        { ActToggleCompact, L"togglecompact", Tr(L"开关精简模式"), Tr(L"标签页和窗口"), L"F12", 0 },
        { ActEnterCompact, L"entercompact", Tr(L"进入精简模式"), Tr(L"标签页和窗口"), L"Ctrl+Alt+Up", 0 },
        { ActExitCompact, L"exitcompact", Tr(L"退出精简模式"), Tr(L"标签页和窗口"), L"Ctrl+Alt+Down", 0 },

        { ActDualPane, L"dualpane", Tr(L"双窗格"), Tr(L"窗格"), L"Ctrl+Shift+S", 0 },
        { ActSplitVertical, L"splitvertical", Tr(L"左右分割窗格"), Tr(L"窗格"), L"Alt+Shift+V", 0 },
        { ActSplitHorizontal, L"splithorizontal", Tr(L"上下分割窗格"), Tr(L"窗格"), L"Alt+Shift+H", 0 },
        { ActClosePane, L"closepane", Tr(L"关闭当前窗格"), Tr(L"窗格"), L"Ctrl+Alt+W", 0 },
        { ActFocusLeftPane, L"focusleftpane", Tr(L"切换到第一个窗格"), Tr(L"窗格"), L"Ctrl+Shift+Left", CmdNotInEdit },
        { ActFocusRightPane, L"focusrightpane", Tr(L"切换到第二个窗格"), Tr(L"窗格"), L"Ctrl+Shift+Right", CmdNotInEdit },
        { ActOpenOtherPane, L"openinotherpane", Tr(L"在另一个窗格中打开"), Tr(L"窗格"), L"", 0 },
        { ActDetailsPane, L"detailspane", Tr(L"详情窗格"), Tr(L"窗格"), L"Ctrl+Alt+I", 0 },
        { ActPreviewPane, L"previewpane", Tr(L"预览窗格"), Tr(L"窗格"), L"Ctrl+Alt+P,Alt+P", 0 },
        { ActLaunchPreviewPopup, L"previewpopup", Tr(L"预览弹窗"), Tr(L"窗格"), L"", 0 }, // Files: Space, which stays with the list (MainWindowInput)

        { ActNewFolder, L"newfolder", Tr(L"新建文件夹"), Tr(L"文件"), L"Ctrl+Shift+N", 0 },
        { ActNewFile, L"newfile", Tr(L"新建文件"), Tr(L"文件"), L"", 0 },
        { ActNewShortcut, L"newshortcut", Tr(L"创建快捷方式"), Tr(L"文件"), L"", 0 },
        { ActCreateFolderWithSelection, L"createfolderwithselection", Tr(L"用选中的项目创建文件夹"), Tr(L"文件"), L"", 0 },
        { ActAddItem, L"additem", Tr(L"新建"), Tr(L"文件"), L"Ctrl+Shift+I", 0 },
        { ActCopyPath, L"copypath", Tr(L"复制路径"), Tr(L"文件"), L"Ctrl+Shift+C", 0 },
        { ActCopyQuoted, L"copyquoted", Tr(L"复制带引号的路径"), Tr(L"文件"), L"Ctrl+Alt+C", 0 },
        // Files' PasteItem / DeleteItem / DeleteItemPermanently: run by FastFiles (status center) while the list has the focus
        { ActPaste, L"paste", Tr(L"粘贴"), Tr(L"文件"), L"Ctrl+V,Shift+Insert", CmdNotInEdit | CmdInView },
        { ActDelete, L"delete", Tr(L"删除"), Tr(L"文件"), L"Delete,Ctrl+D", CmdNotInEdit | CmdInView },
        { ActDeletePermanently, L"deletepermanently", Tr(L"永久删除"), Tr(L"文件"), L"Shift+Delete", CmdNotInEdit | CmdInView },
        { ActPasteInto, L"pasteinto", Tr(L"粘贴到选中的文件夹"), Tr(L"文件"), L"Ctrl+Shift+V", CmdNotInEdit },
        { ActClassicProperties, L"properties", Tr(L"属性"), Tr(L"文件"), L"Alt+Shift+Enter", 0 },
        { ActEmptyBin, L"emptybin", Tr(L"清空回收站"), Tr(L"文件"), L"", 0 },
        { ActRestoreAllBin, L"restoreallbin", Tr(L"还原所有项目"), Tr(L"文件"), L"", 0 },
        { ActRestoreFromBin, L"restorebin", Tr(L"还原"), Tr(L"文件"), L"", 0 },
        { ActEditInNotepad, L"editinnotepad", Tr(L"在记事本中编辑"), Tr(L"文件"), L"", 0 },
        { ActRunWithPowershell, L"runwithpowershell", Tr(L"使用 PowerShell 运行"), Tr(L"文件"), L"", 0 },
        { ActPlayAll, L"playall", Tr(L"全部播放"), Tr(L"文件"), L"", 0 },
        { ActInstallDriver, L"installdriver", Tr(L"安装驱动程序"), Tr(L"文件"), L"", 0 },
        { ActInstallCertificate, L"installcertificate", Tr(L"安装证书"), Tr(L"文件"), L"", 0 },
        { ActOperations, L"operations", Tr(L"状态中心"), Tr(L"应用"), L"", 0 },
        { ActCancelOperations, L"canceloperations", Tr(L"取消所有文件操作"), Tr(L"应用"), L"", 0 },

        { ActTerminal, L"terminal", Tr(L"在终端中打开"), Tr(L"文件"), L"Ctrl+`", 0 },
        { ActTerminalAdmin, L"terminaladmin", Tr(L"以管理员身份打开终端"), Tr(L"文件"), L"Ctrl+Shift+`", 0 },

        { ActLayoutDetails, L"layoutdetails", Tr(L"布局：详细信息"), Tr(L"视图"), L"Ctrl+Shift+1", 0 },
        { ActLayoutList, L"layoutlist", Tr(L"布局：列表"), Tr(L"视图"), L"Ctrl+Shift+2", 0 },
        { ActLayoutTiles, L"layouttiles", Tr(L"布局：卡片"), Tr(L"视图"), L"Ctrl+Shift+3", 0 },
        { ActLayoutLarge, L"layoutlarge", Tr(L"布局：网格"), Tr(L"视图"), L"Ctrl+Shift+4", 0 },
        { ActLayoutColumns, L"layoutcolumns", Tr(L"布局：列视图"), Tr(L"视图"), L"Ctrl+Shift+5", 0 },
        { ActLayoutAdaptive, L"layoutadaptive", Tr(L"自适应布局"), Tr(L"视图"), L"Ctrl+Shift+6", 0 },
        { ActZoomIn, L"zoomin", Tr(L"增加大小"), Tr(L"视图"), L"Ctrl+Plus,Ctrl+NumAdd", 0 },
        { ActZoomOut, L"zoomout", Tr(L"减小大小"), Tr(L"视图"), L"Ctrl+Minus,Ctrl+NumSubtract", 0 },
        { ActToggleHidden, L"hidden", Tr(L"显示/隐藏隐藏项目"), Tr(L"视图"), L"Ctrl+H", CmdNotInEdit },
        { ActToggleDotFiles, L"dotfiles", Tr(L"显示以英文句点 (.) 开头的文件"), Tr(L"视图"), L"", 0 }, // Files' ToggleDotFilesSetting
        { ActToggleExtensions, L"extensions", Tr(L"显示/隐藏文件扩展名"), Tr(L"视图"), L"", 0 },
        { ActToggleCheckboxes, L"checkboxes", Tr(L"项目复选框"), Tr(L"视图"), L"", 0 },
        { BtnFilter, L"filter", Tr(L"筛选栏"), Tr(L"视图"), L"Ctrl+Shift+F", 0 },
        { BtnSidebar, L"sidebar", Tr(L"显示/隐藏侧边栏"), Tr(L"视图"), L"Ctrl+B", 0 },
        { ActToggleToolbar, L"toolbar", Tr(L"显示/隐藏命令栏"), Tr(L"视图"), L"Ctrl+Shift+B", 0 },
        { ActToggleStatusBar, L"statusbar", Tr(L"显示/隐藏状态栏"), Tr(L"视图"), L"", 0 },

        { ActPalette, L"palette", Tr(L"命令面板"), Tr(L"应用"), L"Ctrl+Shift+P", 0 },
        { ActCustomizeToolbar, L"customizetoolbar", Tr(L"自定义工具栏"), Tr(L"应用"), L"", 0 },
        { ActSettings, L"settings", Tr(L"设置"), Tr(L"应用"), L"Ctrl+,", 0 },
        { ActToggleTheme, L"theme", Tr(L"切换浅色/深色主题"), Tr(L"应用"), L"Ctrl+Alt+T", 0 },
        { ActGitMenu, L"git", Tr(L"Git 分支和操作"), Tr(L"应用"), L"", 0 },
        // Files' GitInit / GitClone (ActionCategory.Git)
        { ActGitInit, L"gitinit", Tr(L"初始化存储库"), Tr(L"Git"), L"", 0 },
        { ActGitClone, L"gitclone", Tr(L"克隆存储库"), Tr(L"Git"), L"", 0 },
        { ActOpenSettingsFile, L"settingsfile", Tr(L"编辑配置文件"), Tr(L"应用"), L"Ctrl+Shift+,", 0 },
        { ActHelp, L"help", Tr(L"帮助"), Tr(L"应用"), L"F1", 0 },
        { ActOpenLog, L"log", Tr(L"打开日志文件"), Tr(L"应用"), L"Ctrl+.", 0 },
        { ActOpenLogFolder, L"logfolder", Tr(L"打开日志文件夹"), Tr(L"应用"), L"Ctrl+Shift+.", 0 },
        { ActExitApp, L"exit", Tr(L"退出 FastFiles"), Tr(L"应用"), L"", 0 },
    };
    return list;
}

const MainWindow::CommandDef* MainWindow::FindCommand(const std::wstring& key)
{
    for (const auto& def : Commands())
        if (key == def.key)
            return &def;
    return nullptr;
}

std::vector<uint32_t> MainWindow::EffectiveChords(const CommandDef& def) const
{
    if (const auto* custom = m_settings.Override(def.key))
        return *custom;
    return DefaultChords(def);
}

std::vector<uint32_t> MainWindow::DefaultChords(const CommandDef& def)
{
    std::vector<uint32_t> chords;
    std::wstring text = def.defaults;
    for (size_t start = 0; start <= text.size();)
    {
        const size_t comma = text.find(L',', start);
        const std::wstring part = text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (const uint32_t code = ParseShortcut(part))
            chords.push_back(code);
        if (comma == std::wstring::npos)
            break;
        start = comma + 1;
    }
    return chords;
}

std::wstring MainWindow::ChordsText(const CommandDef& def) const
{
    std::wstring text;
    for (uint32_t code : EffectiveChords(def))
        text += (text.empty() ? L"" : L"  ") + FormatShortcut(code);
    return text;
}

void MainWindow::RebuildKeys()
{
    m_keys.clear();
    for (const auto& def : Commands())
        for (uint32_t code : EffectiveChords(def))
            if (std::none_of(m_keys.begin(), m_keys.end(), [&](const auto& k) { return k.first == code; }))
                m_keys.emplace_back(code, def.id); // first command in table order wins a duplicate
}

bool MainWindow::RunAction(int id)
{
    Tab* tab = Active();
    TabSlot* slot = ActiveSlot();
    const int count = static_cast<int>(m_tabList.size());
    switch (id)
    {
    case IdBack: if (tab) tab->Back(); return true;
    case IdForward: if (tab) tab->Forward(); return true;
    case IdUp: if (tab) tab->Up(); return true;
    case IdRefresh: if (tab) tab->Refresh(); return true;
    case IdNewTab: OnTabNew(); return true;
    case ActCloseTab: CloseTab(m_active); return true;
    case ActReopenTab: ReopenClosedTab(); return true;
    case ActDuplicateTab: if (tab) NewTab(tab->Location(), true); return true;
    case ActNextTab: if (count > 0) ActivateTab((m_active + 1) % count); return true;
    case ActPrevTab: if (count > 0) ActivateTab((m_active + count - 1) % count); return true;
    case ActNewWindow: OpenNewWindow(DefaultLocation().get()); return true;
    case ActCloseWindow: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); return true;
    case ActOpenNewWindow: OpenSelectionInNewWindow(); return true;
    case ActLaunchPreviewPopup:
        // Files' LaunchPreviewPopupAction: one selected item; nothing happens without a provider
        if (tab && tab->SelectedCount() == 1)
            TryQuickLook();
        return true;
    case ActNewFolder: // Files' CreateFolder: the name prompt, then the folder
    case ActNewFile:
    case ActNewShortcut:
    case ActAddItem:
        if (CurrentFolderPath(tab).empty())
        {
            SetStatusText(Tr(L"无法在这里新建"), true);
            return true;
        }
        if (id == ActAddItem)
            ShowAddItemDialog();
        else if (id == ActNewShortcut)
            CreateShortcutFromDialog();
        else
            CreateItemFromDialog(id == ActNewFolder, nullptr);
        return true;
    case ActCopyPath: CopyPath(); return true;
    case ActToggleExtensions: ToggleExtensions(); return true;
    case ActFocusAddress: FocusAddress(); return true;
    case ActFocusSearch: FocusSearch(); return true;
    case ActTerminal: OpenTerminal(false); return true;
    case ActTerminalAdmin: OpenTerminal(true); return true;
    case ActPasteInto: PasteIntoSelection(); return true;
    case ActClassicProperties: if (tab) tab->InvokeVerb("properties", tab->SelectedCount() == 0); return true;
    case ActEmptyBin: EmptyRecycleBin(); return true;
    case ActRestoreAllBin: RestoreFromRecycleBin(true); return true;
    case ActRestoreFromBin: RestoreFromRecycleBin(false); return true;
    case ActLayoutDetails: SetLayoutShortcut(0); return true;
    case ActLayoutList: SetLayoutShortcut(1); return true;
    case ActLayoutTiles: SetLayoutShortcut(2); return true;
    case ActLayoutLarge: SetLayoutShortcut(3); return true;
    case ActLayoutColumns: SetLayoutShortcut(4); return true;
    case ActLayoutAdaptive: SetLayoutShortcut(5); return true;
    case ActFocusLeftPane: case ActFocusRightPane:
        if (!slot || !slot->Split())
            return false; // Ctrl+Shift+arrows stay with the list (extend selection)
        SetPaneFocus(id == ActFocusLeftPane ? 0 : 1, true);
        return true;
    case ActClosePane:
        if (slot && slot->Split())
            ClosePane(slot->focus);
        return true;
    case ActSettings: OpenSettings(); return true;
    case ActPalette: OpenPalette(); return true;
    case ActToggleTheme:
        m_settings.theme = m_theme.dark ? AppSettings::ThemeMode::Light : AppSettings::ThemeMode::Dark;
        SaveAndBroadcast();
        ApplyTheme(false);
        ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
        return true;
    case ActExitApp: ExitApp(); return true;
    case ActHelp: // Files: the online documentation in the browser
        ShellExecuteW(m_hwnd, nullptr, L"https://files.community/docs", nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    case ActOpenLog:
    case ActOpenLogFolder:
    {
        const std::wstring log = Log::Path();
        if (id == ActOpenLogFolder)
        {
            // Opened here, like the settings page's 数据文件夹
            PIDLIST_ABSOLUTE raw = nullptr;
            std::wstring dir = AppDataFile(L"");
            if (!dir.empty() && SUCCEEDED(SHParseDisplayName(dir.c_str(), nullptr, &raw, 0, nullptr)))
            {
                UniquePidl pidl(raw);
                NewTab(pidl.get(), true);
            }
        }
        else if (GetFileAttributesW(log.c_str()) == INVALID_FILE_ATTRIBUTES ||
                 reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, nullptr, log.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            // Files' OpenLogFileAction error dialog: FailedToOpenLogFile as the title, the exception message as content (none here)
            ContentDialog::Notice(m_hwnd, &m_theme, Tr(L"无法打开日志文件"), {});
        return true;
    }
    case ActOpenSettingsFile: // Files: the settings file in its default app
    {
        const std::wstring path = SettingsPath();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            ShellExecuteW(m_hwnd, nullptr, L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    case ActOperations:
        if (!m_center.Wanted())
            return false;
        m_center.Toggle();
        return true;
    case ActCancelOperations: m_ops.CancelAll(); m_center.Refresh(); return true;
    case ActPaste: PasteHere(); return true;
    case ActDelete: DeleteSelection(false); return true;
    case ActDeletePermanently: DeleteSelection(true); return true;
    case ActGitMenu:
        if (m_gitRoot.empty())
            return false;
        ShowGitMenu();
        return true;
    case ActToggleStatusBar:
        m_settings.statusBar = !m_settings.statusBar;
        SaveAndBroadcast();
        ApplySettings();
        return true;
    case ActToggleHidden: ToggleHidden(); return true;
    case ActToggleDotFiles: ToggleDotFiles(); return true;
    case ActCreateFolderWithSelection: CreateFolderWithSelection(); return true;
    case ActGitInit:
        if (GitFolder().size() <= 3)
            return false; // Files: not in a repository, not a drive root
        GitInit();
        return true;
    case ActGitClone:
        if (GitFolder().empty())
            return false; // Files: CanCreateItem && !IsGitRepository
        GitClone();
        return true;
    case ActToggleCheckboxes: ToggleCheckboxes(); return true;
    case ActToggleToolbar: ToggleToolbar(); return true;
    case ActFullScreen: ToggleFullScreen(); return true;
    case ActToggleCompact: SetCompactOverlay(!m_compact); return true;
    case ActEnterCompact: SetCompactOverlay(true); return true;  // Files: IsExecutable only when not compact
    case ActExitCompact: SetCompactOverlay(false); return true;
    case ActCopyQuoted: CopyPathQuoted(); return true;
    case ActZoomIn: case ActZoomOut: if (tab) tab->StepIconSize(id == ActZoomIn ? 1 : -1); return true;
    case ActDetailsPane: ToggleInfoPane(0); return true;
    case ActPreviewPane: ToggleInfoPane(1); return true;
    case ActDualPane: ToggleDualPane(); return true;
    case ActSplitVertical: SplitPanes(false); return true;
    case ActSplitHorizontal: SplitPanes(true); return true;
    case ActFocusOtherPane:
        if (!slot || !slot->Split())
            return false;
        SetPaneFocus(1 - slot->focus, true);
        return true;
    case ActOpenOtherPane: OpenSelectionInOtherPane(); return true;
    case ActGoHome: if (tab) tab->Navigate(HomePidl()); return true;
    case ActCustomizeToolbar: CustomizeToolbar(); return true;
    case ActEditInNotepad: case ActRunWithPowershell: case ActPlayAll: case ActInstallDriver: case ActInstallCertificate:
    {
        UpdateCommandStates();
        if (!tab || !CanRun(id, CurrentBarState()))
            return false;
        const std::vector<std::wstring> paths = tab->SelectedFileSystemPaths();
        auto quoted = [](const std::wstring& path) { return L"\"" + path + L"\""; };
        if (id == ActEditInNotepad) // Files: notepad.exe once per selected file
            for (const auto& path : paths)
                ShellExecuteW(m_hwnd, nullptr, L"notepad.exe", quoted(path).c_str(), nullptr, SW_SHOWNORMAL);
        else if (id == ActRunWithPowershell && paths.size() == 1)
        {
            // Files: powershell.exe "& '<script>'" in the current folder
            std::wstring literal = paths[0];
            for (size_t p = literal.find(L'\''); p != std::wstring::npos; p = literal.find(L'\'', p + 2))
                literal.insert(p, 1, L'\'');
            const std::wstring folder = CurrentFolderPath(tab);
            ShellExecuteW(m_hwnd, nullptr, L"powershell.exe", (L"& '" + literal + L"'").c_str(), folder.empty() ? nullptr : folder.c_str(),
                SW_SHOWNORMAL);
        }
        else if (id == ActPlayAll) // Files: opens the selected items together with their app
            tab->InvokeVerb("open", false);
        else if (id == ActInstallDriver && paths.size() == 1) // Files: InfDefaultInstall.exe, elevated
            ShellExecuteW(m_hwnd, L"runas", L"InfDefaultInstall.exe", quoted(paths[0]).c_str(), nullptr, SW_HIDE);
        else if (id == ActInstallCertificate) // Files: the certificate's "add" verb
            tab->InvokeVerb("add", false);
        return true;
    }
    }
    if (id >= BtnNew && id < BtnLast)
    {
        RunCommand(id);
        return true;
    }
    return false;
}

// --- Command palette (omnibar in ">" mode) ---

void MainWindow::OpenPalette()
{
    FocusAddress();
    SetWindowTextW(m_address, L">");
    SendMessageW(m_address, EM_SETSEL, 1, 1);
    UpdatePalette();
}

void MainWindow::UpdatePalette()
{
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(m_address)) + 1, L'\0');
    text.resize(GetWindowTextW(m_address, text.data(), static_cast<int>(text.size())));
    if (GetFocus() != m_address || text.empty() || text[0] != L'>')
    {
        if (m_paletteOpen)
            ClosePalette(false);
        return;
    }
    if (!m_paletteOpen)
    {
        std::vector<CommandPalette::Entry> entries;
        for (const auto& def : Commands())
            entries.push_back({ def.id, def.name, def.category, ChordsText(def) });
        m_palette.SetEntries(std::move(entries));
        RECT below = m_omni;
        MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&below), 2);
        m_palette.Show(below);
        m_paletteOpen = true;
    }
    const size_t first = text.find_first_not_of(L"> \t");
    m_palette.Filter(first == std::wstring::npos ? std::wstring() : text.substr(first));
}

void MainWindow::ClosePalette(bool restoreChrome)
{
    m_paletteOpen = false;
    m_palette.Hide();
    if (restoreChrome)
    {
        UpdateChrome();
        if (Tab* tab = Active())
            tab->Focus();
    }
}
