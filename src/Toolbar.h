#pragma once
#include "Controls.h"
#include <array>
#include <functional>

// Files' customizable toolbar (Data/Items/ToolbarSections.cs, ToolbarItemDescriptor.cs): the left part of the command bar
// is a list of entries per context. "Always visible" is always shown; each other context appears (after a separator)
// while the selection or folder matches it; "other" only when no specific context matches.
namespace ToolbarModel
{
enum Context : int { Always, Archive, Batch, PowerShell, Image, Media, Font, Driver, Certificate, RecycleBin, Other, ContextCount };
const wchar_t* ContextKey(int context);  // settings key suffix: toolbar.<key>
const wchar_t* ContextName(int context); // Files' zh-Hans name

// What can be put on the toolbar: a separator, the two groups (dropdowns) and the commands
struct Command
{
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* glyph;    // null: the item can only show its label
    const wchar_t* category;
    int id;                  // MainWindow button / action id (0 for the separator)
    bool group = false;      // opens a menu (Files' CommandGroups)
};
const std::vector<Command>& Catalog(); // separator first, then groups, then commands (Files' order)
void SetCatalog(std::vector<Command> commands); // MainWindow fills it once (it owns the ids)
const Command* Find(const std::wstring& key);
inline bool IsSeparator(const std::wstring& key) { return key == L"separator"; }

// Files' ToolbarItemSettingsEntry
struct Entry
{
    std::wstring key;
    bool icon = true;
    bool label = false;
    bool operator==(const Entry& o) const { return key == o.key && icon == o.icon && label == o.label; }
};
using Lists = std::array<std::vector<Entry>, ContextCount>;
std::vector<Entry> Defaults(int context);
// settings.txt form: "key:il,key:i,separator" ("none" = deliberately empty; "" = the defaults)
std::vector<Entry> Parse(const std::wstring& text, int context);
std::wstring Format(const std::vector<Entry>& entries, int context);

// Bar items for one context's entries; ids are firstId + entry index (separators get 0). An entry with neither icon nor
// label is left out (Files doesn't render it). tip(command) gives the tooltip.
std::vector<ButtonBar::Item> BarItems(const std::vector<Entry>& entries, int firstId,
    const std::function<std::wstring(const Command&)>& tip);
}

// Files' ToolbarCustomizationPage, as a window of its own. Returns true when the user saved; lists then hold the result.
bool ShowToolbarDialog(HWND owner, const Theme* theme, ToolbarModel::Lists& lists, int context);
