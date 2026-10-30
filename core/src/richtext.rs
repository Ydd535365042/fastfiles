//! Rich previews for the details pane, as RTF for a RichEdit control: Files' CodePreview (ColorCode languages and
//! colors) and MarkdownPreview (MarkdownTextBlock). Text is already decoded and cut to Files' 10 000 characters.

/// Files' CodePreviewViewModel extension table (1..15), then the source and config files Files leaves to Windows'
/// TXT previewer (black on white, whatever the theme) or plain text: highlighted the same way here. 0 = not code
pub fn language_for(ext: &str) -> u8 {
    let e = ext.trim_start_matches('.').to_ascii_lowercase();
    let table: &[(u8, &str)] = &[
        (1, "aspx"), (2, "acsx"), (3, "cpp,c++,cc,cp,cxx,h,h++,hh,hpp,hxx,inc,inl,ino,ipp,re,tcc,tpp"), (4, "cs,cake,csx,linq"),
        (5, "css,scss"), (6, "fs,fsi,fsx"), (7, "hs"), (8, "razor,cshtml,vbhtml,svelte"), (9, "java"), (10, "js,jsx"), (11, "php"),
        (12, "pwsh,ps1,psd1,psm1"), (13, "ts,tsx"), (14, "vb,vbs"), (15, "xml,axml,xaml,xsd,xsl,xslt,xlf"),
        // Beyond Files' table
        (3, "c,cu,m,mm"), (5, "less,sass"), (8, "vue"), (9, "gradle,groovy,scala"), (10, "mjs,cjs"), (13, "mts,cts"),
        (15, "csproj,vcxproj,props,targets,config,resx,manifest,plist,svg,nuspec,wxs,filters,ruleset"),
        (16, "py,pyw,pyi"), (17, "sql"), (18, "bat,cmd"), (19, "sh,bash,zsh,ksh"), (20, "rs"), (21, "go"), (22, "lua"),
        (23, "rb,rake,gemspec"), (24, "kt,kts,swift,dart"), (25, "pl,pm"), (26, "json,jsonc,json5,geojson,webmanifest"),
        (27, "ini,inf,cfg,conf,toml,yaml,yml,properties,env,editorconfig,gitattributes,gitconfig,reg"),
    ];
    table.iter().find(|(_, list)| list.split(',').any(|x| x == e)).map_or(0, |(id, _)| *id)
}

// Color table indexes (\cfN): 1 text, 2 comment, 3 keyword, 4 string, 5 number, 6 preprocessor, 7 tag, 8 attribute,
// 9 muted, 10 link, 11 code background. ColorCode's VS palettes (light / dark), as Files' CSS renderer also uses.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Tok {
    Text,
    Comment,
    Keyword,
    Str,
    Number,
    Preproc,
    Tag,
    Attr,
}

fn color_index(t: Tok) -> u8 {
    match t {
        Tok::Text => 1,
        Tok::Comment => 2,
        Tok::Keyword => 3,
        Tok::Str => 4,
        Tok::Number => 5,
        Tok::Preproc => 6,
        Tok::Tag => 7,
        Tok::Attr => 8,
    }
}

pub struct Palette {
    pub ink: u32,    // 0x00BBGGRR (COLORREF)
    pub muted: u32,
    pub accent: u32,
    pub dark: bool,
}

fn rgb(c: u32) -> (u32, u32, u32) {
    (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF)
}

fn header(p: &Palette, code_font: &str) -> String {
    let fixed: [(u32, u32, u32); 8] = if p.dark {
        [(0x6A, 0x99, 0x55), (0x56, 0x9C, 0xD6), (0xCE, 0x91, 0x78), (0xB5, 0xCE, 0xA8), (0x9B, 0x9B, 0x9B), (0x56, 0x9C, 0xD6),
            (0x9C, 0xDC, 0xFE), (0, 0, 0)]
    } else {
        [(0x00, 0x80, 0x00), (0x00, 0x00, 0xFF), (0xA3, 0x15, 0x15), (0x00, 0x00, 0x00), (0x80, 0x80, 0x80), (0xA3, 0x15, 0x15),
            (0xFF, 0x00, 0x00), (0, 0, 0)]
    };
    let mut s = String::from("{\\rtf1\\ansi\\ansicpg65001\\deff0{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern ");
    s.push_str(code_font);
    s.push_str(";}}{\\colortbl;");
    let (r, g, b) = rgb(p.ink);
    s.push_str(&format!("\\red{r}\\green{g}\\blue{b};"));
    for (i, (r, g, b)) in fixed.iter().take(7).enumerate() {
        // Numbers have no color of their own in ColorCode's light theme
        let (r, g, b) = if i == 3 && !p.dark { rgb(p.ink) } else { (*r, *g, *b) };
        s.push_str(&format!("\\red{r}\\green{g}\\blue{b};"));
    }
    let (r, g, b) = rgb(p.muted);
    s.push_str(&format!("\\red{r}\\green{g}\\blue{b};"));
    let (r, g, b) = rgb(p.accent);
    s.push_str(&format!("\\red{r}\\green{g}\\blue{b};"));
    let code_bg = if p.dark { (0x2D, 0x2D, 0x2D) } else { (0xF2, 0xF2, 0xF2) };
    s.push_str(&format!("\\red{}\\green{}\\blue{};}}", code_bg.0, code_bg.1, code_bg.2));
    s.push_str("\\uc0\\cf1 ");
    s
}

/// RTF-escapes text (non-ASCII as \uN, UTF-16 units, signed)
fn esc(out: &mut String, text: &str) {
    for c in text.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '{' => out.push_str("\\{"),
            '}' => out.push_str("\\}"),
            '\t' => out.push_str("\\tab "),
            '\r' => {}
            '\n' => out.push_str("\\line "),
            c if (c as u32) < 0x80 => out.push(c),
            c => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    out.push_str(&format!("\\u{} ", *u as i16));
                }
            }
        }
    }
}

/// Lines as paragraphs (RichEdit turns \line into U+000B)
fn esc_lines(out: &mut String, text: &str) {
    for (n, line) in text.split('\n').enumerate() {
        if n > 0 {
            out.push_str("\\par\n");
        }
        esc(out, line);
    }
}

// ------------------------------------------------------------------------------------------------------------ code

struct Lang {
    line: &'static [&'static str],
    block: &'static [(&'static str, &'static str)],
    quotes: &'static str,
    keywords: &'static [&'static str],
    ignore_case: bool,
    preproc: bool,  // '#' at the start of a line
    sigil: bool,    // $variables (PowerShell, PHP, shells, Perl)
    escapes: bool,  // backslash escapes in strings
    triple: bool,   // """ / ''' strings spanning lines (Python)
}

