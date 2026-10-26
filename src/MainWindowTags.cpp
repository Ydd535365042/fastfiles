#include "MainWindow.h"
#include "Drives.h"
#include "Lang.h"
#include "fastfiles_core.h"
#include "ContentDialog.h"
#include <commdlg.h>

// File tags (Files: Settings > Tags, the "Tags" item of the right-click menu, the details pane's tag row, the sidebar's
// Tags section, "tag:" searches) and the file view's right-click menu built around the shell's own (TabMenu.cpp).

namespace
{
// View menu items (TabViewMenu::kAppFirst..); tag items are kTagFirst..kTagLast
enum : UINT { VNewTab = TabViewMenu::kAppFirst, VNewWindow, VOtherPane, VPin, VCopyPath, VTerminal, VRemoveTags, VFolderWithSelection, VFormat,
    VEditNotepad,
    VRefresh, VAscending, VDescending, VLayoutBase, VSortBase = VLayoutBase + 16 }; // (VSortBase .. + MainWindow::kSortMenuIds)

void TidySeparators(HMENU menu)
{
    bool last = true;
    for (int i = 0; i < GetMenuItemCount(menu);)
    {
        MENUITEMINFOW mii{ sizeof(mii), MIIM_FTYPE };
        const bool sep = GetMenuItemInfoW(menu, i, TRUE, &mii) && (mii.fType & MFT_SEPARATOR);
        if (sep && last)
        {
            DeleteMenu(menu, i, MF_BYPOSITION);
            continue;
        }
        last = sep;
        ++i;
    }
    for (int n = GetMenuItemCount(menu); n > 0;)
    {
        MENUITEMINFOW mii{ sizeof(mii), MIIM_FTYPE };
        if (!GetMenuItemInfoW(menu, n - 1, TRUE, &mii) || !(mii.fType & MFT_SEPARATOR))
            break;
        DeleteMenu(menu, --n, MF_BYPOSITION);
    }
}

void InsertItem(HMENU menu, UINT& at, UINT id, const wchar_t* text, bool enabled = true)
{
    InsertMenuW(menu, at++, MF_BYPOSITION | MF_STRING | (enabled ? 0 : MF_GRAYED), id, text);
}

void InsertSeparator(HMENU menu, UINT& at)
{
    InsertMenuW(menu, at++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
}

// --- A one-line text prompt (tag names): in-memory dialog template, no resource file ---

struct Prompt
{
    std::wstring title, label, text;
};

INT_PTR CALLBACK PromptProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* p = reinterpret_cast<Prompt*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, p->title.c_str());
        SetDlgItemTextW(dlg, 100, p->label.c_str());
        SetDlgItemTextW(dlg, 101, p->text.c_str());
        SendDlgItemMessageW(dlg, 101, EM_SETLIMITTEXT, 64, 0);
        SendDlgItemMessageW(dlg, 101, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, 101));
        return FALSE; // focus set
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
        {
            if (LOWORD(wp) == IDOK)
            {
                auto* p = reinterpret_cast<Prompt*>(GetWindowLongPtrW(dlg, DWLP_USER));
                const HWND edit = GetDlgItem(dlg, 101);
                p->text.assign(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
                p->text.resize(GetWindowTextW(edit, p->text.data(), static_cast<int>(p->text.size())));
            }
            EndDialog(dlg, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Returns false when cancelled
bool PromptText(HWND owner, const std::wstring& title, const std::wstring& label, std::wstring& text)
{
    // DLGTEMPLATE + items, WORD aligned (DWORD for each item), sizes in dialog units
    std::vector<WORD> t;
    auto words = [&](std::initializer_list<WORD> w) { t.insert(t.end(), w); };
    auto dword = [&](DWORD d) { words({ LOWORD(d), HIWORD(d) }); };
    auto str = [&](const wchar_t* s) { for (; *s; ++s) t.push_back(*s); t.push_back(0); };
    auto align = [&] { if (t.size() % 2) t.push_back(0); };
    dword(DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dword(0);
    words({ 4, 0, 0, 220, 70 }); // items, x, y, cx, cy
    words({ 0, 0 });             // no menu, default class
    str(L"");                    // title (set in WM_INITDIALOG)
    words({ 9 });
    str(L"Microsoft YaHei UI");
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls)
    {
        align();
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        words({ static_cast<WORD>(x), static_cast<WORD>(y), static_cast<WORD>(cx), static_cast<WORD>(cy), id, 0xFFFF, cls });
        words({ 0, 0 }); // no text (set later), no creation data
    };
    item(SS_LEFT, 8, 8, 204, 10, 100, 0x0082);
    item(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 22, 204, 14, 101, 0x0081);
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, 108, 48, 50, 14, IDOK, 0x0080);
    item(BS_PUSHBUTTON | WS_TABSTOP, 162, 48, 50, 14, IDCANCEL, 0x0080);
    Prompt p{ title, label, text };
    const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.data()), owner, PromptProc,
        reinterpret_cast<LPARAM>(&p));
    if (r != IDOK)
        return false;
    text = p.text;
    return true;
}

