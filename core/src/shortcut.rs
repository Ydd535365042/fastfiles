//! Keyboard shortcuts as text ("Ctrl+Shift+S") and as a packed code: modifiers << 16 | Windows virtual-key code.

pub const MOD_CTRL: u32 = 1;
pub const MOD_SHIFT: u32 = 2;
pub const MOD_ALT: u32 = 4;

/// Display name and virtual-key code of every key a shortcut may end with.
const NAMED: &[(&str, u32)] = &[
    ("Enter", 0x0D), ("Esc", 0x1B), ("Space", 0x20), ("Tab", 0x09), ("Backspace", 0x08),
    ("Delete", 0x2E), ("Insert", 0x2D), ("Home", 0x24), ("End", 0x23), ("PageUp", 0x21), ("PageDown", 0x22),
    ("Left", 0x25), ("Up", 0x26), ("Right", 0x27), ("Down", 0x28),
    ("Plus", 0xBB), ("Minus", 0xBD), ("`", 0xC0), (",", 0xBC), (".", 0xBE), ("/", 0xBF), (";", 0xBA),
    ("'", 0xDE), ("[", 0xDB), ("]", 0xDD), ("\\", 0xDC),
    ("NumAdd", 0x6B), ("NumSubtract", 0x6D), ("NumMultiply", 0x6A), ("NumDivide", 0x6F),
];

/// Accepted on input only (formatting always uses the names above)
const ALIASES: &[(&str, u32)] = &[("Del", 0x2E), ("Escape", 0x1B), ("Return", 0x0D), ("PgUp", 0x21), ("PgDn", 0x22), ("=", 0xBB), ("-", 0xBD)];

fn key_code(name: &str) -> Option<u32> {
    let upper = name.to_ascii_uppercase();
    let b = upper.as_bytes();
    if b.len() == 1 && (b[0].is_ascii_uppercase() || b[0].is_ascii_digit()) {
        return Some(u32::from(b[0])); // VK_A..VK_Z and VK_0..VK_9 equal their ASCII codes
    }
    if let Some(n) = upper.strip_prefix('F').and_then(|n| n.parse::<u32>().ok()) {
        if (1..=24).contains(&n) {
            return Some(0x70 + n - 1);
        }
    }
    if let Some(n) = upper.strip_prefix("NUM").and_then(|n| n.parse::<u32>().ok()) {
        if n <= 9 {
            return Some(0x60 + n);
        }
    }
    NAMED.iter().chain(ALIASES).find(|(n, _)| n.eq_ignore_ascii_case(name)).map(|&(_, vk)| vk)
}

fn key_name(vk: u32) -> Option<String> {
    match vk {
        0x41..=0x5A | 0x30..=0x39 => Some(char::from_u32(vk)?.to_string()),
        0x70..=0x87 => Some(format!("F{}", vk - 0x70 + 1)),
        0x60..=0x69 => Some(format!("Num{}", vk - 0x60)),
        _ => NAMED.iter().find(|&&(_, code)| code == vk).map(|(n, _)| n.to_string()),
    }
}

/// "ctrl + shift + s" -> code. Modifiers in any order, exactly one key, last. 0 when it isn't a valid shortcut.
pub fn parse(text: &str) -> u32 {
    let parts: Vec<&str> = text.split('+').map(str::trim).collect();
    // A trailing "+" means the key itself was typed as "+" ("Ctrl++")
    let (mods_part, key) = match parts.as_slice() {
        [rest @ .., "", ""] if !rest.is_empty() => (rest, "Plus"),
        [rest @ .., key] if !rest.is_empty() || !key.is_empty() => (rest, *key),
        _ => return 0,
    };
    let mut mods = 0;
    for m in mods_part {
        let bit = match m.to_ascii_lowercase().as_str() {
            "ctrl" | "control" => MOD_CTRL,
            "shift" => MOD_SHIFT,
            "alt" => MOD_ALT,
            _ => return 0,
        };
        if mods & bit != 0 {
            return 0;
        }
        mods |= bit;
    }
    match key_code(key) {
        Some(vk) => mods << 16 | vk,
        None => 0,
    }
}

/// Code -> "Ctrl+Shift+S" (empty when the code doesn't name a known key)
pub fn format(code: u32) -> String {
    let (mods, vk) = (code >> 16, code & 0xFFFF);
    let Some(key) = key_name(vk) else { return String::new() };
    let mut out = String::new();
    for (bit, name) in [(MOD_CTRL, "Ctrl+"), (MOD_SHIFT, "Shift+"), (MOD_ALT, "Alt+")] {
        if mods & bit != 0 {
            out.push_str(name);
        }
    }
    out.push_str(&key);
    out
}

/// Whether a user may bind this: it needs Ctrl or Alt, unless it is a function key.
/// Plain or Shift-only letters, arrows, Enter, Delete... belong to typing and list navigation.
pub fn bindable(code: u32) -> bool {
    let (mods, vk) = (code >> 16, code & 0xFFFF);
    if key_name(vk).is_none() || mods & !(MOD_CTRL | MOD_SHIFT | MOD_ALT) != 0 {
        return false;
    }
    (0x70..=0x87).contains(&vk) || mods & (MOD_CTRL | MOD_ALT) != 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_and_format_roundtrip() {
        for text in ["Ctrl+Shift+S", "Alt+Left", "F11", "Ctrl+`", "Ctrl+Shift+Tab", "Ctrl+Plus", "Ctrl+Alt+Enter", "Ctrl+F4", "Ctrl+,", "Num7"] {
            let code = parse(text);
            assert_ne!(code, 0, "{text}");
            assert_eq!(format(code), text);
        }
    }

    #[test]
    fn parse_is_lenient_about_case_order_and_aliases() {
        assert_eq!(format(parse("shift + ctrl + s")), "Ctrl+Shift+S");
        assert_eq!(format(parse("ctrl+del")), "Ctrl+Delete");
        assert_eq!(format(parse("Ctrl++")), "Ctrl+Plus");
        assert_eq!(format(parse("ctrl+=")), "Ctrl+Plus");
        assert_eq!(parse("Ctrl+S"), MOD_CTRL << 16 | 0x53);
    }

    #[test]
    fn rejects_garbage() {
        for text in ["", "Ctrl", "Ctrl+", "Ctrl+Ctrl+S", "Hyper+S", "Ctrl+SS", "F25", "Ctrl+Shift"] {
            assert_eq!(parse(text), 0, "{text}");
        }
    }

    #[test]
    fn bindable_needs_ctrl_alt_or_function_key() {
        assert!(bindable(parse("Ctrl+S")));
        assert!(bindable(parse("Alt+Shift+H")));
        assert!(bindable(parse("F2")));
        assert!(bindable(parse("Shift+F10")));
        assert!(!bindable(parse("S")));
        assert!(!bindable(parse("Shift+S")));
        assert!(!bindable(parse("Delete")));
        assert!(!bindable(0));
    }
}
