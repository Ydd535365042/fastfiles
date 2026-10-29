#pragma once
#include "Common.h"
#include <optional>
#include "Breadcrumb.h"
#include "CommandPalette.h"
#include "Controls.h"
#include "SearchSuggest.h"
#include "FolderSize.h"
#include "Operations.h"
#include "InfoPane.h"
#include "Log.h"
#include "NewItem.h"
#include "Session.h"
#include "Settings.h"
#include "SettingsView.h"
#include "Sidebar.h"
#include "StatusCenter.h"
#include "Tab.h"
#include "TabStrip.h"
#include "Tags.h"
#include "Theme.h"
#include "Toolbar.h"
#include "RoundFrame.h"
#include "Update.h"

std::wstring FindGitExe(); // git.exe on PATH or in the usual install folders; empty when Git isn't installed
struct DroppedFiles;       // MainWindowFileOps.cpp: a drop RouteFileDrop handed to the window

// MainWindowTray.cpp: single instance and start with Windows
std::wstring PrimaryMutexName();
HWND FindPrimaryWindow();
bool ForwardToPrimary(HWND primary, const std::wstring& target); // target may be empty (just bring the window up)
bool ParseLaunchTarget(const std::wstring& arg, UniquePidl& folder, UniquePidl& select, bool forceSelect = false);
// A launch from outside (not a --window of our own) for 此电脑 opens Home instead
void ThisPcToHome(UniquePidl& folder, const UniquePidl& select);
bool AutoStartEnabled();
bool SetAutoStart(bool on);
bool DefaultFileManagerEnabled(); // folders open with this FastFiles (HKCU Folder\shell\open)
bool SetDefaultFileManager(bool on);

// One entry in the tab strip: one pane, or two side by side / stacked (Files' dual pane)
struct TabSlot
{
    ComPtr<Tab> panes[2];
    int focus = 0;           // pane the toolbar, address bar and keys act on
    bool horizontal = false; // false: left | right, true: top / bottom
    int ratio = 500;         // first pane's share, per mille
    std::wstring description, tip; // Files' TabBarItem.Description / ToolTipText ("left | right" when split)
    int icon = -1;
    bool Split() const { return panes[1] != nullptr; }
    Tab* Focused() const { return panes[Split() ? focus : 0].Get(); }
};

