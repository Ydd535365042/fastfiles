//! FastFiles core: everything that doesn't draw UI. Exposed to C++ through a small C ABI (see include/fastfiles_core.h).

mod deflate;
mod dirsize;
mod git;
mod layouts;
mod pins;
mod search;
mod preview;
mod richtext;
mod settings;
mod shortcut;
mod store;
mod tags;
mod walk;
mod zip;

use std::ffi::c_char;
use std::path::PathBuf;
use std::slice;

/// Bytes handed to C++. Free with `ff_buffer_free`.
#[repr(C)]
pub struct FfBuffer {
    pub data: *mut u8,
    pub len: usize,
}

impl FfBuffer {
    fn empty() -> Self {
        FfBuffer { data: std::ptr::null_mut(), len: 0 }
    }

    fn from_vec(bytes: Vec<u8>) -> Self {
        let mut boxed = bytes.into_boxed_slice();
        let out = FfBuffer { data: boxed.as_mut_ptr(), len: boxed.len() };
        std::mem::forget(boxed);
        out
    }
}

/// # Safety
/// `path` must point to `len` valid UTF-16 code units.
unsafe fn path_from_utf16(path: *const u16, len: usize) -> Option<PathBuf> {
    if path.is_null() || len == 0 {
        return None;
    }
    let units = slice::from_raw_parts(path, len);
    String::from_utf16(units).ok().map(PathBuf::from)
}

/// Version string of the core, NUL-terminated, static lifetime.
#[no_mangle]
pub extern "C" fn ff_core_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr().cast()
}

/// Reads a whole file. Returns an empty buffer if it doesn't exist or can't be read.
///
/// # Safety
/// `path` must point to `len` valid UTF-16 code units.
#[no_mangle]
pub unsafe extern "C" fn ff_store_read(path: *const u16, len: usize) -> FfBuffer {
    match path_from_utf16(path, len).and_then(|p| store::read(&p).ok()) {
        Some(bytes) => FfBuffer::from_vec(bytes),
        None => FfBuffer::empty(),
    }
}

/// Writes a file atomically (temp file + rename), creating parent folders. Returns true on success.
///
/// # Safety
/// `path` must point to `len` UTF-16 units; `data` must point to `data_len` bytes (may be null when 0).
#[no_mangle]
pub unsafe extern "C" fn ff_store_write(path: *const u16, len: usize, data: *const u8, data_len: usize) -> bool {
    let Some(p) = path_from_utf16(path, len) else { return false };
    let bytes = if data_len == 0 { &[][..] } else if data.is_null() { return false } else { slice::from_raw_parts(data, data_len) };
    store::write_atomic(&p, bytes).is_ok()
}

/// # Safety
/// `text` must point to `len` valid UTF-16 code units.
unsafe fn string_from_utf16(text: *const u16, len: usize) -> Option<String> {
    if text.is_null() || len == 0 {
        return None;
    }
    String::from_utf16(slice::from_raw_parts(text, len)).ok()
}

/// Pinned sidebar folders as UTF-8, one per line, normalized and de-duplicated.
/// `*exists` is false when the file has never been written (the caller seeds defaults).
///
/// # Safety
/// `file` must point to `len` UTF-16 units; `exists` may be null.
#[no_mangle]
pub unsafe extern "C" fn ff_pins_load(file: *const u16, len: usize, exists: *mut bool) -> FfBuffer {
    let loaded = path_from_utf16(file, len).and_then(|p| pins::load(&p));
    if !exists.is_null() {
        *exists = loaded.is_some();
    }
    match loaded {
        Some(list) if !list.is_empty() => FfBuffer::from_vec(pins::serialize(&list).into_bytes()),
        _ => FfBuffer::empty(),
    }
}

/// Pins `item` at `index` (negative = end); an existing pin is moved there. Returns true if the file changed.
///
/// # Safety
/// `file`/`item` must point to `len`/`item_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_pins_add(file: *const u16, len: usize, item: *const u16, item_len: usize, index: i64) -> bool {
    let (Some(p), Some(item)) = (path_from_utf16(file, len), string_from_utf16(item, item_len)) else { return false };
    let at = usize::try_from(index).ok();
    pins::update(&p, |list| pins::add(list, &item, at)).unwrap_or(false)
}

