#include "Settings.h"
#include "Session.h"
#include "Toolbar.h"
#include "fastfiles_core.h"
#include <sstream>

namespace
{
const uint16_t* U16(const std::wstring& s) { return reinterpret_cast<const uint16_t*>(s.c_str()); }

const wchar_t* const kLayouts[] = { L"details", L"list", L"tiles", L"content", L"small", L"medium", L"large", L"xlarge", L"columns", L"adaptive" };
// layout.sort in SortKey order, layout.group in FileList::kGroup* order, layout.group.unit
const wchar_t* const kSortNames[] = { L"name", L"modified", L"type", L"size", L"tag", L"created" };
const wchar_t* const kGroupNames[] = { L"none", L"name", L"modified", L"created", L"size", L"type", L"tag" };
const wchar_t* const kUnitNames[] = { L"year", L"month", L"day" };

template <size_t N>
int IndexOf(const wchar_t* const (&names)[N], const std::wstring& value)
{
    for (size_t i = 0; i < N; ++i)
        if (value == names[i])
            return static_cast<int>(i);
    return -1;
}

std::vector<std::wstring> SplitNames(const std::wstring& value)
{
    std::vector<std::wstring> out;
    std::wstringstream in(value);
    for (std::wstring part; std::getline(in, part, L',');)
        if (!part.empty())
            out.push_back(part);
    return out;
}

std::wstring JoinNames(const std::vector<std::wstring>& names)
{
    std::wstring out;
    for (const auto& n : names)
        out += (out.empty() ? L"" : L",") + n;
    return out;
}
}

std::string ToUtf8(const std::wstring& text)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, bytes)), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const char* data, size_t len)
{
    if (!data || len == 0)
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, chars)), L'\0');
    if (chars > 0)
        MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), out.data(), chars);
    return out;
}

bool AppSettings::Has(const std::vector<std::wstring>& list, const wchar_t* name) const
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

void AppSettings::Toggle(std::vector<std::wstring>& list, const wchar_t* name, bool present)
{
    const auto it = std::find(list.begin(), list.end(), name);
    if (present && it == list.end())
        list.emplace_back(name);
    else if (!present && it != list.end())
        list.erase(it);
}

const std::vector<uint32_t>* AppSettings::Override(const std::wstring& command) const
{
    for (const auto& [key, chords] : shortcuts)
        if (key == command)
            return &chords;
    return nullptr;
}

std::wstring SettingsPath()
{
    return AppDataFile(L"settings.txt");
}

