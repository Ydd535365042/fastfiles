#pragma once
#include "Theme.h"
#include "Lang.h"
#include <functional>

// Files' ContentDialog (DefaultContentDialogStyle) drawn natively: the owner dims behind it, a card with the title, a
// line of text, then either a list to pick from (AddItemDialog) or labelled text boxes (create item, new shortcut),
// a red line under an invalid box (Files' TeachingTip), and the primary / close buttons. Modal; returns when closed.
namespace ContentDialog
{
struct Item // a list row: icon (shell icon, or a glyph) + header + gray subheader
{
    std::wstring header, subheader;
    HICON icon = nullptr; // not owned
    const wchar_t* glyph = nullptr;
};

struct Field // a labelled text box, optionally with a 浏览 button; or a drop-down (ComboBox) when it has choices
{
    std::wstring label, text, placeholder;
    bool browse = false;
    bool password = false; // PasswordBox
    std::vector<std::wstring> choices; // ComboBox items; `choice` is the selected one
    int choice = 0;
    std::wstring error; // shown under the box while not empty
};

struct Spec
{
    std::wstring title, text;
    std::vector<Item> items;   // list dialog: a click picks a row and closes it
    std::vector<Field> fields; // input dialog
    std::wstring primary, close = Tr(L"取消"); // primary empty: only the close button
    bool primaryEnabled = true;
    // Files' DefaultButton: which button Enter presses. Primary by default; set true for a dangerous confirmation
    // where Files defaults to Close (MessageBox's MB_DEFBUTTON2) so Enter can't accidentally confirm it.
    bool defaultClose = false;
    std::wstring check; // a CheckBox under the fields when not empty
    bool checked = false;
    bool checkEnabled = true; // false: shown grayed, can't be toggled
    int selected = -1; // result: the row picked
    // Input dialogs: called after every edit (and once at the start); update errors / other fields / primaryEnabled
    std::function<void(Spec&, int changedField)> changed;
    std::function<void(Spec&, int field)> browse; // 浏览 pressed: fill fields[field].text
    // Lists that grow while open (ShellNew types still being scanned): returns true when items were replaced
    std::function<bool(Spec&)> poll;
};

enum Result { Closed = 0, Primary = 1, Picked = 2 };
Result Show(HWND owner, const Theme* theme, Spec& spec);

// Short helpers for the plain MessageBoxW-style dialogs (confirm / notice) that used to be system message boxes.
// primary/close default to Files' 是/取消; pass "是"/"否" or other labels to match the Files dialog being replaced.
// defaultClose matches MessageBoxW's MB_DEFBUTTON2: Enter presses close instead of primary, for dialogs where the
// original system dialog defaulted away from the dangerous action.
bool Confirm(HWND owner, const Theme* theme, const std::wstring& title, const std::wstring& text,
    const std::wstring& primary = Tr(L"是"), const std::wstring& close = Tr(L"取消"), bool defaultClose = false);
// A single OK button (MB_OK): title + text, nothing to confirm. close defaults to Files' 确定; some Files dialogs
// this replaces use 关闭 instead (e.g. GitHelpers' DynamicDialog), so it can be passed explicitly.
void Notice(HWND owner, const Theme* theme, const std::wstring& title, const std::wstring& text, const std::wstring& close = Tr(L"确定"));
}
