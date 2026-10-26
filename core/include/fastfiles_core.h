#pragma once
// C ABI of the Rust core (core/src/lib.rs). Keep in sync when adding exports.
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FfBuffer
{
    uint8_t* data;
    size_t len;
} FfBuffer;

const char* ff_core_version(void);
FfBuffer ff_store_read(const uint16_t* path, size_t len);
bool ff_store_write(const uint16_t* path, size_t len, const uint8_t* data, size_t data_len);
void ff_buffer_free(FfBuffer buffer);

// Sidebar pins (core/src/pins.rs). Text is UTF-8, one parsing name per line, in display order.
FfBuffer ff_pins_load(const uint16_t* file, size_t len, bool* exists);
bool ff_pins_add(const uint16_t* file, size_t len, const uint16_t* item, size_t item_len, int64_t index);
bool ff_pins_remove(const uint16_t* file, size_t len, const uint16_t* item, size_t item_len);
bool ff_pins_move(const uint16_t* file, size_t len, size_t from, size_t to);

// Preview pane (core/src/preview.rs)
// kind: 0 binary, 1 UTF-8, 2 UTF-16LE, 3 UTF-16BE, 4 ANSI (decode with CP_ACP). Payload has the BOM stripped.
FfBuffer ff_text_preview(const uint16_t* path, size_t len, size_t limit, uint8_t* kind, bool* truncated);
// Records: u8 utf8 flag, u64 LE uncompressed size, u16 LE name length, name bytes
FfBuffer ff_zip_list(const uint16_t* path, size_t len, size_t max, uint64_t* total);
// Files' CodePreview language for an extension (0 = not code); RTF for the details pane: kind 1 Markdown, 2 code
uint8_t ff_code_language(const uint16_t* ext, size_t len);
FfBuffer ff_rich_preview(const uint16_t* text, size_t len, uint8_t kind, uint8_t language, bool dark, uint32_t ink, uint32_t muted,
    uint32_t accent, const uint16_t* font, size_t font_len);

// Settings (core/src/settings.rs): UTF-8 key=value lines, every known key present, invalid lines dropped.
FfBuffer ff_settings_load(const uint16_t* file, size_t len);
bool ff_settings_save(const uint16_t* file, size_t len, const uint8_t* text, size_t text_len);
// Count of valid settings imported (0: nothing recognizable, nothing written), -1 on I/O failure
int64_t ff_settings_import(const uint16_t* from, size_t from_len, const uint16_t* to, size_t to_len);

// Shortcuts (core/src/shortcut.rs): code = modifiers << 16 | virtual key; modifiers 1 Ctrl, 2 Shift, 4 Alt
uint32_t ff_shortcut_parse(const uint16_t* text, size_t len);
FfBuffer ff_shortcut_format(uint32_t code);
bool ff_shortcut_bindable(uint32_t code);

// Folder size (core/src/dirsize.rs). Blocking: run it on a worker. The core updates *totals atomically while it walks
// (read the fields with std::atomic_ref), and stops soon after *cancel becomes true. Returns false when cancelled.
typedef struct FfDirTotals
{
    uint64_t bytes;
    uint64_t files;
    uint64_t folders;
    uint64_t skipped; // folders that couldn't be listed
} FfDirTotals;
bool ff_dir_size(const uint16_t* path, size_t len, uint32_t threads, FfDirTotals* totals, const bool* cancel);

// Git (core/src/git.rs): repository containing path. UTF-8 "root\nhead"; empty when not in a repository.
FfBuffer ff_git_repo(const uint16_t* path, size_t len, bool* detached);
// Local branch names, UTF-8, one per line
FfBuffer ff_git_branches(const uint16_t* path, size_t len);

// File name search (core/src/search.rs). Words must all appear in the name; * and ? make it a whole-name wildcard.
// Blocking: run on a worker. sink is called from Rust worker threads with each matching path (UTF-16, no NUL).
// flags bit 0: include hidden/system items. Returns 1 finished (or limit reached), 0 cancelled, -1 empty query.
typedef struct FfSearchProgress
{
    uint64_t matches;
    uint64_t files;
    uint64_t folders;
    uint64_t skipped;
} FfSearchProgress;
typedef void (*FfSearchSink)(void* context, const uint16_t* path, size_t len);
int32_t ff_search(const uint16_t* root, size_t root_len, const uint16_t* query, size_t query_len, uint32_t flags, uint64_t limit,
    uint32_t threads, FfSearchProgress* progress, const bool* cancel, FfSearchSink sink, void* context);