AppSettings LoadSettings()
{
    AppSettings s;
    const std::wstring path = SettingsPath();
    const FfBuffer buffer = ff_settings_load(U16(path), path.size());
    const std::wstring text = FromUtf8(reinterpret_cast<const char*>(buffer.data), buffer.len);
    ff_buffer_free(buffer);
    // The core already validated every value; anything unexpected here just keeps the default
    std::wstringstream in(text);
    for (std::wstring line; std::getline(in, line);)
    {
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0 || line[0] == L'#')
            continue;
        const std::wstring key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == L"startup")
            s.startup = value == L"home" ? AppSettings::Startup::Home : value == L"path" ? AppSettings::Startup::Path : AppSettings::Startup::Restore;
        else if (key == L"startup.path")
            s.startupPath = value;
        else if (key == L"newtab")
            s.newTab = value == L"current" ? AppSettings::NewTab::Current : AppSettings::NewTab::Home;
        else if (key == L"theme")
            s.theme = value == L"light" ? AppSettings::ThemeMode::Light : value == L"dark" ? AppSettings::ThemeMode::Dark : AppSettings::ThemeMode::System;
        else if (key == L"theme.background")
            s.themeBackground = value;
        else if (key == L"language")
            s.language = value;
        else if (key == L"theme.font")
            s.themeFont = value;
        else if (key == L"format.date")
            s.dateFormat = value == L"system" ? 1 : value == L"universal" ? 2 : 0;
        else if (key == L"format.size")
            s.sizeDecimal = value == L"decimal";
        else if (key == L"folder.hoverselect")
            s.hoverSelect = value != L"0";
        else if (key == L"theme.image")
            s.bgImage = value;
        else if (key == L"theme.image.fit")
            s.bgImageFit = value == L"none" ? 0 : value == L"fill" ? 1 : value == L"uniform" ? 2 : 3;
        else if (key == L"theme.image.opacity")
            s.bgImageOpacity = std::clamp(_wtoi(value.c_str()), 10, 100);
        else if (key == L"theme.image.vertical")
            s.bgImageVertical = value == L"top" ? 0 : value == L"bottom" ? 2 : 1;
        else if (key == L"theme.image.horizontal")
            s.bgImageHorizontal = value == L"left" ? 0 : value == L"right" ? 2 : 1;
        else if (key == L"theme.backdrop")
        {
            s.themeBackdrop = value == L"solid" ? 0 : value == L"mica" ? 3 : 4;
        }
        else if (key == L"statusbar")
            s.statusBar = value != L"0";
        else if (key == L"toolbar.hidden")
            s.toolbarHidden = SplitNames(value);
        else if (key == L"layout.default")
        {
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(kLayouts)); ++i)
                if (value == kLayouts[i])
                    s.defaultLayout = i;
        }
        else if (key == L"layout.sync")
            s.syncLayout = value == L"1";
        else if (key == L"home.hidden")
            s.homeHidden = SplitNames(value);
        else if (key == L"home.tags")
            s.homeTags = value == L"1";
        else if (key.rfind(L"toolbar.", 0) == 0 && key != L"toolbar.hidden")
        {
            for (int c = 0; c < ToolbarModel::ContextCount; ++c)
                if (key.compare(8, std::wstring::npos, ToolbarModel::ContextKey(c)) == 0)
                    s.toolbar[c] = value;
        }
        else if (key == L"search.engine")
            s.searchEngine = value == L"windows" ? AppSettings::SearchEngine::Windows : AppSettings::SearchEngine::Fast;
        else if (key == L"folder.sizes")
            s.folderSizes = value != L"0";
        else if (key == L"instance.single")
            s.singleInstance = value != L"0";
        else if (key == L"background")
            s.background = value == L"1";
        else if (key == L"tabs.reversewheel")
            s.reverseTabWheel = value == L"1";
        else if (key == L"tabs.actions")
            s.tabActions = value != L"0";
        else if (key == L"folder.hidden")
            s.showHidden = value == L"1";
        else if (key == L"folder.protected")
            s.showProtected = value == L"1";
        else if (key == L"view.columnlines")
            s.columnLines = value == L"1";
        else if (key == L"delete.confirm")
            s.deleteConfirm = value == L"never" ? AppSettings::DeleteConfirm::Never : value == L"permanent" ? AppSettings::DeleteConfirm::Permanent
                                                                                                       : AppSettings::DeleteConfirm::Always;
        else if (key == L"statuscenter")
            s.statusCenterAlways = value != L"operations";
        else if (key == L"statuscenter.tip")
            s.statusCenterTip = value != L"0";
        else if (key == L"tabs.switchnew")
            s.switchNewTab = value == L"1";
        else if (key == L"panes.dualnew")
            s.dualPaneNewTab = value == L"1";
        else if (key == L"panes.arrangement")
            s.paneHorizontal = value == L"horizontal";
        else if (key == L"menu.hidden")
            s.menuHidden = SplitNames(value);
        else if (key == L"folder.dotfiles")
            s.showDotFiles = value != L"0";
        else if (key == L"folder.newtab")
            s.openFoldersNewTab = value == L"1";
        else if (key == L"folder.dblclickup")
            s.doubleClickUp = value != L"0";
        else if (key == L"folder.selectprevious")
            s.selectPrevious = value != L"0";
        else if (key == L"layout.sortpriority")
            s.sortPriority = value == L"files" ? 1 : value == L"together" ? 2 : 0;
        else if (key == L"layout.sort")
            s.defaultSort = std::max(0, IndexOf(kSortNames, value));
        else if (key == L"layout.sort.desc")
            s.defaultSortDesc = value == L"1";
        else if (key == L"layout.group")
            s.defaultGroup = std::max(0, IndexOf(kGroupNames, value));
        else if (key == L"layout.group.desc")
            s.defaultGroupDesc = value == L"1";
        else if (key == L"layout.group.unit")
            s.defaultGroupUnit = std::max(0, IndexOf(kUnitNames, value));
        else if (key == L"layout.size.details")
            s.sizeDetails = std::clamp(_wtoi(value.c_str()), 1, 5);
        else if (key == L"layout.size.list")
            s.sizeList = std::clamp(_wtoi(value.c_str()), 1, 5);
        else if (key == L"layout.size.cards")
            s.sizeCards = std::clamp(_wtoi(value.c_str()), 1, 4);
        else if (key == L"layout.size.grid")
            s.sizeGrid = std::clamp(_wtoi(value.c_str()), 1, 12);
        else if (key == L"layout.size.columns")
            s.sizeColumns = std::clamp(_wtoi(value.c_str()), 1, 5);
        else if (key == L"details.hidden")
            s.detailsHidden = SplitNames(value);
        else if (key == L"tray")
            s.trayIcon = value != L"0";
        else if (key == L"search.history")
        {
            s.searchHistory.clear();
            for (size_t start = 0; start <= value.size();)
            {
                size_t end = value.find(L'\t', start);
                if (end == std::wstring::npos)
                    end = value.size();
                if (end > start)
                    s.searchHistory.push_back(value.substr(start, end - start));
                start = end + 1;
            }
        }
        else if (key.rfind(L"key.", 0) == 0)
        {
            std::vector<uint32_t> chords;
            for (const auto& chord : SplitNames(value))
                if (const uint32_t code = ParseShortcut(chord))
                    chords.push_back(code);
            s.shortcuts.emplace_back(key.substr(4), std::move(chords));
        }
    }
    return s;
}

