//! User settings (settings.txt): `key=value` lines. Every known key has a type and a default, so whatever is on disk
//! (hand edits, an imported file, an older version) comes back complete and valid. Shortcut overrides are `key.<command>`.

use crate::shortcut;
use crate::store;
use std::io;
use std::path::Path;

enum Kind {
    Bool,
    Choice(&'static [&'static str]),
    Text,
    /// Comma-separated identifiers (toolbar buttons hidden by the user)
    Names,
    /// "#AARRGGBB" (or "#RRGGBB", stored opaque), upper case
    Color,
}

/// Known keys in file order: (key, kind, default)
const SCHEMA: &[(&str, Kind, &str)] = &[
    ("startup", Kind::Choice(&["restore", "home", "path"]), "restore"),
    ("startup.path", Kind::Text, ""),
    ("newtab", Kind::Choice(&["home", "current"]), "home"),
    ("theme", Kind::Choice(&["system", "light", "dark"]), "system"),
    // Files' app language: empty = the Windows display language
    ("language", Kind::Text, ""),
    // Files' AppThemeBackgroundColor (#00000000 = default) and AppThemeFontFamily (empty = the standard font)
    ("theme.background", Kind::Color, "#00000000"),
    ("theme.font", Kind::Text, ""),
    // Files' DateTimeFormat (Application / System / Universal), SizeUnitFormat (BinaryUnits / DecimalUnits),
    // SelectFilesOnHover (off)
    ("format.date", Kind::Choice(&["application", "system", "universal"]), "application"),
    ("format.size", Kind::Choice(&["binary", "decimal"]), "binary"),
    ("folder.hoverselect", Kind::Bool, "0"),
    // Files' AppThemeBackdropMaterial (BackdropMaterialType; Files' default is MicaAlt). No acrylic: it has to show the
    // windows behind, which FastFiles can't do (dropped with the user's agreement)
    ("theme.backdrop", Kind::Choice(&["solid", "mica", "micaalt"]), "micaalt"),
    // Files' AppThemeBackgroundImageSource / Fit (UniformToFill) / Opacity (1, steps of .1 from .1) / alignments
    // (Center)
    ("theme.image", Kind::Text, ""),
    ("theme.image.fit", Kind::Choice(&["none", "fill", "uniform", "uniformtofill"]), "uniformtofill"),
    ("theme.image.opacity", Kind::Choice(&["10", "20", "30", "40", "50", "60", "70", "80", "90", "100"]), "100"),
    ("theme.image.vertical", Kind::Choice(&["top", "center", "bottom"]), "center"),
    ("theme.image.horizontal", Kind::Choice(&["left", "center", "right"]), "center"),
    ("statusbar", Kind::Bool, "1"),
    ("toolbar.hidden", Kind::Names, ""),
    // Files' CustomToolbarItems, one line per context: "key:il,key:i,separator"; empty = defaults, "none" = empty
    ("toolbar.always", Kind::Text, ""),
    ("toolbar.archive", Kind::Text, ""),
    ("toolbar.batch", Kind::Text, ""),
    ("toolbar.powershell", Kind::Text, ""),
    ("toolbar.image", Kind::Text, ""),
    ("toolbar.media", Kind::Text, ""),
    ("toolbar.font", Kind::Text, ""),
    ("toolbar.driver", Kind::Text, ""),
    ("toolbar.certificate", Kind::Text, ""),
    ("toolbar.recyclebin", Kind::Text, ""),
    ("toolbar.other", Kind::Text, ""),
    // Files' DefaultLayoutMode: Adaptive
    ("layout.default", Kind::Choice(&["details", "list", "tiles", "content", "small", "medium", "large", "xlarge", "columns", "adaptive"]), "adaptive"),
    ("layout.sync", Kind::Bool, "0"),
    // Home widgets the user turned off: quick, drives, network, recent
    ("home.hidden", Kind::Names, ""),
    // Files' ShowFileTagsWidget: off unless turned on
    ("home.tags", Kind::Bool, "0"),
    // Size of the selected folder in the status bar and details pane (Rust walk in the background)
    // Search box: FastFiles' own file-name search (Rust) or Windows Search (index, file contents)
    ("search.engine", Kind::Choice(&["fast", "windows"]), "fast"),
    ("folder.sizes", Kind::Bool, "1"),
    // Launching FastFiles again opens a tab in the running window instead of a second window
    ("instance.single", Kind::Bool, "1"),
    // Closing the window leaves FastFiles in the notification area
    ("background", Kind::Bool, "0"),
    // Files: ReverseTabScrollDirection (off), ShowTabActions (on)
    ("tabs.reversewheel", Kind::Bool, "0"),
    ("tabs.actions", Kind::Bool, "1"),
    // Files' FoldersSettingsService ShowHiddenItems / ShowProtectedSystemFiles: FastFiles' own, not the system's
    ("folder.hidden", Kind::Bool, "0"),
    ("folder.protected", Kind::Bool, "0"),
    // Vertical lines between the columns of the details view
    ("view.columnlines", Kind::Bool, "0"),
    // Files' DeleteConfirmationPolicy (Always), StatusCenterVisibility (Always), ShowStatusCenterTeachingTip (once)
    ("delete.confirm", Kind::Choice(&["always", "permanent", "never"]), "always"),
    ("statuscenter", Kind::Choice(&["always", "operations"]), "always"),
    ("statuscenter.tip", Kind::Bool, "1"),
    // Files' GeneralSettingsService: AlwaysSwitchToNewlyOpenedTab (off), AlwaysOpenDualPaneInNewTab (off),
    // ShellPaneArrangementOption (vertical = side by side)
    ("tabs.switchnew", Kind::Bool, "0"),
    ("panes.dualnew", Kind::Bool, "0"),
    ("panes.arrangement", Kind::Choice(&["vertical", "horizontal"]), "vertical"),
    // Files' context menu options (Show*: all on) the user turned off: newtab, newwindow, newpane, copypath,
    // folderwithselection, shortcut, pin, compress, sendto, terminal, tags, pinstart
    ("menu.hidden", Kind::Names, ""),
    // Files' FoldersSettingsService: ShowDotFiles (on), OpenFoldersInNewTab (off), DoubleClickToGoUp (on),
    // ScrollToPreviousFolderWhenNavigatingUp (on)
    ("folder.dotfiles", Kind::Bool, "1"),
    ("folder.newtab", Kind::Bool, "0"),
    ("folder.dblclickup", Kind::Bool, "1"),
    ("folder.selectprevious", Kind::Bool, "1"),
    // Files' 排序方式 / 降序排序 (DefaultSortOption Name, DefaultDirectorySortDirection Ascending): folders without their
    // own choice (and every folder when layout.sync is on)
    ("layout.sort", Kind::Choice(&["name", "modified", "created", "size", "type", "tag"]), "name"),
    ("layout.sort.desc", Kind::Bool, "0"),
    // Files' 排序优先级 (DefaultSortDirectoriesAlongsideFiles / DefaultSortFilesFirst: folders first)
    ("layout.sortpriority", Kind::Choice(&["folders", "files", "together"]), "folders"),
    // Files' 分组依据 / 降序分组 / 按日期分组时的单位 (DefaultGroupOption None, DefaultDirectoryGroupDirection Ascending,
    // DefaultGroupByDateUnit Year)
    ("layout.group", Kind::Choice(&["none", "name", "modified", "created", "size", "type", "tag"]), "none"),
    ("layout.group.desc", Kind::Bool, "0"),
    ("layout.group.unit", Kind::Choice(&["year", "month", "day"]), "year"),
    // Files' DetailsViewSize / ListViewSize / CardsViewSize / GridViewSize / ColumnsViewSize (the layout flyout's 大小;
    // Small, Small, Small, Large, Small)
    ("layout.size.details", Kind::Choice(&["1", "2", "3", "4", "5"]), "2"),
    ("layout.size.list", Kind::Choice(&["1", "2", "3", "4", "5"]), "2"),
    ("layout.size.cards", Kind::Choice(&["1", "2", "3", "4"]), "1"),
    ("layout.size.grid", Kind::Choice(&["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"]), "8"),
    ("layout.size.columns", Kind::Choice(&["1", "2", "3", "4", "5"]), "2"),
    // Files' ShowFileTagColumn / ShowDateColumn / ShowDateCreatedColumn / ShowTypeColumn / ShowSizeColumn: the collapsed
    // columns of FastFiles' own details list (Files: only the date created column is off)
    ("details.hidden", Kind::Names, "created"),
    // Files' ShowSystemTrayIcon (on)
    ("tray", Kind::Bool, "1"),
    // Files' PreviousSearchQueriesList: the search box's last 10 searches, newest first, tab-separated
    ("search.history", Kind::Text, ""),
];

