//! Zip archives: create (Files' "Compress to .zip") and extract ("Extract here / to folder"). Standard library only.
//! Extraction never overwrites (conflicting names get " (2)"), refuses paths that would leave the target folder,
//! checks every file's CRC, and reads zip64 archives. Creation writes UTF-8 names and stops cleanly at the classic
//! zip limits (4 GB per file / archive, 65535 entries) instead of writing a broken file.

use crate::deflate::{self, Deflater};
use std::fs::{self, File};
use std::io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Progress shared with the UI (read while the job runs)
#[repr(C)]
#[derive(Default)]
pub struct Progress {
    pub total_bytes: AtomicU64,
    pub done_bytes: AtomicU64,
    pub total_files: AtomicU64,
    pub done_files: AtomicU64,
}

fn fail(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::Other, msg.into())
}

fn cancelled() -> io::Error {
    io::Error::new(io::ErrorKind::Interrupted, "cancelled")
}

// ------------------------------------------------------------------------------------------------------------- times

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct FileTime {
    low: u32,
    high: u32,
}

#[link(name = "kernel32")]
extern "system" {
    fn FileTimeToLocalFileTime(utc: *const FileTime, local: *mut FileTime) -> i32;
    fn LocalFileTimeToFileTime(local: *const FileTime, utc: *mut FileTime) -> i32;
    fn FileTimeToDosDateTime(ft: *const FileTime, date: *mut u16, time: *mut u16) -> i32;
    fn DosDateTimeToFileTime(date: u16, time: u16, ft: *mut FileTime) -> i32;
    fn MultiByteToWideChar(cp: u32, flags: u32, src: *const u8, len: i32, dst: *mut u16, cap: i32) -> i32;
}

const TICKS_TO_UNIX: u64 = 116_444_736_000_000_000; // 100 ns ticks from 1601 to 1970

fn to_filetime(t: SystemTime) -> FileTime {
    let ticks = t.duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64 / 100).unwrap_or(0) + TICKS_TO_UNIX;
    FileTime { low: ticks as u32, high: (ticks >> 32) as u32 }
}

fn from_filetime(ft: FileTime) -> SystemTime {
    let ticks = (ft.high as u64) << 32 | ft.low as u64;
    UNIX_EPOCH + Duration::from_nanos(ticks.saturating_sub(TICKS_TO_UNIX) * 100)
}

/// (time, date) in DOS format, local time like every other zip tool; 1980-01-01 for anything earlier
fn dos_time(t: SystemTime) -> (u16, u16) {
    let utc = to_filetime(t);
    let mut local = FileTime::default();
    let (mut date, mut time) = (0u16, 0u16);
    unsafe {
        if FileTimeToLocalFileTime(&utc, &mut local) == 0 || FileTimeToDosDateTime(&local, &mut date, &mut time) == 0 {
            return (0, 0x21);
        }
    }
    (time, date)
}

fn from_dos_time(time: u16, date: u16) -> Option<SystemTime> {
    let (mut local, mut utc) = (FileTime::default(), FileTime::default());
    unsafe {
        if DosDateTimeToFileTime(date, time, &mut local) == 0 || LocalFileTimeToFileTime(&local, &mut utc) == 0 {
            return None;
        }
    }
    Some(from_filetime(utc))
}

// ------------------------------------------------------------------------------------------------------------- create

struct Source {
    path: PathBuf,
    name: String, // zip name, '/' separated, '/' at the end for folders
    dir: bool,
    size: u64,
    modified: SystemTime,
}

fn collect(path: &Path, name: String, out: &mut Vec<Source>, cancel: &AtomicBool) -> io::Result<()> {
    if cancel.load(Ordering::Relaxed) {
        return Err(cancelled());
    }
    let meta = fs::symlink_metadata(path)?;
    let modified = meta.modified().unwrap_or(UNIX_EPOCH);
    if meta.file_type().is_symlink() {
        return Ok(()); // links and junctions aren't followed, like "Send to > Compressed folder"
    }
    if meta.is_dir() {
        out.push(Source { path: path.to_path_buf(), name: format!("{name}/"), dir: true, size: 0, modified });
        let mut children: Vec<_> = fs::read_dir(path)?.flatten().collect();
        children.sort_by_key(|e| e.file_name());
        for child in children {
            let child_name = format!("{name}/{}", child.file_name().to_string_lossy());
            collect(&child.path(), child_name, out, cancel)?;
        }
    } else {
        out.push(Source { path: path.to_path_buf(), name, dir: false, size: meta.len(), modified });
    }
    Ok(())
}