const C_KW: &[&str] = &["auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
    "float", "for", "goto", "if", "inline", "int", "long", "register", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "bool", "class", "namespace", "new", "delete", "public",
    "private", "protected", "template", "typename", "this", "true", "false", "nullptr", "virtual", "override", "using", "try", "catch",
    "throw", "operator", "friend", "constexpr", "noexcept", "explicit", "mutable", "static_cast", "reinterpret_cast", "const_cast",
    "dynamic_cast", "decltype", "final", "wchar_t", "char16_t", "char32_t", "concept", "requires", "co_await", "co_return", "co_yield"];
const CS_KW: &[&str] = &["abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const",
    "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern", "false", "finally",
    "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock", "long",
    "namespace", "new", "null", "object", "operator", "out", "override", "params", "private", "protected", "public", "readonly", "ref",
    "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try",
    "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while", "var", "async", "await",
    "get", "set", "value", "yield", "partial", "where", "record", "init", "dynamic", "nameof", "when", "required"];
const JAVA_KW: &[&str] = &["abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class", "const", "continue",
    "default", "do", "double", "else", "enum", "extends", "final", "finally", "float", "for", "goto", "if", "implements", "import",
    "instanceof", "int", "interface", "long", "native", "new", "package", "private", "protected", "public", "return", "short", "static",
    "strictfp", "super", "switch", "synchronized", "this", "throw", "throws", "transient", "try", "void", "volatile", "while", "true",
    "false", "null", "var", "record"];
const JS_KW: &[&str] = &["break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else",
    "export", "extends", "finally", "for", "function", "if", "import", "in", "instanceof", "let", "new", "return", "super", "switch",
    "this", "throw", "try", "typeof", "var", "void", "while", "with", "yield", "async", "await", "of", "true", "false", "null",
    "undefined", "static", "get", "set", "from", "as"];
const TS_KW: &[&str] = &["break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else",
    "export", "extends", "finally", "for", "function", "if", "import", "in", "instanceof", "let", "new", "return", "super", "switch",
    "this", "throw", "try", "typeof", "var", "void", "while", "with", "yield", "async", "await", "of", "true", "false", "null",
    "undefined", "static", "get", "set", "from", "as", "interface", "type", "enum", "implements", "public", "private", "protected",
    "readonly", "declare", "namespace", "module", "abstract", "keyof", "any", "number", "string", "boolean", "never", "unknown"];
const PHP_KW: &[&str] = &["abstract", "and", "array", "as", "break", "case", "catch", "class", "clone", "const", "continue", "declare",
    "default", "do", "echo", "else", "elseif", "empty", "endif", "endforeach", "endwhile", "extends", "final", "for", "foreach",
    "function", "global", "if", "implements", "include", "include_once", "instanceof", "interface", "isset", "list", "namespace", "new",
    "or", "print", "private", "protected", "public", "require", "require_once", "return", "static", "switch", "throw", "trait", "try",
    "unset", "use", "var", "while", "true", "false", "null", "fn", "match"];
const PS_KW: &[&str] = &["begin", "break", "catch", "class", "continue", "data", "do", "dynamicparam", "else", "elseif", "end", "exit",
    "filter", "finally", "for", "foreach", "from", "function", "if", "in", "param", "process", "return", "switch", "throw", "trap",
    "try", "until", "using", "while", "workflow", "-eq", "-ne", "-gt", "-ge", "-lt", "-le", "-like", "-notlike", "-match", "-notmatch",
    "-and", "-or", "-not", "-contains", "-in"];
const VB_KW: &[&str] = &["addhandler", "and", "andalso", "as", "boolean", "byref", "byte", "byval", "call", "case", "catch", "class",
    "const", "dim", "do", "double", "each", "else", "elseif", "end", "enum", "error", "event", "exit", "false", "finally", "for",
    "friend", "function", "get", "handles", "if", "implements", "imports", "in", "inherits", "integer", "interface", "is", "let", "long",
    "loop", "me", "mod", "module", "mustinherit", "mybase", "namespace", "new", "next", "not", "nothing", "object", "of", "on", "option",
    "optional", "or", "orelse", "overridable", "overrides", "private", "property", "protected", "public", "readonly", "redim", "resume",
    "return", "select", "set", "shared", "short", "single", "static", "step", "string", "structure", "sub", "then", "throw", "to",
    "true", "try", "typeof", "until", "wend", "while", "with", "withevents"];
const FS_KW: &[&str] = &["abstract", "and", "as", "assert", "base", "begin", "class", "default", "delegate", "do", "done", "downcast",
    "downto", "elif", "else", "end", "exception", "extern", "false", "finally", "for", "fun", "function", "global", "if", "in",
    "inherit", "inline", "interface", "internal", "lazy", "let", "match", "member", "module", "mutable", "namespace", "new", "not",
    "null", "of", "open", "or", "override", "private", "public", "rec", "return", "static", "struct", "then", "to", "true", "try", "type",
    "upcast", "use", "val", "void", "when", "while", "with", "yield"];
const HS_KW: &[&str] = &["case", "class", "data", "default", "deriving", "do", "else", "if", "import", "in", "infix", "infixl",
    "infixr", "instance", "let", "module", "newtype", "of", "then", "type", "where", "qualified", "as", "hiding", "forall"];

const PY_KW: &[&str] = &["False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass",
    "raise", "return", "try", "while", "with", "yield", "match", "case", "self"];
const SQL_KW: &[&str] = &["select", "from", "where", "and", "or", "not", "insert", "into", "values", "update", "set", "delete", "create",
    "table", "view", "index", "drop", "alter", "add", "column", "primary", "key", "foreign", "references", "join", "inner", "left",
    "right", "outer", "full", "cross", "on", "as", "group", "by", "order", "having", "limit", "offset", "union", "all", "distinct",
    "case", "when", "then", "else", "end", "null", "is", "in", "like", "between", "exists", "begin", "commit", "rollback", "declare",
    "procedure", "function", "returns", "return", "if", "int", "integer", "varchar", "nvarchar", "char", "text", "date", "datetime",
    "bit", "decimal", "float", "default", "unique", "check", "constraint", "with", "top", "asc", "desc", "go", "exec", "trigger"];
const BAT_KW: &[&str] = &["echo", "set", "if", "else", "for", "in", "do", "goto", "call", "exit", "not", "exist", "defined", "errorlevel",
    "setlocal", "endlocal", "shift", "pause", "cd", "pushd", "popd", "start", "equ", "neq", "lss", "leq", "gtr", "geq", "off", "on"];
const SH_KW: &[&str] = &["if", "then", "else", "elif", "fi", "case", "esac", "for", "while", "until", "do", "done", "in", "function",
    "select", "return", "exit", "break", "continue", "local", "export", "readonly", "declare", "source", "echo", "set", "unset", "shift",
    "trap", "eval", "exec", "true", "false"];
