#include "MainWindow.h"
#include "Lang.h"
#include "Sandbox.h"

namespace
{
enum : int { MenuSelectAll = 3, MenuInvert, MenuClear, MenuLayoutBase = 40,
    MenuHidden = 60, MenuExtensions, MenuCheckboxes, MenuFilterBar, MenuToolbar, MenuZoomIn, MenuZoomOut, MenuInfoPane,
    MenuDualPane = 80, MenuSplitVertical, MenuSplitHorizontal, MenuClosePane, MenuStatusBar, MenuSortMenuBase = 100 };

}

namespace
{
// Offsets of the sort / group menu ids (MainWindow::SortMenu / GroupMenu): sort keys are their SortKey
enum : UINT { SortAsc = 6, SortDesc, GroupKeys = 8, GroupModifiedUnits = 15, GroupCreatedUnits = 18, GroupAsc = 21, GroupDesc,
    PriorityBase = 23, Unavailable = 31 };
}

// Files' SortBy flyout: 名称, 修改日期, 创建日期, 大小, 类型 (the background menu: 类型 before 大小), 同步状态, 标签, 路径,
// 原始文件夹, 删除日期 (each only where it applies: grayed in the toolbar, left out in the background menu), 升序 / 递减
HMENU MainWindow::SortMenu(Tab* tab, UINT base, bool background)
{
    HMENU menu = CreatePopupMenu();
    SortKey key = SortKey::Name;
    bool desc = false;
    const bool known = tab->GetSort(key, desc);
    auto sort = [&](SortKey k, const wchar_t* text)
    {
        const UINT flags = (known && key == k ? MF_CHECKED : 0) | (tab->CanSortBy(k) ? 0 : MF_GRAYED);
        if (!background || tab->CanSortBy(k))
            AppendMenuW(menu, MF_STRING | flags, base + static_cast<UINT>(k), Tr(text));
    };
    auto unavailable = [&](const wchar_t* text)
    {
        if (!background)
            AppendMenuW(menu, MF_STRING | MF_GRAYED, base + Unavailable, Tr(text));
    };
    sort(SortKey::Name, L"名称");
    sort(SortKey::Modified, L"修改日期");
    sort(SortKey::Created, L"创建日期");
    if (background)
        sort(SortKey::Type, L"类型");
    sort(SortKey::Size, L"大小");
    if (!background)
        sort(SortKey::Type, L"类型");
    unavailable(L"同步状态");
    sort(SortKey::Tag, L"标签");
    unavailable(L"路径");
    unavailable(L"原始文件夹");
    unavailable(L"删除日期");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (!desc ? MF_CHECKED : 0), base + SortAsc, Tr(L"升序"));
    AppendMenuW(menu, MF_STRING | (desc ? MF_CHECKED : 0), base + SortDesc, Tr(L"递减"));
    return menu;
}
// Files' GroupBy flyout: 无, 名称, 修改日期 ▸ 年 / 月 / 日, 创建日期 ▸, 大小, 类型 (background: 类型 before 大小), 同步状态, 标签,
// 原始文件夹, 删除日期 ▸, 文件夹路径, then 升序 / 递减 (only while grouped)
HMENU MainWindow::GroupMenu(Tab* tab, UINT base, bool background)
{
    HMENU menu = CreatePopupMenu();
    const Tab::GroupChoice g = tab->GetGroup();
    auto item = [&](int key, const wchar_t* text)
    {
        const bool can = tab->CanGroup(key);
        if (!background || can)
            AppendMenuW(menu, MF_STRING | (g.key == key ? MF_CHECKED : 0) | (can ? 0 : MF_GRAYED), base + GroupKeys + key, Tr(text));
    };
    auto dates = [&](int key, UINT units, const wchar_t* text)
    {
        HMENU sub = CreatePopupMenu();
        static const wchar_t* names[] = { L"年", L"月", L"日" };
        for (int u = 0; u < 3; ++u)
            AppendMenuW(sub, MF_STRING | (g.key == key && g.unit == u ? MF_CHECKED : 0), base + units + u, Tr(names[u]));
        AppendMenuW(menu, MF_POPUP | (g.key == key ? MF_CHECKED : 0), reinterpret_cast<UINT_PTR>(sub), Tr(text));
    };
    auto unavailable = [&](const wchar_t* text)
    {
        if (!background)
            AppendMenuW(menu, MF_STRING | MF_GRAYED, base + Unavailable, Tr(text));
    };
    item(FileList::kGroupNone, L"无");
    item(FileList::kGroupName, L"名称");
    dates(FileList::kGroupModified, GroupModifiedUnits, L"修改日期");
    dates(FileList::kGroupCreated, GroupCreatedUnits, L"创建日期");
    if (background)
        item(FileList::kGroupType, L"类型");
    item(FileList::kGroupSize, L"大小");
    if (!background)
        item(FileList::kGroupType, L"类型");
    unavailable(L"同步状态");
    item(FileList::kGroupTag, L"标签");
    unavailable(L"原始文件夹");
    if (!background)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, base + Unavailable, Tr(L"删除日期"));
    unavailable(L"文件夹路径");
    const bool grouped = g.key != FileList::kGroupNone;
    if (!background || grouped)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (grouped && !g.descending ? MF_CHECKED : 0) | (grouped ? 0 : MF_GRAYED), base + GroupAsc, Tr(L"升序"));
        AppendMenuW(menu, MF_STRING | (grouped && g.descending ? MF_CHECKED : 0) | (grouped ? 0 : MF_GRAYED), base + GroupDesc, Tr(L"递减"));
    }
    return menu;
}

