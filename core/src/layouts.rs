//! Per-folder layout choices that the shell's own view memory can't hold (Files keeps them in its layout database):
//! the column view, whether the adaptive layout is on or overridden for a folder, and (like Files'
//! LayoutPreferencesItem) the folder's own sort/group choice. One "path\t<mode><sort>" line per folder in
//! layouts.txt, newest last. Plus Files' adaptive layout guess (AdaptiveLayoutHelpers.TryPredictLayout).
//!
//! `<sort>` is zero or more "|s:...", "|g:...", "|p:..." parts appended after the mode text (which may then be
//! empty, when only the sort/group choice is stored): "|s:<sortkey>[.d]|g:<groupkey>[.<unit>][.d]|p:<priority>".
//! Old lines without '|' keep parsing exactly as before.

use crate::store;
use std::io;
use std::path::Path;

/// Kept small: one read per navigation. The oldest choices are dropped first.
const MAX_ENTRIES: usize = 2000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Files' ColumnView for this folder
    Columns,
    /// Adaptive layout turned on for this folder (also when the default layout isn't adaptive)
    Adaptive,
    /// The user picked a layout here (no adaptive layout): the shell view layout 0-7, or None when only the
    /// shell's own view memory knows which
    Manual(Option<u8>),
}

impl Mode {
    fn name(self) -> String {
        match self {
            Mode::Columns => "columns".into(),
            Mode::Adaptive => "adaptive".into(),
            Mode::Manual(None) => "manual".into(),
            Mode::Manual(Some(l)) => format!("manual:{l}"),
        }
    }

    fn parse(s: &str) -> Option<Mode> {
        match s.trim() {
            "columns" => Some(Mode::Columns),
            "adaptive" => Some(Mode::Adaptive),
            "manual" => Some(Mode::Manual(None)),
            other => other.strip_prefix("manual:").and_then(|n| n.parse::<u8>().ok()).filter(|l| *l < 8).map(|l| Mode::Manual(Some(l))),
        }
    }

    /// C ABI code: low byte 1 columns, 2 adaptive, 3 manual; for manual, bits 8-15 hold layout + 1 (0: unknown)
    pub fn code(self) -> u32 {
        match self {
            Mode::Columns => 1,
            Mode::Adaptive => 2,
            Mode::Manual(l) => 3 | (l.map_or(0, |l| u32::from(l) + 1) << 8),
        }
    }

    pub fn from_code(code: u32) -> Option<Mode> {
        match code & 0xFF {
            1 => Some(Mode::Columns),
            2 => Some(Mode::Adaptive),
            3 => {
                let l = (code >> 8) & 0xFF;
                Some(Mode::Manual(if (1..=8).contains(&l) { Some((l - 1) as u8) } else { None }))
            }
            _ => None,
        }
    }
}

// --- Per-folder sort/group choice (Files' LayoutPreferencesItem) ---

/// DirectorySortOption, trimmed to what a folder (outside the recycle bin/cloud drives/search) can sort by
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SortKey {
    Name,
    Modified,
    Type,
    Size,
    Tag,
    Created,
}

const SORT_KEYS: [(&str, SortKey); 6] =
    [("name", SortKey::Name), ("modified", SortKey::Modified), ("type", SortKey::Type), ("size", SortKey::Size), ("tag", SortKey::Tag), ("created", SortKey::Created)];

impl SortKey {
    fn index(self) -> u32 {
        SORT_KEYS.iter().position(|(_, k)| *k == self).unwrap() as u32
    }

    fn from_index(i: u32) -> Option<SortKey> {
        SORT_KEYS.get(i as usize).map(|(_, k)| *k)
    }

    fn name(self) -> &'static str {
        SORT_KEYS[self.index() as usize].0
    }

    fn parse(s: &str) -> Option<SortKey> {
        SORT_KEYS.iter().find(|(n, _)| *n == s).map(|(_, k)| *k)
    }
}

/// DirectoryGroupOption, trimmed the same way (plus None, meaning "not grouped")
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GroupKey {
    None,
    Name,
    Modified,
    Created,
    Size,
    Type,
    Tag,
}

