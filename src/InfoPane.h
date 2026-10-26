#pragma once
#include "Theme.h"
#include "SmoothScroll.h"
#include <memory>

class MediaPreview;

struct InfoPaneEvents
{
    virtual void OnInfoPaneClose() = 0;
    virtual void OnInfoPaneTags(POINT) {} // the tags row was clicked (screen point): edit the item's tags
};

// Files' details/preview pane (UserControls/Pane/InfoPane.xaml): a Details page (thumbnail + property list, the same
// properties Explorer's details pane shows) and a Preview page (images, text/code, zip contents, system preview handlers).
// Everything that can touch the disk runs on a worker; only the preview handler itself is hosted on the UI thread.
class InfoPane
{
public:
    enum class Page { Details, Preview };

    explicit InfoPane(InfoPaneEvents* events) : m_events(events) {}
    ~InfoPane();

    bool Create(HWND parent, int id, const Theme* theme);
    void Destroy();
    void Move(const RECT& rc);
    void Relayout();
    HWND Hwnd() const { return m_hwnd; }
    Page CurrentPage() const { return m_page; }
    void SetPage(Page page);

    // One item (selected item, or the folder itself when nothing is selected)
    void ShowItem(PCIDLIST_ABSOLUTE pidl);
    // Several selected items: count and total size only
    void ShowMany(int count, const std::wstring& size);
    // Rows appended to the details page (folder size from MainWindow's FolderSizer); ignored unless pidl is the item shown
    void SetExtra(PCIDLIST_ABSOLUTE pidl, std::vector<std::pair<std::wstring, std::wstring>> rows);
    // Tags of the item shown (name, color); shown = false hides the row (several items, virtual items)
    void SetTags(std::vector<std::pair<std::wstring, COLORREF>> tags, bool shown);
    PCIDLIST_ABSOLUTE Item() const { return m_pidl.get(); }

    // Worker types (InfoPaneData.cpp)
    // Rich: Markdown / code / .rtf in a RichEdit (Files' MarkdownPreview, CodePreview, RichTextPreview); Media: MediaPreview
    enum class Kind { None, Image, Text, Archive, Handler, Many, Rich, Media };
    struct Result;
    struct ArchiveEntry
    {
        std::wstring name;
        ULONGLONG size = 0;
        int icon = -1;
        bool folder = false;
    };

private:
    static constexpr UINT kMsgResult = WM_APP + 30;
    static constexpr UINT_PTR kHandlerTimer = 1, kResizeTimer = 2;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc, const RECT& rc);
    void Start();
    void OnResult(Result* result);
    void ApplyPreview();
    void LoadHandler();
    void UnloadHandler();
    void LayoutChildren();
    RECT BodyRect() const;
    RECT ThumbRect() const;
    RECT DetailsRect() const;
    bool Horizontal() const; // under the file area and wide: preview beside the properties (Files' Horizontal state)
    int HitTab(POINT pt) const;
    void ClearThumb();

    InfoPaneEvents* m_events;
    const Theme* m_theme = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_text = nullptr;    // read-only edit: text/code preview
    HWND m_list = nullptr;    // list view: archive contents
    HWND m_host = nullptr;    // preview handler parent
    HWND m_rich = nullptr;    // read-only RichEdit: Markdown, code, .rtf
    MediaPreview* m_media = nullptr; // owned; deleted in Destroy (incomplete type here)
    std::wstring m_mediaPath; // what m_media has loaded
    std::string m_rtf;        // what m_rich shows (RTF bytes)
    bool m_rtfWrap = true;
    bool m_rtfFile = false;   // m_rtf came from a .rtf file (auto-colored text follows the theme)
    void SetRich();
    void OpenLink(LONG first, LONG last); // the clicked link's character range
    HFONT m_mono = nullptr;
    HFONT m_bold = nullptr;
    int m_contentHeight = 0;
    SIZE m_lastSize{};
    HBRUSH m_contentBrush = nullptr;
    bool m_darkApplied = false; // list / edit currently have the dark theme classes
    HIMAGELIST m_images = nullptr;

    Page m_page = Page::Details;
    UniquePidl m_pidl;
    unsigned m_generation = 0;
    int m_hotTab = -1; // 0 details, 1 preview, 2 close
    bool m_tracking = false;
    int m_scroll = 0;
    SmoothScroll m_smooth; // the Details page body's wheel animation

    // Current content (from the worker)
    Kind m_kind = Kind::None;
    std::wstring m_name, m_type, m_note;
    std::vector<std::pair<std::wstring, std::wstring>> m_props;
    std::vector<std::pair<std::wstring, std::wstring>> m_extra;
    std::vector<std::pair<std::wstring, COLORREF>> m_tags;
    bool m_tagsShown = false;
    RECT m_tagsRect{}; // client coordinates of the tags row (click target)
    bool m_tagsHot = false;
    HBITMAP m_thumb = nullptr;
    bool m_thumbAlpha = false;
    int m_icon = -1;
    std::vector<ArchiveEntry> m_archive;
    std::wstring m_archiveSummary;
    CLSID m_handlerClsid{};
    ComPtr<IPreviewHandler> m_handler;

    // Site handed to preview handlers; lives as long as the pane (not reference counted)
    struct Frame final : IPreviewHandlerFrame
    {
        HWND owner = nullptr;
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            static const QITAB qit[] = { QITABENT(Frame, IPreviewHandlerFrame), {} };
            return QISearch(this, qit, riid, ppv);
        }
        IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
        IFACEMETHODIMP_(ULONG) Release() override { return 1; }
        IFACEMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override
        {
            if (!info)
                return E_INVALIDARG;
            *info = {};
            return S_OK;
        }
        // Keys the handler doesn't use go back to the main window's accelerators
        IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
    } m_frame;
};
