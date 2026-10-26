//! Preview pane helpers: decide whether a file is text (and how it's encoded), list zip archives.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
#[repr(u8)]
pub enum TextKind {
    Binary = 0,
    Utf8 = 1,
    Utf16Le = 2,
    Utf16Be = 3,
    /// Not valid UTF-8 but looks like text: the caller decodes with the system ANSI code page (GBK here)
    Ansi = 4,
}

/// Classify the first bytes of a file. Returns the kind and the payload with any BOM stripped.
pub fn sniff(bytes: &[u8], truncated: bool) -> (TextKind, &[u8]) {
    if let Some(rest) = bytes.strip_prefix(&[0xEF, 0xBB, 0xBF]) {
        return (TextKind::Utf8, rest);
    }
    if let Some(rest) = bytes.strip_prefix(&[0xFF, 0xFE]) {
        return (TextKind::Utf16Le, rest);
    }
    if let Some(rest) = bytes.strip_prefix(&[0xFE, 0xFF]) {
        return (TextKind::Utf16Be, rest);
    }
    let head = &bytes[..bytes.len().min(8192)];
    if head.contains(&0) {
        return (TextKind::Binary, bytes);
    }
    // Control characters other than tab/CR/LF/FF/ESC are rare in text
    let controls = head.iter().filter(|&&b| b < 0x20 && !matches!(b, b'\t' | b'\n' | b'\r' | 0x0C | 0x1B)).count();
    if controls * 100 > head.len().max(1) * 2 {
        return (TextKind::Binary, bytes);
    }
    // A read cut off mid-character: drop up to 3 trailing bytes of an incomplete UTF-8 sequence
    let mut body = bytes;
    if truncated {
        match std::str::from_utf8(body) {
            Err(e) if e.error_len().is_none() => body = &body[..e.valid_up_to()],
            _ => {}
        }
    }
    if std::str::from_utf8(body).is_ok() {
        (TextKind::Utf8, body)
    } else {
        (TextKind::Ansi, bytes)
    }
}

pub fn read_head(path: &Path, limit: usize) -> io::Result<(Vec<u8>, bool)> {
    let mut file = File::open(path)?;
    let mut bytes = Vec::with_capacity(limit.min(1 << 20));
    file.by_ref().take(limit as u64 + 1).read_to_end(&mut bytes)?;
    let truncated = bytes.len() > limit;
    bytes.truncate(limit);
    Ok((bytes, truncated))
}

#[derive(Debug, PartialEq, Eq)]
pub struct ZipEntry {
    pub name: Vec<u8>,
    pub utf8: bool, // general purpose flag bit 11; otherwise the OEM/ANSI code page
    pub size: u64,
}

fn u16_at(b: &[u8], i: usize) -> Option<u16> {
    b.get(i..i + 2).map(|s| u16::from_le_bytes([s[0], s[1]]))
}
fn u32_at(b: &[u8], i: usize) -> Option<u32> {
    b.get(i..i + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}
fn u64_at(b: &[u8], i: usize) -> Option<u64> {
    b.get(i..i + 8).map(|s| u64::from_le_bytes(s.try_into().unwrap()))
}

fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_string())
}

/// Reads the central directory only (no decompression). `max` caps the number of entries returned;
/// the second value is the archive's total entry count.
pub fn list_zip<R: Read + Seek>(r: &mut R, max: usize) -> io::Result<(Vec<ZipEntry>, u64)> {
    let len = r.seek(SeekFrom::End(0))?;
    // End of central directory: 22 bytes + up to 64 KiB comment
    let tail_len = len.min(22 + 65535) as usize;
    let mut tail = vec![0u8; tail_len];
    r.seek(SeekFrom::Start(len - tail_len as u64))?;
    r.read_exact(&mut tail)?;
    let eocd = (0..=tail_len.saturating_sub(22)).rev().find(|&i| u32_at(&tail, i) == Some(0x0605_4b50)).ok_or_else(|| bad("no end of central directory"))?;
    let mut total = u64::from(u16_at(&tail, eocd + 10).unwrap());
    let mut cd_size = u64::from(u32_at(&tail, eocd + 12).unwrap());
    let mut cd_offset = u64::from(u32_at(&tail, eocd + 16).unwrap());
    // ZIP64 locator sits right before the EOCD
    if eocd >= 20 && u32_at(&tail, eocd - 20) == Some(0x0706_4b50) {
        let at = u64_at(&tail, eocd - 12).unwrap();
        let mut rec = [0u8; 56];
        r.seek(SeekFrom::Start(at))?;
        r.read_exact(&mut rec)?;
        if u32_at(&rec, 0) != Some(0x0606_4b50) {
            return Err(bad("bad zip64 record"));
        }
        total = u64_at(&rec, 32).unwrap();
        cd_size = u64_at(&rec, 40).unwrap();
        cd_offset = u64_at(&rec, 48).unwrap();
    }
    if cd_offset.checked_add(cd_size).map_or(true, |end| end > len) || cd_size > 256 << 20 {
        return Err(bad("central directory out of range"));
    }
    let mut cd = vec![0u8; cd_size as usize];
    r.seek(SeekFrom::Start(cd_offset))?;
    r.read_exact(&mut cd)?;

    let mut out = Vec::new();
    let mut i = 0usize;
    while out.len() < max && u32_at(&cd, i) == Some(0x0201_4b50) {
        let flags = u16_at(&cd, i + 8).unwrap_or(0);
        let mut size = u64::from(u32_at(&cd, i + 24).ok_or_else(|| bad("truncated entry"))?);
        let name_len = usize::from(u16_at(&cd, i + 28).unwrap_or(0));
        let extra_len = usize::from(u16_at(&cd, i + 30).unwrap_or(0));
        let comment_len = usize::from(u16_at(&cd, i + 32).unwrap_or(0));
        let name = cd.get(i + 46..i + 46 + name_len).ok_or_else(|| bad("truncated name"))?.to_vec();
        if size == 0xFFFF_FFFF {
            // ZIP64 extra field (id 1): uncompressed size comes first
            let extra = cd.get(i + 46 + name_len..i + 46 + name_len + extra_len).unwrap_or(&[]);
            let mut j = 0;
            while j + 4 <= extra.len() {
                let id = u16_at(extra, j).unwrap();
                let n = usize::from(u16_at(extra, j + 2).unwrap());
                if id == 1 {
                    size = u64_at(extra, j + 4).unwrap_or(size);
                    break;
                }
                j += 4 + n;
            }
        }
        out.push(ZipEntry { name, utf8: flags & 0x0800 != 0, size });
        i += 46 + name_len + extra_len + comment_len;
    }
    Ok((out, total))
}

