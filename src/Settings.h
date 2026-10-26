#pragma once
#include "Common.h"
#include <array>

// User settings (%APPDATA%\FastFiles\settings.txt). Parsing, validation and defaults live in the Rust core
// (core/src/settings.rs); this is the typed view the UI works with.
struct AppSettings
{
    enum class Startup { Restore, Home, Path };
    enum class NewTab { Home, Current };
    enum class ThemeMode { System, Light, Dark };

    Startup startup = Startup::Restore;
    std::wstring startupPath;
    NewTab newTab = NewTab::Home;
    ThemeMode theme = ThemeMode::System;
    std::wstring themeBackground = L"#00000000"; // Files' AppThemeBackgroundColor (#AARRGGBB)
    std::wstring language; // Files' AppLanguage code, empty = 使用系统设置 (takes effect after a restart)
    std::wstring themeFont;                       // Files' AppThemeFontFamily, empty = the standard font
    int dateFormat = 0;       // Files' DateTimeFormat: 0 应用, 1 系统, 2 通用
    bool sizeDecimal = false; // Files' SizeUnitFormat: binary (default) / decimal
    bool hoverSelect = false; // Files' SelectFilesOnHover
    int themeBackdrop = 4;
    std::wstring bgImage;     // Files' AppThemeBackgroundImageSource
    int bgImageFit = 3;       // 0 无, 1 填充, 2 均匀, 3 均匀填充 (default)
    int bgImageOpacity = 100; // 10..100
    int bgImageVertical = 1, bgImageHorizontal = 1; // 0 top/left, 1 center, 2 bottom/right                        // Files' AppThemeBackdropMaterial: 0 solid, 1 acrylic, 2 thin acrylic, 3 mica, 4 mica alt
    bool statusBar = true;
    std::vector<std::wstring> toolbarHidden; // before 0.9.8: command bar buttons turned off (migrated into toolbar[0])
    // Files' CustomToolbarItems: one entry list per ToolbarModel::Context ("" = that context's defaults)
    std::array<std::wstring, 11> toolbar;
    int defaultLayout = 9;                   // ViewLayout (Files: Adaptive)
    bool syncLayout = false;                 // every folder uses defaultLayout
    std::vector<std::wstring> homeHidden;    // home widgets: quick, drives, network, recent
    bool homeTags = false;                   // Files' ShowFileTagsWidget (default off)
    enum class SearchEngine { Fast, Windows };
    SearchEngine searchEngine = SearchEngine::Fast; // search box: FastFiles (Rust, file names) or Windows Search
    bool folderSizes = true;                 // size the selected folder (status bar, details pane)
    bool singleInstance = true;              // launching again opens a tab in the running window
    bool background = false;                 // closing the window leaves FastFiles in the notification area
    bool reverseTabWheel = false;            // Files: 反转标签页滚轮切换方向
    bool tabActions = true;                  // Files: 在标题栏中显示标签操作按钮
    bool showHidden = false;                 // Files: 显示隐藏的项目 (FastFiles' own, the system setting is untouched)
    bool showProtected = false;              // Files: 显示受保护的系统文件
    bool columnLines = false;                // vertical lines between details columns
    enum class DeleteConfirm { Always, Permanent, Never };
    DeleteConfirm deleteConfirm = DeleteConfirm::Always; // Files' DeleteConfirmationPolicy
    bool statusCenterAlways = true;          // Files' StatusCenterVisibility: Always / DuringOngoingFileOperations
    bool statusCenterTip = true;             // Files' ShowStatusCenterTeachingTip (shown once, on the first operation)
    bool switchNewTab = false;               // Files' AlwaysSwitchToNewlyOpenedTab
    bool dualPaneNewTab = false;             // Files' AlwaysOpenDualPaneInNewTab
    bool paneHorizontal = false;             // Files' ShellPaneArrangementOption: Vertical (side by side) / Horizontal (stacked)
    std::vector<std::wstring> menuHidden;    // Files' context menu options turned off (MenuOptions keys)
    bool showDotFiles = true;                // Files' ShowDotFiles
    bool openFoldersNewTab = false;          // Files' OpenFoldersInNewTab
    bool doubleClickUp = true;               // Files' DoubleClickToGoUp
    bool selectPrevious = true;              // Files' ScrollToPreviousFolderWhenNavigatingUp
    int sortPriority = 0;                    // Files' 排序优先级: 0 folders first, 1 files first, 2 together
    // Files' DefaultSortOption / DefaultDirectorySortDirection (SortKey values) and DefaultGroupOption /
    // DefaultDirectoryGroupDirection / DefaultGroupByDateUnit (FileList::kGroup*; 0 year, 1 month, 2 day)
    int defaultSort = 0;
    bool defaultSortDesc = false;
    int defaultGroup = 0, defaultGroupUnit = 0;
    bool defaultGroupDesc = false;
    // Files' *ViewSizeKind (1-based like Files): details / list 1-5 (Small = 2), cards 1-4 (Small = 1), grid 1-12 (Large = 8)
    int sizeDetails = 2, sizeList = 2, sizeCards = 1, sizeGrid = 8, sizeColumns = 2;
    std::vector<std::wstring> detailsHidden{ L"created" }; // Files' Show*Column off (own list): tag modified created type size
    bool trayIcon = true;                    // Files' ShowSystemTrayIcon
    std::vector<std::wstring> searchHistory; // Files' PreviousSearchQueriesList (newest first, at most 10)
    // Shortcut overrides: command key -> chords (empty vector = unbound). Commands not listed use their defaults.
    std::vector<std::pair<std::wstring, std::vector<uint32_t>>> shortcuts;

    bool Has(const std::vector<std::wstring>& list, const wchar_t* name) const;
    static void Toggle(std::vector<std::wstring>& list, const wchar_t* name, bool present);
    const std::vector<uint32_t>* Override(const std::wstring& command) const;
};

std::wstring SettingsPath();
AppSettings LoadSettings();
bool SaveSettings(const AppSettings& settings);
int ImportSettings(const std::wstring& from); // valid settings found (0: nothing recognizable), -1: I/O error
bool ExportSettings(const std::wstring& to);

// Shortcut codes: modifiers << 16 | virtual key, modifiers 1 Ctrl, 2 Shift, 4 Alt (core/src/shortcut.rs)
constexpr uint32_t kModCtrl = 1, kModShift = 2, kModAlt = 4;
std::wstring FormatShortcut(uint32_t code);
uint32_t ParseShortcut(const std::wstring& text);
bool ShortcutBindable(uint32_t code);

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const char* data, size_t len);
