//! Sidebar pinned folders: one parsing name per line (file paths, UNC paths or `::{GUID}` shell locations), in display order.

use crate::store;
use std::io;
use std::path::Path;

/// Trim, drop a trailing backslash (but keep drive roots like `C:\`), ignore blanks and `#` comments.
fn normalize(line: &str) -> Option<String> {
    let mut s = line.trim().trim_start_matches('\u{feff}').to_string();
    if s.is_empty() || s.starts_with('#') {
        return None;
    }
    let drive_root = |s: &str| s.len() == 3 && s.as_bytes()[1] == b':';
    while s.len() > 1 && s.ends_with('\\') && !drive_root(&s) {
        s.pop();
    }
    if s.len() == 2 && s.as_bytes()[1] == b':' {
        s.push('\\');
    }
    Some(s)
}

fn same(a: &str, b: &str) -> bool {
    a.chars().flat_map(char::to_lowercase).eq(b.chars().flat_map(char::to_lowercase))
}

pub fn parse(text: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for item in text.lines().filter_map(normalize) {
        if !out.iter().any(|p| same(p, &item)) {
            out.push(item);
        }
    }
    out
}

pub fn serialize(pins: &[String]) -> String {
    pins.iter().map(|p| format!("{p}\n")).collect()
}

/// Insert at `index` (clamped; None = end). An existing pin is moved instead of duplicated.
pub fn add(pins: &mut Vec<String>, item: &str, index: Option<usize>) -> bool {
    let Some(item) = normalize(item) else { return false };
    let mut at = index.unwrap_or(pins.len());
    if let Some(old) = pins.iter().position(|p| same(p, &item)) {
        pins.remove(old);
        if old < at {
            at -= 1;
        }
    }
    pins.insert(at.min(pins.len()), item);
    true
}

pub fn remove(pins: &mut Vec<String>, item: &str) -> bool {
    let Some(item) = normalize(item) else { return false };
    let before = pins.len();
    pins.retain(|p| !same(p, &item));
    pins.len() != before
}

/// Move the pin at `from` so it ends up at position `to` of the resulting list.
pub fn move_to(pins: &mut Vec<String>, from: usize, to: usize) -> bool {
    if from >= pins.len() {
        return false;
    }
    let item = pins.remove(from);
    pins.insert(to.min(pins.len()), item);
    true
}

/// None when the file doesn't exist yet (first run: the caller seeds defaults); Some(empty) when the user unpinned everything.
pub fn load(file: &Path) -> Option<Vec<String>> {
    store::read(file).ok().map(|bytes| parse(&String::from_utf8_lossy(&bytes)))
}

pub fn update(file: &Path, change: impl FnOnce(&mut Vec<String>) -> bool) -> io::Result<bool> {
    let mut pins = load(file).unwrap_or_default();
    if !change(&mut pins) {
        return Ok(false);
    }
    store::write_atomic(file, serialize(&pins).as_bytes())?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parse_normalizes_and_dedupes() {
        let pins = parse("\u{feff}C:\\Users\\a\\Desktop\\\r\n\n# comment\nc:\\users\\A\\desktop\nD:\nD:\\\n\\\\server\\share\\\n::{645FF040-5081-101B-9F08-00AA002F954E}\n");
        assert_eq!(pins, v(&["C:\\Users\\a\\Desktop", "D:\\", "\\\\server\\share", "::{645FF040-5081-101B-9F08-00AA002F954E}"]));
        assert_eq!(parse(&serialize(&pins)), pins);
    }

    #[test]
    fn add_moves_existing_instead_of_duplicating() {
        let mut pins = v(&["A", "B", "C"]);
        assert!(add(&mut pins, "D", None));
        assert_eq!(pins, v(&["A", "B", "C", "D"]));
        assert!(add(&mut pins, "a", Some(3)));
        assert_eq!(pins, v(&["B", "C", "a", "D"]));
        assert!(add(&mut pins, "E", Some(99)));
        assert_eq!(pins.last().unwrap(), "E");
        assert!(!add(&mut pins, "   ", None));
    }

    #[test]
    fn remove_and_move() {
        let mut pins = v(&["A", "B", "C"]);
        assert!(remove(&mut pins, "b\\"));
        assert!(!remove(&mut pins, "zzz"));
        assert_eq!(pins, v(&["A", "C"]));
        assert!(move_to(&mut pins, 0, 1));
        assert_eq!(pins, v(&["C", "A"]));
        assert!(!move_to(&mut pins, 5, 0));
    }

    #[test]
    fn file_roundtrip_distinguishes_missing_from_empty() {
        let dir = std::env::temp_dir().join(format!("fastfiles-pins-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let file = dir.join("pins.txt");
        assert!(load(&file).is_none());
        assert!(update(&file, |p| add(p, "C:\\X", None)).unwrap());
        assert!(update(&file, |p| remove(p, "C:\\X")).unwrap());
        assert_eq!(load(&file), Some(Vec::new()));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