/// # Safety
/// `file`/`item` must point to `len`/`item_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_pins_remove(file: *const u16, len: usize, item: *const u16, item_len: usize) -> bool {
    let (Some(p), Some(item)) = (path_from_utf16(file, len), string_from_utf16(item, item_len)) else { return false };
    pins::update(&p, |list| pins::remove(list, &item)).unwrap_or(false)
}

/// # Safety
/// `file` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_pins_move(file: *const u16, len: usize, from: usize, to: usize) -> bool {
    let Some(p) = path_from_utf16(file, len) else { return false };
    pins::update(&p, |list| pins::move_to(list, from, to)).unwrap_or(false)
}

/// First `limit` bytes of a file for the text preview, BOM stripped. `*kind` is a preview::TextKind
/// (0 binary, 1 UTF-8, 2 UTF-16LE, 3 UTF-16BE, 4 ANSI); `*truncated` says whether the file is longer.
///
/// # Safety
/// `path` must point to `len` UTF-16 units; `kind`/`truncated` may be null.
#[no_mangle]
pub unsafe extern "C" fn ff_text_preview(path: *const u16, len: usize, limit: usize, kind: *mut u8, truncated: *mut bool) -> FfBuffer {
    let read = path_from_utf16(path, len).and_then(|p| preview::read_head(&p, limit).ok());
    let Some((bytes, cut)) = read else {
        if !kind.is_null() {
            *kind = preview::TextKind::Binary as u8;
        }
        return FfBuffer::empty();
    };
    let (k, body) = preview::sniff(&bytes, cut);
    if !kind.is_null() {
        *kind = k as u8;
    }
    if !truncated.is_null() {
        *truncated = cut;
    }
    if k == preview::TextKind::Binary {
        return FfBuffer::empty();
    }
    FfBuffer::from_vec(body.to_vec())
}

/// Zip central directory, at most `max` entries. Records: u8 utf8 flag, u64 LE size, u16 LE name length, name bytes.
/// `*total` receives the archive's entry count; returns an empty buffer (and total 0) when it isn't a readable zip.
///
/// # Safety
/// `path` must point to `len` UTF-16 units; `total` may be null.
#[no_mangle]
pub unsafe extern "C" fn ff_zip_list(path: *const u16, len: usize, max: usize, total: *mut u64) -> FfBuffer {
    let listed = path_from_utf16(path, len)
        .and_then(|p| std::fs::File::open(p).ok())
        .and_then(|f| preview::list_zip(&mut std::io::BufReader::new(f), max).ok());
    let Some((entries, count)) = listed else {
        if !total.is_null() {
            *total = 0;
        }
        return FfBuffer::empty();
    };
    if !total.is_null() {
        *total = count;
    }
    let mut out = Vec::new();
    for e in entries {
        let name = &e.name[..e.name.len().min(u16::MAX as usize)];
        out.push(u8::from(e.utf8));
        out.extend_from_slice(&e.size.to_le_bytes());
        out.extend_from_slice(&(name.len() as u16).to_le_bytes());
        out.extend_from_slice(name);
    }
    FfBuffer::from_vec(out)
}

/// Settings file as normalized UTF-8 `key=value` lines: every known key present (defaults filled in), invalid lines dropped.
/// Never empty: a missing file gives the defaults.
///
/// # Safety
/// `file` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_settings_load(file: *const u16, len: usize) -> FfBuffer {
    let s = match path_from_utf16(file, len) {
        Some(p) => settings::load(&p),
        None => settings::parse(""),
    };
    FfBuffer::from_vec(settings::serialize(&s).into_bytes())
}

/// Normalizes `text` (UTF-8 `key=value` lines) and writes it atomically.
///
/// # Safety
/// `file` must point to `len` UTF-16 units; `text` to `text_len` bytes (may be null when 0).
#[no_mangle]
pub unsafe extern "C" fn ff_settings_save(file: *const u16, len: usize, text: *const u8, text_len: usize) -> bool {
    let Some(p) = path_from_utf16(file, len) else { return false };
    let bytes = if text_len == 0 { &[][..] } else if text.is_null() { return false } else { slice::from_raw_parts(text, text_len) };
    settings::save(&p, &String::from_utf8_lossy(bytes)).is_ok()
}