const SHORTCUT_PREFIX: &str = "key.";
const MAX_CHORDS: usize = 3;

fn valid_name(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-' || b == b'.')
}

/// Normalized value, or None when it isn't valid for the kind
fn check(kind: &Kind, value: &str) -> Option<String> {
    let value = value.trim();
    match kind {
        Kind::Bool => match value.to_ascii_lowercase().as_str() {
            "1" | "true" | "yes" | "on" => Some("1".into()),
            "0" | "false" | "no" | "off" => Some("0".into()),
            _ => None,
        },
        Kind::Choice(options) => options.iter().find(|o| o.eq_ignore_ascii_case(value)).map(|o| o.to_string()),
        Kind::Text => (!value.contains(['\r', '\n']) && value.chars().count() <= 1024).then(|| value.to_string()),
        Kind::Names => {
            let mut names: Vec<&str> = Vec::new();
            for n in value.split(',').map(str::trim).filter(|n| !n.is_empty()) {
                if !valid_name(n) {
                    return None;
                }
                if !names.contains(&n) {
                    names.push(n);
                }
            }
            Some(names.join(","))
        }
        Kind::Color => {
            let hex = value.strip_prefix('#')?;
            if !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
                return None;
            }
            match hex.len() {
                8 => Some(format!("#{}", hex.to_ascii_uppercase())),
                6 => Some(format!("#FF{}", hex.to_ascii_uppercase())),
                _ => None,
            }
        }
    }
}

