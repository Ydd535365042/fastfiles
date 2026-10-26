#include "MainWindow.h"
#include "Format.h"
#include "Lang.h"
#include "Sandbox.h"
#include "Session.h"

namespace
{
bool IsEditControl(HWND hwnd)
{
    wchar_t cls[16]{};
    return hwnd && GetClassNameW(hwnd, cls, 16) && _wcsicmp(cls, L"Edit") == 0;
}

bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }
}

bool MainWindow::PreTranslate(MSG* msg)
{
    if (!m_hwnd || (msg->hwnd != m_hwnd && !IsChild(m_hwnd, msg->hwnd)))
        return false;
    switch (msg->message)
    {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        SyncPaneFocus(msg->hwnd); // the pane being clicked or typed into becomes the focused one
        break;
    }
    if (msg->message == WM_XBUTTONUP)
    {
        if (Tab* tab = Active())
            GET_XBUTTON_WPARAM(msg->wParam) == XBUTTON1 ? tab->Back() : tab->Forward();
        return true;
    }
    if (msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST)
        return false;
    // The settings page in a tab: it records shortcuts and moves between its cards itself
    if (Tab* tab = Active(); tab && tab->Settings() && msg->hwnd == tab->Settings()->Hwnd())
    {
        const auto key = static_cast<UINT>(msg->wParam);
        if (tab->Settings()->Capturing() || tab->Settings()->WantsKey(key))
            return false;
        const bool plain = !Down(VK_CONTROL) && !Down(VK_MENU);
        if (plain && (key == VK_UP || key == VK_DOWN || key == VK_TAB || key == VK_HOME || key == VK_END || key == VK_PRIOR ||
                key == VK_NEXT || key == VK_RETURN || key == VK_SPACE || key == VK_DELETE))
            return false;
    }
    if (HandleKey(msg))
        return true;
    // Only the focused pane's view gets the accelerator (the other pane's view would act on its own selection).
    // The key's own window counts too: SyncPaneFocus above already made its pane the active one.
    Tab* tab = Active();
    return tab && (tab->HasFocus() || tab->Contains(msg->hwnd)) && tab->TranslateAccelerator(msg);
}

bool MainWindow::HandleKey(MSG* msg)
{
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)
        return false;
    const auto key = static_cast<UINT>(msg->wParam);
    const bool ctrl = Down(VK_CONTROL), shift = Down(VK_SHIFT), alt = Down(VK_MENU);
    const HWND focus = GetFocus();
    Tab* tab = Active();
    const int count = static_cast<int>(m_tabList.size());

    // Keys the edit boxes own
    if (focus == m_address)
    {
        if (m_paletteOpen && (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT))
        {
            m_palette.Move(key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -8 : 8);
            return true;
        }
        if (key == VK_RETURN)
        {
            if (m_paletteOpen)
            {
                const int id = m_palette.Selected();
                ClosePalette(true);
                if (id)
                    RunAction(id);
            }
            else
                NavigateAddress();
            return true;
        }
        if (key == VK_ESCAPE)
        {
            if (m_paletteOpen)
                ClosePalette(false);
            UpdateChrome();
            if (tab)
                tab->Focus();
            return true;
        }
    }
    if (focus == m_filter)
    {
        if (key == VK_RETURN && tab)
        {
            tab->Focus();
            return true;
        }
        if (key == VK_ESCAPE)
        {
            ToggleFilter();
            return true;
        }
    }
    if (focus == m_search)
    {
        // Files' omnibar suggestions: arrows move through them, Enter takes the highlighted one (else the typed text)
        if (m_suggest.Visible() && (key == VK_UP || key == VK_DOWN))
        {
            m_suggest.Move(key == VK_UP ? -1 : 1);
            return true;
        }
        if (key == VK_RETURN)
        {
            const SearchSuggest::Entry* picked = m_suggest.Visible() ? m_suggest.Selected() : nullptr;
            if (picked)
                PickSuggestion(static_cast<int>(picked - m_suggest.Entries().data()));
            else
            {
                m_suggest.Hide();
                RunSearch();
            }
            return true;
        }
        if (key == VK_ESCAPE && m_suggest.Visible())
        {
            m_suggest.Hide();
            return true;
        }
        if (key == VK_ESCAPE)
        {
            SetWindowTextW(m_search, L"");
            if (tab)
                tab->Focus();
            return true;
        }
    }
    const bool inEdit = IsEditControl(focus);

    // Rebindable commands (Settings > 快捷键)
    const uint32_t code = ((ctrl ? kModCtrl : 0) | (shift ? kModShift : 0) | (alt ? kModAlt : 0)) << 16 | (key & 0xFFFF);
    for (const auto& [chord, id] : m_keys)
    {
        if (chord != code)
            continue;
        const auto& list = Commands();
        const auto def = std::find_if(list.begin(), list.end(), [id = id](const CommandDef& d) { return d.id == id; });
        if (def != list.end() && (def->flags & CmdNotInEdit) && inEdit)
            break;
        // Delete / Ctrl+V belong to the file list; the sidebar, home page, settings... keep them
        if (def != list.end() && (def->flags & CmdInView) && !(tab && !tab->OnHome() && tab->HasFocus()))
            break;
        if (RunAction(id))
            return true;
        break;
    }

    // Fixed: these depend on where the focus is, like in Files
    if (ctrl && !shift && !alt && key >= '1' && key <= '9' && count > 0)
    {
        ActivateTab(key == '9' ? count - 1 : std::min(static_cast<int>(key - '1'), count - 1));
        return true;
    }
    if (!ctrl && !alt)
    {
        if (tab && key == VK_BACK && !inEdit && tab->HasFocus())
        {
            tab->Back();
            return true;
        }
        // Space: QuickLook when it's running (Files' QuickLook integration); otherwise the list keeps its own Space
        if (tab && key == VK_SPACE && !shift && !inEdit && tab->HasFocus() && tab->SelectedCount() == 1 && TryQuickLook())
            return true;
        if (tab && key == VK_BROWSER_BACK) { tab->Back(); return true; }
        if (tab && key == VK_BROWSER_FORWARD) { tab->Forward(); return true; }
    }
    return false;
}