// Files' EditInNotepadAction.IsExecutable: every selected item is a batch, AutoHotkey or cmd script
bool AllScriptFiles(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return false;
    return std::all_of(paths.begin(), paths.end(), [](const std::wstring& path)
    {
        const wchar_t* ext = PathFindExtensionW(path.c_str());
        return _wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0 || _wcsicmp(ext, L".ahk") == 0;
    });
}

// Files edits a tag's color with a color picker; the system color dialog is the native one
bool PickColor(HWND owner, COLORREF& color)
{
    static COLORREF custom[16]{};
    CHOOSECOLORW cc{ sizeof(cc) };
    cc.hwndOwner = owner;
    cc.rgbResult = color;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc))
        return false;
    color = cc.rgbResult;
    return true;
}
bool SameName(const std::wstring& a, const std::wstring& b)
{
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | NORM_IGNOREWIDTH, a.c_str(), static_cast<int>(a.size()),
               b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
}

std::wstring Trim(const std::wstring& s)
{
    const size_t a = s.find_first_not_of(L" \t　");
    if (a == std::wstring::npos)
        return {};
    return s.substr(a, s.find_last_not_of(L" \t　") - a + 1);
}
}

void MainWindow::TagMenu::Clear()
{
    for (HBITMAP b : bitmaps)
        DeleteObject(b);
    bitmaps.clear();
    tags.clear();
    all.clear();
    paths.clear();
    folders.clear();
    drive.clear();
    pinTarget.reset();
    pinned = false;
}

// --- Files' "编辑文件标签" submenu: one toggle per tag (checked when every target has it), then 删除标签 ---

