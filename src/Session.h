#pragma once
#include "Common.h"

struct SessionData
{
    bool hasPlacement = false;
    RECT normal{};
    UINT showCmd = SW_SHOWNORMAL;
    int sidebarWidth = 240; // 96-DPI units
    bool sidebarVisible = true;
    bool checkboxes = false;
    bool toolbarVisible = true;
    bool infoVisible = false;
    int infoWidth = 300; // 96-DPI units
    int infoPage = 0;    // InfoPane::Page
    int infoHeight = 300; // at the bottom (Files' HorizontalSizePx), 96-DPI units
    bool hasSections = false;
    unsigned hiddenSections = 0;    // Sidebar::Section bits
    unsigned collapsedSections = 0;
    int activeTab = 0;
    unsigned homeCollapsed = 0;     // HomeView::Section bits
    struct TabState
    {
        UniquePidl pidl;   // first pane
        UniquePidl second; // second pane (dual pane), null when single
        bool horizontal = false; // panes stacked top/bottom instead of side by side
        int ratio = 500;         // first pane's share, per mille
        int focus = 0;           // 0 or 1
    };
    std::vector<TabState> tabs;
};

// Tabs are stored as raw PIDL bytes so restoring never parses paths (an offline share can't stall startup).
// <program folder>\data\<name> (copied once from %APPDATA%\FastFiles; %APPDATA%\FastFiles when the program folder is
// read-only); the test's folder in test mode
std::wstring AppDataFile(const wchar_t* name);

SessionData LoadSession();
void SaveSession(const SessionData& data);

// One tab (both panes and their arrangement), for a tab dragged to another window or torn out into a new one.
// Files serializes the same thing (TabBarItemParameter with PaneNavigationArguments).
std::vector<BYTE> EncodeTab(const SessionData::TabState& tab);
bool DecodeTab(const BYTE* data, size_t size, SessionData::TabState& tab);
std::wstring BytesToHex(const std::vector<BYTE>& bytes);
std::vector<BYTE> HexToBytes(const std::wstring& hex); // empty on malformed input