void MainWindow::FocusAddress()
{
    SetEditMode(true);
    SetFocus(m_address);
    SendMessageW(m_address, EM_SETSEL, 0, -1);
}

void MainWindow::FocusSearch()
{
    SetFocus(m_search);
    SendMessageW(m_search, EM_SETSEL, 0, -1);
}

namespace
{
// search-ms: wants every reserved character percent-encoded as UTF-8
std::wstring UrlEncode(const std::wstring& text)
{
    std::wstring out;
    wchar_t hex[4];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
    for (unsigned char c : utf8)
    {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<wchar_t>(c);
        }
        else
        {
            swprintf_s(hex, L"%%%02X", c);
            out += hex;
        }
    }
    return out;
}
}

// --- Search suggestions (Files' PopulateOmnibarSuggestionsForSearchMode) ---

void MainWindow::UpdateSearchSuggest()
{
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(m_search)) + 1, L'\0');
    text.resize(GetWindowTextW(m_search, text.data(), static_cast<int>(text.size())));
    RECT box = m_searchFrame;
    MapWindowPoints(m_hwnd, HWND_DESKTOP, reinterpret_cast<POINT*>(&box), 2);
    ++m_suggestRequest;
    std::vector<SearchSuggest::Entry> entries;
    if (text.find_first_not_of(L" \t") == std::wstring::npos)
    {
        // Nothing typed: the previous searches
        for (const auto& q : m_settings.searchHistory)
            entries.push_back({ SearchSuggest::Kind::Recent, q, L"", -1 });
        m_suggestQuery.clear();
        m_suggest.Show(box, std::move(entries));
        return;
    }
    // The query first ("Search for"), then the current folder's matching items once found
    m_suggestQuery = { { SearchSuggest::Kind::Query, text, L"", -1 } };
    m_suggest.Show(box, m_suggestQuery);
    Tab* tab = Active();
    const std::wstring folder = CurrentFolderPath(tab);
    if (!folder.empty() && text.rfind(L"tag:", 0) != 0)
        m_suggest.FindItems(folder, text, m_settings.showHidden, m_suggestRequest);
}

void MainWindow::OnSuggestItems(unsigned request, std::vector<SearchSuggest::Entry>* raw)
{
    std::unique_ptr<std::vector<SearchSuggest::Entry>> items(raw);
    if (request != m_suggestRequest || GetFocus() != m_search || m_suggestQuery.empty())
        return; // typed on since, or the box lost the focus
    std::vector<SearchSuggest::Entry> entries = m_suggestQuery;
    entries.insert(entries.end(), std::make_move_iterator(items->begin()), std::make_move_iterator(items->end()));
    RECT box = m_searchFrame;
    MapWindowPoints(m_hwnd, HWND_DESKTOP, reinterpret_cast<POINT*>(&box), 2);
    m_suggest.Show(box, std::move(entries));
}