/// Replaces the settings at `to` with those read from `from`. Returns how many valid settings were found
/// (0: nothing recognizable, `to` untouched), or -1 when `from` can't be read or `to` can't be written.
///
/// # Safety
/// `from`/`to` must point to `from_len`/`to_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_settings_import(from: *const u16, from_len: usize, to: *const u16, to_len: usize) -> i64 {
    let (Some(f), Some(t)) = (path_from_utf16(from, from_len), path_from_utf16(to, to_len)) else { return -1 };
    settings::import(&f, &t).map(|n| n as i64).unwrap_or(-1)
}

/// "Ctrl+Shift+S" -> modifiers << 16 | virtual key (modifiers: 1 Ctrl, 2 Shift, 4 Alt); 0 if invalid.
///
/// # Safety
/// `text` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_shortcut_parse(text: *const u16, len: usize) -> u32 {
    string_from_utf16(text, len).map(|s| shortcut::parse(&s)).unwrap_or(0)
}

/// Code -> "Ctrl+Shift+S" as UTF-8; empty when the key has no name.
#[no_mangle]
pub extern "C" fn ff_shortcut_format(code: u32) -> FfBuffer {
    let text = shortcut::format(code);
    if text.is_empty() {
        FfBuffer::empty()
    } else {
        FfBuffer::from_vec(text.into_bytes())
    }
}

/// Whether users may bind this code (needs Ctrl or Alt, unless it's a function key).
#[no_mangle]
pub extern "C" fn ff_shortcut_bindable(code: u32) -> bool {
    shortcut::bindable(code)
}

/// Sums the folder at `path` into `*totals` (updated atomically while running). Returns false when `*cancel` stopped it.
///
/// # Safety
/// `path` must point to `len` UTF-16 units; `totals` must point to a zeroed, 8-byte aligned FfDirTotals and `cancel`
/// to a bool, both valid (and only accessed atomically by others) until this returns.
#[no_mangle]
pub unsafe extern "C" fn ff_dir_size(path: *const u16, len: usize, threads: u32, totals: *mut dirsize::Totals, cancel: *const bool) -> bool {
    let Some(p) = path_from_utf16(path, len) else { return true };
    if totals.is_null() || cancel.is_null() {
        return true;
    }
    // AtomicBool has the same layout as bool
    let cancel = &*cancel.cast::<std::sync::atomic::AtomicBool>();
    dirsize::measure(&p, threads as usize, &*totals, cancel)
}

/// Receives one search match: UTF-16 path (not NUL-terminated). Called from worker threads.
pub type FfSearchSink = unsafe extern "C" fn(context: *mut std::ffi::c_void, path: *const u16, len: usize);

/// File name search below `root` (see core/src/search.rs for the query syntax). Blocking; matches go to `sink` as
/// they're found, `*progress` is updated atomically. flags bit 0: include hidden/system items.
/// Returns 1 finished, 0 cancelled, -1 empty query.
///
/// # Safety
/// `root`/`query` must point to `root_len`/`query_len` UTF-16 units; `progress` to a zeroed FfSearchProgress and
/// `cancel` to a bool, both valid until this returns; `sink` must be callable from any thread with `context`.
#[no_mangle]
pub unsafe extern "C" fn ff_search(root: *const u16, root_len: usize, query: *const u16, query_len: usize, flags: u32, limit: u64,
    threads: u32, progress: *mut search::Progress, cancel: *const bool, sink: FfSearchSink, context: *mut std::ffi::c_void) -> i32 {
    let (Some(r), Some(q)) = (path_from_utf16(root, root_len), string_from_utf16(query, query_len)) else { return -1 };
    let Some(compiled) = search::compile(&q) else { return -1 };
    if progress.is_null() || cancel.is_null() {
        return -1;
    }
    let cancel = &*cancel.cast::<std::sync::atomic::AtomicBool>();
    let options = search::Options { include_hidden: flags & 1 != 0, limit: limit.max(1) };
    // Raw pointers aren't Sync; the caller guarantees the context may be used from any thread
    let ctx = context as usize;
    let done = search::search(&r, &compiled, &options, threads as usize, &*progress, cancel, |p| {
        use std::os::windows::ffi::OsStrExt;
        let wide: Vec<u16> = p.as_os_str().encode_wide().collect();
        sink(ctx as *mut std::ffi::c_void, wide.as_ptr(), wide.len());
    });
    i32::from(done)
}