struct Written {
    name: String,
    dir: bool,
    method: u16,
    time: u16,
    date: u16,
    crc: u32,
    compressed: u64,
    size: u64,
    offset: u64,
}

const LIMIT: u64 = 0xFFFF_FFFF;

/// Zips `items` (files or folders; each is stored under its own name) into `archive`, which must not exist.
/// On any failure or cancel the partial archive is deleted.
pub fn create(items: &[PathBuf], archive: &Path, progress: &Progress, cancel: &AtomicBool) -> io::Result<()> {
    let mut sources = Vec::new();
    for item in items {
        let name = item.file_name().map(|n| n.to_string_lossy().into_owned()).ok_or_else(|| fail("无法压缩驱动器根目录"))?;
        collect(item, name, &mut sources, cancel)?;
    }
    if sources.len() > 0xFFFF {
        return Err(fail("项目太多：zip 格式最多 65535 个（不支持 zip64 写入）"));
    }
    if let Some(big) = sources.iter().find(|s| s.size >= LIMIT) {
        return Err(fail(format!("“{}”超过 4 GB，zip 格式（不带 zip64）放不下", big.name.trim_end_matches('/'))));
    }
    progress.total_bytes.store(sources.iter().map(|s| s.size).sum(), Ordering::Relaxed);
    progress.total_files.store(sources.iter().filter(|s| !s.dir).count() as u64, Ordering::Relaxed);

    let file = fs::OpenOptions::new().write(true).create_new(true).open(archive)?;
    let result = write_archive(BufWriter::with_capacity(1 << 20, file), &sources, progress, cancel);
    if result.is_err() {
        let _ = fs::remove_file(archive);
    }
    result
}