void MainWindow::PickSuggestion(int row)
{
    const auto& all = m_suggest.Entries();
    if (row < 0 || row >= static_cast<int>(all.size()))
        return;
    const SearchSuggest::Entry e = all[row];
    m_suggest.Hide();
    if (e.kind == SearchSuggest::Kind::Item)
    {
        // Files' NavigationHelpers.OpenPath: a folder opens here, a file opens with its app
        SetWindowTextW(m_search, L"");
        Tab* tab = Active();
        PIDLIST_ABSOLUTE raw = nullptr;
        if (!tab || FAILED(SHParseDisplayName(e.path.c_str(), nullptr, &raw, 0, nullptr)))
            return;
        UniquePidl pidl(raw);
        const DWORD attrs = GetFileAttributesW(e.path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            tab->Navigate(pidl.get());
        else if (!Sandbox::Active()) // test mode: no app is started
            ShellExecuteW(m_hwnd, nullptr, e.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        tab->Focus();
        return;
    }
    // A previous search or the query itself: search for it
    SetWindowTextW(m_search, e.text.c_str());
    RunSearch();
}

void MainWindow::SaveSearchQuery(const std::wstring& query)
{
    // Files' SaveSearchQueryToList: to the front, no duplicate, at most 10
    auto& list = m_settings.searchHistory;
    list.erase(std::remove(list.begin(), list.end(), query), list.end());
    list.insert(list.begin(), query);
    if (list.size() > 10)
        list.resize(10);
    SaveSettings(m_settings);
}

void MainWindow::RunSearch()
{
    Tab* tab = Active();
    if (!tab)
        return;
    std::wstring query(static_cast<size_t>(GetWindowTextLengthW(m_search)) + 1, L'\0');
    query.resize(GetWindowTextW(m_search, query.data(), static_cast<int>(query.size())));
    const size_t first = query.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return;
    query = query.substr(first, query.find_last_not_of(L" \t") - first + 1);
    SaveSearchQuery(query); // Files' Omnibar_QuerySubmitted: SubmitSearch, then SaveSearchQueryToList

    // "tag:名称": the tag's files (Files' tag search)
    if (RunTagQuery(tab, query))
        return;
    // FastFiles search (Rust walk, results in this tab) unless the user picked Windows Search or this isn't a file folder
    if (m_settings.searchEngine == AppSettings::SearchEngine::Fast && tab->StartSearch(query))
    {
        tab->Focus();
        return;
    }
    // Windows Search results folder: same results and columns as Explorer's search box
    std::wstring uri = L"search-ms:displayname=" + UrlEncode(L"“" + query + Tr(L"”的搜索结果")) + L"&crumb=" + UrlEncode(query);
    const std::wstring scope = PidlName(tab->Location(), SIGDN_FILESYSPATH);
    if (!scope.empty())
        uri += L"&crumb=location:" + UrlEncode(scope);
    PIDLIST_ABSOLUTE raw = nullptr;
    if (FAILED(SHParseDisplayName(uri.c_str(), nullptr, &raw, 0, nullptr)))
    {
        SetStatusText(Tr(L"无法启动搜索"), true);
        return;
    }
    UniquePidl pidl(raw);
    tab->Navigate(pidl.get());
    tab->Focus();
}
void MainWindow::NavigateAddress()
{
    Tab* tab = Active();
    if (!tab)
        return;
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(m_address)) + 1, L'\0');
    text.resize(GetWindowTextW(m_address, text.data(), static_cast<int>(text.size())));
    const size_t first = text.find_first_not_of(L" \t\"");
    const size_t last = text.find_last_not_of(L" \t\"");
    text = first == std::wstring::npos ? std::wstring() : text.substr(first, last - first + 1);

    std::wstring current = IsPagePidl(tab->Location()) ? LocationName(tab->Location()) : PidlName(tab->Location(), SIGDN_FILESYSPATH);
    if (current.empty())
        current = PidlName(tab->Location(), SIGDN_DESKTOPABSOLUTEEDITING);
    if (text.empty() || text == current)
    {
        UpdateChrome();
        tab->Focus();
        return;
    }
    if (text == LocationName(HomePidl()) || _wcsicmp(text.c_str(), L"home") == 0)
    {
        tab->Navigate(HomePidl());
        tab->Focus();
        return;
    }
    // Files' omnibar accepts "Settings" (and its localized name) as a location too
    if (text == LocationName(SettingsPidl()) || _wcsicmp(text.c_str(), L"settings") == 0)
    {
        tab->Navigate(SettingsPidl());
        tab->Focus();
        return;
    }

    wchar_t expanded[MAX_PATH * 4];
    const DWORD n = ExpandEnvironmentStringsW(text.c_str(), expanded, ARRAYSIZE(expanded));
    if (n > 0 && n <= ARRAYSIZE(expanded))
        text = expanded;

    PIDLIST_ABSOLUTE raw = nullptr;
    SFGAOF attrs = 0;
    if (FAILED(SHParseDisplayName(text.c_str(), nullptr, &raw, SFGAO_FOLDER, &attrs)))
    {
        MessageBeep(MB_ICONWARNING);
        SetStatusText(Tr(L"找不到“") + text + L"”", true);
        return;
    }
    UniquePidl pidl(raw);
    if (attrs & SFGAO_FOLDER)
    {
        tab->Navigate(pidl.get());
        tab->Focus();
        return;
    }
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = m_hwnd;
    sei.lpIDList = pidl.get();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
    {
        MessageBeep(MB_ICONWARNING);
        SetStatusText(Tr(L"无法打开“") + text + L"”", true);
    }
    UpdateChrome();
}