/// Free name for a new file or folder: `path` itself, or "name (2).ext"... UTF-16 out (free the buffer).
///
/// # Safety
/// `path` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_unique_path(path: *const u16, len: usize) -> FfBuffer {
    use std::os::windows::ffi::OsStrExt;
    let Some(p) = path_from_utf16(path, len) else { return FfBuffer::empty() };
    let wide: Vec<u8> = zip::unique(&p).as_os_str().encode_wide().flat_map(u16::to_le_bytes).collect();
    FfBuffer::from_vec(wide)
}

/// Git repository containing `path`: UTF-8 "root\nhead", empty when there is none.
///
/// # Safety
/// `path` must point to `len` UTF-16 units; `detached` may be null.
#[no_mangle]
pub unsafe extern "C" fn ff_git_repo(path: *const u16, len: usize, detached: *mut bool) -> FfBuffer {
    let repo = path_from_utf16(path, len).and_then(|p| git::find(&p));
    if !detached.is_null() {
        *detached = repo.as_ref().is_some_and(|r| r.detached);
    }
    match repo {
        Some(r) => FfBuffer::from_vec(format!("{}\n{}", r.root.display(), r.head).into_bytes()),
        None => FfBuffer::empty(),
    }
}

/// Local branches of the repository containing `path`, UTF-8, one per line.
///
/// # Safety
/// `path` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_git_branches(path: *const u16, len: usize) -> FfBuffer {
    match path_from_utf16(path, len).and_then(|p| git::find(&p)) {
        Some(r) => FfBuffer::from_vec(git::branches(&r).join("\n").into_bytes()),
        None => FfBuffer::empty(),
    }
}

/// Tag definitions as UTF-8 "id\tname\t#RRGGBB" lines, in display order (Files' defaults until the user changes them).
///
/// # Safety
/// `file` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_tags_list(file: *const u16, len: usize) -> FfBuffer {
    let Some(p) = path_from_utf16(file, len) else { return FfBuffer::empty() };
    let text: String = tags::read(&p, |db| db.tags.iter().map(|t| format!("{}\t{}\t{}\n", t.id, t.name, t.color)).collect());
    FfBuffer::from_vec(text.into_bytes())
}

/// New tag at the end of the list; returns its id (UTF-8), empty when the name or color is invalid.
///
/// # Safety
/// Strings must point to the given number of UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_tags_add(file: *const u16, len: usize, name: *const u16, name_len: usize, color: *const u16, color_len: usize) -> FfBuffer {
    let (Some(p), Some(n), Some(c)) = (path_from_utf16(file, len), string_from_utf16(name, name_len), string_from_utf16(color, color_len)) else {
        return FfBuffer::empty();
    };
    match tags::update(&p, |db| {
        let id = tags::add_tag(db, &n, &c);
        (id.is_some(), id)
    }) {
        Ok(Some(id)) => FfBuffer::from_vec(id.into_bytes()),
        _ => FfBuffer::empty(),
    }
}

/// Renames and/or recolors a tag (a zero length leaves that part as it is). True when it changed.
///
/// # Safety
/// Strings must point to the given number of UTF-16 units (may be null when the length is 0).
#[no_mangle]
pub unsafe extern "C" fn ff_tags_edit(file: *const u16, len: usize, id: *const u16, id_len: usize, name: *const u16, name_len: usize,
    color: *const u16, color_len: usize) -> bool {
    let (Some(p), Some(id)) = (path_from_utf16(file, len), string_from_utf16(id, id_len)) else { return false };
    let (n, c) = (string_from_utf16(name, name_len), string_from_utf16(color, color_len));
    tags::update(&p, |db| {
        let changed = tags::edit_tag(db, &id, n.as_deref(), c.as_deref());
        (changed, changed)
    })
    .unwrap_or(false)
}