// Compress / extract removed at the user's request (core/src/zip.rs, core/src/deflate.rs): FastFiles has no
// built-in archive support, the shell's own menu items (WinRAR, 7-Zip...) cover it. ff_zip_create / ff_zip_extract /
// ff_zip_info / ff_zip_folder_for and FfZipProgress are gone; ff_unique_path is generic (copy/move/new-item naming
// too) and stays.
// UTF-16 result (len in bytes): a free name for path ("x (2).zip")
FfBuffer ff_unique_path(const uint16_t* path, size_t len);

// File tags (core/src/tags.rs). file = tags.txt. Ids and names are UTF-8.
// Definitions: "id\tname\t#RRGGBB" lines in display order (Files' five defaults on first use).
FfBuffer ff_tags_list(const uint16_t* file, size_t len);
FfBuffer ff_tags_add(const uint16_t* file, size_t len, const uint16_t* name, size_t name_len, const uint16_t* color, size_t color_len);
// Zero name_len / color_len leaves that part unchanged
bool ff_tags_edit(const uint16_t* file, size_t len, const uint16_t* id, size_t id_len, const uint16_t* name, size_t name_len,
    const uint16_t* color, size_t color_len);
bool ff_tags_remove(const uint16_t* file, size_t len, const uint16_t* id, size_t id_len);
bool ff_tags_move(const uint16_t* file, size_t len, size_t from, size_t to);
// Comma-separated tag ids of one path (follows renames/moves by NTFS file id)
FfBuffer ff_tags_of(const uint16_t* file, size_t len, const uint16_t* path, size_t path_len);
// paths: UTF-16, NUL separated
bool ff_tags_set(const uint16_t* file, size_t len, const uint16_t* paths, size_t paths_len, const uint16_t* id, size_t id_len, bool on);
// Paths (one per line) tagged id (any tag when id_len is 0) inside root (anywhere when root_len is 0). Worker thread.
FfBuffer ff_tags_items(const uint16_t* file, size_t len, const uint16_t* id, size_t id_len, const uint16_t* root, size_t root_len);

// --- Per-folder layout choices (layouts.txt) and Files' adaptive layout guess ---
// Mode: 0 none, 1 column view, 2 adaptive on, 3 picked by hand
uint32_t ff_layout_get(const uint16_t* file, size_t len, const uint16_t* path, size_t path_len);
bool ff_layout_set(const uint16_t* file, size_t len, const uint16_t* path, size_t path_len, uint32_t mode); // mode 0 forgets it
// 0 undecided, 1 details, 2 grid: desktop.ini [ViewState] Mode, else >60% pictures/music/video in the first 200 entries
uint32_t ff_layout_predict(const uint16_t* path, size_t len);
uint32_t ff_layout_classify(const uint16_t* names, size_t len); // names: UTF-16, NUL separated

// Per-folder sort/group choice (Files' LayoutPreferencesItem), stored alongside the layout mode above but
// independently: setting one keeps the other, and the folder's line is dropped only once both are empty.
// sort key (bits 0-3) 0 name, 1 modified, 2 type, 3 size, 4 tag, 5 created; group key (bits 8-11) 0 none, 1 name,
// 2 modified, 3 created, 4 size, 5 type, 6 tag; date unit (bits 12-13, group by modified/created only) 0 year,
// 1 month, 2 day; priority (bits 16-17) 0-2. Returns/takes 0 when there's no sort record; otherwise bit 31 is set,
// bit 4 is sort-descending, bit 14 is group-descending.
uint32_t ff_layout_get_sort(const uint16_t* file, size_t len, const uint16_t* path, size_t path_len);
bool ff_layout_set_sort(const uint16_t* file, size_t len, const uint16_t* path, size_t path_len, uint32_t code); // code 0 forgets it

#ifdef __cplusplus
}
#endif
