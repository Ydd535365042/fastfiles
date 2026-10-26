#pragma once
#include "Theme.h"
#include <atomic>
#include <memory>

// Files' omnibar search mode suggestions (NavigationToolbarViewModel.PopulateOmnibarSuggestionsForSearchMode), under
// the search box: with nothing typed, the previous searches (PreviousSearchQueriesList, newest first, up to 10); while
// typing, "Search for `text`" first, then up to 10 items of the current folder whose names match (FolderSearch,
// MaxItemCount 10, after 200 ms). The search box keeps the focus; the list never activates.
class SearchSuggest
{
public:
    enum class Kind { Query, Recent, Item };
    struct Entry
    {
        Kind kind = Kind::Item;
        std::wstring text; // the query, or the item's name
        std::wstring path; // Item: its full path
        int icon = -1;     // Item: system image list index
    };

    ~SearchSuggest();
    bool Create(HWND owner, const Theme* theme);
    void Show(const RECT& below, std::vector<Entry> entries); // screen rect of the search box
    void Hide();
    bool Visible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    void Move(int delta);
    const Entry* Selected() const; // null when nothing is highlighted
    const std::vector<Entry>& Entries() const { return m_entries; }
    void Relayout();

    // Folder items matching text, found on a worker (Rust ff_search, current folder and below, at most 10); posts
    // kMsgItems to the owner with lParam an owned std::vector<Entry>* and wParam the request number
    void FindItems(const std::wstring& folder, const std::wstring& text, bool hidden, unsigned request);

    static constexpr UINT kMsgPick = WM_APP + 72;  // a row was clicked: wParam = row
    static constexpr UINT kMsgItems = WM_APP + 73; // FindItems finished

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc, const RECT& rc);
    void Resize();
    int RowAt(int y) const;

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    const Theme* m_theme = nullptr;
    std::vector<Entry> m_entries;
    int m_selected = -1;
    int m_hot = -1;
    RECT m_below{};
    std::shared_ptr<std::atomic<bool>> m_cancel; // the running FindItems
};