/// Deletes a tag and takes it off every file.
///
/// # Safety
/// Strings must point to the given number of UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_tags_remove(file: *const u16, len: usize, id: *const u16, id_len: usize) -> bool {
    let (Some(p), Some(id)) = (path_from_utf16(file, len), string_from_utf16(id, id_len)) else { return false };
    tags::update(&p, |db| {
        let changed = tags::remove_tag(db, &id);
        (changed, changed)
    })
    .unwrap_or(false)
}

/// # Safety
/// `file` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_tags_move(file: *const u16, len: usize, from: usize, to: usize) -> bool {
    let Some(p) = path_from_utf16(file, len) else { return false };
    tags::update(&p, |db| {
        let changed = tags::move_tag(db, from, to);
        (changed, changed)
    })
    .unwrap_or(false)
}

/// Tag ids of one file or folder, UTF-8, comma separated (follows a rename or move made since it was tagged).
///
/// # Safety
/// Strings must point to the given number of UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_tags_of(file: *const u16, len: usize, path: *const u16, path_len: usize) -> FfBuffer {
    let (Some(p), Some(item)) = (path_from_utf16(file, len), string_from_utf16(path, path_len)) else { return FfBuffer::empty() };
    if !p.exists() {
        return FfBuffer::empty(); // nothing tagged yet; don't create the file just to look
    }
    let ids = tags::update(&p, |db| {
        let (ids, moved) = tags::tags_of(db, &item, &tags::Disk);
        (moved, ids)
    })
    .unwrap_or_default();
    FfBuffer::from_vec(ids.join(",").into_bytes())
}

/// Adds (`on`) or removes tag `id` on every path in `paths` (UTF-16, NUL separated). True when anything changed.
///
/// # Safety
/// Strings must point to the given number of UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_tags_set(file: *const u16, len: usize, paths: *const u16, paths_len: usize, id: *const u16, id_len: usize, on: bool) -> bool {
    let (Some(p), Some(list), Some(id)) = (path_from_utf16(file, len), string_from_utf16(paths, paths_len), string_from_utf16(id, id_len)) else {
        return false;
    };
    let items: Vec<String> = list.split('\0').filter(|s| !s.is_empty()).map(str::to_string).collect();
    tags::update(&p, |db| {
        let changed = tags::set(db, &items, &id, on, &tags::Disk);
        (changed, changed)
    })
    .unwrap_or(false)
}

/// Files tagged `id` (any tag when `id_len` is 0), inside `root` when `root_len` > 0; UTF-8, one path per line.
/// Stored paths are brought up to date first (renamed/moved files followed, deleted ones forgotten): may touch every
/// tagged file, so call it from a worker.
///
/// # Safety
/// Strings must point to the given number of UTF-16 units (may be null when the length is 0).
#[no_mangle]
pub unsafe extern "C" fn ff_tags_items(file: *const u16, len: usize, id: *const u16, id_len: usize, root: *const u16, root_len: usize) -> FfBuffer {
    let Some(p) = path_from_utf16(file, len) else { return FfBuffer::empty() };
    if !p.exists() {
        return FfBuffer::empty();
    }
    let (id, root) = (string_from_utf16(id, id_len), string_from_utf16(root, root_len));
    let found = tags::update(&p, |db| {
        let changed = tags::reconcile(db, &tags::Disk);
        (changed, tags::items_with(db, id.as_deref(), root.as_deref()))
    })
    .unwrap_or_default();
    FfBuffer::from_vec(found.join("\n").into_bytes())
}

/// The folder's stored layout choice in layouts.txt: 0 none, 1 column view, 2 adaptive on, 3 picked by hand
/// (bits 8-15: the shell layout + 1 when known).
///
/// # Safety
/// `file`/`path` must point to `len`/`path_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_layout_get(file: *const u16, len: usize, path: *const u16, path_len: usize) -> u32 {
    let (Some(f), Some(p)) = (path_from_utf16(file, len), string_from_utf16(path, path_len)) else { return 0 };
    layouts::get(&layouts::load(&f), &p).map_or(0, layouts::Mode::code)
}

