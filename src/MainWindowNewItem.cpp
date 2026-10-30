#include "MainWindow.h"
#include "Lang.h"
#include "ContentDialog.h"
#include "DarkMode.h"
#include "NewItem.h"
#include "Sandbox.h"

// Files' "create" commands: the 新建 flyout (文件夹 / 文件 / 快捷方式 + the ShellNew types), Ctrl+Shift+I's AddItemDialog,
// the name prompt (DynamicDialogFactory.GetFor_CreateItemDialog) and the new-shortcut dialog (CreateShortcutDialog).

namespace
{
constexpr wchar_t kFolderGlyph[] = L"\xE838", kFileGlyph[] = L"\xE8A5", kShortcutGlyph[] = L"\xE71B";
enum : UINT { MenuFolder = 1, MenuFile, MenuShortcut, MenuEntry = 100 };

// A font glyph as a menu bitmap (32bpp premultiplied): drawn white on black, the brightness becomes the alpha
HBITMAP GlyphBitmap(const Theme& theme, const wchar_t* glyph, int size)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp)
        return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    LOGFONTW lf{};
    GetObjectW(theme.icons, sizeof(lf), &lf);
    lf.lfHeight = -size;
    lf.lfQuality = ANTIALIASED_QUALITY; // grayscale: ClearType's colored fringes would leak into the alpha
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme.glyphMask);
    RECT r{ 0, 0, size, size };
    DrawTextW(dc, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, old);
    DeleteDC(dc);
    const COLORREF ink = theme.ink;
    auto* px = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; ++i)
    {
        const DWORD a = std::max({ px[i] & 0xFF, (px[i] >> 8) & 0xFF, (px[i] >> 16) & 0xFF });
        px[i] = (a << 24) | ((GetRValue(ink) * a / 255) << 16) | ((GetGValue(ink) * a / 255) << 8) | (GetBValue(ink) * a / 255);
    }
    return bmp;
}

void SetItemBitmap(HMENU menu, UINT id, HBITMAP bmp)
{
    MENUITEMINFOW mii{ sizeof(mii), MIIM_BITMAP };
    mii.hbmpItem = bmp;
    SetMenuItemInfoW(menu, id, FALSE, &mii);
}

// Files' DialogDisplayHelper.ShowDialogAsync: title, message, a primary and (optionally) a close button
bool Message(HWND owner, const Theme* theme, const std::wstring& title, const std::wstring& text, const std::wstring& primary,
    const std::wstring& close)
{
    ContentDialog::Spec spec;
    spec.title = title;
    spec.text = text;
    spec.primary = primary;
    spec.close = close;
    return ContentDialog::Show(owner, theme, spec) == ContentDialog::Primary;
}
}

void MainWindow::SelectCreated(const std::wstring& path)
{
    if (path.empty())
        return;
    if (!Sandbox::Active()) // test mode: Windows' Recent list isn't touched
        SHAddToRecentDocs(SHARD_PATHW, path.c_str()); // Files: IWindowsRecentItemsService.Add
    if (Tab* tab = Active())
    {
        tab->Focus();
        tab->SelectPath(path);
    }
}

std::wstring MainWindow::CreateItemFromDialog(bool folder, const NewItem::Entry* entry, bool select)
{
    Tab* tab = Active();
    const std::wstring dir = CurrentFolderPath(tab);
    if (dir.empty())
    {
        SetStatusText(Tr(L"无法在这里新建"), true);
        return {};
    }
    const std::vector<NewItem::Entry> entries = NewItem::Entries();
    std::wstring name;
    // Files skips the name prompt for ShellNew types that have a Command
    if (folder || !entry || entry->command.empty())
    {
        ContentDialog::Spec spec;
        spec.title = std::wstring(Tr(L"创建新 ")) + (folder ? Tr(L"文件夹") : Tr(L"文件")); // CreateNewItemTitle with the lowercased type
        spec.text = Tr(L"输入项目名称");
        ContentDialog::Field field;
        field.placeholder = Tr(L"输入项目名称");
        // Prefilled (Files #17845): 新建文件夹, or 新建 <type> for a ShellNew type
        if (folder)
            field.text = Tr(L"新建文件夹");
        else if (entry)
            field.text = NewItem::FilterName(Tr(L"新建 ") + entry->name);
        spec.fields.push_back(field);
        spec.primary = Tr(L"创建");
        spec.changed = [](ContentDialog::Spec& s, int)
        {
            const std::wstring& text = s.fields[0].text;
            const bool valid = NewItem::ValidName(text);
            s.primaryEnabled = valid;
            s.fields[0].error = !text.empty() && !valid ? Tr(L"指定的项目名称无效") : L"";
        };
        if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary)
            return {};
        name = spec.fields[0].text;
    }
    bool denied = false;
    std::wstring created;
    if (folder)
        created = NewItem::CreateFolder(dir, name.find_first_not_of(L' ') == std::wstring::npos ? Tr(L"新建文件夹") : name, &denied);
    else
    {
        std::wstring fileName = name.find_first_not_of(L' ') == std::wstring::npos ? (entry ? entry->name : Tr(L"新建文件")) : name;
        if (entry)
            fileName += entry->extension;
        created = NewItem::CreateNewFile(dir, fileName, entries, &denied);
    }
    if (created.empty())
    {
        if (denied)
            Message(m_hwnd, &m_theme, Tr(L"拒绝访问"), Tr(L"无法创建此项目"), L"", Tr(L"确定"));
        else
            SetStatusText(Tr(L"无法在这里新建"), true);
        return {};
    }
    if (select)
        SelectCreated(created);
    return created;
}

