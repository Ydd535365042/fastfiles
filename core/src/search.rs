//! File name search (Files' search box without the Windows index): a parallel walk below the current folder that
//! streams matching paths to the caller as it finds them.
//!
//! Query: words separated by spaces must all appear in the name (any order, case-insensitive). A query containing
//! `*` or `?` is a wildcard pattern matched against the whole name instead (`*.pdf`, `report-??.docx`).

use crate::walk::{self, Entry, Visitor};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

#[derive(Debug, PartialEq)]
pub enum Query {
    Words(Vec<Vec<char>>),
    Pattern(Vec<char>),
}

fn lower(s: &str) -> Vec<char> {
    s.chars().flat_map(char::to_lowercase).collect()
}

/// None when the query has nothing to match on
pub fn compile(query: &str) -> Option<Query> {
    let q = query.trim();
    if q.is_empty() {
        return None;
    }
    if q.contains(['*', '?']) {
        let p = lower(q);
        return (!p.iter().all(|&c| c == '*')).then_some(Query::Pattern(p));
    }
    Some(Query::Words(q.split_whitespace().map(lower).collect()))
}

fn contains(hay: &[char], needle: &[char]) -> bool {
    needle.is_empty() || hay.windows(needle.len()).any(|w| w == needle)
}

/// Wildcard match over the whole name: `*` any run, `?` one character (iterative, linear backtracking)
fn glob(name: &[char], pat: &[char]) -> bool {
    let (mut n, mut p) = (0, 0);
    let (mut star, mut mark) = (usize::MAX, 0);
    while n < name.len() {
        if p < pat.len() && (pat[p] == '?' || pat[p] == name[n]) {
            n += 1;
            p += 1;
        } else if p < pat.len() && pat[p] == '*' {
            star = p;
            mark = n;
            p += 1;
        } else if star != usize::MAX {
            p = star + 1;
            mark += 1;
            n = mark;
        } else {
            return false;
        }
    }
    pat[p..].iter().all(|&c| c == '*')
}

pub fn matches(query: &Query, name: &str) -> bool {
    let name = lower(name);
    match query {
        Query::Words(words) => words.iter().all(|w| contains(&name, w)),
        Query::Pattern(p) => glob(&name, p),
    }
}

/// Progress, readable while the search runs
#[repr(C)]
#[derive(Default)]
pub struct Progress {
    pub matches: AtomicU64,
    pub files: AtomicU64,
    pub folders: AtomicU64,
    pub skipped: AtomicU64,
}

pub struct Options {
    /// Hidden and system items are matched (and their folders entered) only when the user shows hidden items
    pub include_hidden: bool,
    /// Stop after this many matches
    pub limit: u64,
}

struct Searcher<'a, F: Fn(&Path) + Sync> {
    query: &'a Query,
    options: &'a Options,
    progress: &'a Progress,
    stop: &'a AtomicBool,
    sink: F,
}

impl<F: Fn(&Path) + Sync> Visitor for Searcher<'_, F> {
    fn entry(&self, e: &Entry) -> bool {
        if e.is_hidden() && !self.options.include_hidden {
            return false;
        }
        if e.is_dir() {
            self.progress.folders.fetch_add(1, Ordering::Relaxed);
        } else {
            self.progress.files.fetch_add(1, Ordering::Relaxed);
        }
        if let Some(name) = e.name.to_str() {
            if matches(self.query, name) {
                let n = self.progress.matches.fetch_add(1, Ordering::Relaxed) + 1;
                if n <= self.options.limit {
                    (self.sink)(&e.dir.join(e.name));
                }
                if n >= self.options.limit {
                    self.stop.store(true, Ordering::Relaxed);
                }
            }
        }
        true
    }
    fn skipped(&self, _dir: &Path) {
        self.progress.skipped.fetch_add(1, Ordering::Relaxed);
    }
}

/// Searches below `root`, calling `sink` (from worker threads) for each match. Returns false when `cancel` stopped it;
/// reaching `limit` is a normal finish.
pub fn search(root: &Path, query: &Query, options: &Options, threads: usize, progress: &Progress, cancel: &AtomicBool, sink: impl Fn(&Path) + Sync) -> bool {
    // The walk watches one flag: the caller's cancel, or our own when the limit is hit
    let stop = AtomicBool::new(false);
    let searcher = Searcher { query, options, progress, stop: &stop, sink };
    let done = AtomicBool::new(false);
    std::thread::scope(|scope| {
        scope.spawn(|| {
            while !done.load(Ordering::Relaxed) {
                if cancel.load(Ordering::Relaxed) {
                    stop.store(true, Ordering::Relaxed);
                    return;
                }
                std::thread::sleep(std::time::Duration::from_millis(15));
            }
        });
        walk::walk(root, threads, &stop, &searcher);
        done.store(true, Ordering::Relaxed);
    });
    !cancel.load(Ordering::Relaxed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::sync::Mutex;

    #[test]
    fn words_and_patterns() {
        let q = compile("  Report 2024 ").unwrap();
        assert!(matches(&q, "Annual-2024-REPORT.pdf"));
        assert!(!matches(&q, "report.pdf"));
        let p = compile("*.PDF").unwrap();
        assert!(matches(&p, "a.pdf") && !matches(&p, "a.pdf.txt"));
        let p = compile("f?.bin").unwrap();
        assert!(matches(&p, "f1.bin") && !matches(&p, "f10.bin"));
        assert!(matches(&compile("a*b*c").unwrap(), "aXXbYYc") && !matches(&compile("a*b*c").unwrap(), "aXXcYYb"));
        assert!(matches(&compile("文档").unwrap(), "我的文档.txt"));
        assert_eq!(compile("   "), None);
        assert_eq!(compile("**"), None);
    }

    #[test]
    fn finds_files_and_folders_and_honors_limit_and_hidden() {
        let root = std::env::temp_dir().join(format!("ff-search-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        for i in 0..30 {
            let d = root.join(format!("dir{}", i % 5));
            fs::create_dir_all(&d).unwrap();
            fs::write(d.join(format!("note{i}.txt")), b"x").unwrap();
        }
        fs::create_dir_all(root.join("notes-folder")).unwrap();
        let found = Mutex::new(Vec::new());
        let progress = Progress::default();
        let opts = Options { include_hidden: true, limit: 1000 };
        assert!(search(&root, &compile("note").unwrap(), &opts, 4, &progress, &AtomicBool::new(false), |p| found.lock().unwrap().push(p.to_path_buf())));
        assert_eq!(found.lock().unwrap().len(), 31);
        assert_eq!(progress.folders.load(Ordering::Relaxed), 6);

        let found = Mutex::new(0);
        let progress = Progress::default();
        let opts = Options { include_hidden: true, limit: 5 };
        assert!(search(&root, &compile("*.txt").unwrap(), &opts, 4, &progress, &AtomicBool::new(false), |_| *found.lock().unwrap() += 1));
        assert_eq!(*found.lock().unwrap(), 5);
        fs::remove_dir_all(&root).unwrap();
    }
}