/// Stores the folder's layout choice (codes as ff_layout_get; 0 forgets it). True when the file changed.
///
/// # Safety
/// `file`/`path` must point to `len`/`path_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_layout_set(file: *const u16, len: usize, path: *const u16, path_len: usize, mode: u32) -> bool {
    let (Some(f), Some(p)) = (path_from_utf16(file, len), string_from_utf16(path, path_len)) else { return false };
    layouts::update(&f, &p, layouts::Mode::from_code(mode)).unwrap_or(false)
}

/// Files' adaptive layout guess for a local folder: 0 undecided, 1 details, 2 grid (desktop.ini, else 200 entries).
///
/// # Safety
/// `path` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_layout_predict(path: *const u16, len: usize) -> u32 {
    path_from_utf16(path, len).map_or(0, |p| layouts::predict(&p) as u32)
}

/// The same guess from names already listed (UTF-16, NUL separated): used for network folders after they load.
///
/// # Safety
/// `names` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_layout_classify(names: *const u16, len: usize) -> u32 {
    let Some(list) = string_from_utf16(names, len) else { return 0 };
    layouts::classify(list.split('\0').filter(|s| !s.is_empty())) as u32
}

/// The folder's stored sort/group choice in layouts.txt: 0 when there's none, else bit 31 set, bits 0-3 sort key,
/// bit 4 sort descending, bits 8-11 group key, bits 12-13 date unit, bit 14 group descending, bits 16-17 priority
/// (see layouts::Sort::code).
///
/// # Safety
/// `file`/`path` must point to `len`/`path_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_layout_get_sort(file: *const u16, len: usize, path: *const u16, path_len: usize) -> u32 {
    let (Some(f), Some(p)) = (path_from_utf16(file, len), string_from_utf16(path, path_len)) else { return 0 };
    layouts::get_sort(&layouts::load(&f), &p).code()
}

/// Stores the folder's sort/group choice (code as ff_layout_get_sort; 0 forgets it). The folder's layout `Mode`,
/// if any, is kept (and ff_layout_set keeps this in turn); the entry is dropped only when both are empty.
/// True when the file changed.
///
/// # Safety
/// `file`/`path` must point to `len`/`path_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_layout_set_sort(file: *const u16, len: usize, path: *const u16, path_len: usize, code: u32) -> bool {
    let (Some(f), Some(p)) = (path_from_utf16(file, len), string_from_utf16(path, path_len)) else { return false };
    layouts::update_sort(&f, &p, layouts::Sort::from_code(code)).unwrap_or(false)
}

/// # Safety
/// `buffer` must come from this library and not have been freed already.
#[no_mangle]
pub unsafe extern "C" fn ff_buffer_free(buffer: FfBuffer) {
    if !buffer.data.is_null() {
        drop(Box::from_raw(slice::from_raw_parts_mut(buffer.data, buffer.len)));
    }
}

/// Files' CodePreview language for an extension (UTF-16, with or without the dot); 0 = not a code file
///
/// # Safety
/// `ext` must point to `len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_code_language(ext: *const u16, len: usize) -> u8 {
    string_from_utf16(ext, len).map_or(0, |e| richtext::language_for(&e))
}

/// RTF for the details pane: kind 1 = Markdown, 2 = code in `language` (ff_code_language). Colors are COLORREFs;
/// `font` is the monospace font (UTF-16). Returns ASCII RTF (free the buffer).
///
/// # Safety
/// `text`/`font` must point to `len`/`font_len` UTF-16 units.
#[no_mangle]
pub unsafe extern "C" fn ff_rich_preview(text: *const u16, len: usize, kind: u8, language: u8, dark: bool, ink: u32, muted: u32,
    accent: u32, font: *const u16, font_len: usize) -> FfBuffer {
    // Lossy: a cut at the character limit may split a surrogate pair
    let body = if len == 0 || text.is_null() { String::new() } else { String::from_utf16_lossy(slice::from_raw_parts(text, len)) };
    let font = string_from_utf16(font, font_len).filter(|f| !f.is_empty() && f.chars().all(|c| c.is_ascii_alphanumeric() || c == ' '))
        .unwrap_or_else(|| "Consolas".into());
    let p = richtext::Palette { ink, muted, accent, dark };
    let rtf = if kind == 1 { richtext::markdown_rtf(&body, &p, &font) } else { richtext::code_rtf(&body, language, &p, &font) };
    FfBuffer::from_vec(rtf.into_bytes())
}