void MainWindow::AppendTagMenu(HMENU menu, UINT position, TagMenu& state)
{
    state.tags = Tags::List();
    state.all.assign(state.tags.size(), !state.paths.empty());
    // Reading tags is one small file read per item; past a few hundred items the check marks just aren't computed
    if (state.paths.size() <= 256)
        for (const auto& path : state.paths)
        {
            const auto ids = Tags::Of(path);
            for (size_t i = 0; i < state.tags.size(); ++i)
                if (state.all[i] && std::find(ids.begin(), ids.end(), state.tags[i].id) == ids.end())
                    state.all[i] = false;
        }
    else
        state.all.assign(state.tags.size(), false);

    // Checked + bitmap: themed menus frame the dot (like Explorer's View menu); the dot also carries a tick
    HMENU sub = CreatePopupMenu();
    const int dot = GetSystemMetricsForDpi(SM_CXSMICON, m_dpi);
    for (size_t i = 0; i < state.tags.size() && i <= kTagLast - kTagFirst; ++i)
    {
        HBITMAP bmp = Tags::MenuDot(state.tags[i].color, dot, state.all[i]);
        state.bitmaps.push_back(bmp);
        MENUITEMINFOW mii{ sizeof(mii), MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_STATE };
        mii.wID = kTagFirst + static_cast<UINT>(i);
        mii.dwTypeData = const_cast<wchar_t*>(state.tags[i].name.c_str());
        mii.hbmpItem = bmp;
        mii.fState = (state.all[i] ? MFS_CHECKED : 0) | (state.paths.empty() ? MFS_DISABLED : 0);
        InsertMenuItemW(sub, static_cast<UINT>(i), TRUE, &mii);
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sub, MF_STRING | (state.paths.empty() ? MF_GRAYED : 0), VRemoveTags, Tr(L"删除标签"));
    InsertMenuW(menu, position, MF_BYPOSITION | MF_POPUP | (state.paths.empty() ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(sub), Tr(L"编辑文件标签"));
}

bool MainWindow::RunTagCommand(UINT id, TagMenu& state)
{
    if (id == VRemoveTags)
    {
        // Files' 删除标签: every tag off every target
        bool changed = false;
        for (const auto& tag : Tags::List())
            changed |= Tags::Set(state.paths, tag.id, false);
        if (changed)
            Tags::Broadcast();
        return true;
    }
    if (id < kTagFirst || id > kTagLast)
        return false;
    const size_t index = id - kTagFirst;
    if (index >= state.tags.size() || state.paths.empty())
        return true;
    const FileTag& tag = state.tags[index];
    const bool add = !state.all[index]; // Files: a tag some items lack is added to all; one they all have is removed
    if (Tags::Set(state.paths, tag.id, add))
        Tags::Broadcast();
    return true;
}

void MainWindow::ShowTagsMenu(POINT screen)
{
    Tab* tab = Active();
    if (!tab)
        return;
    m_tagMenu.Clear();
    m_tagMenu.paths = tab->SelectedFileSystemPaths();
    if (m_tagMenu.paths.empty())
    {
        // Details pane of a folder with nothing selected: the folder itself
        if (const std::wstring here = PidlName(tab->Location(), SIGDN_FILESYSPATH); !here.empty() && !tab->InResults())
            m_tagMenu.paths.push_back(here);
    }
    if (m_tagMenu.paths.empty())
        return;
    HMENU host = CreatePopupMenu();
    AppendTagMenu(host, 0, m_tagMenu);
    HMENU sub = GetSubMenu(host, 0);
    const UINT cmd = TrackPopupMenuEx(sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(host);
    RunTagCommand(cmd, m_tagMenu);
    m_tagMenu.Clear();
}

// --- Details pane row ---

void MainWindow::UpdateInfoTags(PCIDLIST_ABSOLUTE item, bool single)
{
    if (!m_info)
        return;
    const std::wstring path = single && item ? PidlName(item, SIGDN_FILESYSPATH) : std::wstring();
    if (path.empty())
    {
        m_info->SetTags({}, false);
        return;
    }
    const auto all = Tags::List();
    std::vector<std::pair<std::wstring, COLORREF>> chips;
    for (const auto& id : Tags::Of(path))
        if (const FileTag* t = Tags::Find(all, id))
            chips.emplace_back(t->name, t->color);
    m_info->SetTags(std::move(chips), true);
}

void MainWindow::OnInfoPaneTags(POINT screen)
{
    ShowTagsMenu(screen);
}

// tags.txt changed (this window or another): sidebar, details pane, settings page, tag results
void MainWindow::OnTagsChanged()
{
    if (m_sidebar)
        m_sidebar->LoadTags();
    if (!(m_prefs.homeHidden & (1u << HomeView::SecTags)))
        ScanTagged(); // the Home tags widget follows (Files' OnTagsUpdated)
    m_infoKey = -1;
    UpdateInfoPane();
    ForEachPane([](Tab* tab) { tab->RebuildSettings(); });
    const auto tags = Tags::List();
    ForEachPane([&](Tab* tab)
    {
        if (tab->TagId().empty())
            return;
        if (const FileTag* t = Tags::Find(tags, tab->TagId()))
        {
            tab->SetTagName(t->name); // may have been renamed
            tab->Refresh();           // items may have gained or lost the tag
        }
        else
            tab->Up(); // the tag was deleted: back to where its results were opened
    });
    for (int i = 0; i < static_cast<int>(m_tabList.size()); ++i)
        UpdateTabLabel(i);
    UpdateChrome();
}

// --- Opening a tag's files ---

void MainWindow::OpenTag(const std::wstring& id, const std::wstring& name, OpenMode mode, bool here)
{
    if (mode == OpenMode::NewTab || mode == OpenMode::NewWindow)
    {
        // A tab of its own, on Home underneath (Back / Up return there)
        const int index = NewTab(HomePidl(), true);
        if (index >= 0 && index < static_cast<int>(m_tabList.size()))
            m_tabList[index].Focused()->StartTagSearch(id, name, false);
        return;
    }
    if (Tab* tab = Active(); tab && tab->StartTagSearch(id, name, here))
        tab->Focus();
}

void MainWindow::OnSidebarTag(const std::wstring& id, const std::wstring& name, OpenMode mode)
{
    const std::wstring tagId = id, tagName = name; // the sidebar may rebuild its entries meanwhile
    OpenTag(tagId, tagName, mode, false);
}


// "tag:名称" in the search box (Files' tag search syntax)
bool MainWindow::RunTagQuery(Tab* tab, const std::wstring& query)
{
    std::wstring name;
    if (query.size() > 4 && _wcsnicmp(query.c_str(), L"tag:", 4) == 0)
        name = Trim(query.substr(4));
    if (name.size() >= 2 && name.front() == L'"' && name.back() == L'"')
        name = name.substr(1, name.size() - 2); // tag:"two words"
    if (name.empty())
        return false;
    for (const auto& tag : Tags::List())
        if (SameName(tag.name, name))
        {
            if (tab->StartTagSearch(tag.id, tag.name, true))
                tab->Focus();
            return true;
        }
    MessageBeep(MB_ICONWARNING);
    SetStatusText(Tr(L"没有名为“") + name + Tr(L"”的标签"), true);
    return true;
}

// --- The file view's right-click menu (TabMenu.cpp builds the shell part) ---

void MainWindow::OnTabViewMenu(Tab* tab, TabViewMenu& menu)
{
    // The right-clicked pane becomes the one commands act on
    if (TabSlot* slot = ActiveSlot(); slot && slot->Split())
        for (int i = 0; i < 2; ++i)
            if (slot->panes[i].Get() == tab && slot->focus != i)
                SetPaneFocus(i, false);
    m_tagMenu.Clear();
    HMENU m = menu.menu;
    const bool fsFolder = !PidlName(tab->Location(), SIGDN_FILESYSPATH).empty() && !tab->InResults();
    // Files' order (ContentPageContextFlyoutFactory): ... 属性, 固定到侧边栏, 压缩, 解压, 在终端中打开, 编辑文件标签
    auto afterProperties = [&]
    {
        const int props = menu.VerbIndex(L"properties");
        return props >= 0 ? static_cast<UINT>(props) + 1 : static_cast<UINT>(GetMenuItemCount(m));
    };

    if (menu.background && menu.ownList)
    {
        // FastFiles' own file list: Files' 布局 / 排序方式 / 刷新 first (DefView's 查看 / 排序方式 aren't there)
        UINT top = 0;
        HMENU layouts = CreatePopupMenu();
        const int shown = static_cast<int>(tab->DisplayedLayout());
        // Files' 布局 submenu: 详细信息 / 列表 / 卡片 / 网格 / 列视图, 自适应布局
        static const wchar_t* layoutNames[] = { L"详细信息", L"列表", L"卡片", L"网格", L"列视图" };
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(layoutNames)); ++i)
        {
            const int id = static_cast<int>(kFilesLayouts[i]);
            const bool columns = kFilesLayouts[i] == ViewLayout::Columns;
            AppendMenuW(layouts, MF_STRING | (shown == id ? MF_CHECKED : 0) | (columns && !tab->CanColumns() ? MF_GRAYED : 0), VLayoutBase + id,
                Tr(layoutNames[i]));
        }
        if (!m_settings.syncLayout)
            AppendMenuW(layouts, MF_STRING | (tab->AdaptiveOn() ? MF_CHECKED : 0), VLayoutBase + static_cast<int>(ViewLayout::Adaptive), Tr(L"自适应布局"));
        InsertMenuW(m, top++, MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(layouts), Tr(L"布局"));
        // Files' 排序方式 / 分组依据 (ContentPageContextFlyoutFactory: what can't run here left out)
        InsertMenuW(m, top++, MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(SortMenu(tab, VSortBase, true)), Tr(L"排序方式"));
        InsertMenuW(m, top++, MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(GroupMenu(tab, VSortBase, true)), Tr(L"分组依据"));
        InsertItem(m, top, VRefresh, Tr(L"刷新"));
        InsertSeparator(m, top);
    }
    if (menu.background)
    {
        // Nothing selected: copy path, pin and terminal act on the folder itself (Files: CopyItemPath, PinFolderToSidebar,
        // OpenTerminal are executable without a selection). Files' zh-Hans text for this command is "复制项目路径"
        // (Strings/zh-Hans/Resources.resw, CopyItemPath), distinct from the toolbar "更多" flyout's CopyPath="复制路径"
        // (MainWindowView.cpp ShowMoreMenu, a different Files command) - this item was wrongly using that text.
        UINT at = afterProperties();
        InsertSeparator(m, at);
        if (fsFolder && MenuOptions::Shown(MenuOptions::CopyPath))
            InsertItem(m, at, VCopyPath, Tr(L"复制项目路径"));
        if (m_sidebar && MenuOptions::Shown(MenuOptions::Pin) && !tab->InResults() && tab->Location() && !ILIsEmpty(tab->Location()))
        {
            m_tagMenu.pinTarget = ClonePidl(tab->Location());
            m_tagMenu.pinned = m_sidebar->IsPinned(m_tagMenu.pinTarget.get());
            InsertItem(m, at, VPin, m_tagMenu.pinned ? Tr(L"从侧边栏取消固定") : Tr(L"固定到侧边栏"));
        }
        if (fsFolder && MenuOptions::Shown(MenuOptions::Terminal))
            InsertItem(m, at, VTerminal, Tr(L"在终端中打开"));
        // Files' FormatDrive (ContentPageContextFlyoutFactory): in a drive's root with nothing selected
        m_tagMenu.drive = Drives::CanFormat(PidlName(tab->Location(), SIGDN_FILESYSPATH)) ? PidlName(tab->Location(), SIGDN_FILESYSPATH) : std::wstring();
        if (!m_tagMenu.drive.empty())
            InsertItem(m, at, VFormat, Tr(L"格式化"));
        TidySeparators(m);
        return;
    }

    // Folders (1-5, every item a folder): 在新标签页/新窗口/其他窗格中打开 after the default item. The shell's own
    // "open in new window" starts an Explorer window, so those verbs go.
    RemoveHiddenShellItems(m, menu.shell, TabViewMenu::kShellFirst, TabViewMenu::kShellLast);
    m_tagMenu.folders = tab->SelectedFolders();
    if (m_tagMenu.folders.size() > 5)
        m_tagMenu.folders.clear();
    if (!m_tagMenu.folders.empty())
    {
        for (int i = GetMenuItemCount(m) - 1; i >= 0; --i)
        {
            const UINT id = GetMenuItemID(m, i);
            wchar_t verb[64]{};
            if (id >= TabViewMenu::kShellFirst && id <= TabViewMenu::kShellLast &&
                SUCCEEDED(menu.shell->GetCommandString(id - TabViewMenu::kShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 64)))
                for (const wchar_t* v : { L"opennewwindow", L"opennewprocess", L"opennewtab" })
                    if (_wcsicmp(verb, v) == 0)
                    {
                        DeleteMenu(m, i, MF_BYPOSITION);
                        break;
                    }
        }
        const int def = static_cast<int>(GetMenuDefaultItem(m, TRUE, 0));
        UINT at = def >= 0 ? static_cast<UINT>(def) + 1 : 0;
        if (MenuOptions::Shown(MenuOptions::NewTab))
            InsertItem(m, at, VNewTab, Tr(L"在新标签页中打开"));
        if (MenuOptions::Shown(MenuOptions::NewWindow))
            InsertItem(m, at, VNewWindow, Tr(L"在新窗口中打开"));
        if (m_tagMenu.folders.size() == 1 && MenuOptions::Shown(MenuOptions::NewPane))
            InsertItem(m, at, VOtherPane, Tr(L"在另一个窗格中打开"));
        InsertSeparator(m, at);
    }

    // 复制项目路径 and 用选中的项目创建文件夹 right after the shell's 复制 (Files: CopyItemPath, CreateFolderWithSelection)
    m_tagMenu.paths = tab->SelectedFileSystemPaths();
    if (!m_tagMenu.paths.empty())
    {
        const int copy = menu.VerbIndex(L"copy");
        UINT at = copy >= 0 ? static_cast<UINT>(copy) + 1 : afterProperties();
        if (MenuOptions::Shown(MenuOptions::CopyPath))
            InsertItem(m, at, VCopyPath, Tr(L"复制项目路径"));
        if (MenuOptions::Shown(MenuOptions::FolderWithSelection) && fsFolder)
            InsertItem(m, at, VFolderWithSelection, Tr(L"用选中的项目创建文件夹"));
    }

    UINT at = afterProperties();
    InsertSeparator(m, at);
    if (m_sidebar && !m_tagMenu.folders.empty() && MenuOptions::Shown(MenuOptions::Pin))
    {
        // Files pins every selected folder; "unpin" when they're all pinned already
        m_tagMenu.pinned = std::all_of(m_tagMenu.folders.begin(), m_tagMenu.folders.end(),
            [&](const UniquePidl& f) { return m_sidebar->IsPinned(f.get()); });
        InsertItem(m, at, VPin, m_tagMenu.pinned ? Tr(L"从侧边栏取消固定") : Tr(L"固定到侧边栏"));
    }
    // Files' EditInNotepadAction: only for .bat/.cmd/.ahk scripts (opening them normally would run them)
    if (!m_tagMenu.paths.empty() && AllScriptFiles(m_tagMenu.paths) && !tab->InResults())
        InsertItem(m, at, VEditNotepad, Tr(L"在记事本中编辑"));
    if (m_tagMenu.folders.size() == 1 && MenuOptions::Shown(MenuOptions::Terminal))
        InsertItem(m, at, VTerminal, Tr(L"在终端中打开"));
    if (!m_tagMenu.paths.empty() && MenuOptions::Shown(MenuOptions::Tags))
    {
        // Files' BaseLayoutPage.ItemContextFlyout_Opening builds 编辑文件标签 last, after every base item (including
        // the terminal/storage-sense/format-drive group and the shell's own "显示更多选项" overflow) - not next to
        // Pin/Compress like the items above. Append at the very end of the menu, not at the running `at` cursor.
        UINT end = static_cast<UINT>(GetMenuItemCount(m));
        InsertSeparator(m, end);
        AppendTagMenu(m, end, m_tagMenu);
    }
    TidySeparators(m);
}