const GROUP_KEYS: [(&str, GroupKey); 7] = [
    ("none", GroupKey::None),
    ("name", GroupKey::Name),
    ("modified", GroupKey::Modified),
    ("created", GroupKey::Created),
    ("size", GroupKey::Size),
    ("type", GroupKey::Type),
    ("tag", GroupKey::Tag),
];

impl GroupKey {
    fn index(self) -> u32 {
        GROUP_KEYS.iter().position(|(_, k)| *k == self).unwrap() as u32
    }

    fn from_index(i: u32) -> Option<GroupKey> {
        GROUP_KEYS.get(i as usize).map(|(_, k)| *k)
    }

    fn name(self) -> &'static str {
        GROUP_KEYS[self.index() as usize].0
    }

    fn parse(s: &str) -> Option<GroupKey> {
        GROUP_KEYS.iter().find(|(n, _)| *n == s).map(|(_, k)| *k)
    }
}

/// GroupByDateUnit, for grouping by DateModified/DateCreated
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DateUnit {
    Year,
    Month,
    Day,
}

const DATE_UNITS: [(&str, DateUnit); 3] = [("year", DateUnit::Year), ("month", DateUnit::Month), ("day", DateUnit::Day)];

impl DateUnit {
    fn index(self) -> u32 {
        DATE_UNITS.iter().position(|(_, u)| *u == self).unwrap() as u32
    }

    fn from_index(i: u32) -> Option<DateUnit> {
        DATE_UNITS.get(i as usize).map(|(_, u)| *u)
    }

    fn name(self) -> &'static str {
        DATE_UNITS[self.index() as usize].0
    }

    fn parse(s: &str) -> Option<DateUnit> {
        DATE_UNITS.iter().find(|(n, _)| *n == s).map(|(_, u)| *u)
    }
}

/// A folder's own sort/group choice (Files' LayoutPreferencesItem, minus what only applies to the recycle bin,
/// cloud drives or search results). Stored and set independently from the layout `Mode`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Sort {
    pub sort: Option<(SortKey, bool)>,
    /// key, descending, and (for Modified/Created only) the date unit to group by
    pub group: Option<(GroupKey, bool, Option<DateUnit>)>,
    /// SortDirectoriesAlongsideFiles (0) / SortFilesFirst-ish choice; kept as an opaque 0-2 priority
    pub priority: Option<u8>,
}

impl Sort {
    fn is_empty(&self) -> bool {
        self.sort.is_none() && self.group.is_none() && self.priority.is_none()
    }

    /// Appended after the mode text, e.g. "|s:name.d|g:modified.month|p:1". Empty when there's nothing to store.
    fn suffix(&self) -> String {
        let mut out = String::new();
        if let Some((key, desc)) = self.sort {
            out.push_str("|s:");
            out.push_str(key.name());
            if desc {
                out.push_str(".d");
            }
        }
        if let Some((key, desc, unit)) = self.group {
            out.push_str("|g:");
            out.push_str(key.name());
            if let Some(unit) = unit {
                out.push('.');
                out.push_str(unit.name());
            }
            if desc {
                out.push_str(".d");
            }
        }
        if let Some(p) = self.priority {
            out.push_str("|p:");
            out.push_str(&p.to_string());
        }
        out
    }

    /// Parses the "|s:...|g:...|p:..." parts after the mode text. Unknown/malformed parts are dropped, never a panic.
    fn parse(s: &str) -> Sort {
        let mut out = Sort::default();
        for part in s.split('|').filter(|p| !p.is_empty()) {
            let Some((tag, rest)) = part.split_once(':') else { continue };
            match tag {
                "s" => {
                    let (name, desc) = match rest.strip_suffix(".d") {
                        Some(n) => (n, true),
                        None => (rest, false),
                    };
                    if let Some(key) = SortKey::parse(name) {
                        out.sort = Some((key, desc));
                    }
                }
                "g" => {
                    let (name, desc) = match rest.strip_suffix(".d") {
                        Some(n) => (n, true),
                        None => (rest, false),
                    };
                    let mut segs = name.split('.');
                    let Some(key) = segs.next().and_then(GroupKey::parse) else { continue };
                    let unit = segs.next().and_then(DateUnit::parse);
                    if segs.next().is_some() {
                        continue; // trailing garbage: ignore the whole part
                    }
                    out.group = Some((key, desc, unit));
                }
                "p" => {
                    if let Some(p) = rest.parse::<u8>().ok().filter(|p| *p < 3) {
                        out.priority = Some(p);
                    }
                }
                _ => {}
            }
        }
        out
    }