// Files' SortFoldersFirst / SortFilesFirst / SortFilesAndFoldersTogether (the own list's 排序优先级)
void MainWindow::AppendSortPriority(HMENU menu, Tab* tab, UINT base)
{
    static const wchar_t* names[] = { L"先排序文件夹", L"先排序文件", L"将文件和文件夹一同排序" };
    const UINT can = tab->List() ? 0 : MF_GRAYED;
    const int on = tab->SortPriority(); // the folder's (Files' FolderSettings.SortDirectoriesAlongsideFiles / SortFilesFirst)
    for (int i = 0; i < 3; ++i)
        AppendMenuW(menu, MF_STRING | can | (on == i ? MF_CHECKED : 0), base + PriorityBase + i, Tr(names[i]));
}

bool MainWindow::RunSortCommand(Tab* tab, UINT offset)
{
    SortKey key = SortKey::Name;
    bool desc = false;
    tab->GetSort(key, desc);
    Tab::GroupChoice g = tab->GetGroup();
    if (offset <= static_cast<UINT>(SortKey::Created))
        tab->SetSort(static_cast<SortKey>(offset), desc);
    else if (offset == SortAsc || offset == SortDesc)
        tab->SetSort(key, offset == SortDesc);
    else if (offset >= GroupKeys && offset <= GroupKeys + FileList::kGroupTag)
    {
        g.key = static_cast<int>(offset - GroupKeys);
        tab->SetGroup(g);
    }
    else if (offset >= GroupModifiedUnits && offset < GroupModifiedUnits + 6)
    {
        g.key = offset < GroupCreatedUnits ? FileList::kGroupModified : FileList::kGroupCreated;
        g.unit = static_cast<int>(offset - (offset < GroupCreatedUnits ? GroupModifiedUnits : GroupCreatedUnits));
        tab->SetGroup(g);
    }
    else if (offset == GroupAsc || offset == GroupDesc)
    {
        g.descending = offset == GroupDesc;
        tab->SetGroup(g);
    }
    else if (offset >= PriorityBase && offset < PriorityBase + 3)
        tab->SetSortPriority(static_cast<int>(offset - PriorityBase)); // this folder's (synced: everyone's default)
    else
        return false;
    return true;
}