fn write_archive(mut w: BufWriter<File>, sources: &[Source], progress: &Progress, cancel: &AtomicBool) -> io::Result<()> {
    let mut written = Vec::with_capacity(sources.len());
    let mut offset = 0u64;
    let mut chunk = vec![0u8; 1 << 16];
    for s in sources {
        if cancel.load(Ordering::Relaxed) {
            return Err(cancelled());
        }
        if offset >= LIMIT {
            return Err(fail("压缩包超过 4 GB，zip 格式（不带 zip64）放不下"));
        }
        let (time, date) = dos_time(s.modified);
        let method: u16 = if s.dir || s.size == 0 { 0 } else { 8 };
        let name = s.name.as_bytes();
        let header_at = offset;
        // Local header with placeholder CRC and sizes, patched once the data is written
        let mut h = Vec::with_capacity(30 + name.len());
        h.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        h.extend_from_slice(&20u16.to_le_bytes());
        h.extend_from_slice(&0x0800u16.to_le_bytes()); // UTF-8 names
        h.extend_from_slice(&method.to_le_bytes());
        h.extend_from_slice(&time.to_le_bytes());
        h.extend_from_slice(&date.to_le_bytes());
        h.extend_from_slice(&[0u8; 12]);
        h.extend_from_slice(&(name.len() as u16).to_le_bytes());
        h.extend_from_slice(&0u16.to_le_bytes());
        h.extend_from_slice(name);
        w.write_all(&h)?;
        offset += h.len() as u64;
        let (mut crc, mut compressed, mut size) = (0u32, 0u64, 0u64);
        if method == 8 {
            let mut input = File::open(&s.path)?;
            let mut deflater = Deflater::new();
            loop {
                if cancel.load(Ordering::Relaxed) {
                    return Err(cancelled());
                }
                let n = input.read(&mut chunk)?;
                if n == 0 {
                    break;
                }
                crc = deflate::crc32_update(crc, &chunk[..n]);
                size += n as u64;
                let out = deflater.write_input(&chunk[..n]);
                compressed += out.len() as u64;
                w.write_all(&out)?;
                progress.done_bytes.fetch_add(n as u64, Ordering::Relaxed);
            }
            let out = deflater.finish();
            compressed += out.len() as u64;
            w.write_all(&out)?;
            if size >= LIMIT || offset + compressed >= LIMIT {
                return Err(fail("压缩包超过 4 GB，zip 格式（不带 zip64）放不下"));
            }
            let end = offset + compressed;
            w.seek(SeekFrom::Start(header_at + 14))?;
            w.write_all(&crc.to_le_bytes())?;
            w.write_all(&(compressed as u32).to_le_bytes())?;
            w.write_all(&(size as u32).to_le_bytes())?;
            w.seek(SeekFrom::Start(end))?;
            offset = end;
        }
        if !s.dir {
            progress.done_files.fetch_add(1, Ordering::Relaxed);
        }
        written.push(Written { name: s.name.clone(), dir: s.dir, method, time, date, crc, compressed, size, offset: header_at });
    }
    let cd_start = offset;
    for e in &written {
        let name = e.name.as_bytes();
        let mut c = Vec::with_capacity(46 + name.len());
        c.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        c.extend_from_slice(&(0x0300u16 | 20).to_le_bytes()); // made by: "Unix" host so attributes are read as-is
        c.extend_from_slice(&20u16.to_le_bytes());
        c.extend_from_slice(&0x0800u16.to_le_bytes());
        c.extend_from_slice(&e.method.to_le_bytes());
        c.extend_from_slice(&e.time.to_le_bytes());
        c.extend_from_slice(&e.date.to_le_bytes());
        c.extend_from_slice(&e.crc.to_le_bytes());
        c.extend_from_slice(&(e.compressed as u32).to_le_bytes());
        c.extend_from_slice(&(e.size as u32).to_le_bytes());
        c.extend_from_slice(&(name.len() as u16).to_le_bytes());
        c.extend_from_slice(&[0u8; 8]); // extra, comment, disk, internal attributes
        // External attributes: Unix mode in the high half (dir 040755 / file 0100644) and the DOS directory bit
        let attrs: u32 = if e.dir { 0o040755 << 16 | 0x10 } else { 0o100644 << 16 };
        c.extend_from_slice(&attrs.to_le_bytes());
        c.extend_from_slice(&(e.offset as u32).to_le_bytes());
        c.extend_from_slice(name);
        w.write_all(&c)?;
        offset += c.len() as u64;
    }
    if offset >= LIMIT {
        return Err(fail("压缩包超过 4 GB，zip 格式（不带 zip64）放不下"));
    }
    let count = written.len() as u16;
    let mut end = Vec::with_capacity(22);
    end.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    end.extend_from_slice(&[0u8; 4]);
    end.extend_from_slice(&count.to_le_bytes());
    end.extend_from_slice(&count.to_le_bytes());
    end.extend_from_slice(&((offset - cd_start) as u32).to_le_bytes());
    end.extend_from_slice(&(cd_start as u32).to_le_bytes());
    end.extend_from_slice(&0u16.to_le_bytes());
    w.write_all(&end)?;
    w.flush()?;
    Ok(())
}

// ------------------------------------------------------------------------------------------------------------- extract

pub struct Entry {
    pub name: String,
    pub dir: bool,
    method: u16,
    flags: u16,
    time: u16,
    date: u16,
    crc: u32,
    compressed: u64,
    pub size: u64,
    offset: u64,
    legacy: Vec<u8>, // raw name bytes when it has no UTF-8 flag and isn't plain ASCII (Files' "encoding undetermined")
}