class MainWindow final : public TabEvents, public SidebarEvents, public BreadcrumbEvents, public TabStripEvents, public ButtonBarEvents,
                         public InfoPaneEvents, public SettingsHost
{
public:
    bool Create(PCIDLIST_ABSOLUTE startup, bool restoreSession, PCUITEMID_CHILD select = nullptr);
    void SetLaunchMode(bool primary, bool hidden); // before Create: primary instance (tray, receives launches), start in the tray
    bool PreTranslate(MSG* msg);

    void OnTabNavigated(Tab* tab) override;
    void OnTabLoading(Tab* tab, bool loading) override;
    void OnTabNavigationFailed(Tab* tab) override;
    void OnTabOpen(Tab* tab, PCIDLIST_ABSOLUTE pidl, OpenMode mode, bool activate) override;
    void OnTabHomeAction(Tab* tab, HomeAction action, const std::wstring& key, PCIDLIST_ABSOLUTE pidl, int section) override;
    void OnSidebarOpen(PCIDLIST_ABSOLUTE pidl, OpenMode mode) override;
    void OnSidebarNotice(const std::wstring& text) override;
    void OnSidebarChanged() override;
    std::vector<SettingsPage> BuildSettingsPages() override;
    std::wstring SetShortcut(const std::wstring& command, uint32_t code) override;
    void ResetShortcut(const std::wstring& command) override;
    void ClearShortcut(const std::wstring& command) override;
    void OnCrumbNavigate(PCIDLIST_ABSOLUTE pidl, bool newTab) override;
    void OnCrumbMenu(PCIDLIST_ABSOLUTE pidl, POINT screen) override;
    void OnCrumbEdit() override;
    void OnTabSelect(int index) override;
    void OnTabClose(int index) override;
    void OnTabMoved(int from, int to) override;
    void OnTabNew() override;
    void OnTabMenu(int index, POINT screen) override;
    void OnTabWheel(int delta) override;
    std::vector<BYTE> TabPayload(int index) override;
    void OnTabTornOut(int index, POINT screen) override;
    void OnTabMovedAway(int index) override;
    void OnTabArrived(const std::vector<BYTE>& payload, int index) override;
    PCIDLIST_ABSOLUTE TabFolder(int index) override;
    void OnFoldersToNewTabs(const std::vector<UniquePidl>& folders) override;
    void OnTabActions(const RECT& button) override;
    void SetStartPosition(POINT screen); // before Create: a tab torn out of another window lands here
    void SetStartTab(SessionData::TabState tab); // before Create: --tab, the torn-out tab (both panes)
    void OnBarDropdown(int id, const RECT& screen) override;
    void OnBarContext(int id, const RECT& screen) override;
    void OnInfoPaneClose() override;
    void OnTabViewMenu(Tab* tab, TabViewMenu& menu) override;
    void OnTabViewCommand(Tab* tab, UINT id) override;
    void OnSidebarTag(const std::wstring& id, const std::wstring& name, OpenMode mode) override;
    void OnInfoPaneTags(POINT screen) override;

private:
    enum : int { IdTabs = 100, IdBack, IdForward, IdUp, IdRefresh, IdAddress, IdNewTab, IdStatus, IdCrumb, IdCmdBar, IdNavBar, IdSearch, IdFilter, IdSidebar, IdInfoPane,
        IdStatusCenter };
    enum : int { CmdNewTab = 200, CmdDuplicate, CmdClose, CmdCloseOthers, CmdCloseLeft, CmdCloseRight, CmdReopen, CmdMoveToWindow };
    enum : int { BtnNew = 300, BtnCut, BtnCopy, BtnPaste, BtnRename, BtnShare, BtnDelete, BtnProperties, BtnSelect, BtnSort, BtnLayout, BtnSidebar,
        BtnRunAs, BtnRotateLeft, BtnRotateRight, BtnWallpaper, BtnInstallFont, BtnEmptyBin, BtnFilter, BtnMore, BtnInfoPane, BtnDualPane, BtnSettings,
        BtnRestoreAll, BtnRestore, BtnUpdate, BtnLast };
    enum : int { IdSideFoot = 120, IdUpdateBar = 121 };
    static constexpr int kSideFoot = 44; // settings button under the sidebar (blueprint .side .foot)
    static UINT SettingsChangedMessage(); // registered: another FastFiles process saved settings
    static UINT DebugFrameCountMessage(); // registered: test mode only, SmoothFrame::DebugFrameCount() as the LRESULT
    static std::wstring CurrentFolderPath(Tab* tab); // file system folder shown, empty for virtual folders
    // Commands without a button; reachable by WM_COMMAND (tests, future command palette)
    enum : int { ActToggleHidden = 400, ActToggleCheckboxes, ActToggleToolbar, ActFullScreen, ActCopyQuoted, ActZoomIn, ActZoomOut, ActDetailsPane, ActPreviewPane,
        ActDualPane, ActSplitVertical, ActSplitHorizontal, ActClosePane, ActFocusOtherPane, ActOpenOtherPane, ActGoHome,
        ActCloseTab, ActReopenTab, ActDuplicateTab, ActNextTab, ActPrevTab, ActNewWindow, ActCloseWindow, ActNewFolder, ActCopyPath,
        ActToggleExtensions, ActFocusAddress, ActFocusSearch, ActTerminal, ActTerminalAdmin, ActPasteInto, ActOpenNewWindow,
        ActLayoutDetails, ActLayoutList, ActLayoutTiles, ActLayoutLarge, ActFocusLeftPane, ActFocusRightPane,
        ActSettings, ActPalette, ActToggleTheme, ActEmptyBin, ActClassicProperties, ActToggleStatusBar, ActExitApp, ActGitMenu, ActOperations, ActCancelOperations,
        ActToggleCompact, ActEnterCompact, ActExitCompact, ActLayoutColumns, ActLayoutAdaptive, ActRestoreAllBin, ActRestoreFromBin,
        ActEditInNotepad, ActRunWithPowershell, ActPlayAll, ActInstallDriver, ActInstallCertificate, ActCustomizeToolbar,
        ActHelp, ActOpenLog, ActOpenLogFolder, ActOpenSettingsFile, ActNewFile, ActNewShortcut, ActAddItem, ActLaunchPreviewPopup,
        ActPaste, ActDelete, ActDeletePermanently, ActToggleDotFiles, ActCreateFolderWithSelection, ActGitInit, ActGitClone,
        ActLast };
    // Customized command bar buttons: kCustomBase + context * kCustomStride + entry index
    static constexpr int kCustomBase = 2000, kCustomStride = 100;
    static constexpr UINT kMsgRecent = WM_APP + 60; // lParam: owned std::vector<HomeItem>* from the recent-files scan
    static constexpr UINT kMsgTagged = WM_APP + 67; // lParam: owned std::vector<HomeTagGroup>* (home tags widget)
    static constexpr int kSettingsAbout = 7;        // settings page indices used from menus
    static constexpr int kSettingsShortcuts = 4;
    static constexpr int kSettingsTags = 5;
    static constexpr int kSettingsAdvanced = 6;
    static constexpr UINT_PTR kStatusTimer = 1;
    static constexpr UINT_PTR kFilterTimer = 2;
    static constexpr UINT_PTR kInfoTimer = 3; // selection polling while the details pane is open
    static constexpr UINT_PTR kMoveTimer = 4; // dragged over the live material: repaint once it rests
    static constexpr UINT_PTR kUpdateTimer = 5; // Files' startup + periodic update check (every 5 hours)
    static constexpr int kSplitter = 6;   // 96-DPI units (SidebarResizer 4 + ContentPresenter margin 2)
    static constexpr int kTitleRow = 40;  // tab strip + caption buttons
    static constexpr int kNavRow = 48;
    static constexpr int kCmdRow = 44;
    static constexpr int kStatusRow = 32;
    static constexpr int kFilterRow = 40;
    static constexpr int kPaneGap = 6;  // splitter between dual panes
    static constexpr int kPaneBar = 2;  // accent bar over each pane (lit on the focused one)

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HitTestFrame(POINT screen);

    // MainWindowLayout.cpp: chrome and layout
    void CreateChildren();
    void UpdateFonts();
    void Layout();
    void PaintChrome(HDC hdc, const RECT& rc);
    RECT ViewRect() const { return m_viewRect; }
    bool OverSplitter(int x, int y) const;
    bool OverInfoSplitter(int x, int y) const;
    void OnCommand(int id);
    void OnDestroy();
    void SetEditMode(bool editing);

    // MainWindowToolbar.cpp: Files' customizable command bar (contexts, enable states, customize / unpin)
    struct BarState
    {
        int selected = 0;
        unsigned any = 0, all = 0; // SelectionKinds of the selection
        bool fs = false, clip = false, bin = false, binItems = false;
    };
    static void BuildToolbarCatalog();
    BarState CurrentBarState() const;
    static bool CanRun(int command, const BarState& s); // Files' IsExecutable for what the bar can hold
    static unsigned ActiveContexts(const BarState& s);
    void RebuildCommandBar(unsigned contexts);
    int CustomCommand(int barId) const; // command id behind a customized button, 0 if none
    void UpdateToggleButtons();         // checked state of filter / dual pane / details pane, wherever they sit
    void LoadToolbarLists();            // m_settings.toolbar -> m_toolbarLists (and rebuild when it changed)
    void CustomizeToolbar();
    void UnpinToolbarItem(int barId);
    ToolbarModel::Lists m_toolbarLists;
    std::array<std::wstring, ToolbarModel::ContextCount> m_toolbarApplied;
    bool m_toolbarLoaded = false;
    unsigned m_barContexts = ~0u;
    long long m_barKey = -1;

    // MainWindowNewItem.cpp: Files' create commands (name prompt, AddItemDialog, new shortcut)
    std::wstring CreateItemFromDialog(bool folder, const NewItem::Entry* entry, bool select = true); // the created item's path
    void CreateFolderWithSelection(); // Files' CreateFolderWithSelection: a new folder, then the selection moved into it
    void CreateShortcutFromDialog();
    void ShowAddItemDialog();
    void SelectCreated(const std::wstring& path);

    // MainWindowCommands.cpp: command bar
    void CreateCommandBar();
    void RunCommand(int id);
    void ShowCommandMenu(int id, const RECT& button);
    // Files' 排序方式 / 分组依据 submenus (Toolbar.xaml sort flyout; the background menu hides what can't run here),
    // ids base + offset (kSortMenuIds of them), and running one
    static constexpr UINT kSortMenuIds = 32;
    HMENU SortMenu(Tab* tab, UINT base, bool background);
    HMENU GroupMenu(Tab* tab, UINT base, bool background);
    void AppendSortPriority(HMENU menu, Tab* tab, UINT base);
    bool RunSortCommand(Tab* tab, UINT offset);
    void UpdateCommandStates();
    void SetLayoutShortcut(int index);
    void CopyPath();
    void ToggleSidebar();
    void ToggleFilter();
    void ToggleHidden();
    void ToggleDotFiles();
    void ToggleExtensions();
    void ToggleCheckboxes();
    void ToggleToolbar();
    void ToggleFullScreen();
    void ApplyPrefsToTabs();
    void ShowNewMenu(const RECT& button);
    void ShowMoreMenu(const RECT& button);
    void ShowHistoryMenu(bool back, const RECT& button);
    void OpenTerminal(bool admin, const std::wstring& folder = {});
    void CopyPathQuoted();
    void OpenSelectionInNewWindow();
    void OpenSelectionInOtherPane();
    void PasteIntoSelection();
    void ToggleInfoPane(int page = -1); // -1 toggles; 0/1 opens on that page, or closes if already showing it
    void UpdateInfoPane();
    bool TryQuickLook(); // Space: Files' preview popup (QuickLook, Seer or PowerToys Peek)
    void SwitchPreviewPopup();
    std::wstring PreviewPopupPath() const;
    int m_popupProvider = 0; // the popup Space last opened: 1 QuickLook, 2 Seer, 3 Peek
    std::wstring m_popupPath;

    // MainWindowTabs.cpp
    Tab* Active() const;
    int IndexOf(const Tab* tab, int* pane = nullptr) const; // slot index; pane receives 0/1
    UniquePidl DefaultLocation() const;
    // Returns the slot index. restore: a saved tab (its own panes; no 以双窗格模式打开标签页)
    int NewTab(PCIDLIST_ABSOLUTE pidl, bool activate, bool append = false, bool restore = false);
    int OpenInNewTab(PCIDLIST_ABSOLUTE pidl, bool activate = false); // Files' OpenPathInNewTab: activate || 总是切换到新创建的标签页
    ComPtr<Tab> MakeTab(PCIDLIST_ABSOLUTE pidl);
    void CloseTab(int index);
    void CloseOtherTabs(int keep);
    void CloseTabsSide(int keep, bool left);
    void ReopenClosedTab();
    void MoveTab(int from, int to);
    void OpenNewWindow(PCIDLIST_ABSOLUTE pidl);
    void ActivateTab(int index);
    void UpdateTabLabel(int index);
    void UpdateChrome();

    // MainWindowPanes.cpp: dual pane
    template <class F> void ForEachPane(F&& f) const
    {
        for (const auto& slot : m_tabList)
            for (const auto& pane : slot.panes)
                if (pane)
                    f(pane.Get());
    }
    TabSlot* ActiveSlot();
    void PlacePanes();
    void PaneRects(const TabSlot& slot, RECT out[2], RECT* split) const;
    void SetPaneFocus(int pane, bool focusView);
    void SyncPaneFocus(HWND hwnd);
    void AddPane(PCIDLIST_ABSOLUTE pidl, bool horizontal, bool focus);
    void ClosePane(int pane);
    void OpenInOtherPane(PCIDLIST_ABSOLUTE pidl);
    void ToggleDualPane();
    void SplitPanes(bool horizontal);
    bool OverPaneSplitter(int x, int y) const;

    // MainWindowHome.cpp: home page data shared by every pane
    void RefreshHome();
    void ScanRecent();
    void EnsureHomeFresh();
    void OnRecentScanned(std::vector<HomeItem>* items);

    // MainWindowKeys.cpp: command table, key map, command palette
    struct CommandDef
    {
        int id;
        const wchar_t* key;      // settings key (key.<key>=...)
        const wchar_t* name;
        const wchar_t* category;
        const wchar_t* defaults; // comma-separated chords, may be empty
        unsigned flags;
    };
    enum : unsigned { CmdNotInEdit = 1, // edit boxes keep the key (Ctrl+H, Ctrl+Shift+arrows, ...)
        CmdInView = 2 };                // only while the file view has the focus (Delete, Ctrl+V); elsewhere the key goes on
    static const std::vector<CommandDef>& Commands();
    static const CommandDef* FindCommand(const std::wstring& key);
    std::vector<uint32_t> EffectiveChords(const CommandDef& def) const;
    static std::vector<uint32_t> DefaultChords(const CommandDef& def);
    std::wstring ChordsText(const CommandDef& def) const;
    void RebuildKeys();
    bool RunAction(int id); // false when the command doesn't apply right now (the key then goes on to the view)
    void OpenPalette();
    void ClosePalette(bool restoreChrome);
    void UpdatePalette();

    // MainWindowSettings.cpp
    void ApplySettings();
    void ApplyTheme(bool force);
    void SetLooksActive(bool active); // the tab row and the backdrop's active / inactive look (WM_ACTIVATE)
    void SaveAndBroadcast();
    void OpenSettings(int page = 0);
    void SetDefaultLayout(int layout); // also what the layout menu calls when "same layout everywhere" is on
    void SetLayoutFromUser(ViewLayout layout);
    // MainWindowRecycle.cpp
    void RestoreFromRecycleBin(bool all); // Files' RestoreAllRecycleBin / RestoreRecycleBin (confirm, then move back)

    void SetShellFlag(DWORD mask, bool on); // writes Explorer's value without the system-wide broadcast
    void ShellSettingsChanged();            // re-read shell settings in this process, refresh our views
    static constexpr WPARAM kShellFlagsChanged = 1; // SettingsChangedMessage wParam: only shell flags changed

    // MainWindowInput.cpp
    bool HandleKey(MSG* msg);
    void FocusAddress();
    void FocusSearch();
    void NavigateAddress();
    void RunSearch();
    void SetStatusText(const std::wstring& text, bool hold);
    void UpdateStatus();
    void RestoreSession(PCIDLIST_ABSOLUTE startup);
    void SaveSessionState();

    // MainWindowStatus.cpp: folder size and Git in the status bar
    struct GitResult
    {
        unsigned serial = 0;
        std::wstring root, head;
        bool detached = false;
        int ahead = -1, behind = -1; // vs the upstream (0/0 without one); -1: git.exe not available
    };
    struct GitRunResult
    {
        std::wstring label, output;
        bool ok = false;
    };
    static constexpr UINT kMsgGit = WM_APP + 62;     // lParam: owned GitResult*
    static constexpr UINT kMsgGitDone = WM_APP + 63; // lParam: owned GitRunResult*
    void UpdateFolderSize();
    std::wstring FolderSizeText() const;
    void UpdateGit(bool force);
    void OnGitResult(GitResult* result);
    void ShowGitMenu();
    void ShowGitSyncMenu();  // Files' GitNetworkActions flyout: pull / push / sync
    // In the repository root, or in `dir` (Files' GitInit / GitClone run in the current folder)
    void RunGitCommand(std::vector<std::wstring> commands, const std::wstring& label, const std::wstring& dir = {});
    void GitInit();  // Files' GitInitAction (初始化存储库)
    void GitClone(); // Files' GitCloneAction (CloneRepoDialog: 存储库 URL, 克隆)
    std::wstring GitFolder() const; // the current file-system folder when it isn't in a repository
    void OnGitDone(GitRunResult* result);

    // MainWindowOperations.cpp: the status center (Files' StatusCenter). Compress / extract removed at the user's
    // request: FastFiles has no built-in archive support, the shell's own menu items (WinRAR, 7-Zip...) cover it.
    void OnOperationDone(unsigned id);
    void UpdateActivity();
    bool ConfirmCloseWithOperations();
    StatusCenter m_center;

    // MainWindowFileOps.cpp: Files' FilesystemHelpers (paste, delete, drops) on the status center
    bool OnTabVerb(Tab* tab, const wchar_t* verb, bool background) override;
    void OnTabColumnsDefault(Tab* tab, unsigned hidden) override;
    void OnTabSortDefault(Tab* tab, const ListSort& sort) override;
    void OnTabLayoutSize(Tab* tab, int mode, int size) override;
    void OnTabCycleLayout(Tab* tab, ViewLayout layout) override;
    void PasteHere();                                  // Files' PasteItem: the clipboard into the current folder
    void PasteShortcutHere();                          // Files' PasteItemAsShortcut: a .lnk here for each clipboard item
    void ShareSelection(Tab* tab);                     // Files' ShareItem (Share.h)
    bool PasteTo(const std::wstring& destination);     // false: nothing for us (virtual items): the shell's paste
    bool DeleteSelectionOf(Tab* tab, bool permanently); // false: not file system items: the shell's delete
    void DeleteSelection(bool permanently);
    void DeleteItems(std::vector<std::wstring> paths, std::vector<UniquePidl> pidls, const std::vector<std::wstring>& names, bool permanently);
    void StartTransfer(std::vector<std::wstring> sources, const std::wstring& destination, bool move);
    bool ResolveConflicts(const std::vector<std::wstring>& sources, const std::vector<bool>& conflict, std::vector<Operations::Collision>& out);
    void EmptyRecycleBin();                            // Files' EmptyRecycleBinAction (confirm, then a card)
    void OnOperationAsk(unsigned id);                  // a worker's question (run as administrator, file in use)
    void OnDroppedFiles(DroppedFiles* drop);

    // MainWindowTags.cpp: file tags (menus, details pane row, sidebar, tag results) and the file view's right-click menu
    struct TagMenu
    {
        std::vector<FileTag> tags;
        std::vector<bool> all;           // every target already has tags[i]
        std::vector<std::wstring> paths; // what the menu tags
        std::vector<HBITMAP> bitmaps;    // colored dots shown in the menu
        std::vector<UniquePidl> folders; // view menu: selected folders (open in new tab ...)
        UniquePidl pinTarget;            // view menu: folder pin / unpin acts on
        bool pinned = false;
        std::wstring drive;              // view menu in a drive root: 格式化 acts on it
        void Clear();
    };
    static constexpr UINT kTagFirst = TabViewMenu::kAppFirst + 0x100; // tag submenu items (one per tag)
    static constexpr UINT kTagLast = kTagFirst + 0xFF;
    void AppendTagMenu(HMENU menu, UINT position, TagMenu& state);
    bool RunTagCommand(UINT id, TagMenu& state); // false: not a tag menu id
    void ShowTagsMenu(POINT screen);             // the selection's tags (details pane row)
    void UpdateInfoTags(PCIDLIST_ABSOLUTE item, bool single);
    void OnTagsChanged();
    void OpenTag(const std::wstring& id, const std::wstring& name, OpenMode mode, bool here);
    bool RunTagQuery(Tab* tab, const std::wstring& query); // "tag:名称" in the search box
    void AddTagsSettings(std::vector<SettingsPage>& pages, HWND owner);
    TagMenu m_tagMenu;

    // MainWindowTray.cpp: background process and single instance
    static constexpr UINT kMsgTray = WM_APP + 64;   // notification icon callback
    static constexpr UINT kMsgLaunch = WM_APP + 65; // lParam: owned std::wstring* forwarded by a later launch
    bool HandleTrayMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    void UpdateTray();
    void HideToTray();
    void ShowFromTray();
    void ShowTrayMenu(POINT pt);
    void ExitApp();
    void RestartApp(); // Files' RestartCommand (language change): a new process once this one has quit

    // MainWindowSettings.cpp: 关于 > 检查更新 (Update.h: the GitHub releases of the repository)
    enum class UpdateStage { Idle, Checking, Latest, Available, Downloading, Failed };
    void CheckForUpdates();
    void StartUpdate();
    void OnUpdateResult(Update::Result* result);
    std::wstring UpdateCheckText() const;
    // Files' UpdateAppButton: right of the omnibar only while a newer release is there (checked in the background at
    // startup and every 5 hours, silently: no network, no button); here it also says which version
    void AutoCheckForUpdates();
    void UpdateUpdateButton();
    bool UpdateButtonShown() const { return m_updateStage == UpdateStage::Available || m_updateStage == UpdateStage::Downloading; }
    ButtonBar m_updateBar;
    bool m_updateAuto = false;  // the running check is the background one (a failure shows nothing)
    std::wstring m_updateLabel; // the button's text (kept: ButtonBar items point at nothing of ours)
    UpdateStage m_updateStage = UpdateStage::Idle;
    Update::Release m_updateRelease;
    int m_updatePercent = 0;
    std::wstring m_updateError;

    HWND m_hwnd = nullptr;
    HWND m_address = nullptr;
    HWND m_search = nullptr;
    Theme m_theme;
    TabStrip m_strip;
    RoundFrame m_round; // rounded corners (Windows 8 / 10: region + border / shadow window)
    ButtonBar m_nav;
    ButtonBar m_cmd;
    ButtonBar m_sideFoot;
    StatusBar m_status;
    Breadcrumb m_crumb;
    HBRUSH m_contentBrush = nullptr;
    UINT m_dpi = 96;
    RECT m_omni{}, m_searchFrame{}, m_viewRect{};
    RECT m_cmdCard{}, m_fileCard{}, m_infoCard{}; // Files' rounded cards (toolbar, file area, details pane)
    static constexpr int kCardRadius = 8, kCardGap = 4, kCardMargin = 8, kCardInset = 3; // MainPage: 0,0,8,8 padding, 4 between
    int m_bodyTop = 0;
    int m_sidebarWidth = 240; // 96-DPI units
    bool m_sidebarVisible = true;
    bool m_dragging = false;
    bool m_editing = false;
    bool m_restoreSession = true;
    ULONGLONG m_statusHoldUntil = 0;
    std::wstring m_statusText;
    std::wstring m_sizeText;
    long long m_sizeKey = -1;
    HWND m_lastFocus = nullptr;
    int m_looksActive = -1; // SetLooksActive's last state (-1 none yet)

    Sidebar* m_sidebar = nullptr;
    std::unique_ptr<InfoPane> m_info;
    bool m_infoVisible = false;
    int m_infoWidth = 300; // 96-DPI units
    int m_infoHeight = 300; // when it sits under the file area (Files: window content 700 wide or less)
    bool m_infoBottom = false;
    bool m_infoDragging = false;
    RECT m_infoRect{};
    long long m_infoKey = -1;
    unsigned m_navSerial = 0; // bumped on every location change (info pane key)
    UniquePidl m_startupSelect; // file passed on the command line: selected once its folder opens
    std::vector<TabSlot> m_tabList;
    RECT m_paneSplit{};       // splitter between the active slot's panes (empty when single)
    bool m_paneDragging = false;
    std::vector<HomeItem> m_recent;
    bool m_recentReady = false;
    bool m_recentScanning = false;
    ULONGLONG m_recentScanned = 0; // tick of the last finished scan
    // Home tags widget (only scanned while it is turned on)
    std::vector<HomeTagGroup> m_tagged;
    bool m_taggedReady = false, m_taggedScanning = false, m_taggedDirty = false;
    ULONGLONG m_taggedScanned = 0;
    void ScanTagged();
    void OnTaggedScanned(std::vector<HomeTagGroup>* groups);
    std::vector<UniquePidl> m_closedTabs;
    ViewPrefs m_prefs;
    bool m_toolbarVisible = true;
    bool m_filterVisible = false;
    bool m_fullScreen = false;
    WINDOWPLACEMENT m_restorePlacement{ sizeof(WINDOWPLACEMENT) };
    bool m_compact = false; // Files' 精简模式 (F12): small, always on top
    WINDOWPLACEMENT m_compactRestore{ sizeof(WINDOWPLACEMENT) };
    bool m_hasStartPosition = false;
    POINT m_startPosition{};
    std::optional<SessionData::TabState> m_startTab;
    void SetCompactOverlay(bool on);
    void RemoveTab(int index, bool remember); // remember: offer it to "reopen closed tab" (a tab moved to another window isn't)
    int InsertSlot(int index, const SessionData::TabState& state); // both panes; not activated
    SessionData::TabState SlotState(int index) const;
    void OpenTabInNewWindow(int index, const POINT* drop); // Files: OpenTabInNewWindowAsync (serialized tab, drop point)
    void RefreshTabHeaders(); // Files: RefreshTabPathHints (same-named tabs get a parent folder hint)
    unsigned m_selKinds = 0, m_selAll = 0; // SelectionKinds: any / every selected item
    long long m_selKey = -1;
    unsigned m_selStamp = 0;
    bool m_inRecycleBin = false;
    HWND m_filter = nullptr;
    RECT m_filterFrame{};
    ComPtr<IContextMenu2> m_menuHandler; // owner-drawn menu (system New menu) being tracked
    int m_active = -1;

    AppSettings m_settings;
    std::vector<std::pair<uint32_t, int>> m_keys; // effective shortcut -> command id
    CommandPalette m_palette;
    bool m_paletteOpen = false;
    // Dragging the window over the live material: no repaint while it moves, once it rests or is dropped (WM_MOVE)
    bool m_moving = false;
    void RepaintMaterial(); // our surfaces that show the material, for where the window is now
    // Files' search mode suggestions under the search box (MainWindowInput.cpp)
    SearchSuggest m_suggest;
    unsigned m_suggestRequest = 0;
    std::vector<SearchSuggest::Entry> m_suggestQuery; // the "Search for" row shown while the items are looked for
    void UpdateSearchSuggest();
    void OnSuggestItems(unsigned request, std::vector<SearchSuggest::Entry>* items);
    void PickSuggestion(int row);
    void SaveSearchQuery(const std::wstring& query); // Files' SaveSearchQueryToList
    bool m_statusVisible = true;   // the setting
    bool m_statusShown = true;     // ... and not on Home / Settings (Files' BaseShellPage.IsStatusBarVisible)
    bool StatusWanted() const;
    void RefreshShellViews();   // our views re-read a changed shell setting (no system broadcast)
    void ApplyHiddenSettings(); // m_settings.showHidden / showProtected -> views, sidebar
    int m_themeApplied = -1; // -1 never, 0 light, 1 dark

    Operations m_ops;
    FolderSizer m_sizer;
    long long m_folderKey = -1;
    std::wstring m_folderTarget; // folder the selection points at ("" = nothing to size)
    UniquePidl m_folderPidl;
    ULONGLONG m_folderSince = 0; // when the target last changed (walks start once it rests)
    std::wstring m_gitDir, m_gitRoot, m_gitHead;
    bool m_gitDetached = false;
    bool m_gitBusy = false;
    unsigned m_gitSerial = 0;
    int m_gitAhead = -1, m_gitBehind = -1;
    std::wstring m_gitFetchedRoot;  // Files fetches origin when a repository is entered: once per root ...
    ULONGLONG m_gitFetchedAt = 0;   // ... and not again within a minute

    bool m_primary = false;      // first plain launch: owns the tray icon and receives later launches
    bool m_startHidden = false;  // --background (start with Windows)
    bool m_hiddenToTray = false;
    bool m_trayShown = false;
    bool m_quitting = false;     // tray > 退出: close for real even with background on
    bool m_showMaximized = false; // started hidden from a maximized session
};