/// Files' smart extract test: how many distinct first path segments the entries have (0, 1, or 2 = "more than one").
/// Empty segments, "." and ".." are skipped, like GetFirstMeaningfulSegment.
pub fn zip_top_level<R: Read + Seek>(r: &mut R) -> io::Result<u8> {
    let (entries, _) = list_zip(r, usize::MAX)?;
    let mut first: Option<Vec<u8>> = None;
    for e in &entries {
        let Some(seg) = e.name.split(|&b| b == b'/' || b == b'\\').find(|s| !s.is_empty() && *s != b"." && *s != b"..") else {
            continue;
        };
        match &first {
            None => first = Some(seg.to_vec()),
            Some(f) if f.as_slice() != seg => return Ok(2),
            _ => {}
        }
    }
    Ok(u8::from(first.is_some()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn sniff_kinds() {
        assert_eq!(sniff(b"\xEF\xBB\xBFhi", false), (TextKind::Utf8, &b"hi"[..]));
        assert_eq!(sniff(b"\xFF\xFEh\0i\0", false).0, TextKind::Utf16Le);
        assert_eq!(sniff("fn main() {}\n中文".as_bytes(), false).0, TextKind::Utf8);
        assert_eq!(sniff(b"MZ\x90\0\x03\0\0\0", false).0, TextKind::Binary);
        assert_eq!(sniff(b"\xC4\xE3\xBA\xC3 gbk text\r\n", false).0, TextKind::Ansi);
        // "中" is E4 B8 AD; a read cut after E4 B8 is still UTF-8 text
        let (kind, body) = sniff(b"ab\xE4\xB8", true);
        assert_eq!((kind, body), (TextKind::Utf8, &b"ab"[..]));
    }

    fn build_zip(entries: &[(&str, u32, bool)]) -> Vec<u8> {
        let mut cd = Vec::new();
        for (name, size, utf8) in entries {
            cd.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
            cd.extend_from_slice(&[20, 0, 20, 0]);
            cd.extend_from_slice(&(if *utf8 { 0x0800u16 } else { 0 }).to_le_bytes());
            cd.extend_from_slice(&[0; 10]); // method, time, date, crc
            cd.extend_from_slice(&size.to_le_bytes()); // compressed
            cd.extend_from_slice(&size.to_le_bytes()); // uncompressed
            cd.extend_from_slice(&(name.len() as u16).to_le_bytes());
            cd.extend_from_slice(&[0; 12]); // extra, comment, disk, attrs
            cd.extend_from_slice(&[0; 4]); // local header offset
            cd.extend_from_slice(name.as_bytes());
        }
        let mut zip = vec![0u8; 10]; // stand-in for local headers + data
        let offset = zip.len() as u32;
        zip.extend_from_slice(&cd);
        zip.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        zip.extend_from_slice(&[0; 4]);
        zip.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        zip.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        zip.extend_from_slice(&(cd.len() as u32).to_le_bytes());
        zip.extend_from_slice(&offset.to_le_bytes());
        zip.extend_from_slice(&[3, 0, b'x', b'y', b'z']); // comment
        zip
    }

    #[test]
    fn lists_central_directory() {
        let zip = build_zip(&[("src/", 0, false), ("src/main.rs", 120, true), ("readme.txt", 5, false)]);
        let (entries, total) = list_zip(&mut Cursor::new(zip.clone()), 100).unwrap();
        assert_eq!(total, 3);
        assert_eq!(entries[1], ZipEntry { name: b"src/main.rs".to_vec(), utf8: true, size: 120 });
        let (capped, _) = list_zip(&mut Cursor::new(zip), 2).unwrap();
        assert_eq!(capped.len(), 2);
        assert!(list_zip(&mut Cursor::new(b"not a zip at all, definitely".to_vec()), 10).is_err());
    }

    #[test]
    fn top_level_items() {
        let one = build_zip(&[("src/", 0, false), ("src/main.rs", 1, false), ("./src\\lib.rs", 1, false)]);
        assert_eq!(zip_top_level(&mut Cursor::new(one)).unwrap(), 1);
        let two = build_zip(&[("src/main.rs", 1, false), ("readme.txt", 1, false)]);
        assert_eq!(zip_top_level(&mut Cursor::new(two)).unwrap(), 2);
        let file = build_zip(&[("/report.docx", 1, false)]);
        assert_eq!(zip_top_level(&mut Cursor::new(file)).unwrap(), 1);
        assert_eq!(zip_top_level(&mut Cursor::new(build_zip(&[("../", 0, false)]))).unwrap(), 0);
    }
}