fn u16_at(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}
fn u32_at(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(b[i..i + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(b[i..i + 8].try_into().unwrap())
}

const CP_OEMCP: u32 = 1;
const CP_UTF8: u32 = 65001;
const MB_ERR_INVALID_CHARS: u32 = 8;

fn multibyte(cp: u32, raw: &[u8], flags: u32) -> Option<String> {
    if raw.is_empty() {
        return Some(String::new());
    }
    unsafe {
        let n = MultiByteToWideChar(cp, flags, raw.as_ptr(), raw.len() as i32, std::ptr::null_mut(), 0);
        if n <= 0 {
            return None;
        }
        let mut wide = vec![0u16; n as usize];
        MultiByteToWideChar(cp, flags, raw.as_ptr(), raw.len() as i32, wide.as_mut_ptr(), n);
        Some(String::from_utf16_lossy(&wide))
    }
}

/// Names without the UTF-8 flag: in `codepage` when one was picked (Files' encoding box), else UTF-8 if they decode as
/// such, otherwise the OEM code page (what Explorer and most Windows zip tools wrote, e.g. GBK on Chinese systems)
fn decode_name(raw: &[u8], utf8: bool, codepage: u32) -> String {
    if utf8 || codepage == CP_UTF8 {
        return String::from_utf8_lossy(raw).into_owned();
    }
    if codepage == 0 {
        if let Ok(s) = std::str::from_utf8(raw) {
            return s.to_string();
        }
    }
    multibyte(if codepage == 0 { CP_OEMCP } else { codepage }, raw, 0).unwrap_or_else(|| String::from_utf8_lossy(raw).into_owned())
}

pub fn read_entries(f: &mut File, codepage: u32) -> io::Result<Vec<Entry>> {
    let len = f.seek(SeekFrom::End(0))?;
    let tail = len.min(22 + 65535);
    f.seek(SeekFrom::Start(len - tail))?;
    let mut buf = vec![0u8; tail as usize];
    f.read_exact(&mut buf)?;
    let eocd = (0..buf.len().saturating_sub(21)).rev().find(|&i| u32_at(&buf, i) == 0x0605_4b50).ok_or_else(|| fail("不是 zip 文件，或文件已损坏"))?;
    let mut count = u16_at(&buf, eocd + 10) as u64;
    let mut cd_size = u32_at(&buf, eocd + 12) as u64;
    let mut cd_offset = u32_at(&buf, eocd + 16) as u64;
    // zip64: locator right before the end record points at the zip64 end record
    if eocd >= 20 && u32_at(&buf, eocd - 20) == 0x0706_4b50 {
        let at = u64_at(&buf, eocd - 12);
        let mut z = [0u8; 56];
        f.seek(SeekFrom::Start(at))?;
        f.read_exact(&mut z)?;
        if u32_at(&z, 0) == 0x0606_4b50 {
            count = u64_at(&z, 32);
            cd_size = u64_at(&z, 40);
            cd_offset = u64_at(&z, 48);
        }
    }
    if cd_size > 512 << 20 || cd_offset + cd_size > len {
        return Err(fail("zip 目录损坏"));
    }
    let mut cd = vec![0u8; cd_size as usize];
    f.seek(SeekFrom::Start(cd_offset))?;
    f.read_exact(&mut cd)?;
    let mut entries = Vec::with_capacity(count.min(1 << 20) as usize);
    let mut i = 0usize;
    while i + 46 <= cd.len() && u32_at(&cd, i) == 0x0201_4b50 {
        let flags = u16_at(&cd, i + 8);
        let method = u16_at(&cd, i + 10);
        let time = u16_at(&cd, i + 12);
        let date = u16_at(&cd, i + 14);
        let crc = u32_at(&cd, i + 16);
        let mut compressed = u32_at(&cd, i + 20) as u64;
        let mut size = u32_at(&cd, i + 24) as u64;
        let name_len = u16_at(&cd, i + 28) as usize;
        let extra_len = u16_at(&cd, i + 30) as usize;
        let comment_len = u16_at(&cd, i + 32) as usize;
        let external = u32_at(&cd, i + 38);
        let mut offset = u32_at(&cd, i + 42) as u64;
        let end = i + 46 + name_len + extra_len + comment_len;
        if end > cd.len() {
            return Err(fail("zip 目录损坏"));
        }
        let raw = &cd[i + 46..i + 46 + name_len];
        // zip64 extra field: the 0xFFFFFFFF values, in order
        let mut x = i + 46 + name_len;
        let xend = x + extra_len;
        while x + 4 <= xend {
            let id = u16_at(&cd, x);
            let sz = u16_at(&cd, x + 2) as usize;
            if id == 1 {
                let mut p = x + 4;
                for field in [&mut size, &mut compressed, &mut offset] {
                    if *field == LIMIT && p + 8 <= x + 4 + sz {
                        *field = u64_at(&cd, p);
                        p += 8;
                    }
                }
            }
            x += 4 + sz;
        }
        let unicode = flags & 0x0800 != 0;
        let name = decode_name(raw, unicode, codepage).replace('\\', "/");
        let dir = name.ends_with('/') || external & 0x10 != 0 && size == 0;
        let legacy = if !unicode && !raw.is_ascii() { raw.to_vec() } else { Vec::new() };
        entries.push(Entry { name, dir, method, flags, time, date, crc, compressed, size, offset, legacy });
        i = end;
    }
    Ok(entries)
}

const RESERVED: [&str; 22] = ["CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"];

/// Relative path for an entry name, or None if it would escape the target folder or isn't a valid Windows name
pub fn safe_relative(name: &str) -> Option<PathBuf> {
    let mut out = PathBuf::new();
    for part in name.split('/').filter(|p| !p.is_empty() && *p != ".") {
        if part == ".." || part.contains(':') || part.chars().any(|c| (c as u32) < 32 || "<>\"|?*".contains(c)) {
            return None;
        }
        let trimmed = part.trim_end_matches([' ', '.']);
        if trimmed.is_empty() {
            return None;
        }
        let stem = trimmed.split('.').next().unwrap_or("").to_ascii_uppercase();
        if RESERVED.contains(&stem.as_str()) {
            return None;
        }
        out.push(trimmed);
    }
    let ok = !out.as_os_str().is_empty() && out.components().all(|c| matches!(c, Component::Normal(_)));
    ok.then_some(out)
}

/// `path`, or "name (2).ext", "name (3).ext"... if it exists
pub fn unique(path: &Path) -> PathBuf {
    if fs::symlink_metadata(path).is_err() {
        return path.to_path_buf();
    }
    let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let ext = path.extension().map(|e| format!(".{}", e.to_string_lossy())).unwrap_or_default();
    let parent = path.parent().unwrap_or(Path::new(""));
    (2..).map(|n| parent.join(format!("{stem} ({n}){ext}"))).find(|p| fs::symlink_metadata(p).is_err()).unwrap()
}

pub struct ExtractResult {
    pub skipped: u64, // unsafe names left out
}

/// What the extract dialog needs before it opens (Files' IsEncryptedAsync / IsEncodingUndeterminedAsync / DetectEncodingAsync)
pub struct Info {
    pub encrypted: bool,
    pub unsupported: bool, // a method other than store / deflate: 7z.dll extracts it
    pub undetermined: bool, // names without the UTF-8 flag that aren't ASCII
    pub detected: u32,      // likely code page for those names, 0 when no guess
}

pub fn info(archive: &Path) -> io::Result<Info> {
    let mut f = File::open(archive)?;
    let entries = read_entries(&mut f, 0)?;
    let encrypted = entries.iter().any(|e| e.flags & 1 != 0 || e.method == 99);
    let unsupported = entries.iter().any(|e| !e.dir && e.method != 0 && e.method != 8);
    let legacy: Vec<&[u8]> = entries.iter().filter(|e| !e.legacy.is_empty()).map(|e| e.legacy.as_slice()).collect();
    let detected = if legacy.is_empty() { 0 } else { detect_codepage(&legacy) };
    Ok(Info { encrypted, unsupported, undetermined: !legacy.is_empty(), detected })
}

/// Best guess for legacy (non-UTF-8) names. Files runs a charset detector over the bytes; this is the small version for the
/// code pages such names realistically come in: the candidates that decode every name strictly, scored by how many
/// characters land in that script's common ranges (kana for Shift-JIS, hangul for Korean, CJK ideographs for all three
/// Chinese / Japanese ones). The OEM code page goes first so it wins ties. 0 when nothing convincing.
fn detect_codepage(names: &[&[u8]]) -> u32 {
    if names.iter().all(|n| std::str::from_utf8(n).is_ok()) {
        return CP_UTF8;
    }
    extern "system" {
        fn GetOEMCP() -> u32;
    }
    let oem = unsafe { GetOEMCP() };
    let mut candidates = vec![oem];
    for cp in [936u32, 932, 950, 949] {
        if cp != oem {
            candidates.push(cp);
        }
    }
    let non_ascii: usize = names.iter().map(|n| n.iter().filter(|b| **b >= 0x80).count()).sum();
    let (mut best, mut best_score) = (0u32, 0usize);
    for cp in candidates {
        let mut score = 0usize;
        let mut ok = true;
        for n in names {
            let Some(text) = multibyte(cp, n, MB_ERR_INVALID_CHARS) else {
                ok = false;
                break;
            };
            for c in text.chars() {
                let u = c as u32;
                score += match u {
                    0x3040..=0x30FF => if cp == 932 { 4 } else { 0 }, // hiragana / katakana
                    0xAC00..=0xD7A3 => if cp == 949 { 4 } else { 0 }, // hangul
                    0x4E00..=0x9FFF => 2,                             // CJK ideographs
                    0x3000..=0x303F | 0xFF00..=0xFFEF => 1,           // CJK punctuation, full / halfwidth forms
                    _ => 0,
                };
            }
        }
        if ok && score > best_score {
            (best, best_score) = (cp, score);
        }
    }
    // Two bytes per character in these code pages: a real match scores about the number of non-ASCII bytes
    if best_score * 2 >= non_ascii.max(1) { best } else { 0 }
}

/// Extracts every entry of `archive` under `target` (created if needed). Existing files are never replaced.
/// `codepage` decodes names without the UTF-8 flag (0: UTF-8 when valid, else OEM).
pub fn extract(archive: &Path, target: &Path, codepage: u32, progress: &Progress, cancel: &AtomicBool) -> io::Result<ExtractResult> {
    let mut f = File::open(archive)?;
    let entries = read_entries(&mut f, codepage)?;
    // Refuse up front rather than leave half an archive behind
    if entries.iter().any(|e| e.flags & 1 != 0) {
        return Err(fail("这个压缩包有密码，暂不支持解压加密的 zip"));
    }
    if let Some(e) = entries.iter().find(|e| !e.dir && e.method != 0 && e.method != 8) {
        let what = match e.method {
            9 => "Deflate64",
            12 => "BZIP2",
            14 => "LZMA",
            93 => "Zstandard",
            95 => "XZ",
            99 => "AES 加密",
            _ => "未知",
        };
        return Err(fail(format!("“{}”使用 {what} 压缩方式，暂不支持（只支持存储和 Deflate）", e.name)));
    }
    progress.total_bytes.store(entries.iter().map(|e| e.size).sum(), Ordering::Relaxed);
    progress.total_files.store(entries.iter().filter(|e| !e.dir).count() as u64, Ordering::Relaxed);
    fs::create_dir_all(target)?;
    let mut skipped = 0u64;
    for e in &entries {
        if cancel.load(Ordering::Relaxed) {
            return Err(cancelled());
        }
        let Some(rel) = safe_relative(&e.name) else {
            skipped += 1;
            continue;
        };
        let dest = target.join(&rel);
        if e.dir {
            fs::create_dir_all(&dest)?;
            continue;
        }
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent)?;
        }
        let dest = unique(&dest);
        let mut head = [0u8; 30];
        f.seek(SeekFrom::Start(e.offset))?;
        f.read_exact(&mut head)?;
        if u32_at(&head, 0) != 0x0403_4b50 {
            return Err(fail(format!("“{}”的数据损坏", e.name)));
        }
        let data_at = e.offset + 30 + u16_at(&head, 26) as u64 + u16_at(&head, 28) as u64;
        f.seek(SeekFrom::Start(data_at))?;
        let out = File::create(&dest)?;
        let written = write_entry(&mut f, e, out, progress, cancel);
        match written {
            Ok((size, crc)) if size == e.size && crc == e.crc => {}
            Ok(_) => {
                let _ = fs::remove_file(&dest);
                return Err(fail(format!("“{}”校验失败，压缩包可能已损坏", e.name)));
            }
            Err(err) => {
                let _ = fs::remove_file(&dest);
                return Err(err);
            }
        }
        if let Some(t) = from_dos_time(e.time, e.date) {
            if let Ok(file) = fs::OpenOptions::new().write(true).open(&dest) {
                let _ = file.set_modified(t);
            }
        }
        progress.done_files.fetch_add(1, Ordering::Relaxed);
    }
    Ok(ExtractResult { skipped })
}