void MainWindow::CreateCommandBar()
{
    // Filled by RebuildCommandBar from the user's toolbar (ApplySettings -> LoadToolbarLists)
    m_cmd.Create(m_hwnd, IdCmdBar, &m_theme, this, {}, &m_theme.toolbar);
}

void MainWindow::OnBarDropdown(int id, const RECT& screen)
{
    if (const int command = CustomCommand(id))
        id = command;
    if (id == BtnNew)
        ShowNewMenu(screen);
    else if (id == BtnMore)
        ShowMoreMenu(screen);
    else
        ShowCommandMenu(id, screen);
}
void MainWindow::RunCommand(int id)
{
    if (id == BtnSettings)
    {
        OpenSettings();
        return;
    }
    Tab* tab = Active();
    if (!tab)
        return;
    tab->Focus();
    switch (id)
    {
    case BtnCut: tab->InvokeVerb("cut", false); break;
    case BtnCopy: tab->InvokeVerb("copy", false); break;
    case BtnPaste: PasteHere(); break;
    case BtnRename: tab->Rename(); break;
    case BtnDelete: DeleteSelection(GetKeyState(VK_SHIFT) < 0); break;
    case BtnShare: ShareSelection(tab); break;
    case BtnProperties:
        if (!tab->InvokeVerb("properties", tab->SelectedCount() == 0))
            SetStatusText(Tr(L"无法打开属性"), true);
        break;
    case BtnSidebar: ToggleSidebar(); break;
    case BtnFilter: ToggleFilter(); break;
    case BtnInfoPane: ToggleInfoPane(); break;
    case BtnDualPane: ToggleDualPane(); break;
    case BtnRunAs: tab->InvokeVerb("runas", false); break;
    case BtnRotateLeft: tab->InvokeVerb("rotate270", false); break;
    case BtnRotateRight: tab->InvokeVerb("rotate90", false); break;
    case BtnWallpaper:
        if (!tab->InvokeVerb("setdesktopwallpaper", false))
            SetStatusText(Tr(L"无法设为壁纸"), true);
        break;
    case BtnInstallFont: tab->InvokeVerb("install", false); break;
    case BtnEmptyBin: EmptyRecycleBin(); break;
    case BtnRestoreAll: RestoreFromRecycleBin(true); break;
    case BtnRestore: RestoreFromRecycleBin(false); break;
    }
    m_barKey = -1;
}
void MainWindow::ShowCommandMenu(int id, const RECT& button)
{
    Tab* tab = Active();
    if (!tab)
        return;
    const wchar_t* const layoutNames[] = { Tr(L"详细信息\tCtrl+Shift+1"), Tr(L"列表\tCtrl+Shift+2"), Tr(L"卡片\tCtrl+Shift+3"),
        Tr(L"网格\tCtrl+Shift+4"), Tr(L"列视图\tCtrl+Shift+5") };
    HMENU menu = CreatePopupMenu();
    if (id == BtnSelect)
    {
        AppendMenuW(menu, MF_STRING, MenuSelectAll, Tr(L"全选\tCtrl+A"));
        AppendMenuW(menu, MF_STRING, MenuInvert, Tr(L"反向选择"));
        AppendMenuW(menu, MF_STRING, MenuClear, Tr(L"取消选择"));
    }
    else if (id == BtnSort)
    {
        // Files' sort button: 排序方式 ▸, 分组依据 ▸, then the sort priority toggles
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(SortMenu(tab, MenuSortMenuBase, false)), Tr(L"排序方式"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(GroupMenu(tab, MenuSortMenuBase, false)), Tr(L"分组依据"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendSortPriority(menu, tab, MenuSortMenuBase);
    }
    else if (id == BtnLayout)
    {
        // Files' layout flyout: 详细信息 / 列表 / 卡片 / 网格 / 列视图 (not in the recycle bin; FastFiles: file system folders),
        // then the size (增加大小 / 减小大小: its slider's steps)
        const int current = static_cast<int>(tab->DisplayedLayout());
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(layoutNames)); ++i)
        {
            const bool columns = kFilesLayouts[i] == ViewLayout::Columns;
            AppendMenuW(menu, MF_STRING | (current == static_cast<int>(kFilesLayouts[i]) ? MF_CHECKED : 0) | (columns && !tab->CanColumns() ? MF_GRAYED : 0),
                MenuLayoutBase + static_cast<int>(kFilesLayouts[i]), layoutNames[i]);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuZoomIn, Tr(L"增加大小\tCtrl+加号"));
        AppendMenuW(menu, MF_STRING, MenuZoomOut, Tr(L"减小大小\tCtrl+减号"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        SHELLSTATEW shell{};
        SHGetSetSettings(&shell, SSF_SHOWEXTENSIONS | SSF_SHOWALLOBJECTS, FALSE);
        AppendMenuW(menu, MF_STRING | (m_settings.showHidden ? MF_CHECKED : 0), MenuHidden, Tr(L"隐藏的项目\tCtrl+H"));
        AppendMenuW(menu, MF_STRING | (shell.fShowExtensions ? MF_CHECKED : 0), MenuExtensions, Tr(L"文件扩展名"));
        AppendMenuW(menu, MF_STRING | (m_prefs.checkboxes ? MF_CHECKED : 0), MenuCheckboxes, Tr(L"项目复选框"));
        // Files' toggle next to 显示隐藏项目/文件扩展名; not loaded while folders share one layout
        if (!m_settings.syncLayout)
            AppendMenuW(menu, MF_STRING | (tab->AdaptiveOn() ? MF_CHECKED : 0), MenuLayoutBase + static_cast<int>(ViewLayout::Adaptive),
                Tr(L"自适应布局\tCtrl+Shift+6"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (m_filterVisible ? MF_CHECKED : 0), MenuFilterBar, Tr(L"筛选栏\tCtrl+Shift+F"));
        AppendMenuW(menu, MF_STRING | (m_toolbarVisible ? MF_CHECKED : 0), MenuToolbar, Tr(L"命令栏\tCtrl+Shift+B"));
        AppendMenuW(menu, MF_STRING | (m_statusVisible ? MF_CHECKED : 0), MenuStatusBar, Tr(L"状态栏"));
        AppendMenuW(menu, MF_STRING | (m_infoVisible ? MF_CHECKED : 0), MenuInfoPane, Tr(L"详情/预览窗格\tCtrl+Alt+I"));
        const TabSlot* slot = ActiveSlot();
        const bool split = slot && slot->Split();
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (split ? MF_CHECKED : 0), MenuDualPane, Tr(L"双窗格\tCtrl+Shift+S"));
        AppendMenuW(menu, MF_STRING | (split && !slot->horizontal ? MF_CHECKED : 0), MenuSplitVertical, Tr(L"左右排列\tAlt+Shift+V"));
        AppendMenuW(menu, MF_STRING | (split && slot->horizontal ? MF_CHECKED : 0), MenuSplitHorizontal, Tr(L"上下排列\tAlt+Shift+H"));
        AppendMenuW(menu, MF_STRING | (split ? 0 : MF_GRAYED), MenuClosePane, Tr(L"关闭当前窗格\tCtrl+Alt+W"));
    }
    POINT pt{ button.left, button.bottom };
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    tab->Focus();
    if (cmd == MenuSelectAll) tab->SelectAll();
    else if (cmd == MenuInvert) tab->InvertSelection();
    else if (cmd == MenuClear) tab->ClearSelection();
    else if (cmd >= MenuSortMenuBase && cmd < MenuSortMenuBase + static_cast<int>(kSortMenuIds)) RunSortCommand(tab, static_cast<UINT>(cmd - MenuSortMenuBase));
    else if (cmd >= MenuLayoutBase && cmd < MenuLayoutBase + 10) SetLayoutFromUser(static_cast<ViewLayout>(cmd - MenuLayoutBase));
    else if (cmd == MenuZoomIn || cmd == MenuZoomOut) tab->StepIconSize(cmd == MenuZoomIn ? 1 : -1);
    else if (cmd == MenuHidden) ToggleHidden();
    else if (cmd == MenuExtensions) ToggleExtensions();
    else if (cmd == MenuCheckboxes) ToggleCheckboxes();
    else if (cmd == MenuFilterBar) ToggleFilter();
    else if (cmd == MenuToolbar) ToggleToolbar();
    else if (cmd == MenuInfoPane) ToggleInfoPane();
    else if (cmd == MenuDualPane) ToggleDualPane();
    else if (cmd == MenuSplitVertical || cmd == MenuSplitHorizontal) SplitPanes(cmd == MenuSplitHorizontal);
    else if (cmd == MenuClosePane) RunAction(ActClosePane);
    else if (cmd == MenuStatusBar) RunAction(ActToggleStatusBar);
    m_barKey = -1;
}

void MainWindow::SetLayoutShortcut(int index)
{
    // Ctrl+Shift+1-6: details, list, tiles, large icons, column view, adaptive layout (Files' LayoutActions)
    if (Active() && index >= 0 && index < static_cast<int>(ARRAYSIZE(kFilesLayouts)))
        SetLayoutFromUser(kFilesLayouts[index]);
    else if (Active() && index == static_cast<int>(ARRAYSIZE(kFilesLayouts)))
        SetLayoutFromUser(ViewLayout::Adaptive);
}

void MainWindow::CopyPath()
{
    Tab* tab = Active();
    if (!tab)
        return;
    const std::wstring text = tab->SelectionPaths();
    if (text.empty() || Sandbox::CopyText(text) || !OpenClipboard(m_hwnd))
        return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

void MainWindow::ToggleSidebar()
{
    m_sidebarVisible = !m_sidebarVisible;
    if (m_sidebar && m_sidebar->Hwnd())
        ShowWindow(m_sidebar->Hwnd(), m_sidebarVisible ? SW_SHOW : SW_HIDE);
    Layout();
    UpdateToggleButtons();
}

void MainWindow::UpdateCommandStates()
{
    Tab* tab = Active();
    // What the selection holds is only re-read when the selection may have changed (it asks every item for its name)
    const int selected = tab ? tab->SelectedCount() : 0;
    const long long selKey = (static_cast<long long>(selected) << 33) | (static_cast<long long>(m_inRecycleBin) << 32) |
        static_cast<unsigned>(tab ? tab->FocusedIndex() : -1);
    // (the own list says when its selection changed: the same count and focus can be other items after a re-list)
    const unsigned stamp = tab ? tab->SelectionStamp() : 0;
    if (selKey != m_selKey || stamp != m_selStamp)
    {
        m_selKey = selKey;
        m_selStamp = stamp;
        m_selKinds = selected > 0 && tab ? tab->SelectionKinds(&m_selAll) : 0;
        if (selected == 0)
            m_selAll = 0;
    }
    const BarState s = CurrentBarState();
    const unsigned contexts = ActiveContexts(s);
    const long long key = (static_cast<long long>(s.all) << 40) | (static_cast<long long>(s.any) << 24) |
        (static_cast<long long>(std::min(s.selected, 0xFFFFF)) << 4) | (s.fs ? 1 : 0) | (s.clip ? 2 : 0) | (s.binItems ? 4 : 0) | (s.bin ? 8 : 0);
    if (contexts != m_barContexts)
        RebuildCommandBar(contexts); // a context came or went (Files' PopulateToolbarItems)
    else if (key == m_barKey)
        return;
    m_barKey = key;
    // Files' IsEnabled = the command's IsExecutable
    for (int c = 0; c < ToolbarModel::ContextCount; ++c)
        for (int i = 0; i < static_cast<int>(m_toolbarLists[c].size()); ++i)
        {
            const int barId = kCustomBase + c * kCustomStride + i;
            if (const int command = CustomCommand(barId))
                m_cmd.SetEnabled(barId, CanRun(command, s));
        }
}