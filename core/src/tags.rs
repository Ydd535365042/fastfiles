//! File tags (Files' "File tags": named, colored labels on files and folders). One UTF-8 text database:
//!
//! ```text
//! #fastfiles-tags 1
//! T<TAB>id<TAB>name<TAB>#RRGGBB          tag definitions, in display order
//! I<TAB>path<TAB>volume<TAB>fileid<TAB>id,id    tagged items
//! ```
//!
//! Items remember the NTFS volume serial + file id next to the path (Files keeps the file reference number for the
//! same reason), so a file renamed or moved on the same volume, even by another program, keeps its tags: when the
//! stored path no longer exists the id is opened again and the path updated. Items whose file is gone for sure are
//! dropped; ones on a volume that isn't mounted right now are kept.

use crate::store;
use std::io;
use std::path::Path;
use std::sync::Mutex;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Tag {
    pub id: String,
    pub name: String,
    pub color: String, // #RRGGBB
}

/// Volume serial + 64-bit file id; 0/0 when unknown (network drives, FAT without stable ids)
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Identity {
    pub volume: u32,
    pub file: u64,
}

impl Identity {
    fn known(&self) -> bool {
        self.file != 0
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    pub path: String,
    pub id: Identity,
    pub tags: Vec<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Db {
    pub tags: Vec<Tag>,
    pub items: Vec<Item>,
}

/// Files' default tags (FileTagsSettingsService: Home, Work, Photos, Important), seeded on first use
pub fn defaults() -> Db {
    let tags = [("家庭", "#0072BD"), ("工作", "#D95319"), ("照片", "#EDB120"), ("重要", "#77AC30")]
        .iter()
        .enumerate()
        .map(|(i, (name, color))| Tag { id: format!("t{}", i + 1), name: name.to_string(), color: color.to_string() })
        .collect();
    Db { tags, items: Vec::new() }
}

const HEADER: &str = "#fastfiles-tags 1";

/// Printable, single line, trimmed, at most 64 characters; None when nothing is left
pub fn clean_name(name: &str) -> Option<String> {
    let s: String = name.chars().filter(|c| !c.is_control()).collect::<String>().trim().chars().take(64).collect();
    let s = s.trim().to_string();
    (!s.is_empty()).then_some(s)
}

/// "#RRGGBB" (also accepts "RRGGBB" and "#RGB"), upper case
pub fn clean_color(color: &str) -> Option<String> {
    let hex = color.trim().trim_start_matches('#');
    if !hex.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let full = match hex.len() {
        6 => hex.to_string(),
        3 => hex.chars().flat_map(|c| [c, c]).collect(),
        _ => return None,
    };
    Some(format!("#{}", full.to_ascii_uppercase()))
}

/// Trailing backslash trimmed except on drive roots; no "\\?\" prefix
pub fn normalize(path: &str) -> Option<String> {
    let mut s = path.trim().to_string();
    if let Some(rest) = s.strip_prefix(r"\\?\UNC\") {
        s = format!(r"\\{rest}");
    } else if let Some(rest) = s.strip_prefix(r"\\?\") {
        s = rest.to_string();
    }
    while s.len() > 1 && s.ends_with('\\') && !(s.len() == 3 && s.as_bytes()[1] == b':') {
        s.pop();
    }
    if s.len() == 2 && s.as_bytes()[1] == b':' {
        s.push('\\');
    }
    (!s.is_empty() && !s.contains(['\t', '\n', '\r'])).then_some(s)
}

fn same(a: &str, b: &str) -> bool {
    a.chars().flat_map(char::to_lowercase).eq(b.chars().flat_map(char::to_lowercase))
}

/// `path` is `root` or inside it (case-insensitive)
pub fn within(root: &str, path: &str) -> bool {
    let r = root.trim_end_matches('\\');
    if path.len() < r.len() || !path.is_char_boundary(r.len()) || !same(&path[..r.len()], r) {
        return false;
    }
    path.len() == r.len() || path.as_bytes()[r.len()] == b'\\'
}

pub fn parse(text: &str) -> Db {
    let mut db = Db::default();
    for line in text.lines() {
        let line = line.trim_start_matches('\u{feff}').trim_end_matches('\r');
        let fields: Vec<&str> = line.split('\t').collect();
        match fields.as_slice() {
            ["T", id, name, color] => {
                let (Some(name), Some(color)) = (clean_name(name), clean_color(color)) else { continue };
                let id = id.trim();
                if id.is_empty() || id.contains(',') || db.tags.iter().any(|t| t.id == id) {
                    continue;
                }
                db.tags.push(Tag { id: id.to_string(), name, color });
            }
            ["I", path, volume, file, ids] => {
                let Some(path) = normalize(path) else { continue };
                let id = Identity {
                    volume: u32::from_str_radix(volume, 16).unwrap_or(0),
                    file: u64::from_str_radix(file, 16).unwrap_or(0),
                };
                let mut tags: Vec<String> = Vec::new();
                for t in ids.split(',').map(str::trim).filter(|t| !t.is_empty()) {
                    if !tags.iter().any(|x| x == t) {
                        tags.push(t.to_string());
                    }
                }
                if tags.is_empty() || db.items.iter().any(|i| same(&i.path, &path)) {
                    continue;
                }
                db.items.push(Item { path, id, tags });
            }
            _ => {}
        }
    }
    // Items may only reference tags that exist
    let known: Vec<String> = db.tags.iter().map(|t| t.id.clone()).collect();
    for item in &mut db.items {
        item.tags.retain(|t| known.contains(t));
    }
    db.items.retain(|i| !i.tags.is_empty());
    db
}

pub fn serialize(db: &Db) -> String {
    let mut out = format!("{HEADER}\n");
    for t in &db.tags {
        out += &format!("T\t{}\t{}\t{}\n", t.id, t.name, t.color);
    }
    for i in &db.items {
        out += &format!("I\t{}\t{:08X}\t{:016X}\t{}\n", i.path, i.id.volume, i.id.file, i.tags.join(","));
    }
    out
}

/// Next free id: "t" + (largest numeric suffix + 1)
fn next_id(db: &Db) -> String {
    let max = db.tags.iter().filter_map(|t| t.id.strip_prefix('t')?.parse::<u64>().ok()).max().unwrap_or(0);
    format!("t{}", max + 1)
}

/// New tag at the end; returns its id
pub fn add_tag(db: &mut Db, name: &str, color: &str) -> Option<String> {
    let (name, color) = (clean_name(name)?, clean_color(color)?);
    let id = next_id(db);
    db.tags.push(Tag { id: id.clone(), name, color });
    Some(id)
}

pub fn edit_tag(db: &mut Db, id: &str, name: Option<&str>, color: Option<&str>) -> bool {
    let name = match name {
        Some(n) => match clean_name(n) {
            Some(n) => Some(n),
            None => return false,
        },
        None => None,
    };
    let color = match color {
        Some(c) => match clean_color(c) {
            Some(c) => Some(c),
            None => return false,
        },
        None => None,
    };
    let Some(tag) = db.tags.iter_mut().find(|t| t.id == id) else { return false };
    let before = tag.clone();
    if let Some(n) = name {
        tag.name = n;
    }
    if let Some(c) = color {
        tag.color = c;
    }
    *tag != before
}

/// Deletes the tag and takes it off every item (items left with no tags are forgotten)
pub fn remove_tag(db: &mut Db, id: &str) -> bool {
    let before = db.tags.len();
    db.tags.retain(|t| t.id != id);
    if db.tags.len() == before {
        return false;
    }
    for item in &mut db.items {
        item.tags.retain(|t| t != id);
    }
    db.items.retain(|i| !i.tags.is_empty());
    true
}

pub fn move_tag(db: &mut Db, from: usize, to: usize) -> bool {
    if from >= db.tags.len() {
        return false;
    }
    let tag = db.tags.remove(from);
    db.tags.insert(to.min(db.tags.len()), tag);
    from != to
}

/// Where the file system says a path or file id is now (a trait so reconciliation can be tested without disks)
pub trait Locate {
    /// Identity of an existing path; None when it doesn't exist. Some(default) when it exists but has no stable id.
    fn identity(&self, path: &str) -> Option<Identity>;
    fn find(&self, id: Identity) -> Found;
}

#[derive(Debug, PartialEq, Eq)]
pub enum Found {
    At(String),
    Gone,    // its volume is here and the id doesn't open: deleted
    Unknown, // volume not mounted, network, no permission...: keep the item as it is
}

fn position(db: &Db, path: &str) -> Option<usize> {
    db.items.iter().position(|i| same(&i.path, path))
}

/// Tag ids of `path`. When it isn't stored under that name, a stored item with the same file id whose own path is gone
/// is the same file renamed or moved: its path is updated (returns true in the second value: save the db).
pub fn tags_of(db: &mut Db, path: &str, fs: &dyn Locate) -> (Vec<String>, bool) {
    let Some(path) = normalize(path) else { return (Vec::new(), false) };
    if let Some(i) = position(db, &path) {
        return (db.items[i].tags.clone(), false);
    }
    if db.items.is_empty() {
        return (Vec::new(), false);
    }
    let Some(id) = fs.identity(&path).filter(Identity::known) else { return (Vec::new(), false) };
    let Some(i) = db.items.iter().position(|it| it.id == id) else { return (Vec::new(), false) };
    if fs.identity(&db.items[i].path).is_some_and(|other| other == id) {
        return (Vec::new(), false); // hard link: the stored name still exists and is the same file
    }
    db.items[i].path = path;
    (db.items[i].tags.clone(), true)
}

/// Adds (`on`) or removes tag `id` on each path. Returns true when anything changed.
pub fn set(db: &mut Db, paths: &[String], id: &str, on: bool, fs: &dyn Locate) -> bool {
    if !db.tags.iter().any(|t| t.id == id) {
        return false;
    }
    let mut changed = false;
    for raw in paths {
        let Some(path) = normalize(raw) else { continue };
        let (_, moved) = tags_of(db, &path, fs); // follows a rename first, so the moved item is the one edited
        changed |= moved;
        match (position(db, &path), on) {
            (Some(i), true) => {
                if !db.items[i].tags.iter().any(|t| t == id) {
                    db.items[i].tags.push(id.to_string());
                    changed = true;
                }
                if let Some(now) = fs.identity(&path) {
                    if now != db.items[i].id {
                        db.items[i].id = now;
                        changed = true;
                    }
                }
            }
            (Some(i), false) => {
                let before = db.items[i].tags.len();
                db.items[i].tags.retain(|t| t != id);
                if db.items[i].tags.len() != before {
                    changed = true;
                    if db.items[i].tags.is_empty() {
                        db.items.remove(i);
                    }
                }
            }
            (None, true) => {
                let identity = fs.identity(&path).unwrap_or_default();
                db.items.push(Item { path, id: identity, tags: vec![id.to_string()] });
                changed = true;
            }
            (None, false) => {}
        }
    }
    changed
}

/// Brings every stored path up to date (before listing a tag's files). Returns true when the db changed.
pub fn reconcile(db: &mut Db, fs: &dyn Locate) -> bool {
    let mut changed = false;
    let mut keep = Vec::with_capacity(db.items.len());
    for mut item in std::mem::take(&mut db.items) {
        match fs.identity(&item.path) {
            Some(now) => {
                // Still there (an editor's save-by-rename gives a new id: the tags stay with the name)
                if now.known() && now != item.id {
                    item.id = now;
                    changed = true;
                }
                keep.push(item);
            }
            None if item.id.known() => match fs.find(item.id) {
                Found::At(path) => {
                    if let Some(p) = normalize(&path) {
                        item.path = p;
                        changed = true;
                    }
                    keep.push(item);
                }
                Found::Gone => changed = true,
                Found::Unknown => keep.push(item),
            },
            None => keep.push(item), // no id to follow; maybe its drive comes back
        }
    }
    // Two items may now share a path (a file moved onto another tagged name): merge them
    let mut merged: Vec<Item> = Vec::with_capacity(keep.len());
    for item in keep {
        if let Some(existing) = merged.iter_mut().find(|m| same(&m.path, &item.path)) {
            for t in item.tags {
                if !existing.tags.contains(&t) {
                    existing.tags.push(t);
                }
            }
            changed = true;
        } else {
            merged.push(item);
        }
    }
    db.items = merged;
    changed
}

/// Paths tagged `id` (all tags when None), inside `root` when given, in stored order
pub fn items_with(db: &Db, id: Option<&str>, root: Option<&str>) -> Vec<String> {
    db.items
        .iter()
        .filter(|i| id.map_or(true, |id| i.tags.iter().any(|t| t == id)))
        .filter(|i| root.map_or(true, |r| within(r, &i.path)))
        .map(|i| i.path.clone())
        .collect()
}

// ------------------------------------------------------------------------------------------------------------ file

// Every read-modify-write of tags.txt in this process goes through here, so two threads can't lose each other's edits
static LOCK: Mutex<()> = Mutex::new(());

pub fn load(file: &Path) -> Db {
    match store::read(file) {
        Ok(bytes) => parse(&String::from_utf8_lossy(&bytes)),
        Err(_) => defaults(),
    }
}

/// Runs `change` on the current db and saves it when it returns true. The result of `change` is passed through.
pub fn update<R>(file: &Path, change: impl FnOnce(&mut Db) -> (bool, R)) -> io::Result<R> {
    let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let existed = file.exists();
    let mut db = load(file);
    let (dirty, result) = change(&mut db);
    if dirty || !existed {
        store::write_atomic(file, serialize(&db).as_bytes())?;
    }
    Ok(result)
}

pub fn read<R>(file: &Path, look: impl FnOnce(&Db) -> R) -> R {
    let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    look(&load(file))
}

// --------------------------------------------------------------------------------------------------------- windows

#[repr(C)]
#[derive(Default)]
struct ByHandleInfo {
    attributes: u32,
    created: [u32; 2],
    accessed: [u32; 2],
    written: [u32; 2],
    volume: u32,
    size_high: u32,
    size_low: u32,
    links: u32,
    index_high: u32,
    index_low: u32,
}

#[repr(C)]
struct FileIdDescriptor {
    size: u32,
    kind: u32, // 0 = FileIdType
    id: u64,   // first member of the union (the union is 16 bytes: ExtendedFileId)
    _rest: u64,
}

type Handle = *mut std::ffi::c_void;
const INVALID: Handle = -1isize as Handle;
const FILE_READ_ATTRIBUTES: u32 = 0x80;
const SHARE_ALL: u32 = 0x1 | 0x2 | 0x4;
const OPEN_EXISTING: u32 = 3;
const BACKUP_SEMANTICS: u32 = 0x0200_0000;
const DRIVE_FIXED: u32 = 3;
const DRIVE_REMOVABLE: u32 = 2;
const ERROR_INVALID_PARAMETER: u32 = 87;
const ERROR_FILE_NOT_FOUND: u32 = 2;
const ERROR_PATH_NOT_FOUND: u32 = 3;
const ERROR_DELETE_PENDING: u32 = 303;

#[link(name = "kernel32")]
extern "system" {
    fn CreateFileW(name: *const u16, access: u32, share: u32, sa: *mut std::ffi::c_void, disposition: u32, flags: u32, template: Handle) -> Handle;
    fn GetFileInformationByHandle(file: Handle, info: *mut ByHandleInfo) -> i32;
    fn OpenFileById(hint: Handle, id: *const FileIdDescriptor, access: u32, share: u32, sa: *mut std::ffi::c_void, flags: u32) -> Handle;
    fn GetFinalPathNameByHandleW(file: Handle, out: *mut u16, len: u32, flags: u32) -> u32;
    fn CloseHandle(h: Handle) -> i32;
    fn GetLogicalDrives() -> u32;
    fn GetDriveTypeW(root: *const u16) -> u32;
    fn GetVolumeInformationW(root: *const u16, name: *mut u16, name_len: u32, serial: *mut u32, max_component: *mut u32,
        flags: *mut u32, fs: *mut u16, fs_len: u32) -> i32;
    fn GetLastError() -> u32;
    fn SetThreadErrorMode(mode: u32, old: *mut u32) -> i32;
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Local disks only: a network drive's "file id" can't be opened again, and asking may wake a sleeping server
fn local(path: &str) -> bool {
    let b = path.as_bytes();
    if b.len() < 3 || b[1] != b':' {
        return false;
    }
    let root = wide(&path[..3]);
    matches!(unsafe { GetDriveTypeW(root.as_ptr()) }, DRIVE_FIXED | DRIVE_REMOVABLE)
}

pub struct Disk;

impl Locate for Disk {
    fn identity(&self, path: &str) -> Option<Identity> {
        let w = wide(path);
        let mut old = 0;
        unsafe { SetThreadErrorMode(1, &mut old) }; // SEM_FAILCRITICALERRORS: no "insert a disk" prompts
        let h = unsafe { CreateFileW(w.as_ptr(), FILE_READ_ATTRIBUTES, SHARE_ALL, std::ptr::null_mut(), OPEN_EXISTING, BACKUP_SEMANTICS, std::ptr::null_mut()) };
        unsafe { SetThreadErrorMode(old, std::ptr::null_mut()) };
        if h == INVALID {
            // Exists but can't be opened (in use exclusively, no rights): still there, just no id
            return Path::new(path).symlink_metadata().ok().map(|_| Identity::default());
        }
        let mut info = ByHandleInfo::default();
        let ok = unsafe { GetFileInformationByHandle(h, &mut info) } != 0;
        unsafe { CloseHandle(h) };
        if !ok || !local(path) {
            return Some(Identity::default());
        }
        Some(Identity { volume: info.volume, file: (info.index_high as u64) << 32 | info.index_low as u64 })
    }

    fn find(&self, id: Identity) -> Found {
        if !id.known() {
            return Found::Unknown;
        }
        let mask = unsafe { GetLogicalDrives() };
        for letter in 0..26u8 {
            if mask & (1 << letter) == 0 {
                continue;
            }
            let root = format!("{}:\\", (b'A' + letter) as char);
            let wroot = wide(&root);
            if !matches!(unsafe { GetDriveTypeW(wroot.as_ptr()) }, DRIVE_FIXED | DRIVE_REMOVABLE) {
                continue;
            }
            let mut serial = 0u32;
            let ok = unsafe {
                GetVolumeInformationW(wroot.as_ptr(), std::ptr::null_mut(), 0, &mut serial, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), 0)
            } != 0;
            if !ok || serial != id.volume {
                continue;
            }
            let hint = unsafe { CreateFileW(wroot.as_ptr(), FILE_READ_ATTRIBUTES, SHARE_ALL, std::ptr::null_mut(), OPEN_EXISTING, BACKUP_SEMANTICS, std::ptr::null_mut()) };
            if hint == INVALID {
                return Found::Unknown;
            }
            let desc = FileIdDescriptor { size: std::mem::size_of::<FileIdDescriptor>() as u32, kind: 0, id: id.file, _rest: 0 };
            let h = unsafe { OpenFileById(hint, &desc, FILE_READ_ATTRIBUTES, SHARE_ALL, std::ptr::null_mut(), BACKUP_SEMANTICS) };
            let err = if h == INVALID { unsafe { GetLastError() } } else { 0 };
            unsafe { CloseHandle(hint) };
            if h == INVALID {
                return match err {
                    ERROR_INVALID_PARAMETER | ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND | ERROR_DELETE_PENDING => Found::Gone,
                    _ => Found::Unknown,
                };
            }
            let mut buf = vec![0u16; 1024];
            let n = unsafe { GetFinalPathNameByHandleW(h, buf.as_mut_ptr(), buf.len() as u32, 0) } as usize;
            unsafe { CloseHandle(h) };
            if n == 0 || n >= buf.len() {
                return Found::Unknown;
            }
            let path = String::from_utf16_lossy(&buf[..n]);
            // A file in the recycle bin isn't "moved"; keep the tag in case it's restored, but don't follow it there
            if path.to_ascii_lowercase().contains("\\$recycle.bin\\") {
                return Found::Unknown;
            }
            return normalize(&path).map_or(Found::Unknown, Found::At);
        }
        Found::Unknown
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::HashMap;

    #[derive(Default)]
    struct Fake {
        files: RefCell<HashMap<String, Identity>>, // lowercase path -> id
        mounted: u32,
    }

    impl Fake {
        fn put(&self, path: &str, file: u64) {
            self.files.borrow_mut().insert(path.to_lowercase(), Identity { volume: self.mounted, file });
        }
        fn rename(&self, from: &str, to: &str) {
            let id = self.files.borrow_mut().remove(&from.to_lowercase()).unwrap();
            self.files.borrow_mut().insert(to.to_lowercase(), id);
        }
        fn delete(&self, path: &str) {
            self.files.borrow_mut().remove(&path.to_lowercase());
        }
    }

    impl Locate for Fake {
        fn identity(&self, path: &str) -> Option<Identity> {
            self.files.borrow().get(&path.to_lowercase()).copied()
        }
        fn find(&self, id: Identity) -> Found {
            if id.volume != self.mounted {
                return Found::Unknown;
            }
            self.files.borrow().iter().find(|(_, v)| **v == id).map_or(Found::Gone, |(k, _)| Found::At(k.clone()))
        }
    }

    fn paths(p: &[&str]) -> Vec<String> {
        p.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn defaults_and_roundtrip() {
        let mut db = defaults();
        assert_eq!(db.tags.len(), 4);
        let fs = Fake { mounted: 7, ..Default::default() };
        fs.put(r"C:\a\x.txt", 11);
        assert!(set(&mut db, &paths(&[r"C:\a\x.txt\"]), "t2", true, &fs));
        let text = serialize(&db);
        assert!(text.starts_with(HEADER));
        assert_eq!(parse(&text), db);
        assert_eq!(db.items[0].path, r"C:\a\x.txt");
        assert_eq!(db.items[0].id, Identity { volume: 7, file: 11 });
    }

    #[test]
    fn parse_drops_bad_lines_and_unknown_tags() {
        let db = parse("\u{feff}#fastfiles-tags 1\r\nT\tt1\tWork\t#abc\nT\tt1\tDup\t#000000\nT\tt2\t\t#000000\nT\tt3\tBad\tzz\n\
                        I\tC:\\x\t0\t0\tt1,t9,t1\nI\tC:\\y\t0\t0\tt9\nI\tc:\\X\t0\t0\tt1\ngarbage\n");
        assert_eq!(db.tags, vec![Tag { id: "t1".into(), name: "Work".into(), color: "#AABBCC".into() }]);
        assert_eq!(db.items.len(), 1);
        assert_eq!(db.items[0].tags, vec!["t1".to_string()]);
    }

    #[test]
    fn tag_editing() {
        let mut db = Db::default();
        let a = add_tag(&mut db, "  工作\t", "#FF0000").unwrap();
        let b = add_tag(&mut db, "Home", "00ff00").unwrap();
        assert_eq!((a.as_str(), b.as_str()), ("t1", "t2"));
        assert_eq!(db.tags[0].name, "工作");
        assert!(add_tag(&mut db, "   ", "#000000").is_none());
        assert!(add_tag(&mut db, "x", "red").is_none());
        assert!(edit_tag(&mut db, &a, Some("Work"), None));
        assert!(!edit_tag(&mut db, &a, Some("Work"), None));
        assert!(!edit_tag(&mut db, &a, Some(""), None));
        assert!(edit_tag(&mut db, &b, None, Some("#123")));
        assert_eq!(db.tags[1].color, "#112233");
        assert!(move_tag(&mut db, 1, 0));
        assert_eq!(db.tags[0].id, b);
        let fs = Fake::default();
        set(&mut db, &paths(&[r"C:\f"]), &a, true, &fs);
        set(&mut db, &paths(&[r"C:\f", r"C:\g"]), &b, true, &fs);
        assert!(remove_tag(&mut db, &a));
        assert_eq!(db.items.len(), 2);
        assert!(remove_tag(&mut db, &b));
        assert!(db.items.is_empty());
        assert!(!remove_tag(&mut db, "t99"));
    }

    #[test]
    fn set_add_remove_is_idempotent() {
        let mut db = defaults();
        let fs = Fake { mounted: 1, ..Default::default() };
        fs.put(r"D:\p", 5);
        let p = paths(&[r"D:\p"]);
        assert!(set(&mut db, &p, "t1", true, &fs));
        assert!(!set(&mut db, &p, "t1", true, &fs));
        assert!(set(&mut db, &p, "t3", true, &fs));
        assert_eq!(tags_of(&mut db, r"d:\P", &fs).0, vec!["t1".to_string(), "t3".to_string()]);
        assert!(set(&mut db, &p, "t1", false, &fs));
        assert!(!set(&mut db, &p, "t1", false, &fs));
        assert!(set(&mut db, &p, "t3", false, &fs));
        assert!(db.items.is_empty());
        assert!(!set(&mut db, &p, "nope", true, &fs));
    }

    #[test]
    fn follows_renames_by_file_id() {
        let mut db = defaults();
        let fs = Fake { mounted: 9, ..Default::default() };
        fs.put(r"C:\docs\report.docx", 42);
        set(&mut db, &paths(&[r"C:\docs\report.docx"]), "t4", true, &fs);
        fs.rename(r"C:\docs\report.docx", r"C:\archive\report-final.docx");
        // Looked up under the new name: found by id, path updated
        let (tags, moved) = tags_of(&mut db, r"C:\archive\report-final.docx", &fs);
        assert_eq!(tags, vec!["t4".to_string()]);
        assert!(moved);
        assert_eq!(db.items[0].path, r"C:\archive\report-final.docx");
        // Reconcile also finds moves nobody looked at
        fs.rename(r"C:\archive\report-final.docx", r"C:\x.docx");
        assert!(reconcile(&mut db, &fs));
        assert_eq!(db.items[0].path.to_lowercase(), r"c:\x.docx");
    }

    #[test]
    fn reconcile_drops_deleted_keeps_unmounted() {
        let mut db = defaults();
        let fs = Fake { mounted: 3, ..Default::default() };
        fs.put(r"C:\gone", 1);
        fs.put(r"C:\kept", 2);
        set(&mut db, &paths(&[r"C:\gone", r"C:\kept"]), "t1", true, &fs);
        // An item from a volume that isn't mounted
        db.items.push(Item { path: r"E:\usb\file".into(), id: Identity { volume: 99, file: 7 }, tags: vec!["t1".into()] });
        fs.delete(r"C:\gone");
        assert!(reconcile(&mut db, &fs));
        let left: Vec<&str> = db.items.iter().map(|i| i.path.as_str()).collect();
        assert_eq!(left, vec![r"C:\kept", r"E:\usb\file"]);
        assert!(!reconcile(&mut db, &fs));
    }

    #[test]
    fn same_name_new_id_keeps_tags() {
        // Editors save by writing a new file and renaming it over the old one: new id, same name
        let mut db = defaults();
        let fs = Fake { mounted: 1, ..Default::default() };
        fs.put(r"C:\notes.txt", 10);
        set(&mut db, &paths(&[r"C:\notes.txt"]), "t2", true, &fs);
        fs.delete(r"C:\notes.txt");
        fs.put(r"C:\notes.txt", 11);
        assert!(reconcile(&mut db, &fs));
        assert_eq!(db.items[0].id.file, 11);
        assert_eq!(tags_of(&mut db, r"C:\notes.txt", &fs).0, vec!["t2".to_string()]);
    }

    #[test]
    fn hard_link_does_not_steal_tags() {
        let mut db = defaults();
        let fs = Fake { mounted: 1, ..Default::default() };
        fs.put(r"C:\a", 5);
        fs.put(r"C:\b", 5); // second name of the same file
        set(&mut db, &paths(&[r"C:\a"]), "t1", true, &fs);
        let (tags, moved) = tags_of(&mut db, r"C:\b", &fs);
        assert!(tags.is_empty() && !moved);
        assert_eq!(db.items[0].path, r"C:\a");
    }

    #[test]
    fn items_with_scope() {
        let mut db = defaults();
        let fs = Fake::default();
        set(&mut db, &paths(&[r"C:\w\a", r"C:\w2\b", r"D:\c"]), "t1", true, &fs);
        set(&mut db, &paths(&[r"C:\w\a"]), "t2", true, &fs);
        assert_eq!(items_with(&db, Some("t1"), Some(r"C:\w")), vec![r"C:\w\a".to_string()]);
        assert_eq!(items_with(&db, Some("t1"), Some(r"c:\")).len(), 2);
        assert_eq!(items_with(&db, Some("t2"), None), vec![r"C:\w\a".to_string()]);
        assert_eq!(items_with(&db, None, None).len(), 3);
    }

    #[test]
    fn normalize_paths() {
        assert_eq!(normalize(r"\\?\C:\x\").as_deref(), Some(r"C:\x"));
        assert_eq!(normalize(r"\\?\UNC\srv\share\f").as_deref(), Some(r"\\srv\share\f"));
        assert_eq!(normalize("D:").as_deref(), Some(r"D:\"));
        assert_eq!(normalize(r"C:\").as_deref(), Some(r"C:\"));
        assert!(normalize("a\tb").is_none());
        assert!(within(r"C:\", r"C:\x"));
        assert!(!within(r"C:\w", r"C:\w2\x"));
    }

    #[test]
    fn real_disk_identity_and_find() {
        let dir = std::env::temp_dir().join(format!("fastfiles-tags-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let a = dir.join("a.txt");
        std::fs::write(&a, b"x").unwrap();
        let disk = Disk;
        let id = disk.identity(a.to_str().unwrap()).unwrap();
        if id.known() {
            let b = dir.join("b.txt");
            std::fs::rename(&a, &b).unwrap();
            assert!(disk.identity(a.to_str().unwrap()).is_none());
            match disk.find(id) {
                Found::At(p) => assert!(same(&p, b.to_str().unwrap()), "{p}"),
                other => panic!("{other:?}"),
            }
            std::fs::remove_file(&b).unwrap();
            assert_ne!(disk.find(id), Found::At(b.to_str().unwrap().to_string()));
        }
        // File db: load seeds defaults, update writes atomically
        let file = dir.join("tags.txt");
        let tags = read(&file, |db| db.tags.len());
        assert_eq!(tags, 4);
        let id = update(&file, |db| {
            let id = add_tag(db, "新标签", "#102030");
            (id.is_some(), id)
        })
        .unwrap();
        assert_eq!(id.as_deref(), Some("t5"));
        assert_eq!(load(&file).tags.len(), 5);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