fn write_entry(f: &mut File, e: &Entry, out: File, progress: &Progress, cancel: &AtomicBool) -> io::Result<(u64, u32)> {
    let mut w = BufWriter::with_capacity(1 << 20, out);
    let src = BufReader::with_capacity(1 << 16, Read::by_ref(f).take(e.compressed));
    let result = if e.method == 8 {
        deflate::inflate(src, &mut w, cancel, &progress.done_bytes)?
    } else {
        let mut src = src;
        let (mut total, mut crc) = (0u64, 0u32);
        let mut chunk = vec![0u8; 1 << 16];
        loop {
            if cancel.load(Ordering::Relaxed) {
                return Err(cancelled());
            }
            let n = src.read(&mut chunk)?;
            if n == 0 {
                break;
            }
            crc = deflate::crc32_update(crc, &chunk[..n]);
            w.write_all(&chunk[..n])?;
            total += n as u64;
            progress.done_bytes.fetch_add(n as u64, Ordering::Relaxed);
        }
        (total, crc)
    };
    w.flush()?;
    Ok(result)
}

/// Folder name for "Extract to <name>\": the archive name without its extension
pub fn folder_for(archive: &Path) -> PathBuf {
    let stem = archive.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "archive".into());
    archive.with_file_name(stem.trim_end_matches([' ', '.']))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("ff-zip-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn pseudo_random(n: usize, seed: u32) -> Vec<u8> {
        let mut x = seed;
        (0..n).map(|_| { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x as u8 }).collect()
    }

    #[test]
    fn deflate_roundtrip_text_random_and_repeats() {
        let text: Vec<u8> = (0..20000).flat_map(|i| format!("line {i} of some text 文本\n").into_bytes()).collect();
        let cases = [text, pseudo_random(300_000, 7), vec![b'a'; 200_000], Vec::new(), b"x".to_vec()];
        for data in cases {
            let mut d = Deflater::new();
            let mut packed = Vec::new();
            for part in data.chunks(7777) {
                packed.extend(d.write_input(part));
            }
            packed.extend(d.finish());
            let mut out = Vec::new();
            let (n, crc) = deflate::inflate(&packed[..], &mut out, &AtomicBool::new(false), &AtomicU64::new(0)).unwrap();
            assert_eq!(out, data);
            assert_eq!(n as usize, data.len());
            assert_eq!(crc, deflate::crc32_update(0, &data));
            if data.len() > 1000 {
                // Random data falls back to stored blocks: at most a few bytes per 64 KB more
                assert!(packed.len() <= data.len() + data.len() / 10000 + 16, "{} -> {}", data.len(), packed.len());
            }
        }
    }

    #[test]
    fn crc_known_value() {
        assert_eq!(deflate::crc32_update(0, b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn zip_roundtrip_with_folders_and_unicode() {
        let root = temp("roundtrip");
        let src = root.join("源 文件夹");
        fs::create_dir_all(src.join("sub").join("empty")).unwrap();
        fs::write(src.join("a.txt"), "hello hello hello hello".repeat(1000)).unwrap();
        fs::write(src.join("sub").join("中文.bin"), pseudo_random(100_000, 3)).unwrap();
        fs::write(src.join("zero"), b"").unwrap();
        let archive = root.join("out.zip");
        let p = Progress::default();
        create(&[src.clone()], &archive, &p, &AtomicBool::new(false)).unwrap();
        assert_eq!(p.done_files.load(Ordering::Relaxed), 3);
        assert!(create(&[src.clone()], &archive, &Progress::default(), &AtomicBool::new(false)).is_err(), "never overwrites");

        let target = root.join("x");
        let r = extract(&archive, &target, 0, &Progress::default(), &AtomicBool::new(false)).unwrap();
        assert_eq!(r.skipped, 0);
        let back = target.join("源 文件夹");
        assert_eq!(fs::read(back.join("a.txt")).unwrap(), fs::read(src.join("a.txt")).unwrap());
        assert_eq!(fs::read(back.join("sub").join("中文.bin")).unwrap(), fs::read(src.join("sub").join("中文.bin")).unwrap());
        assert!(back.join("sub").join("empty").is_dir());
        assert_eq!(fs::read(back.join("zero")).unwrap().len(), 0);
        // Second extraction into the same place keeps both copies
        extract(&archive, &target, 0, &Progress::default(), &AtomicBool::new(false)).unwrap();
        assert!(back.join("a (2).txt").exists());
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn unsafe_names_are_refused() {
        for bad in ["../evil.txt", "a/../../b", "C:/Windows/x", "c:x", "con", "dir/NUL.txt", "a\u{1}b", "...", "x/ /y"] {
            assert!(safe_relative(bad).is_none(), "{bad}");
        }
        assert_eq!(safe_relative("dir/./file. ").unwrap(), PathBuf::from("dir").join("file"));
        assert_eq!(safe_relative("/abs/ok.txt").unwrap(), PathBuf::from("abs").join("ok.txt"));
    }

    #[test]
    fn corrupt_data_is_detected_and_removed() {
        let root = temp("corrupt");
        let src = root.join("f.txt");
        fs::write(&src, "abcdefgh".repeat(5000)).unwrap();
        let archive = root.join("c.zip");
        create(&[src], &archive, &Progress::default(), &AtomicBool::new(false)).unwrap();
        let mut bytes = fs::read(&archive).unwrap();
        bytes[60] ^= 0x55; // inside the compressed data
        fs::write(&archive, &bytes).unwrap();
        let target = root.join("t");
        assert!(extract(&archive, &target, 0, &Progress::default(), &AtomicBool::new(false)).is_err());
        assert!(!target.join("f.txt").exists());
        fs::remove_dir_all(&root).unwrap();
    }

    /// One stored entry with `name` as raw bytes and the given general-purpose flags / method
    fn raw_zip(name: &[u8], flags: u16, method: u16, data: &[u8]) -> Vec<u8> {
        let crc = deflate::crc32_update(0, data);
        let mut local = Vec::new();
        local.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        local.extend_from_slice(&20u16.to_le_bytes());
        local.extend_from_slice(&flags.to_le_bytes());
        local.extend_from_slice(&method.to_le_bytes());
        local.extend_from_slice(&[0; 4]); // time, date
        local.extend_from_slice(&crc.to_le_bytes());
        local.extend_from_slice(&(data.len() as u32).to_le_bytes());
        local.extend_from_slice(&(data.len() as u32).to_le_bytes());
        local.extend_from_slice(&(name.len() as u16).to_le_bytes());
        local.extend_from_slice(&0u16.to_le_bytes());
        local.extend_from_slice(name);
        local.extend_from_slice(data);
        let mut cd = Vec::new();
        cd.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        cd.extend_from_slice(&20u16.to_le_bytes());
        cd.extend_from_slice(&20u16.to_le_bytes());
        cd.extend_from_slice(&flags.to_le_bytes());
        cd.extend_from_slice(&method.to_le_bytes());
        cd.extend_from_slice(&[0; 4]);
        cd.extend_from_slice(&crc.to_le_bytes());
        cd.extend_from_slice(&(data.len() as u32).to_le_bytes());
        cd.extend_from_slice(&(data.len() as u32).to_le_bytes());
        cd.extend_from_slice(&(name.len() as u16).to_le_bytes());
        cd.extend_from_slice(&[0; 12]); // extra, comment, disk, internal attrs, external attrs
        cd.extend_from_slice(&0u32.to_le_bytes()); // local header offset
        cd.extend_from_slice(name);
        let mut out = local;
        let cd_at = out.len() as u32;
        out.extend_from_slice(&cd);
        out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        out.extend_from_slice(&[0; 4]);
        out.extend_from_slice(&1u16.to_le_bytes());
        out.extend_from_slice(&1u16.to_le_bytes());
        out.extend_from_slice(&(cd.len() as u32).to_le_bytes());
        out.extend_from_slice(&cd_at.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out
    }

    #[test]
    fn legacy_names_detection_and_code_page() {
        let root = temp("legacy");
        // "中文.txt" in GBK, no UTF-8 flag
        let gbk = [0xD6u8, 0xD0, 0xCE, 0xC4, b'.', b't', b'x', b't'];
        let archive = root.join("gbk.zip");
        fs::write(&archive, raw_zip(&gbk, 0, 0, b"hi")).unwrap();
        let i = info(&archive).unwrap();
        assert!(i.undetermined && !i.encrypted && !i.unsupported);
        assert_eq!(i.detected, 936);
        let target = root.join("t");
        extract(&archive, &target, 936, &Progress::default(), &AtomicBool::new(false)).unwrap();
        assert_eq!(fs::read(target.join("中文.txt")).unwrap(), b"hi");
        // "テスト.txt" in Shift-JIS: kana make 932 the guess
        let sjis = [0x83u8, 0x65, 0x83, 0x58, 0x83, 0x67, b'.', b't', b'x', b't'];
        let archive = root.join("sjis.zip");
        fs::write(&archive, raw_zip(&sjis, 0, 0, b"x")).unwrap();
        assert_eq!(info(&archive).unwrap().detected, 932);
        extract(&archive, &target, 932, &Progress::default(), &AtomicBool::new(false)).unwrap();
        assert!(target.join("テスト.txt").exists());
        // ASCII or flagged UTF-8 names are determined; encryption and other methods are reported
        let archive = root.join("plain.zip");
        fs::write(&archive, raw_zip("文.txt".as_bytes(), 0x0801, 14, b"x")).unwrap();
        let i = info(&archive).unwrap();
        assert!(!i.undetermined && i.encrypted && i.unsupported);
        fs::remove_dir_all(&root).unwrap();
    }
}