void MainWindow::CreateFolderWithSelection()
{
    // Files' CreateFolderWithSelectionAsync: the selection (taken first), the 创建新 文件夹 prompt, then a move into it
    Tab* tab = Active();
    std::vector<std::wstring> items = tab && !tab->InResults() ? tab->SelectedFileSystemPaths() : std::vector<std::wstring>{};
    if (items.empty() || CurrentFolderPath(tab).empty())
        return;
    const std::wstring folder = CreateItemFromDialog(true, nullptr, false);
    if (folder.empty())
        return;
    StartTransfer(std::move(items), folder, true);
    SelectCreated(folder);
}

void MainWindow::CreateShortcutFromDialog()
{
    Tab* tab = Active();
    const std::wstring dir = CurrentFolderPath(tab);
    if (dir.empty())
    {
        SetStatusText(Tr(L"无法在这里新建"), true);
        return;
    }
    wchar_t drive[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemDrive", drive, MAX_PATH);
    ContentDialog::Spec spec;
    spec.title = Tr(L"创建新的快捷方式");
    spec.text = Tr(L"创建本地或网络程序、文件、目录、计算机或 Internet 地址的快捷方式。");
    ContentDialog::Field target, name;
    target.label = Tr(L"输入此项目的位置：");
    target.placeholder = std::wstring(drive[0] ? drive : L"C:") + L"\\Users\\";
    target.browse = true;
    name.label = Tr(L"输入项目名称");
    spec.fields = { target, name };
    spec.primary = Tr(L"创建");
    auto parsed = std::make_shared<NewItem::Target>();
    spec.changed = [parsed](ContentDialog::Spec& s, int field)
    {
        if (field <= 0)
        {
            *parsed = NewItem::ParseTarget(s.fields[0].text);
            if (field == 0 && (parsed->exists || parsed->valid) && !parsed->autoName.empty())
                s.fields[1].text = parsed->autoName; // Files' AutoFillName
        }
        const bool hasTarget = !s.fields[0].text.empty();
        const bool nameOk = NewItem::ValidName(s.fields[1].text);
        s.fields[0].error = hasTarget && !parsed->valid ? Tr(L"无效位置") : L"";
        s.fields[1].error = hasTarget && !nameOk ? Tr(L"指定的项目名称无效") : L"";
        s.primaryEnabled = parsed->valid && nameOk && hasTarget;
    };
    spec.browse = [owner = m_hwnd, theme = &m_theme](ContentDialog::Spec& s, int field)
    {
        const DarkMode::DialogScope dark(theme->dark, theme->chrome, theme->ink);
        // Files: SHBrowseForFolder with BIF_BROWSEINCLUDEFILES (folders and files)
        BROWSEINFOW bi{};
        bi.hwndOwner = GetActiveWindow() ? GetActiveWindow() : owner;
        bi.ulFlags = BIF_BROWSEINCLUDEFILES | BIF_NEWDIALOGSTYLE;
        bi.lpszTitle = Tr(L"选择文件夹");
        if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi))
        {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(pidl, path))
                s.fields[field].text = path;
            CoTaskMemFree(pidl);
        }
    };
    if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Primary)
        return;
    const NewItem::Target t = NewItem::ParseTarget(spec.fields[0].text);
    std::wstring created = NewItem::CreateShortcut(dir, spec.fields[1].text, t, nullptr);
    if (created.empty())
    {
        // Files' HandleShortcutCannotBeCreated: offer the desktop instead
        if (!Message(m_hwnd, &m_theme, Tr(L"在此位置创建快捷方式需要管理员权限"), Tr(L"要改为在桌面上创建快捷方式吗？"), Tr(L"创建"), Tr(L"取消")))
            return;
        PWSTR desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        {
            NewItem::CreateShortcut(desktop, spec.fields[1].text, t, nullptr);
            CoTaskMemFree(desktop);
        }
        return;
    }
    if (Tab* now = Active())
    {
        now->Focus();
        now->SelectPath(created);
    }
}