    /// C ABI code: 0 when empty, else bit 31 set, bits 0-3 sort key (0xF: none stored), bit 4 sort descending,
    /// bits 8-11 group key (0xF: none stored), bits 12-13 date unit, bit 14 group descending, bits 16-17 priority
    /// (3: none stored).
    pub fn code(&self) -> u32 {
        if self.is_empty() {
            return 0;
        }
        let mut code = 1u32 << 31;
        code |= match self.sort {
            Some((key, desc)) => key.index() & 0xF | u32::from(desc) << 4,
            None => 0xF,
        };
        code |= match self.group {
            Some((key, desc, unit)) => (key.index() & 0xF) << 8 | (unit.map_or(0, DateUnit::index) & 0x3) << 12 | u32::from(desc) << 14,
            None => 0xF << 8,
        };
        code |= match self.priority {
            Some(p) => (u32::from(p) & 0x3) << 16,
            None => 0x3 << 16,
        };
        code
    }

    pub fn from_code(code: u32) -> Sort {
        if code & (1 << 31) == 0 {
            return Sort::default();
        }
        let sort_idx = code & 0xF;
        let sort = (sort_idx != 0xF).then(|| SortKey::from_index(sort_idx)).flatten().map(|k| (k, code & (1 << 4) != 0));
        let group_idx = (code >> 8) & 0xF;
        let group = (group_idx != 0xF).then(|| GroupKey::from_index(group_idx)).flatten().map(|k| {
            let unit = if matches!(k, GroupKey::Modified | GroupKey::Created) { DateUnit::from_index((code >> 12) & 0x3) } else { None };
            (k, code & (1 << 14) != 0, unit)
        });
        let priority_bits = (code >> 16) & 0x3;
        let priority = (priority_bits != 0x3).then_some(priority_bits as u8);
        Sort { sort, group, priority }
    }
}