void MainWindow::OnTabViewCommand(Tab* tab, UINT id)
{
    switch (id)
    {
    case 0: break;
    case VNewTab:
        // In the background unless 总是切换到新创建的标签页 (Files' AlwaysSwitchToNewlyOpenedTab)
        for (const auto& folder : m_tagMenu.folders)
            OpenInNewTab(folder.get());
        break;
    case VNewWindow:
        for (const auto& folder : m_tagMenu.folders)
            OpenNewWindow(folder.get());
        break;
    case VOtherPane:
        if (!m_tagMenu.folders.empty())
        {
            const UniquePidl target = ClonePidl(m_tagMenu.folders[0].get());
            OpenInOtherPane(target.get());
        }
        break;
    case VPin:
        if (m_sidebar && m_tagMenu.pinTarget)
        {
            // Background: the folder itself
            if (m_tagMenu.pinned)
                m_sidebar->Unpin(m_tagMenu.pinTarget.get());
            else
                m_sidebar->Pin(m_tagMenu.pinTarget.get());
        }
        else if (m_sidebar)
        {
            for (const auto& folder : m_tagMenu.folders)
                m_tagMenu.pinned ? m_sidebar->Unpin(folder.get()) : m_sidebar->Pin(folder.get());
        }
        break;
    case VCopyPath: CopyPath(); break;
    case VFolderWithSelection: CreateFolderWithSelection(); break;
    case VFormat: Drives::Format(m_hwnd, m_tagMenu.drive); break;
    case VRefresh: tab->Refresh(); break;
    case VAscending:
    case VDescending:
    {
        SortKey key = SortKey::Name;
        bool desc = false;
        tab->GetSort(key, desc);
        tab->SetSort(key, id == VDescending);
        break;
    }
    case VTerminal:
        if (m_tagMenu.folders.size() == 1)
            OpenTerminal(false, PidlName(m_tagMenu.folders[0].get(), SIGDN_FILESYSPATH));
        else
            OpenTerminal(false);
        break;
    case VEditNotepad:
        // Files' EditInNotepadAction: notepad.exe on each selected script (it runs a hidden PowerShell command to
        // launch it; ShellExecuteExW with the path as lpFile does the same thing more directly)
        for (const auto& path : m_tagMenu.paths)
        {
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_ASYNCOK;
            sei.lpVerb = L"open";
            sei.lpFile = L"notepad.exe";
            sei.nShow = SW_SHOWNORMAL;
            const std::wstring quoted = L"\"" + path + L"\"";
            sei.lpParameters = quoted.c_str();
            ShellExecuteExW(&sei);
        }
        break;
    default:
        if (id >= VSortBase && id < VSortBase + kSortMenuIds)
            RunSortCommand(tab, id - VSortBase);
        else if (id >= VLayoutBase && id < VLayoutBase + 10)
            SetLayoutFromUser(static_cast<ViewLayout>(id - VLayoutBase));
        else
            RunTagCommand(id, m_tagMenu);
        break;
    }
    m_tagMenu.Clear();
}
// --- Settings > 标签 ---