void MainWindow::SetStatusText(const std::wstring& text, bool hold)
{
    if (hold)
        m_statusHoldUntil = GetTickCount64() + 4000;
    if (text != m_statusText)
    {
        m_statusText = text;
        SetWindowTextW(m_status.Hwnd(), text.c_str());
    }
}

void MainWindow::UpdateStatus()
{
    UpdateFolderSize();
    UpdateActivity();
    if (GetTickCount64() < m_statusHoldUntil)
        return;
    int items = 0, selected = 0;
    Tab* tab = Active();
    std::wstring text;
    if (tab && tab->OnHome() && m_prefs.home)
    {
        const HomeData& home = *m_prefs.home;
        text = Tr(L"快速访问 ") + std::to_wstring(home.quick.size()) + Tr(L" 项 · 驱动器 ") + std::to_wstring(home.drives.size()) + Tr(L" 个");
        if (!home.network.empty())
            text += Tr(L" · 网络位置 ") + std::to_wstring(home.network.size()) + Tr(L" 个");
        if (home.recentReady)
            text += Tr(L" · 最近使用的文件 ") + std::to_wstring(home.recent.size()) + Tr(L" 个");
    }
    else if (tab && tab->GetCounts(items, selected))
    {
        text = tab->InResults() ? tab->SearchStatus() : std::to_wstring(items) + Tr(L" 个项目");
        if (selected > 0)
        {
            // Size is only re-summed when the selection actually changes
            const long long key = (static_cast<long long>(selected) << 32) | static_cast<unsigned>(tab->FocusedIndex());
            if (key != m_sizeKey)
            {
                m_sizeKey = key;
                ULONGLONG bytes = 0;
                m_sizeText = tab->SelectionSize(bytes) ? Format::Size(bytes) : L"";
            }
            text += Tr(L"\t已选择 ") + std::to_wstring(selected) + Tr(L" 个项目");
            const std::wstring folder = selected == 1 && m_sizeText.empty() ? FolderSizeText() : std::wstring();
            if (!m_sizeText.empty())
                text += L"，" + m_sizeText;
            else if (!folder.empty())
                text += L"，" + folder;
        }
    }
    SetStatusText(text, false);
    UpdateCommandStates();
    if (m_popupProvider)
        SwitchPreviewPopup();
}