/// "Ctrl+Shift+s, F9" -> "Ctrl+Shift+S,F9"; empty stays empty (= unbound). None if any chord is invalid.
fn check_shortcuts(value: &str) -> Option<String> {
    let mut chords: Vec<String> = Vec::new();
    for part in value.split(',').map(str::trim).filter(|p| !p.is_empty()) {
        let code = shortcut::parse(part);
        if code == 0 || !shortcut::bindable(code) {
            return None;
        }
        let text = shortcut::format(code);
        if !chords.contains(&text) {
            chords.push(text);
        }
    }
    (chords.len() <= MAX_CHORDS).then(|| chords.join(","))
}

/// Parsed settings: every schema key (default when missing/invalid), then shortcut overrides sorted by command.
pub struct Settings {
    pub values: Vec<(String, String)>,
    /// How many lines of the input were recognized and valid
    pub recognized: usize,
}

pub fn parse(text: &str) -> Settings {
    let mut values: Vec<(String, String)> = SCHEMA.iter().map(|(k, _, d)| (k.to_string(), d.to_string())).collect();
    let mut shortcuts: Vec<(String, String)> = Vec::new();
    let mut recognized = 0;
    for line in text.lines() {
        let line = line.trim().trim_start_matches('\u{feff}');
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else { continue };
        let key = key.trim();
        if let Some(command) = key.strip_prefix(SHORTCUT_PREFIX) {
            if let (true, Some(v)) = (valid_name(command), check_shortcuts(value)) {
                recognized += 1;
                match shortcuts.iter_mut().find(|(c, _)| c == command) {
                    Some(slot) => slot.1 = v,
                    None => shortcuts.push((command.to_string(), v)),
                }
            }
            continue;
        }
        if let Some(i) = SCHEMA.iter().position(|(k, _, _)| *k == key) {
            if let Some(v) = check(&SCHEMA[i].1, value) {
                recognized += 1;
                values[i].1 = v; // last one wins, like editing the file by hand
            }
        }
    }
    shortcuts.sort_by(|a, b| a.0.cmp(&b.0));
    values.extend(shortcuts.into_iter().map(|(c, v)| (format!("{SHORTCUT_PREFIX}{c}"), v)));
    Settings { values, recognized }
}

pub fn serialize(settings: &Settings) -> String {
    let mut out = String::from("# FastFiles settings. Unknown or invalid lines are dropped on the next save.\n");
    for (k, v) in &settings.values {
        out.push_str(k);
        out.push('=');
        out.push_str(v);
        out.push('\n');
    }
    out
}

pub fn load(path: &Path) -> Settings {
    let text = store::read(path).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    parse(&text)
}

/// Normalizes `text` and writes it atomically
pub fn save(path: &Path, text: &str) -> io::Result<()> {
    store::write_atomic(path, serialize(&parse(text)).as_bytes())
}