void MainWindow::AddTagsSettings(std::vector<SettingsPage>& pages, HWND owner)
{
    // Files' Settings > 标签: the tag list with 编辑 / 删除, and 新标签 (name + a random color)
    SettingsPage p{ Glyph::Tag, Tr(L"标签"), {} };
    SettingsRow head;
    head.kind = SettingsRow::Kind::Header;
    head.label = Tr(L"标签");
    p.rows.push_back(std::move(head));

    auto nameTaken = [](const std::wstring& name, const std::wstring& except)
    {
        for (const auto& other : Tags::List())
            if (other.id != except && SameName(other.name, name))
                return true;
        return false;
    };
    for (const auto& tag : Tags::List())
    {
        SettingsRow r;
        r.kind = SettingsRow::Kind::Button;
        r.label = tag.name;
        r.swatch = true;
        r.swatchColor = tag.color;
        r.button = Tr(L"编辑");
        r.press = [this, owner, tag, nameTaken]
        {
            POINT pt;
            GetCursorPos(&pt);
            enum : UINT { ERename = 1, EColor, EDelete };
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ERename, Tr(L"重命名"));
            AppendMenuW(menu, MF_STRING, EColor, Tr(L"颜色"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, EDelete, Tr(L"删除"));
            const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
            DestroyMenu(menu);
            bool changed = false;
            if (cmd == ERename)
            {
                std::wstring name = tag.name;
                if (!PromptText(owner, Tr(L"编辑"), Tr(L"名称"), name))
                    return;
                name = Trim(name);
                if (name.empty() || nameTaken(name, tag.id))
                {
                    // Files' TagsPage shows "无效名称" (InvalidTagNameWarning) inline under the name box, not as a
                    // popup; FastFiles' rename is its own popup dialog (PromptText) so this stays a dialog, Files' text.
                    ContentDialog::Notice(owner, &m_theme, Tr(L"无效名称"), {});
                    return;
                }
                changed = name != tag.name && Tags::Rename(tag.id, name);
            }
            else if (cmd == EColor)
            {
                COLORREF color = tag.color;
                changed = PickColor(owner, color) && Tags::Recolor(tag.id, color);
            }
            else if (cmd == EDelete)
            {
                // Files' TagsPage delete button: a Flyout (no title), ConfirmDeleteTag text, 取消 / 删除 (accent).
                // The Flyout has no Enter-confirms-delete behaviour at all; the old MessageBox defaulted away from
                // deleting too (MB_DEFBUTTON2), so close stays the keyboard default here.
                if (ContentDialog::Confirm(owner, &m_theme, L"FastFiles", Tr(L"确定要删除此标签吗？"), Tr(L"删除"), Tr(L"取消"), true))
                    changed = Tags::Remove(tag.id);
            }
            if (changed)
                Tags::Broadcast();
        };
        p.rows.push_back(std::move(r));
    }
    SettingsRow add;
    add.kind = SettingsRow::Kind::Button;
    add.label = Tr(L"创建新标签");
    add.button = Tr(L"新标签");
    add.press = [this, owner, nameTaken]
    {
        std::wstring name;
        if (!PromptText(owner, Tr(L"创建新标签"), Tr(L"名称"), name))
            return;
        name = Trim(name);
        if (name.empty() || nameTaken(name, L""))
        {
            // Files' TagsPage shows this inline (InvalidTagNameWarning) while adding a tag too; same text here.
            ContentDialog::Notice(owner, &m_theme, Tr(L"无效名称"), {});
            return;
        }
        // Files: ColorHelpers.RandomColor()
        LARGE_INTEGER seed{};
        QueryPerformanceCounter(&seed);
        const unsigned v = static_cast<unsigned>(seed.QuadPart * 2654435761u);
        if (!Tags::Add(name, RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)).empty())
            Tags::Broadcast();
    };
    p.rows.push_back(std::move(add));
    pages.push_back(std::move(p));
}