const RS_KW: &[&str] = &["as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "false", "fn",
    "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct",
    "super", "trait", "true", "type", "unsafe", "use", "where", "while", "bool", "char", "str", "u8", "u16", "u32", "u64", "u128",
    "usize", "i8", "i16", "i32", "i64", "i128", "isize", "f32", "f64", "String", "Vec", "Option", "Result", "Some", "None", "Ok", "Err"];
const GO_KW: &[&str] = &["break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "for", "func", "go",
    "goto", "if", "import", "interface", "map", "package", "range", "return", "select", "struct", "switch", "type", "var", "true",
    "false", "nil", "iota", "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
    "float32", "float64", "string", "byte", "rune", "bool", "error", "any"];
const LUA_KW: &[&str] = &["and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil",
    "not", "or", "repeat", "return", "then", "true", "until", "while", "self"];
const RB_KW: &[&str] = &["BEGIN", "END", "alias", "and", "begin", "break", "case", "class", "def", "defined?", "do", "else", "elsif",
    "end", "ensure", "false", "for", "if", "in", "module", "next", "nil", "not", "or", "redo", "rescue", "retry", "return", "self",
    "super", "then", "true", "undef", "unless", "until", "when", "while", "yield", "require", "attr_accessor", "attr_reader", "puts"];
// Kotlin, Swift and Dart share one list (their C-like syntax colors the same way)
const KT_KW: &[&str] = &["abstract", "as", "async", "await", "break", "case", "catch", "class", "const", "continue", "data", "default",
    "defer", "do", "else", "enum", "extension", "extends", "false", "final", "finally", "for", "fun", "func", "guard", "if", "import",
    "in", "init", "interface", "internal", "is", "let", "late", "mixin", "nil", "null", "object", "open", "override", "package",
    "private", "protected", "protocol", "public", "required", "return", "sealed", "self", "static", "struct", "super", "switch",
    "this", "throw", "throws", "true", "try", "typealias", "val", "var", "void", "when", "where", "while", "with"];
const PL_KW: &[&str] = &["my", "our", "local", "sub", "if", "elsif", "else", "unless", "while", "until", "for", "foreach", "do", "last",
    "next", "redo", "return", "use", "no", "package", "require", "print", "eq", "ne", "lt", "gt", "le", "ge", "and", "or", "not"];

fn lang(id: u8) -> Option<Lang> {
    let c_like = |keywords| Lang { line: &["//"], block: &[("/*", "*/")], quotes: "\"'", keywords, ignore_case: false, preproc: false,
        sigil: false, escapes: true, triple: false };
    Some(match id {
        3 => Lang { preproc: true, ..c_like(C_KW) },
        4 => Lang { preproc: true, ..c_like(CS_KW) },
        9 => c_like(JAVA_KW),
        10 => Lang { quotes: "\"'`", ..c_like(JS_KW) },
        13 => Lang { quotes: "\"'`", ..c_like(TS_KW) },
        11 => Lang { line: &["//", "#"], sigil: true, ..c_like(PHP_KW) },
        12 => Lang { line: &["#"], block: &[("<#", "#>")], ignore_case: true, sigil: true, escapes: false, ..c_like(PS_KW) },
        14 => Lang { line: &["'", "REM "], block: &[], quotes: "\"", ignore_case: true, escapes: false, ..c_like(VB_KW) },
        6 => Lang { block: &[("(*", "*)")], quotes: "\"", preproc: true, escapes: false, ..c_like(FS_KW) },
        7 => Lang { line: &["--"], block: &[("{-", "-}")], quotes: "\"", escapes: false, ..c_like(HS_KW) },
        16 => Lang { line: &["#"], block: &[], triple: true, ..c_like(PY_KW) },
        17 => Lang { line: &["--"], quotes: "'\"", ignore_case: true, escapes: false, ..c_like(SQL_KW) },
        18 => Lang { line: &["::", "REM ", "@REM "], block: &[], quotes: "\"", ignore_case: true, escapes: false, ..c_like(BAT_KW) },
        19 => Lang { line: &["#"], block: &[], sigil: true, ..c_like(SH_KW) },
        20 => Lang { quotes: "\"", preproc: true, ..c_like(RS_KW) },
        21 => Lang { quotes: "\"'`", ..c_like(GO_KW) },
        22 => Lang { line: &["--"], block: &[("--[[", "]]")], ..c_like(LUA_KW) },
        23 => Lang { line: &["#"], block: &[("=begin", "=end")], ..c_like(RB_KW) },
        24 => c_like(KT_KW),
        25 => Lang { line: &["#"], block: &[], sigil: true, ..c_like(PL_KW) },
        _ => return None,
    })
}

fn starts(chars: &[char], i: usize, s: &str, ignore_case: bool) -> bool {
    let mut j = i;
    for c in s.chars() {
        let Some(&x) = chars.get(j) else { return false };
        if x != c && !(ignore_case && x.to_ascii_lowercase() == c.to_ascii_lowercase()) {
            return false;
        }
        j += 1;
    }
    true
}