/// Trailing backslashes off (drive roots keep theirs), so "C:\a\" and "c:\A" are one folder
fn normalize(path: &str) -> Option<String> {
    let mut s = path.trim().trim_start_matches('\u{feff}').to_string();
    if s.is_empty() || s.contains('\t') || s.contains('\n') {
        return None;
    }
    let drive_root = |s: &str| s.len() == 3 && s.as_bytes()[1] == b':';
    while s.len() > 1 && (s.ends_with('\\') || s.ends_with('/')) && !drive_root(&s) {
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

/// One folder's stored choices: the layout `Mode` (when known) and/or its sort/group `Sort` (when known). An entry
/// is only ever kept around while at least one of the two is set.
pub type Entry = (String, Option<Mode>, Sort);

/// Splits "<mode><sort-suffix>" into the mode text (may be empty) and the sort suffix text (starting at the first
/// '|', may be empty). Old lines (no '|' at all) come back as (whole string, "").
fn split_mode_and_sort(s: &str) -> (&str, &str) {
    s.find('|').map_or((s, ""), |i| s.split_at(i))
}

pub fn parse(text: &str) -> Vec<Entry> {
    let mut out: Vec<Entry> = Vec::new();
    for line in text.lines() {
        let Some((path, rest)) = line.rsplit_once('\t') else { continue };
        let Some(path) = normalize(path) else { continue };
        let (mode_text, sort_text) = split_mode_and_sort(rest);
        let mode = Mode::parse(mode_text);
        let sort = Sort::parse(sort_text);
        if mode.is_none() && sort.is_empty() {
            continue; // neither half is recognizable: drop the line, same as a fully bogus one before
        }
        // A later line wins (it was written more recently)
        out.retain(|(p, _, _)| !same(p, &path));
        out.push((path, mode, sort));
    }
    out
}

pub fn serialize(entries: &[Entry]) -> String {
    entries.iter().map(|(p, m, s)| format!("{p}\t{}{}\n", m.map_or(String::new(), Mode::name), s.suffix())).collect()
}

#[test]
fn codes_round_trip() {
    for m in [Mode::Columns, Mode::Adaptive, Mode::Manual(None), Mode::Manual(Some(0)), Mode::Manual(Some(7))] {
        assert_eq!(Mode::from_code(m.code()), Some(m));
        assert_eq!(Mode::parse(&m.name()), Some(m));
    }
    assert_eq!(Mode::parse("manual:8"), None);
    assert_eq!(Mode::from_code(0), None);
}

pub fn get(entries: &[Entry], path: &str) -> Option<Mode> {
    let path = normalize(path)?;
    entries.iter().rev().find(|(p, _, _)| same(p, &path)).and_then(|(_, m, _)| *m)
}

pub fn get_sort(entries: &[Entry], path: &str) -> Sort {
    let Some(path) = normalize(path) else { return Sort::default() };
    entries.iter().rev().find(|(p, _, _)| same(p, &path)).map_or(Sort::default(), |(_, _, s)| *s)
}

/// Moves (or creates) the folder's entry to the end, after `change` updates its mode/sort in place. Drops the
/// entry when `change` leaves both halves empty. False when nothing about the entry actually changed.
fn touch(entries: &mut Vec<Entry>, path: &str, change: impl FnOnce(&mut Option<Mode>, &mut Sort)) -> bool {
    let Some(path) = normalize(path) else { return false };
    let old = entries.iter().position(|(p, _, _)| same(p, &path));
    let (mut mode, mut sort) = old.map_or((None, Sort::default()), |i| (entries[i].1, entries[i].2));
    change(&mut mode, &mut sort);
    if let Some(i) = old {
        if entries[i].1 == mode && entries[i].2 == sort && i + 1 == entries.len() {
            return false; // already there, already last, nothing to move
        }
        entries.remove(i);
    } else if mode.is_none() && sort.is_empty() {
        return false; // nothing stored before, nothing to store now
    }
    if mode.is_some() || !sort.is_empty() {
        entries.push((path, mode, sort));
        if entries.len() > MAX_ENTRIES {
            let extra = entries.len() - MAX_ENTRIES;
            entries.drain(..extra);
        }
    }
    true
}

/// Sets (or with None, forgets) the folder's mode; the sort/group choice, if any, is kept. False when nothing changed.
pub fn set(entries: &mut Vec<Entry>, path: &str, mode: Option<Mode>) -> bool {
    touch(entries, path, |m, _| *m = mode)
}

/// Sets (or with the default `Sort`, forgets) the folder's sort/group choice; the mode, if any, is kept.
pub fn set_sort(entries: &mut Vec<Entry>, path: &str, sort: Sort) -> bool {
    touch(entries, path, |_, s| *s = sort)
}

pub fn load(file: &Path) -> Vec<Entry> {
    store::read(file).map(|bytes| parse(&String::from_utf8_lossy(&bytes))).unwrap_or_default()
}

pub fn update(file: &Path, path: &str, mode: Option<Mode>) -> io::Result<bool> {
    let mut entries = load(file);
    if !set(&mut entries, path, mode) {
        return Ok(false);
    }
    store::write_atomic(file, serialize(&entries).as_bytes())?;
    Ok(true)
}

pub fn update_sort(file: &Path, path: &str, sort: Sort) -> io::Result<bool> {
    let mut entries = load(file);
    if !set_sort(&mut entries, path, sort) {
        return Ok(false);
    }
    store::write_atomic(file, serialize(&entries).as_bytes())?;
    Ok(true)
}

// --- Adaptive layout (Files' AdaptiveLayoutHelpers) ---

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Prediction {
    /// Nothing to go on (empty or unreadable folder): the folder keeps the layout it has
    Undecided = 0,
    Details = 1,
    Grid = 2,
}

// FileExtensionHelpers.IsImageFile / IsAudioFile / IsVideoFile
const MEDIA: &[&str] = &[
    "png", "bmp", "jpg", "jpeg", "jfif", "gif", "tiff", "tif", "webp", "jxr", // images
    "mp3", "m4a", "ogg", "oga", "wav", "wma", "aac", "adt", "adts", "cda", "flac", // audio
    "avi", "mp4", "webm", "mov", "qt", "m4v", "mp4v", "3g2", "3gp2", "3gp", "3gpp", "mkv", // video
];

pub fn is_media(name: &str) -> bool {
    // Path.GetExtension: the text after the last dot of the last component
    let name = name.rsplit(['\\', '/']).next().unwrap_or(name);
    match name.rfind('.') {
        Some(dot) if dot + 1 < name.len() => {
            let ext = &name[dot + 1..];
            MEDIA.iter().any(|m| m.eq_ignore_ascii_case(ext))
        }
        _ => false,
    }
}

/// desktop.ini [ViewState] Mode=Pictures/Videos -> grid; any other ViewState -> details; no section -> None
fn desktop_ini_layout(bytes: &[u8]) -> Option<Prediction> {
    let text = if bytes.len() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE {
        let units: Vec<u16> = bytes[2..].chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
        String::from_utf16_lossy(&units)
    } else {
        String::from_utf8_lossy(bytes).into_owned()
    };
    let mut in_view_state = false;
    let mut found = false;
    let mut mode = String::new();
    for line in text.lines() {
        let line = line.trim().trim_start_matches('\u{feff}');
        if line.starts_with('[') {
            in_view_state = line.trim_end_matches(']').trim_start_matches('[').trim().eq_ignore_ascii_case("ViewState");
            found |= in_view_state;
            continue;
        }
        if in_view_state {
            if let Some((key, value)) = line.split_once('=') {
                if key.trim().eq_ignore_ascii_case("Mode") {
                    mode = value.trim().to_string();
                }
            }
        }
    }
    if !found {
        return None;
    }
    Some(if mode.eq_ignore_ascii_case("Pictures") || mode.eq_ignore_ascii_case("Videos") { Prediction::Grid } else { Prediction::Details })
}

/// GetContentLayout: grid when more than 60% of the names are pictures, music or videos
pub fn classify<'a>(names: impl IntoIterator<Item = &'a str>) -> Prediction {
    let (mut total, mut media) = (0usize, 0usize);
    for name in names {
        total += 1;
        if is_media(name) {
            media += 1;
        }
    }
    if total == 0 {
        return Prediction::Undecided;
    }
    if 100.0 * media as f32 / total as f32 > 60.0 { Prediction::Grid } else { Prediction::Details }
}

/// TryPredictLayout: desktop.ini first, else the first 200 entries. The caller skips network folders (Files does,
/// so navigation never waits on the network).
pub fn predict(dir: &Path) -> Prediction {
    if let Ok(bytes) = std::fs::read(dir.join("desktop.ini")) {
        if let Some(p) = desktop_ini_layout(&bytes) {
            return p;
        }
    }
    let Ok(entries) = std::fs::read_dir(dir) else { return Prediction::Undecided };
    let names: Vec<String> = entries.filter_map(Result::ok).take(200).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    classify(names.iter().map(String::as_str))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_set_and_get() {
        let mut e = parse("C:\\a\\\tcolumns\nbogus\nD:\tmanual\nc:\\A\tadaptive\n\\\\srv\\share\\x\tnope\n");
        assert_eq!(e.len(), 2);
        assert_eq!(get(&e, "C:\\a"), Some(Mode::Adaptive)); // the later line won
        assert_eq!(get(&e, "d:\\"), Some(Mode::Manual(None)));
        assert_eq!(get(&e, "C:\\b"), None);
        assert!(set(&mut e, "C:\\b\\", Some(Mode::Columns)));
        assert!(!set(&mut e, "c:\\B", Some(Mode::Columns)));
        assert!(set(&mut e, "C:\\a", None));
        assert!(!set(&mut e, "C:\\a", None));
        assert_eq!(parse(&serialize(&e)), e);
        assert_eq!(serialize(&e), "D:\\\tmanual\nC:\\b\tcolumns\n");
    }

    #[test]
    fn oldest_entries_are_dropped() {
        let mut e = Vec::new();
        for i in 0..MAX_ENTRIES + 5 {
            set(&mut e, &format!("C:\\f{i}"), Some(Mode::Manual(None)));
        }
        assert_eq!(e.len(), MAX_ENTRIES);
        assert_eq!(get(&e, "C:\\f0"), None);
        assert_eq!(get(&e, &format!("C:\\f{}", MAX_ENTRIES + 4)), Some(Mode::Manual(None)));
    }

    #[test]
    fn media_names_and_threshold() {
        assert!(is_media("a.JPG") && is_media("C:\\x\\clip.mkv") && is_media("song.flac"));
        assert!(!is_media("a.txt") && !is_media("noext") && !is_media("trailing."));
        assert_eq!(classify(["a.jpg", "b.png", "c.txt"]), Prediction::Grid); // 66%
        assert_eq!(classify(["a.jpg", "b.txt", "c.txt", "d.png", "e.gif"]), Prediction::Details); // exactly 60%
        assert_eq!(classify(std::iter::empty::<&str>()), Prediction::Undecided);
    }

    #[test]
    fn desktop_ini_view_state() {
        assert_eq!(desktop_ini_layout(b"[ViewState]\r\nMode=Pictures\r\n"), Some(Prediction::Grid));
        assert_eq!(desktop_ini_layout(b"[.ShellClassInfo]\r\nIconResource=x\r\n[ViewState]\r\nMode=\r\nVid=\r\n"), Some(Prediction::Details));
        assert_eq!(desktop_ini_layout(b"[.ShellClassInfo]\r\nLocalizedResourceName=@x\r\n"), None);
        let mut utf16 = vec![0xFF, 0xFE];
        for u in "[ViewState]\r\nMode=Videos\r\n".encode_utf16() {
            utf16.extend_from_slice(&u.to_le_bytes());
        }
        assert_eq!(desktop_ini_layout(&utf16), Some(Prediction::Grid));
    }

    #[test]
    fn predict_samples_folder() {
        let dir = std::env::temp_dir().join(format!("fastfiles-layouts-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(predict(&dir), Prediction::Undecided);
        for n in ["1.jpg", "2.jpg", "3.png", "x.txt"] {
            std::fs::write(dir.join(n), b"").unwrap();
        }
        assert_eq!(predict(&dir), Prediction::Grid);
        std::fs::write(dir.join("desktop.ini"), b"[ViewState]\r\nMode=Documents\r\n").unwrap();
        assert_eq!(predict(&dir), Prediction::Details);
        let file = dir.join("layouts.txt");
        assert!(update(&file, "C:\\Photos", Some(Mode::Columns)).unwrap());
        assert_eq!(get(&load(&file), "c:\\photos\\"), Some(Mode::Columns));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn sort_code_round_trip() {
        let samples = [
            Sort::default(),
            Sort { sort: Some((SortKey::Name, false)), group: None, priority: None },
            Sort { sort: Some((SortKey::Size, true)), group: Some((GroupKey::None, false, None)), priority: None },
            Sort { sort: Some((SortKey::Created, true)), group: Some((GroupKey::Modified, true, Some(DateUnit::Month))), priority: Some(0) },
            Sort { sort: None, group: Some((GroupKey::Tag, false, None)), priority: Some(2) },
            Sort { sort: Some((SortKey::Tag, false)), group: Some((GroupKey::Created, false, Some(DateUnit::Day))), priority: Some(1) },
        ];
        for s in samples {
            assert_eq!(Sort::from_code(s.code()), s, "{s:?}");
        }
        assert_eq!(Sort::from_code(0), Sort::default());
    }

    #[test]
    fn sort_suffix_round_trip() {
        for s in [
            Sort::default(),
            Sort { sort: Some((SortKey::Name, true)), group: None, priority: None },
            Sort { sort: Some((SortKey::Modified, false)), group: Some((GroupKey::Created, true, Some(DateUnit::Year))), priority: Some(1) },
            Sort { sort: None, group: None, priority: Some(0) },
        ] {
            assert_eq!(Sort::parse(&s.suffix()), s, "{s:?}");
        }
    }

    #[test]
    fn old_format_lines_still_parse() {
        let e = parse("C:\\a\tcolumns\nD:\\\tmanual:3\n");
        assert_eq!(e.len(), 2);
        assert_eq!(get(&e, "C:\\a"), Some(Mode::Columns));
        assert_eq!(get_sort(&e, "C:\\a"), Sort::default());
        assert_eq!(get(&e, "D:\\"), Some(Mode::Manual(Some(3))));
        assert_eq!(get_sort(&e, "D:\\"), Sort::default());
        // and a mode-only entry still serializes exactly as before (no trailing '|')
        assert_eq!(serialize(&e), "C:\\a\tcolumns\nD:\\\tmanual:3\n");
    }

    #[test]
    fn mode_and_sort_are_independent() {
        let mut e: Vec<Entry> = Vec::new();
        assert!(set(&mut e, "C:\\a", Some(Mode::Columns)));
        let sort = Sort { sort: Some((SortKey::Size, true)), group: Some((GroupKey::Name, false, None)), priority: Some(1) };
        assert!(set_sort(&mut e, "C:\\a", sort));
        assert_eq!(get(&e, "C:\\a"), Some(Mode::Columns));
        assert_eq!(get_sort(&e, "c:\\A"), sort);

        // forgetting the mode keeps the sort
        assert!(set(&mut e, "C:\\a", None));
        assert_eq!(get(&e, "C:\\a"), None);
        assert_eq!(get_sort(&e, "C:\\a"), sort);

        // re-adding the mode doesn't disturb the sort
        assert!(set(&mut e, "C:\\a", Some(Mode::Adaptive)));
        assert_eq!(get_sort(&e, "C:\\a"), sort);

        // forgetting the sort keeps the mode
        assert!(set_sort(&mut e, "C:\\a", Sort::default()));
        assert_eq!(get(&e, "C:\\a"), Some(Mode::Adaptive));
        assert_eq!(get_sort(&e, "C:\\a"), Sort::default());

        // forgetting both removes the line entirely
        assert!(set(&mut e, "C:\\a", None));
        assert_eq!(e.len(), 0);
        assert_eq!(serialize(&e), "");
    }

    #[test]
    fn forgetting_both_halves_at_once_drops_the_line() {
        let mut e: Vec<Entry> = Vec::new();
        let sort = Sort { sort: Some((SortKey::Name, false)), group: None, priority: None };
        assert!(set_sort(&mut e, "C:\\only-sort", sort));
        assert_eq!(e.len(), 1);
        assert!(set_sort(&mut e, "C:\\only-sort", Sort::default()));
        assert_eq!(e.len(), 0);

        // setting an empty sort on a folder that has neither is a no-op
        assert!(!set_sort(&mut e, "C:\\nothing", Sort::default()));
        assert_eq!(e.len(), 0);
    }

    #[test]
    fn malformed_sort_suffixes_are_ignored() {
        for bad in ["|s:bogus", "|s:name.x", "|g:bogus", "|g:name.month.extra", "|p:9", "|p:nope", "|q:x", "|s:", "|"] {
            assert_eq!(Sort::parse(bad), Sort::default(), "{bad:?} should be ignored");
        }
        // a malformed part doesn't poison the valid parts around it
        let s = Sort::parse("|s:bogus|g:name.d|p:9");
        assert_eq!(s, Sort { sort: None, group: Some((GroupKey::Name, true, None)), priority: None });

        // and a line with a malformed suffix after a valid mode keeps the mode
        let e = parse("C:\\a\tcolumns|s:bogus\n");
        assert_eq!(e.len(), 1);
        assert_eq!(get(&e, "C:\\a"), Some(Mode::Columns));
        assert_eq!(get_sort(&e, "C:\\a"), Sort::default());
    }

    #[test]
    fn sort_only_entry_uses_empty_mode_text() {
        let mut e: Vec<Entry> = Vec::new();
        let sort = Sort { sort: Some((SortKey::Type, false)), group: None, priority: None };
        assert!(set_sort(&mut e, "C:\\a", sort));
        let text = serialize(&e);
        assert_eq!(text, "C:\\a\t|s:type\n");
        let reparsed = parse(&text);
        assert_eq!(get(&reparsed, "C:\\a"), None);
        assert_eq!(get_sort(&reparsed, "C:\\a"), sort);
    }

    #[test]
    fn sort_cap_respected() {
        let mut e: Vec<Entry> = Vec::new();
        for i in 0..MAX_ENTRIES + 5 {
            set_sort(&mut e, &format!("C:\\s{i}"), Sort { sort: Some((SortKey::Name, false)), group: None, priority: None });
        }
        assert_eq!(e.len(), MAX_ENTRIES);
        assert_eq!(get_sort(&e, "C:\\s0"), Sort::default());
        assert_eq!(get_sort(&e, &format!("C:\\s{}", MAX_ENTRIES + 4)).sort, Some((SortKey::Name, false)));
    }
}