/// Reads `from`; if it contains at least one valid setting, it replaces the settings at `to`. Returns the count.
pub fn import(from: &Path, to: &Path) -> io::Result<usize> {
    let bytes = store::read(from)?;
    let settings = parse(&String::from_utf8_lossy(&bytes));
    if settings.recognized == 0 {
        return Ok(0);
    }
    store::write_atomic(to, serialize(&settings).as_bytes())?;
    Ok(settings.recognized)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn get<'a>(s: &'a Settings, key: &str) -> Option<&'a str> {
        s.values.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }

    #[test]
    fn empty_input_gives_all_defaults() {
        let s = parse("");
        assert_eq!(s.recognized, 0);
        assert_eq!(s.values.len(), SCHEMA.len());
        assert_eq!(get(&s, "theme"), Some("system"));
        assert_eq!(get(&s, "statusbar"), Some("1"));
    }

    #[test]
    fn invalid_values_fall_back_and_valid_ones_normalize() {
        let s = parse("\u{feff}theme=DARK\nstartup=nowhere\nstatusbar=off\nbogus=1\nlayout.default=Tiles\ntoolbar.hidden= share , cut,share ,\n");
        assert_eq!(get(&s, "theme"), Some("dark"));
        assert_eq!(get(&s, "startup"), Some("restore"));
        assert_eq!(get(&s, "statusbar"), Some("0"));
        assert_eq!(get(&s, "layout.default"), Some("tiles"));
        assert_eq!(get(&s, "toolbar.hidden"), Some("share,cut"));
        assert_eq!(get(&s, "bogus"), None);
        assert_eq!(s.recognized, 4);
    }

    #[test]
    fn colors_normalize() {
        let s = parse("theme.background=#32ffb900\n");
        assert_eq!(get(&s, "theme.background"), Some("#32FFB900"));
        let s = parse("theme.background=#0078d7\n");
        assert_eq!(get(&s, "theme.background"), Some("#FF0078D7"));
        for bad in ["red", "#12345", "#GG000000", "32FFB900"] {
            let s = parse(&format!("theme.background={bad}\n"));
            assert_eq!(get(&s, "theme.background"), Some("#00000000"), "{bad}");
        }
    }

    #[test]
    fn shortcut_overrides_are_validated_sorted_and_deduplicated() {
        let s = parse("key.dualpane=ctrl+shift+d, CTRL+SHIFT+D\nkey.back=\nkey.bad=Q\nkey.four=F1,F2,F3,F4\nkey.three=F1,F2,F3\nkey.a.b=Alt+1\n");
        assert_eq!(get(&s, "key.dualpane"), Some("Ctrl+Shift+D"));
        assert_eq!(get(&s, "key.back"), Some("")); // explicitly unbound
        assert_eq!(get(&s, "key.bad"), None);
        assert_eq!(get(&s, "key.four"), None);
        assert_eq!(get(&s, "key.three"), Some("F1,F2,F3"));
        let keys: Vec<&str> = s.values.iter().map(|(k, _)| k.as_str()).filter(|k| k.starts_with("key.")).collect();
        assert_eq!(keys, ["key.a.b", "key.back", "key.dualpane", "key.three"]);
    }

    #[test]
    fn serialize_then_parse_is_stable() {
        let s = parse("theme=light\nkey.x=Ctrl+K\nstartup.path=D:\\工作\n");
        let text = serialize(&s);
        assert_eq!(serialize(&parse(&text)), text);
        assert_eq!(get(&parse(&text), "startup.path"), Some("D:\\工作"));
    }

    #[test]
    fn import_needs_something_recognizable() {
        let dir = std::env::temp_dir().join(format!("fastfiles-settings-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let (src, dst) = (dir.join("in.txt"), dir.join("settings.txt"));
        store::write_atomic(&dst, b"theme=dark\n").unwrap();
        std::fs::write(&src, "hello\nworld\n").unwrap();
        assert_eq!(import(&src, &dst).unwrap(), 0);
        assert_eq!(get(&load(&dst), "theme"), Some("dark"));
        std::fs::write(&src, "theme=light\nnewtab=current\n").unwrap();
        assert_eq!(import(&src, &dst).unwrap(), 2);
        assert_eq!(get(&load(&dst), "theme"), Some("light"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