bool SaveSettings(const AppSettings& s)
{
    static const wchar_t* const startup[] = { L"restore", L"home", L"path" };
    static const wchar_t* const theme[] = { L"system", L"light", L"dark" };
    std::wstring text;
    text += L"startup=" + std::wstring(startup[static_cast<int>(s.startup)]) + L"\n";
    text += L"startup.path=" + s.startupPath + L"\n";
    text += std::wstring(L"newtab=") + (s.newTab == AppSettings::NewTab::Current ? L"current" : L"home") + L"\n";
    text += L"theme=" + std::wstring(theme[static_cast<int>(s.theme)]) + L"\n";
    text += L"theme.background=" + s.themeBackground + L"\n";
    text += L"language=" + s.language + L"\n";
    text += L"theme.font=" + s.themeFont + L"\n";
    text += std::wstring(L"format.date=") + (s.dateFormat == 1 ? L"system" : s.dateFormat == 2 ? L"universal" : L"application") + L"\n";
    text += std::wstring(L"format.size=") + (s.sizeDecimal ? L"decimal" : L"binary") + L"\n";
    text += std::wstring(L"folder.hoverselect=") + (s.hoverSelect ? L"1" : L"0") + L"\n";
    static const wchar_t* const fits[] = { L"none", L"fill", L"uniform", L"uniformtofill" };
    static const wchar_t* const verticals[] = { L"top", L"center", L"bottom" };
    static const wchar_t* const horizontals[] = { L"left", L"center", L"right" };
    text += L"theme.image=" + s.bgImage + L"\n";
    text += std::wstring(L"theme.image.fit=") + fits[std::clamp(s.bgImageFit, 0, 3)] + L"\n";
    text += L"theme.image.opacity=" + std::to_wstring(std::clamp(s.bgImageOpacity / 10 * 10, 10, 100)) + L"\n";
    text += std::wstring(L"theme.image.vertical=") + verticals[std::clamp(s.bgImageVertical, 0, 2)] + L"\n";
    text += std::wstring(L"theme.image.horizontal=") + horizontals[std::clamp(s.bgImageHorizontal, 0, 2)] + L"\n";
    text += std::wstring(L"theme.backdrop=") + (s.themeBackdrop == 0 ? L"solid" : s.themeBackdrop == 3 ? L"mica" : L"micaalt") + L"\n";
    text += std::wstring(L"statusbar=") + (s.statusBar ? L"1" : L"0") + L"\n";
    text += L"toolbar.hidden=" + JoinNames(s.toolbarHidden) + L"\n";
    for (int c = 0; c < ToolbarModel::ContextCount; ++c)
        text += std::wstring(L"toolbar.") + ToolbarModel::ContextKey(c) + L"=" + s.toolbar[c] + L"\n";
    text += L"layout.default=" + std::wstring(kLayouts[std::clamp(s.defaultLayout, 0, static_cast<int>(ARRAYSIZE(kLayouts)) - 1)]) + L"\n";
    text += std::wstring(L"layout.sync=") + (s.syncLayout ? L"1" : L"0") + L"\n";
    text += L"home.hidden=" + JoinNames(s.homeHidden) + L"\n";
    text += std::wstring(L"home.tags=") + (s.homeTags ? L"1" : L"0") + L"\n";
    text += std::wstring(L"search.engine=") + (s.searchEngine == AppSettings::SearchEngine::Windows ? L"windows" : L"fast") + L"\n";
    text += std::wstring(L"folder.sizes=") + (s.folderSizes ? L"1" : L"0") + L"\n";
    text += std::wstring(L"instance.single=") + (s.singleInstance ? L"1" : L"0") + L"\n";
    text += std::wstring(L"background=") + (s.background ? L"1" : L"0") + L"\n";
    text += std::wstring(L"tabs.reversewheel=") + (s.reverseTabWheel ? L"1" : L"0") + L"\n";
    text += std::wstring(L"tabs.actions=") + (s.tabActions ? L"1" : L"0") + L"\n";
    text += std::wstring(L"folder.hidden=") + (s.showHidden ? L"1" : L"0") + L"\n";
    text += std::wstring(L"folder.protected=") + (s.showProtected ? L"1" : L"0") + L"\n";
    text += std::wstring(L"view.columnlines=") + (s.columnLines ? L"1" : L"0") + L"\n";
    static const wchar_t* const confirm[] = { L"always", L"permanent", L"never" };
    text += L"delete.confirm=" + std::wstring(confirm[static_cast<int>(s.deleteConfirm)]) + L"\n";
    text += std::wstring(L"statuscenter=") + (s.statusCenterAlways ? L"always" : L"operations") + L"\n";
    text += std::wstring(L"statuscenter.tip=") + (s.statusCenterTip ? L"1" : L"0") + L"\n";
    text += std::wstring(L"tabs.switchnew=") + (s.switchNewTab ? L"1" : L"0") + L"\n";
    text += std::wstring(L"panes.dualnew=") + (s.dualPaneNewTab ? L"1" : L"0") + L"\n";
    text += std::wstring(L"panes.arrangement=") + (s.paneHorizontal ? L"horizontal" : L"vertical") + L"\n";
    text += L"menu.hidden=" + JoinNames(s.menuHidden) + L"\n";
    text += std::wstring(L"folder.dotfiles=") + (s.showDotFiles ? L"1" : L"0") + L"\n";
    text += std::wstring(L"folder.newtab=") + (s.openFoldersNewTab ? L"1" : L"0") + L"\n";
    text += std::wstring(L"folder.dblclickup=") + (s.doubleClickUp ? L"1" : L"0") + L"\n";
    text += std::wstring(L"folder.selectprevious=") + (s.selectPrevious ? L"1" : L"0") + L"\n";
    text += std::wstring(L"layout.sort=") + kSortNames[std::clamp(s.defaultSort, 0, 5)] + L"\n";
    text += std::wstring(L"layout.sort.desc=") + (s.defaultSortDesc ? L"1" : L"0") + L"\n";
    text += std::wstring(L"layout.sortpriority=") + (s.sortPriority == 1 ? L"files" : s.sortPriority == 2 ? L"together" : L"folders") + L"\n";
    text += std::wstring(L"layout.group=") + kGroupNames[std::clamp(s.defaultGroup, 0, 6)] + L"\n";
    text += std::wstring(L"layout.group.desc=") + (s.defaultGroupDesc ? L"1" : L"0") + L"\n";
    text += std::wstring(L"layout.group.unit=") + kUnitNames[std::clamp(s.defaultGroupUnit, 0, 2)] + L"\n";
    text += L"details.hidden=" + JoinNames(s.detailsHidden) + L"\n";
    text += L"layout.size.details=" + std::to_wstring(s.sizeDetails) + L"\nlayout.size.list=" + std::to_wstring(s.sizeList) +
        L"\nlayout.size.cards=" + std::to_wstring(s.sizeCards) + L"\nlayout.size.grid=" + std::to_wstring(s.sizeGrid) +
        L"\nlayout.size.columns=" + std::to_wstring(s.sizeColumns) + L"\n";
    text += std::wstring(L"tray=") + (s.trayIcon ? L"1" : L"0") + L"\n";
    {
        std::wstring history;
        for (const auto& q : s.searchHistory)
            history += (history.empty() ? L"" : L"\t") + q;
        text += L"search.history=" + history + L"\n";
    }
    for (const auto& [command, chords] : s.shortcuts)
    {
        std::wstring value;
        for (uint32_t c : chords)
            value += (value.empty() ? L"" : L",") + FormatShortcut(c);
        text += L"key." + command + L"=" + value + L"\n";
    }
    const std::wstring path = SettingsPath();
    const std::string utf8 = ToUtf8(text);
    return ff_settings_save(U16(path), path.size(), reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
}

int ImportSettings(const std::wstring& from)
{
    const std::wstring to = SettingsPath();
    return static_cast<int>(ff_settings_import(U16(from), from.size(), U16(to), to.size()));
}

bool ExportSettings(const std::wstring& to)
{
    const std::wstring from = SettingsPath();
    const FfBuffer buffer = ff_settings_load(U16(from), from.size());
    const bool ok = ff_store_write(U16(to), to.size(), buffer.data, buffer.len);
    ff_buffer_free(buffer);
    return ok;
}

std::wstring FormatShortcut(uint32_t code)
{
    const FfBuffer buffer = ff_shortcut_format(code);
    std::wstring text = FromUtf8(reinterpret_cast<const char*>(buffer.data), buffer.len);
    ff_buffer_free(buffer);
    return text;
}

uint32_t ParseShortcut(const std::wstring& text)
{
    return text.empty() ? 0 : ff_shortcut_parse(U16(text), text.size());
}

bool ShortcutBindable(uint32_t code)
{
    return ff_shortcut_bindable(code);
}