void MainWindow::RestoreSession(PCIDLIST_ABSOLUTE startup)
{
    SessionData data = LoadSession();
    if (!m_restoreSession)
    {
        // Extra windows (Ctrl+N) share the sidebar setup but not the tabs or the placement
        data.hasPlacement = false;
        data.tabs.clear();
        data.activeTab = 0;
    }
    if (data.hasSections && m_sidebar)
        m_sidebar->SetSectionState(data.hiddenSections, data.collapsedSections);
    m_sidebarWidth = data.sidebarWidth;
    m_infoWidth = data.infoWidth;
    m_infoHeight = data.infoHeight;
    if (data.infoVisible)
        ToggleInfoPane(data.infoPage);
    m_prefs.checkboxes = data.checkboxes;
    if (!data.toolbarVisible)
        ToggleToolbar();
    if (!data.sidebarVisible)
        ToggleSidebar();
    m_prefs.homeCollapsed = data.homeCollapsed;
    // Settings > 常规 > 启动时: last tabs (default), the home page, or one folder
    if (m_settings.startup != AppSettings::Startup::Restore)
    {
        data.tabs.clear();
        data.activeTab = 0;
        PIDLIST_ABSOLUTE raw = nullptr;
        if (m_restoreSession && m_settings.startup == AppSettings::Startup::Path && !m_settings.startupPath.empty() &&
            SUCCEEDED(SHParseDisplayName(m_settings.startupPath.c_str(), nullptr, &raw, 0, nullptr)))
        {
            SessionData::TabState state;
            state.pidl.reset(raw);
            data.tabs.push_back(std::move(state));
        }
    }
    for (const auto& state : data.tabs)
        InsertSlot(static_cast<int>(m_tabList.size()), state);
    int active = std::min(data.activeTab, static_cast<int>(m_tabList.size()) - 1);
    if (m_startTab)
    {
        // --tab: a tab torn out of (or moved from) another window, both panes as they were
        active = InsertSlot(static_cast<int>(m_tabList.size()), *m_startTab);
        m_startTab.reset();
    }
    else if (startup)
    {
        NewTab(startup, false, true);
        active = static_cast<int>(m_tabList.size()) - 1;
        if (m_startupSelect)
            m_tabList.back().panes[0]->SelectAfterNavigate(reinterpret_cast<PCUITEMID_CHILD>(m_startupSelect.get()));
        m_startupSelect.reset();
    }
    if (m_tabList.empty())
    {
        NewTab(nullptr, false, true);
        active = 0;
    }

    if (data.hasPlacement)
    {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        wp.showCmd = data.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        if (m_startHidden)
        {
            // Placed but not shown; the first ShowFromTray maximizes if it was
            m_showMaximized = wp.showCmd == SW_SHOWMAXIMIZED;
            wp.showCmd = SW_HIDE;
        }
        wp.rcNormalPosition = data.normal;
        SetWindowPlacement(m_hwnd, &wp);
    }
    else if (!m_startHidden)
    {
        ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    }
    if (m_startHidden)
    {
        // --background: the window exists (tabs ready) but stays in the tray until opened
        m_hiddenToTray = true;
        ShowWindow(m_hwnd, SW_HIDE);
    }
    Layout();
    ActivateTab(std::max(0, active));
}

void MainWindow::SaveSessionState()
{
    SessionData data;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    // Full screen and the compact overlay are temporary: remember the window as it was before them
    if (m_fullScreen ? (wp = m_restorePlacement, true) : m_compact ? (wp = m_compactRestore, true) : GetWindowPlacement(m_hwnd, &wp) != FALSE)
    {
        data.hasPlacement = true;
        data.normal = wp.rcNormalPosition;
        const bool maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
        data.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
    data.sidebarWidth = m_sidebarWidth;
    data.sidebarVisible = m_sidebarVisible;
    data.checkboxes = m_prefs.checkboxes;
    data.toolbarVisible = m_toolbarVisible;
    data.infoVisible = m_infoVisible;
    data.infoWidth = m_infoWidth;
    data.infoHeight = m_infoHeight;
    data.infoPage = m_info && m_info->CurrentPage() == InfoPane::Page::Preview ? 1 : 0;
    if (m_sidebar)
    {
        data.hasSections = true;
        data.hiddenSections = m_sidebar->HiddenSections();
        data.collapsedSections = m_sidebar->CollapsedSections();
    }
    data.homeCollapsed = m_prefs.homeCollapsed;
    data.activeTab = std::max(0, m_active);
    for (const auto& slot : m_tabList)
    {
        SessionData::TabState state;
        state.pidl = ClonePidl(slot.panes[0]->Location());
        if (slot.Split())
        {
            state.second = ClonePidl(slot.panes[1]->Location());
            state.horizontal = slot.horizontal;
            state.ratio = slot.ratio;
            state.focus = slot.focus;
        }
        data.tabs.push_back(std::move(state));
    }
    SaveSession(data);
}
