//! Parallel folder walk shared by folder size and file search. Workers pop folders from one stack, list them, and push
//! the subfolders they find; reparse points (junctions, symlinks) are reported but not entered, like Explorer.

use std::fs;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex};

const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x10;
const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
const FILE_ATTRIBUTE_SYSTEM: u32 = 0x4;
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;

/// One directory entry as seen by a visitor (from the listing itself: no extra system call per entry)
pub struct Entry<'a> {
    pub dir: &'a Path,
    pub name: &'a std::ffi::OsStr,
    pub attributes: u32,
    pub size: u64,
}

impl Entry<'_> {
    pub fn is_dir(&self) -> bool {
        self.attributes & FILE_ATTRIBUTE_DIRECTORY != 0
    }
    pub fn is_hidden(&self) -> bool {
        self.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) != 0
    }
}

pub trait Visitor: Sync {
    /// Called for every entry. Returning false for a folder keeps the walk out of it.
    fn entry(&self, entry: &Entry) -> bool;
    /// A folder that couldn't be listed
    fn skipped(&self, _dir: &Path) {}
    /// After each folder: totals can be flushed per folder instead of per entry
    fn folder_done(&self) {}
}

struct Queue {
    pending: Vec<PathBuf>,
    busy: usize,
}

/// Walks below `root` (not reported itself). Returns false when `cancel` stopped it.
pub fn walk<V: Visitor>(root: &Path, threads: usize, cancel: &AtomicBool, visitor: &V) -> bool {
    let queue = Mutex::new(Queue { pending: vec![root.to_path_buf()], busy: 0 });
    let wake = Condvar::new();
    std::thread::scope(|scope| {
        for _ in 0..threads.max(1) {
            scope.spawn(|| worker(&queue, &wake, cancel, visitor));
        }
    });
    !cancel.load(Ordering::Relaxed)
}

fn worker<V: Visitor>(queue: &Mutex<Queue>, wake: &Condvar, cancel: &AtomicBool, visitor: &V) {
    let mut found = Vec::new();
    loop {
        let dir = {
            let mut q = queue.lock().unwrap();
            loop {
                if cancel.load(Ordering::Relaxed) {
                    wake.notify_all();
                    return;
                }
                if let Some(dir) = q.pending.pop() {
                    q.busy += 1;
                    break dir;
                }
                if q.busy == 0 {
                    wake.notify_all();
                    return;
                }
                q = wake.wait(q).unwrap();
            }
        };
        list(&dir, cancel, visitor, &mut found);
        let mut q = queue.lock().unwrap();
        q.busy -= 1;
        let more = !found.is_empty();
        q.pending.append(&mut found);
        if more || q.busy == 0 {
            wake.notify_all();
        }
    }
}

fn list<V: Visitor>(dir: &Path, cancel: &AtomicBool, visitor: &V, found: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        visitor.skipped(dir);
        return;
    };
    for entry in entries.flatten() {
        if cancel.load(Ordering::Relaxed) {
            break;
        }
        let Ok(meta) = entry.metadata() else { continue };
        let name = entry.file_name();
        let e = Entry { dir, name: &name, attributes: meta.file_attributes(), size: meta.file_size() };
        let enter = visitor.entry(&e);
        if e.is_dir() && enter && e.attributes & FILE_ATTRIBUTE_REPARSE_POINT == 0 {
            found.push(entry.path());
        }
    }
    visitor.folder_done();
}