fn ident(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// Tokens for one of the C-like / script languages
fn lex_generic(text: &str, l: &Lang) -> Vec<(Tok, String)> {
    let chars: Vec<char> = text.chars().collect();
    let mut out: Vec<(Tok, String)> = Vec::new();
    let mut push = |t: Tok, s: String| match out.last_mut() {
        Some((lt, ls)) if *lt == t => ls.push_str(&s),
        _ => out.push((t, s)),
    };
    let mut i = 0;
    let mut line_start = true;
    while i < chars.len() {
        let c = chars[i];
        if l.preproc && line_start && c == '#' {
            let end = chars[i..].iter().position(|&x| x == '\n').map_or(chars.len(), |p| i + p);
            push(Tok::Preproc, chars[i..end].iter().collect());
            i = end;
            continue;
        }
        if c == '\n' {
            line_start = true;
            push(Tok::Text, "\n".into());
            i += 1;
            continue;
        }
        if !c.is_whitespace() {
            line_start = false;
        }
        if let Some(&(open, close)) = l.block.iter().find(|(o, _)| starts(&chars, i, o, false)) {
            let body_start = i + open.chars().count();
            let mut j = body_start;
            while j < chars.len() && !starts(&chars, j, close, false) {
                j += 1;
            }
            let end = (j + close.chars().count()).min(chars.len());
            push(Tok::Comment, chars[i..end].iter().collect());
            i = end;
            continue;
        }
        if l.line.iter().any(|m| starts(&chars, i, m, l.ignore_case)) && !(c == '#' && l.sigil && i > 0 && chars[i - 1] == '$') {
            let end = chars[i..].iter().position(|&x| x == '\n').map_or(chars.len(), |p| i + p);
            push(Tok::Comment, chars[i..end].iter().collect());
            i = end;
            continue;
        }
        if l.triple && (starts(&chars, i, "\"\"\"", false) || starts(&chars, i, "'''", false)) {
            // Python's """ / ''' strings (docstrings) run over lines
            let close: String = std::iter::repeat(c).take(3).collect();
            let end = (i + 3..chars.len()).find(|&j| starts(&chars, j, &close, false)).map_or(chars.len(), |j| j + 3);
            push(Tok::Str, chars[i..end].iter().collect());
            i = end;
            continue;
        }
        if l.quotes.contains(c) {
            // C# verbatim @"..." doubles its quotes; elsewhere backslash escapes
            let verbatim = i > 0 && chars[i - 1] == '@';
            let mut j = i + 1;
            while j < chars.len() {
                if chars[j] == c {
                    if verbatim && chars.get(j + 1) == Some(&c) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                if chars[j] == '\\' && !verbatim && l.escapes {
                    j += 2;
                    continue;
                }
                if chars[j] == '\n' && c != '`' && !verbatim {
                    break;
                }
                j += 1;
            }
            let end = (j + 1).min(chars.len());
            push(Tok::Str, chars[i..end].iter().collect());
            i = end;
            continue;
        }
        if c.is_ascii_digit() && (i == 0 || !ident(chars[i - 1])) {
            let mut j = i;
            while j < chars.len() && (chars[j].is_ascii_alphanumeric() || chars[j] == '.' || chars[j] == '_') {
                j += 1;
            }
            push(Tok::Number, chars[i..j].iter().collect());
            i = j;
            continue;
        }
        if l.sigil && c == '$' {
            let mut j = i + 1;
            while j < chars.len() && (ident(chars[j]) || chars[j] == ':') {
                j += 1;
            }
            push(Tok::Attr, chars[i..j].iter().collect());
            i = j;
            continue;
        }
        if ident(c) || (c == '-' && l.ignore_case && chars.get(i + 1).is_some_and(|x| x.is_alphabetic())) {
            let mut j = i + 1;
            while j < chars.len() && ident(chars[j]) {
                j += 1;
            }
            let word: String = chars[i..j].iter().collect();
            let key = if l.ignore_case { word.to_ascii_lowercase() } else { word.clone() };
            let t = if l.keywords.contains(&key.as_str()) { Tok::Keyword } else { Tok::Text };
            push(t, word);
            i = j;
            continue;
        }
        push(Tok::Text, c.to_string());
        i += 1;
    }
    out
}

/// XML / HTML / ASPX: comments, tags, attribute names and values
fn lex_markup(text: &str) -> Vec<(Tok, String)> {
    let chars: Vec<char> = text.chars().collect();
    let mut out: Vec<(Tok, String)> = Vec::new();
    let mut i = 0;
    let find = |from: usize, s: &str| (from..chars.len()).find(|&j| starts(&chars, j, s, false));
    while i < chars.len() {
        if starts(&chars, i, "<!--", false) {
            let end = find(i + 4, "-->").map_or(chars.len(), |j| j + 3);
            out.push((Tok::Comment, chars[i..end].iter().collect()));
            i = end;
        } else if starts(&chars, i, "<![CDATA[", false) {
            let end = find(i, "]]>").map_or(chars.len(), |j| j + 3);
            out.push((Tok::Text, chars[i..end].iter().collect()));
            i = end;
        } else if chars[i] == '<' && chars.get(i + 1).is_some_and(|c| c.is_alphabetic() || "/?!%@".contains(*c)) {
            // "<" name, then attributes until ">"
            let mut j = i + 1;
            while j < chars.len() && "/?!%@".contains(chars[j]) {
                j += 1;
            }
            out.push((Tok::Tag, chars[i..j].iter().collect()));
            let n = j;
            while j < chars.len() && (ident(chars[j]) || ":-.".contains(chars[j])) {
                j += 1;
            }
            out.push((Tok::Tag, chars[n..j].iter().collect()));
            while j < chars.len() && chars[j] != '>' {
                let c = chars[j];
                if c == '"' || c == '\'' {
                    let end = (j + 1..chars.len()).find(|&k| chars[k] == c).map_or(chars.len(), |k| k + 1);
                    out.push((Tok::Str, chars[j..end].iter().collect()));
                    j = end;
                } else if ident(c) {
                    let s = j;
                    while j < chars.len() && (ident(chars[j]) || ":-.".contains(chars[j])) {
                        j += 1;
                    }
                    out.push((Tok::Attr, chars[s..j].iter().collect()));
                } else {
                    out.push((Tok::Tag, c.to_string()));
                    j += 1;
                }
            }
            if j < chars.len() {
                out.push((Tok::Tag, ">".into()));
                j += 1;
            }
            i = j;
        } else {
            let end = (i + 1..chars.len()).find(|&j| chars[j] == '<').unwrap_or(chars.len());
            out.push((Tok::Text, chars[i..end].iter().collect()));
            i = end;
        }
    }
    out
}

/// Files' CssPreviewRenderer: comments, strings, @rules / selectors as keywords, property names
fn lex_css(text: &str) -> Vec<(Tok, String)> {
    let chars: Vec<char> = text.chars().collect();
    let mut out: Vec<(Tok, String)> = Vec::new();
    let mut i = 0;
    let mut depth = 0;
    while i < chars.len() {
        let c = chars[i];
        if starts(&chars, i, "/*", false) {
            let end = (i + 2..chars.len()).find(|&j| starts(&chars, j, "*/", false)).map_or(chars.len(), |j| j + 2);
            out.push((Tok::Comment, chars[i..end].iter().collect()));
            i = end;
        } else if c == '"' || c == '\'' {
            let end = (i + 1..chars.len()).find(|&j| chars[j] == c || chars[j] == '\n').map_or(chars.len(), |j| j + 1);
            out.push((Tok::Str, chars[i..end].iter().collect()));
            i = end;
        } else if c == '{' || c == '}' {
            depth = if c == '{' { depth + 1 } else { (depth - 1).max(0) };
            out.push((Tok::Text, c.to_string()));
            i += 1;
        } else if depth > 0 && (ident(c) || c == '-') {
            // Inside a block: "name:" is a property, the rest is a value
            let mut j = i;
            while j < chars.len() && (ident(chars[j]) || chars[j] == '-') {
                j += 1;
            }
            let mut k = j;
            while k < chars.len() && chars[k] == ' ' {
                k += 1;
            }
            let t = if chars.get(k) == Some(&':') { Tok::Attr } else { Tok::Text };
            out.push((t, chars[i..j].iter().collect()));
            i = j;
        } else if depth == 0 && !c.is_whitespace() {
            let end = (i..chars.len()).find(|&j| chars[j] == '{' || starts(&chars, j, "/*", false) || chars[j] == '\n').unwrap_or(chars.len());
            out.push((Tok::Keyword, chars[i..end].iter().collect()));
            i = end;
        } else {
            out.push((Tok::Text, c.to_string()));
            i += 1;
        }
    }
    out
}

/// JSON: keys (a string followed by ':') as attributes, string values, numbers, true / false / null
fn lex_json(text: &str) -> Vec<(Tok, String)> {
    let chars: Vec<char> = text.chars().collect();
    let mut out: Vec<(Tok, String)> = Vec::new();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c == '"' {
            let mut j = i + 1;
            while j < chars.len() && chars[j] != '"' && chars[j] != '\n' {
                j += if chars[j] == '\\' { 2 } else { 1 };
            }
            let end = (j + 1).min(chars.len());
            let mut k = end;
            while k < chars.len() && (chars[k] == ' ' || chars[k] == '\t') {
                k += 1;
            }
            let t = if chars.get(k) == Some(&':') { Tok::Attr } else { Tok::Str };
            out.push((t, chars[i..end].iter().collect()));
            i = end;
        } else if starts(&chars, i, "//", false) {
            // JSONC / JSON5 comments
            let end = (i..chars.len()).find(|&j| chars[j] == '\n').unwrap_or(chars.len());
            out.push((Tok::Comment, chars[i..end].iter().collect()));
            i = end;
        } else if starts(&chars, i, "/*", false) {
            let end = (i + 2..chars.len()).find(|&j| starts(&chars, j, "*/", false)).map_or(chars.len(), |j| j + 2);
            out.push((Tok::Comment, chars[i..end].iter().collect()));
            i = end;
        } else if c == '-' || c.is_ascii_digit() {
            let mut j = i + 1;
            while j < chars.len() && (chars[j].is_ascii_alphanumeric() || ".+-".contains(chars[j])) {
                j += 1;
            }
            out.push((Tok::Number, chars[i..j].iter().collect()));
            i = j;
        } else if c.is_alphabetic() {
            let mut j = i + 1;
            while j < chars.len() && ident(chars[j]) {
                j += 1;
            }
            let word: String = chars[i..j].iter().collect();
            let t = if ["true", "false", "null"].contains(&word.as_str()) { Tok::Keyword } else { Tok::Text };
            out.push((t, word));
            i = j;
        } else {
            out.push((Tok::Text, c.to_string()));
            i += 1;
        }
    }
    out
}

/// INI / TOML / YAML / .properties / .reg: comments, [sections], keys before '=' or ':', then the value
fn lex_keyvalue(text: &str) -> Vec<(Tok, String)> {
    let mut out: Vec<(Tok, String)> = Vec::new();
    for (n, line) in text.split('\n').enumerate() {
        if n > 0 {
            out.push((Tok::Text, "\n".into()));
        }
        let body = line.trim_start();
        let indent = &line[..line.len() - body.len()];
        if !indent.is_empty() {
            out.push((Tok::Text, indent.into()));
        }
        if body.starts_with('#') || body.starts_with(';') || body.starts_with("//") {
            out.push((Tok::Comment, body.into()));
            continue;
        }
        if body.starts_with('[') {
            out.push((Tok::Keyword, body.into()));
            continue;
        }
        if body == "---" || body == "..." {
            out.push((Tok::Preproc, body.into()));
            continue;
        }
        // YAML list items: "- " then the rest as a value or a key
        let (dash, rest) = match body.strip_prefix("- ") {
            Some(r) => ("- ", r),
            None => ("", body),
        };
        if !dash.is_empty() {
            out.push((Tok::Text, dash.into()));
        }
        let split = rest.char_indices().find(|&(_, c)| c == '=' || c == ':').map(|(k, _)| k)
            .filter(|&k| k > 0 && !rest[..k].contains('"') && !rest[..k].contains('\''));
        let (key, value) = match split {
            Some(k) => (&rest[..k], &rest[k..]),
            None => ("", rest),
        };
        if !key.is_empty() {
            out.push((Tok::Attr, key.into()));
        }
        // The value: its separator, then a trailing " #" comment apart
        let mut v = value;
        if let Some(sep) = v.chars().next().filter(|c| *c == '=' || *c == ':') {
            out.push((Tok::Text, sep.to_string()));
            v = &v[1..];
        }
        // (a '#' inside a quoted value is not a comment: the quotes before it must be balanced)
        let (val, comment) = match v.find(" #").or_else(|| v.find(" ;")) {
            Some(k) if v[..k].matches('"').count() % 2 == 0 && v[..k].matches('\'').count() % 2 == 0 => (&v[..k], &v[k..]),
            _ => (v, ""),
        };
        let t = val.trim();
        let kind = if t.is_empty() {
            Tok::Text
        } else if t.starts_with('"') || t.starts_with('\'') {
            Tok::Str
        } else if t.parse::<f64>().is_ok() || t.starts_with("0x") || t.starts_with("dword:") {
            Tok::Number
        } else if ["true", "false", "yes", "no", "on", "off", "null", "~"].contains(&t.to_ascii_lowercase().as_str()) {
            Tok::Keyword
        } else if key.is_empty() && dash.is_empty() {
            Tok::Text
        } else {
            Tok::Str
        };
        if !val.is_empty() {
            out.push((kind, val.into()));
        }
        if !comment.is_empty() {
            out.push((Tok::Comment, comment.into()));
        }
    }
    out
}

fn tokens(text: &str, language: u8) -> Vec<(Tok, String)> {
    match language {
        1 | 2 | 8 | 15 => lex_markup(text),
        5 => lex_css(text),
        26 => lex_json(text),
        27 => lex_keyvalue(text),
        id => match lang(id) {
            Some(l) => lex_generic(text, &l),
            None => vec![(Tok::Text, text.to_string())],
        },
    }
}

pub fn code_rtf(text: &str, language: u8, p: &Palette, code_font: &str) -> String {
    let mut s = header(p, code_font);
    s.push_str("\\f1\\fs20 ");
    for (t, part) in tokens(text, language) {
        s.push_str(&format!("\\cf{} ", color_index(t)));
        // A paragraph per line (\line would put U+000B soft breaks into copied text)
        for (n, piece) in part.split('\n').enumerate() {
            if n > 0 {
                s.push_str("\\par\n");
            }
            esc(&mut s, piece);
        }
    }
    s.push('}');
    s
}

// -------------------------------------------------------------------------------------------------------- markdown

fn link_field(out: &mut String, url: &str, label: &str) {
    out.push_str("{\\field{\\*\\fldinst{HYPERLINK \"");
    esc(out, &url.replace('"', "%22"));
    out.push_str("\"}}{\\fldrslt{\\cf10\\ul ");
    inline(out, label);
    out.push_str("\\ulnone\\cf1}}}");
}

/// Emphasis, code spans, links, images (alt text), strikethrough; raw HTML stays text (Files escapes it)
fn inline(out: &mut String, text: &str) {
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    let mut plain = String::new();
    let flush = |out: &mut String, plain: &mut String| {
        esc(out, plain);
        plain.clear();
    };
    while i < chars.len() {
        let c = chars[i];
        if c == '\\' && i + 1 < chars.len() && chars[i + 1].is_ascii_punctuation() {
            plain.push(chars[i + 1]);
            i += 2;
            continue;
        }
        if c == '`' {
            let ticks = chars[i..].iter().take_while(|&&x| x == '`').count();
            let fence: String = std::iter::repeat('`').take(ticks).collect();
            let rest: String = chars[i + ticks..].iter().collect();
            if let Some(end) = rest.find(&fence) {
                flush(out, &mut plain);
                out.push_str("{\\f1\\highlight11 ");
                esc(out, rest[..end].trim());
                out.push('}');
                i += ticks + rest[..end].chars().count() + ticks;
                continue;
            }
        }
        // [label](url) and ![alt](url)
        let image = c == '!' && chars.get(i + 1) == Some(&'[');
        if c == '[' || image {
            let open = if image { i + 1 } else { i };
            if let Some(close) = (open + 1..chars.len()).find(|&j| chars[j] == ']') {
                if chars.get(close + 1) == Some(&'(') {
                    if let Some(paren) = (close + 2..chars.len()).find(|&j| chars[j] == ')') {
                        let label: String = chars[open + 1..close].iter().collect();
                        let target: String = chars[close + 2..paren].iter().collect();
                        let url = target.split_whitespace().next().unwrap_or("").trim_matches(['<', '>']);
                        flush(out, &mut plain);
                        if image {
                            out.push_str("{\\cf9 ");
                            esc(out, &label);
                            out.push('}');
                        } else {
                            link_field(out, url, &label);
                        }
                        i = paren + 1;
                        continue;
                    }
                }
            }
        }
        // <https://...> autolinks
        if c == '<' {
            let rest: String = chars[i + 1..].iter().collect();
            if let Some(end) = rest.find('>') {
                let inner = &rest[..end];
                if inner.starts_with("http://") || inner.starts_with("https://") || inner.starts_with("mailto:") {
                    flush(out, &mut plain);
                    link_field(out, inner, inner);
                    i += 2 + inner.chars().count();
                    continue;
                }
            }
        }
        if c == '*' || c == '_' || c == '~' {
            let run = chars[i..].iter().take_while(|&&x| x == c).count().min(3);
            // "_" inside a word is not emphasis
            let intraword = c == '_' && i > 0 && ident(chars[i - 1]);
            if !(c == '~' && run < 2) && !intraword && chars.get(i + run).is_some_and(|x| !x.is_whitespace()) {
                let marker: String = std::iter::repeat(c).take(run).collect();
                let rest: String = chars[i + run..].iter().collect();
                if let Some(end) = rest.find(&marker).filter(|&e| e > 0) {
                    flush(out, &mut plain);
                    let (on, off) = match (c, run) {
                        ('~', _) => ("\\strike ", "\\strike0 "),
                        (_, 1) => ("\\i ", "\\i0 "),
                        (_, 2) => ("\\b ", "\\b0 "),
                        _ => ("\\b\\i ", "\\b0\\i0 "),
                    };
                    out.push('{');
                    out.push_str(on);
                    inline(out, &rest[..end]);
                    out.push_str(off);
                    out.push('}');
                    i += run + rest[..end].chars().count() + run;
                    continue;
                }
            }
        }
        plain.push(c);
        i += 1;
    }
    flush(out, &mut plain);
}

fn is_rule(t: &str) -> bool {
    let s: String = t.chars().filter(|c| !c.is_whitespace()).collect();
    s.len() >= 3 && (s.chars().all(|c| c == '-') || s.chars().all(|c| c == '*') || s.chars().all(|c| c == '_'))
}

/// "- x", "* x", "+ x", "1. x", "1) x": (indent, ordered number, rest)
fn list_item(line: &str) -> Option<(usize, Option<String>, &str)> {
    let indent = line.len() - line.trim_start().len();
    let t = line.trim_start();
    if let Some(rest) = t.strip_prefix("- ").or_else(|| t.strip_prefix("* ")).or_else(|| t.strip_prefix("+ ")) {
        return Some((indent, None, rest));
    }
    let digits = t.chars().take_while(|c| c.is_ascii_digit()).count();
    if digits > 0 && digits < 10 {
        let rest = &t[digits..];
        if let Some(r) = rest.strip_prefix(". ").or_else(|| rest.strip_prefix(") ")) {
            return Some((indent, Some(t[..digits].to_string()), r));
        }
    }
    None
}

fn table_cells(line: &str) -> Vec<String> {
    let t = line.trim().trim_start_matches('|').trim_end_matches('|');
    t.split('|').map(|c| c.trim().to_string()).collect()
}

fn is_table_rule(line: &str) -> bool {
    let t = line.trim();
    t.contains('-') && t.contains('|') && t.chars().all(|c| "|-: ".contains(c))
}

pub fn markdown_rtf(text: &str, p: &Palette, code_font: &str) -> String {
    let mut s = header(p, code_font);
    s.push_str("\\f0\\fs21\\sa120 ");
    let lines: Vec<&str> = text.lines().collect();
    let mut para: Vec<&str> = Vec::new();
    let flush_para = |s: &mut String, para: &mut Vec<&str>| {
        if para.is_empty() {
            return;
        }
        s.push_str("\\pard\\sa120 ");
        let mut joined = String::new();
        for (n, l) in para.iter().enumerate() {
            let hard = l.ends_with("  ") || l.ends_with('\\');
            joined.push_str(l.trim_end().trim_end_matches('\\'));
            if n + 1 < para.len() {
                joined.push_str(if hard { "\n" } else { " " });
            }
        }
        inline(s, &joined);
        s.push_str("\\par\n");
        para.clear();
    };
    let mut i = 0;
    while i < lines.len() {
        let line = lines[i];
        let t = line.trim_start();
        // Fenced code
        if t.starts_with("```") || t.starts_with("~~~") {
            flush_para(&mut s, &mut para);
            let fence = &t[..3];
            let mut code = String::new();
            i += 1;
            while i < lines.len() && !lines[i].trim_start().starts_with(fence) {
                code.push_str(lines[i]);
                code.push('\n');
                i += 1;
            }
            i += 1;
            s.push_str("\\pard\\li120\\ri120\\sa120{\\f1\\fs19\\highlight11 ");
            esc_lines(&mut s, code.trim_end_matches('\n'));
            s.push_str("}\\par\n");
            continue;
        }
        if t.is_empty() {
            flush_para(&mut s, &mut para);
            i += 1;
            continue;
        }
        // Setext heading: the paragraph so far underlined with === / ---
        if !para.is_empty() && !t.is_empty() && (t.chars().all(|c| c == '=') || (t.chars().all(|c| c == '-') && t.len() >= 2)) {
            let level = if t.starts_with('=') { 1 } else { 2 };
            let title = para.join(" ");
            para.clear();
            heading(&mut s, level, &title);
            i += 1;
            continue;
        }
        let hashes = t.chars().take_while(|&c| c == '#').count();
        if (1..=6).contains(&hashes) && (t.len() == hashes || t[hashes..].starts_with(' ')) {
            flush_para(&mut s, &mut para);
            heading(&mut s, hashes, t[hashes..].trim().trim_end_matches('#').trim());
            i += 1;
            continue;
        }
        if is_rule(t) && para.is_empty() {
            s.push_str("\\pard\\sa120{\\cf9 ");
            esc(&mut s, &"\u{2500}".repeat(24));
            s.push_str("}\\par\n");
            i += 1;
            continue;
        }
        if t.starts_with('>') {
            flush_para(&mut s, &mut para);
            let mut quote = String::new();
            while i < lines.len() && lines[i].trim_start().starts_with('>') {
                let q = lines[i].trim_start()[1..].trim_start();
                if !quote.is_empty() {
                    quote.push(' ');
                }
                quote.push_str(q);
                i += 1;
            }
            s.push_str("\\pard\\li360\\sa120{\\cf9 ");
            inline(&mut s, &quote);
            s.push_str("}\\par\n");
            continue;
        }
        if let Some((indent, number, rest)) = list_item(line) {
            flush_para(&mut s, &mut para);
            let level = (indent / 2).min(4) as u32;
            let left = 360 + level * 360;
            let task = rest.strip_prefix("[ ] ").map(|r| ("\u{2610} ", r)).or_else(|| {
                rest.strip_prefix("[x] ").or_else(|| rest.strip_prefix("[X] ")).map(|r| ("\u{2611} ", r))
            });
            let marker = match &number {
                Some(n) => format!("{n}."),
                None => if level % 2 == 0 { "\u{2022}".into() } else { "\u{25E6}".into() },
            };
            s.push_str(&format!("\\pard\\li{left}\\fi-240\\sa40 "));
            esc(&mut s, &marker);
            s.push_str("\\tab ");
            match task {
                Some((box_, r)) => {
                    esc(&mut s, box_);
                    inline(&mut s, r);
                }
                None => inline(&mut s, rest),
            }
            s.push_str("\\par\n");
            i += 1;
            continue;
        }
        // GFM table: header row, separator, body rows
        if t.contains('|') && i + 1 < lines.len() && is_table_rule(lines[i + 1]) {
            flush_para(&mut s, &mut para);
            let head = table_cells(t);
            let cols = head.len().max(1);
            let width = 9000 / cols as u32;
            let row = |s: &mut String, cells: &[String], bold: bool| {
                s.push_str("\\trowd\\trgaph80");
                for c in 0..cols {
                    s.push_str(&format!("\\clbrdrb\\brdrs\\brdrw10\\brdrcf9\\cellx{}", width * (c as u32 + 1)));
                }
                for c in 0..cols {
                    s.push_str("\\pard\\intbl ");
                    if bold {
                        s.push_str("{\\b ");
                    }
                    inline(s, cells.get(c).map_or("", |x| x.as_str()));
                    if bold {
                        s.push('}');
                    }
                    s.push_str("\\cell ");
                }
                s.push_str("\\row\n");
            };
            row(&mut s, &head, true);
            i += 2;
            while i < lines.len() && lines[i].contains('|') && !lines[i].trim().is_empty() {
                row(&mut s, &table_cells(lines[i]), false);
                i += 1;
            }
            s.push_str("\\pard\\sa120\\par\n");
            continue;
        }
        // Indented code (4 spaces) outside a paragraph
        if para.is_empty() && (line.starts_with("    ") || line.starts_with('\t')) {
            let mut code = String::new();
            while i < lines.len() && (lines[i].starts_with("    ") || lines[i].starts_with('\t') || lines[i].trim().is_empty()) {
                code.push_str(lines[i].strip_prefix("    ").or_else(|| lines[i].strip_prefix('\t')).unwrap_or(""));
                code.push('\n');
                i += 1;
            }
            s.push_str("\\pard\\li120\\sa120{\\f1\\fs19\\highlight11 ");
            esc_lines(&mut s, code.trim_end_matches('\n'));
            s.push_str("}\\par\n");
            continue;
        }
        para.push(line);
        i += 1;
    }
    flush_para(&mut s, &mut para);
    s.push('}');
    s
}

fn heading(s: &mut String, level: usize, title: &str) {
    // MarkdownTextBlock's heading sizes (half-points): 28, 21, 18, 16, 14, 14 px
    let size = [42, 32, 27, 24, 21, 21][level.clamp(1, 6) - 1];
    s.push_str(&format!("\\pard\\sb160\\sa80{{\\b\\fs{size} "));
    inline(s, title);
    s.push_str("}\\par\n");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pal() -> Palette {
        Palette { ink: 0, muted: 0x808080, accent: 0xD47800, dark: false }
    }

    #[test]
    fn languages_follow_files_table() {
        assert_eq!(language_for(".CS"), 4);
        assert_eq!(language_for("hpp"), 3);
        assert_eq!(language_for(".xaml"), 15);
        assert_eq!(language_for(".ps1"), 12);
        assert_eq!(language_for(".log"), 0);
        assert_eq!(language_for(".txt"), 0);
    }

    #[test]
    fn code_tokens() {
        let t = tokens("// hi\nint x = 42; /* c */ \"s\\\"q\" #x", 3);
        let kinds: Vec<Tok> = t.iter().map(|(k, _)| *k).collect();
        assert_eq!(t[0], (Tok::Comment, "// hi".into()));
        assert!(t.contains(&(Tok::Keyword, "int".into())));
        assert!(t.contains(&(Tok::Number, "42".into())));
        assert!(t.contains(&(Tok::Comment, "/* c */".into())));
        assert!(t.contains(&(Tok::Str, "\"s\\\"q\"".into())));
        assert!(!kinds.contains(&Tok::Preproc), "# mid-line is not a directive");
        let t = tokens("#include <x>\nreturn", 3);
        assert_eq!(t[0], (Tok::Preproc, "#include <x>".into()));
        let ps = tokens("$Path = 'a' # note\nIf ($x -eq 1) {}", 12);
        assert!(ps.contains(&(Tok::Attr, "$Path".into())));
        assert!(ps.contains(&(Tok::Comment, "# note".into())));
        assert!(ps.contains(&(Tok::Keyword, "If".into())));
        assert!(ps.contains(&(Tok::Keyword, "-eq".into())));
        let vb = tokens("Dim s As String ' c", 14);
        assert!(vb.contains(&(Tok::Keyword, "Dim".into())) && vb.contains(&(Tok::Comment, "' c".into())));
    }

    #[test]
    fn languages_beyond_files_table() {
        for (ext, id) in [(".py", 16), (".SQL", 17), (".bat", 18), (".sh", 19), (".rs", 20), (".go", 21), (".lua", 22), (".rb", 23),
            (".kt", 24), (".swift", 24), (".pl", 25), (".json", 26), (".yaml", 27), (".ini", 27), (".toml", 27), (".c", 3), (".mjs", 10),
            (".csproj", 15), (".vue", 8)] {
            assert_eq!(language_for(ext), id, "{ext}");
        }
        assert_eq!(language_for(".log"), 0);
        assert_eq!(language_for(".csv"), 0);
        assert_eq!(language_for(".html"), 0, "HTML keeps its preview handler (the page, as Files)");
        // Every id in the table has a lexer (none falls back to one plain run)
        for id in [3, 4, 6, 7, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25] {
            assert!(lang(id).is_some(), "language {id} has no lexer");
        }
    }

    #[test]
    fn new_lexers() {
        let py = tokens("def f(x):\n    \"\"\"doc\nmore\"\"\"\n    return 'a' # c", 16);
        assert!(py.contains(&(Tok::Keyword, "def".into())) && py.contains(&(Tok::Keyword, "return".into())));
        assert!(py.contains(&(Tok::Str, "\"\"\"doc\nmore\"\"\"".into())), "docstring over lines: {py:?}");
        assert!(py.contains(&(Tok::Comment, "# c".into())));
        let sql = tokens("SELECT name FROM t WHERE x = 'it''s' -- c", 17);
        assert!(sql.contains(&(Tok::Keyword, "SELECT".into())) && sql.contains(&(Tok::Keyword, "WHERE".into())));
        assert!(sql.contains(&(Tok::Comment, "-- c".into())));
        let bat = tokens("@echo off\nREM note\n:: also\nset X=\"C:\\a\\\"", 18);
        assert!(bat.contains(&(Tok::Comment, "REM note".into())) && bat.contains(&(Tok::Comment, ":: also".into())));
        assert!(bat.contains(&(Tok::Str, "\"C:\\a\\\"".into())), "no backslash escapes in batch: {bat:?}");
        let rs = tokens("#[derive(Debug)]\nfn main() { let s = \"a\\\"b\"; } // c", 20);
        assert!(rs.contains(&(Tok::Preproc, "#[derive(Debug)]".into())) && rs.contains(&(Tok::Keyword, "fn".into())));
        assert!(rs.contains(&(Tok::Str, "\"a\\\"b\"".into())));
        let ps = tokens("$p = \"C:\\dir\\\" # c", 12);
        assert!(ps.contains(&(Tok::Str, "\"C:\\dir\\\"".into())), "PowerShell escapes with `, not \\: {ps:?}");
        let json = tokens("{ \"name\": \"x\", \"n\": -1.5e3, \"ok\": true }", 26);
        assert!(json.contains(&(Tok::Attr, "\"name\"".into())) && json.contains(&(Tok::Str, "\"x\"".into())));
        assert!(json.contains(&(Tok::Number, "-1.5e3".into())) && json.contains(&(Tok::Keyword, "true".into())));
        let ini = tokens("[core]\n; c\nname = \"x\" # t\ncount=3\nenabled: yes\n- item", 27);
        assert!(ini.contains(&(Tok::Keyword, "[core]".into())) && ini.contains(&(Tok::Comment, "; c".into())));
        assert!(ini.contains(&(Tok::Attr, "name ".into())) && ini.contains(&(Tok::Str, " \"x\"".into())));
        assert!(ini.contains(&(Tok::Comment, " # t".into())) && ini.contains(&(Tok::Number, "3".into())));
        assert!(ini.contains(&(Tok::Keyword, " yes".into())));
        let text: String = ini.iter().map(|(_, s)| s.as_str()).collect();
        assert_eq!(text, "[core]\n; c\nname = \"x\" # t\ncount=3\nenabled: yes\n- item", "no text lost or added");
    }

    #[test]
    fn markup_and_css() {
        let t = tokens("<a href=\"x\"><!-- c -->text</a>", 15);
        assert!(t.contains(&(Tok::Attr, "href".into())));
        assert!(t.contains(&(Tok::Str, "\"x\"".into())));
        assert!(t.contains(&(Tok::Comment, "<!-- c -->".into())));
        assert!(t.contains(&(Tok::Text, "text".into())));
        let c = tokens("a.b { color: red; } /* z */", 5);
        assert!(c.contains(&(Tok::Keyword, "a.b ".into())));
        assert!(c.contains(&(Tok::Attr, "color".into())));
        assert!(c.contains(&(Tok::Comment, "/* z */".into())));
    }

    #[test]
    fn rtf_escapes_and_unicode() {
        let r = code_rtf("a{b}\\ 中\u{1F600}", 0, &pal(), "Consolas");
        assert!(r.starts_with("{\\rtf1"));
        assert!(r.contains("a\\{b\\}\\\\ "));
        assert!(r.contains("\\u20013 "));
        assert!(r.contains("\\u-10179 \\u-8704 "), "surrogate pair as signed units");
        assert!(r.ends_with('}'));
    }

    #[test]
    fn markdown_blocks() {
        let md = "# Title\n\nSome *em* and **bold** and `code` [link](https://x.org).\n\n- one\n- [x] two\n\n1. first\n\n> quote\n\n```\nfn x() {}\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n<b>raw</b>\n";
        let r = markdown_rtf(md, &pal(), "Consolas");
        assert!(r.contains("{\\b\\fs42 Title}"));
        assert!(r.contains("{\\i em\\i0 }") && r.contains("{\\b bold\\b0 }"));
        assert!(r.contains("{\\f1\\highlight11 code}"));
        assert!(r.contains("HYPERLINK \"https://x.org\"") && r.contains("\\ul link"));
        assert!(r.contains("\\u8226 \\tab one") && r.contains("\\u9745  two"));
        assert!(r.contains("1.\\tab first"));
        assert!(r.contains("\\li360") && r.contains("quote"));
        assert!(r.contains("fn x() \\{\\}"));
        assert!(r.contains("\\trowd") && r.contains("\\cell"));
        assert!(r.contains("<b>raw</b>"), "raw HTML stays text");
        assert_eq!(r.matches('{').count() - r.matches("\\{").count(), r.matches('}').count() - r.matches("\\}").count(), "balanced groups");
    }
}