void MainWindow::ShowAddItemDialog()
{
    if (CurrentFolderPath(Active()).empty())
    {
        SetStatusText(Tr(L"无法在这里新建"), true);
        return;
    }
    NewItem::StartScan();
    // Files' AddItemDialog: 文件夹 / 文件 / 快捷方式, then every ShellNew type (name + extension, with its icon)
    ContentDialog::Spec spec;
    spec.title = Tr(L"创建新项目");
    spec.text = Tr(L"从下面选择新项目的类型");
    spec.close = Tr(L"取消");
    auto entries = std::make_shared<std::vector<NewItem::Entry>>();
    auto fill = [entries](ContentDialog::Spec& s)
    {
        s.items = { { Tr(L"文件夹"), Tr(L"创建空文件夹"), nullptr, kFolderGlyph }, { Tr(L"文件"), Tr(L"创建空文件"), nullptr, kFileGlyph },
            { Tr(L"快捷方式"), Tr(L"创建快捷方式"), nullptr, kShortcutGlyph } };
        for (const auto& e : *entries)
            s.items.push_back({ e.name, e.extension, e.icon, e.icon ? nullptr : kFileGlyph });
    };
    const bool ready = NewItem::Ready();
    if (ready)
        *entries = NewItem::Entries();
    fill(spec);
    if (!ready)
        spec.poll = [entries, fill](ContentDialog::Spec& s)
        {
            if (!NewItem::Ready())
                return false;
            *entries = NewItem::Entries();
            fill(s);
            s.poll = nullptr;
            return true;
        };
    if (ContentDialog::Show(m_hwnd, &m_theme, spec) != ContentDialog::Picked)
        return;
    if (spec.selected == 0)
        CreateItemFromDialog(true, nullptr);
    else if (spec.selected == 1)
        CreateItemFromDialog(false, nullptr);
    else if (spec.selected == 2)
        CreateShortcutFromDialog();
    else if (spec.selected - 3 < static_cast<int>(entries->size()))
    {
        const NewItem::Entry entry = (*entries)[spec.selected - 3];
        CreateItemFromDialog(false, &entry);
    }
}

void MainWindow::ShowNewMenu(const RECT& button)
{
    if (!Active())
        return;
    NewItem::StartScan();
    // Files' NewItem group: 文件夹 / 文件 / 快捷方式, then the ShellNew types (CanCreateFileInPage)
    const std::vector<NewItem::Entry> entries = NewItem::Entries();
    const int size = GetSystemMetrics(SM_CXSMICON);
    std::vector<HBITMAP> bitmaps;
    HMENU menu = CreatePopupMenu();
    auto add = [&](UINT id, const std::wstring& text, HBITMAP bmp)
    {
        AppendMenuW(menu, MF_STRING, id, text.c_str());
        if (bmp)
        {
            SetItemBitmap(menu, id, bmp);
            bitmaps.push_back(bmp);
        }
    };
    add(MenuFolder, Tr(L"文件夹\tCtrl+Shift+N"), GlyphBitmap(m_theme, Glyph::Folder, size));
    add(MenuFile, Tr(L"文件"), GlyphBitmap(m_theme, L"\xE7C3", size));
    add(MenuShortcut, Tr(L"快捷方式"), GlyphBitmap(m_theme, kShortcutGlyph, size));
    if (!entries.empty())
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < entries.size(); ++i)
        add(MenuEntry + static_cast<UINT>(i), entries[i].name,
            entries[i].icon ? NewItem::IconBitmap(entries[i].icon, size) : GlyphBitmap(m_theme, L"\xE7C3", size));
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    for (HBITMAP b : bitmaps)
        DeleteObject(b);
    if (cmd == MenuFolder)
        CreateItemFromDialog(true, nullptr);
    else if (cmd == MenuFile)
        CreateItemFromDialog(false, nullptr);
    else if (cmd == MenuShortcut)
        CreateShortcutFromDialog();
    else if (cmd >= MenuEntry && cmd - MenuEntry < entries.size())
        CreateItemFromDialog(false, &entries[cmd - MenuEntry]);
    m_barKey = -1;
}
