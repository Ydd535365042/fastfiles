//! Folder size: sums file sizes the way Explorer's Properties dialog does (logical size, reparse points counted but not
//! followed). Totals are published while the walk runs so the UI can show progress.

use crate::walk::{self, Entry, Visitor};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

/// Running totals, shared with the caller (C++ reads them while the walk is in progress)
#[repr(C)]
#[derive(Default)]
pub struct Totals {
    pub bytes: AtomicU64,
    pub files: AtomicU64,
    pub folders: AtomicU64,
    /// Folders that couldn't be listed (access denied and the like)
    pub skipped: AtomicU64,
}

struct Sizer<'a>(&'a Totals);

impl Visitor for Sizer<'_> {
    fn entry(&self, e: &Entry) -> bool {
        if e.is_dir() {
            self.0.folders.fetch_add(1, Ordering::Relaxed);
        } else {
            self.0.bytes.fetch_add(e.size, Ordering::Relaxed);
            self.0.files.fetch_add(1, Ordering::Relaxed);
        }
        true
    }
    fn skipped(&self, _dir: &Path) {
        self.0.skipped.fetch_add(1, Ordering::Relaxed);
    }
}

/// Walks `root` (not counted itself) with up to `threads` workers. Returns false when cancelled.
pub fn measure(root: &Path, threads: usize, totals: &Totals, cancel: &AtomicBool) -> bool {
    walk::walk(root, threads, cancel, &Sizer(totals))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ff-dirsize-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn sums_nested_files_and_counts_folders() {
        let root = temp_dir("nested");
        fs::write(root.join("a.bin"), vec![0u8; 1000]).unwrap();
        for i in 0..20 {
            let sub = root.join(format!("d{i}")).join("deeper");
            fs::create_dir_all(&sub).unwrap();
            fs::write(sub.join("b.bin"), vec![1u8; 10]).unwrap();
        }
        let totals = Totals::default();
        assert!(measure(&root, 4, &totals, &AtomicBool::new(false)));
        assert_eq!(totals.bytes.load(Ordering::Relaxed), 1000 + 20 * 10);
        assert_eq!(totals.files.load(Ordering::Relaxed), 21);
        assert_eq!(totals.folders.load(Ordering::Relaxed), 40);
        assert_eq!(totals.skipped.load(Ordering::Relaxed), 0);
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn missing_root_is_skipped_and_cancel_is_reported() {
        let totals = Totals::default();
        assert!(measure(Path::new(r"C:\definitely\not\here\ff"), 2, &totals, &AtomicBool::new(false)));
        assert_eq!(totals.skipped.load(Ordering::Relaxed), 1);
        let root = temp_dir("cancel");
        assert!(!measure(&root, 2, &Totals::default(), &AtomicBool::new(true)));
        fs::remove_dir_all(&root).unwrap();
    }
}
