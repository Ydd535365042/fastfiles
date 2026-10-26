//! Deflate (RFC 1951) for zip archives, standard library only. Inflate handles every block type. Deflate uses LZ77
//! with hash chains and the fixed Huffman code, falling back to stored blocks whenever that would be smaller (photos,
//! video, archives), so incompressible data never grows by more than a few bytes per 64 KB.

use std::io::{self, Read, Write};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

pub fn crc32_update(crc: u32, data: &[u8]) -> u32 {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    let table = TABLE.get_or_init(|| {
        let mut t = [0u32; 256];
        for (i, slot) in t.iter_mut().enumerate() {
            let mut c = i as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
            *slot = c;
        }
        t
    });
    let mut c = !crc;
    for &b in data {
        c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    !c
}

const LEN_BASE: [u16; 29] = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEN_EXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE: [u16; 30] = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const CL_ORDER: [usize; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_string())
}

// ---------------------------------------------------------------------------------------------------------------- inflate

struct BitReader<R: Read> {
    src: R,
    buf: [u8; 65536],
    pos: usize,
    len: usize,
    acc: u64,
    n: u32,
    /// Zero bytes fed past the end (the last code may be read ahead a little); more than a few means truncated data
    pad: u32,
}

impl<R: Read> BitReader<R> {
    fn new(src: R) -> Self {
        BitReader { src, buf: [0; 65536], pos: 0, len: 0, acc: 0, n: 0, pad: 0 }
    }

    fn next_byte(&mut self) -> io::Result<u8> {
        if self.pos == self.len {
            self.len = self.src.read(&mut self.buf)?;
            self.pos = 0;
            if self.len == 0 {
                self.pad += 1;
                if self.pad > 8 {
                    return Err(bad("压缩数据不完整"));
                }
                return Ok(0);
            }
        }
        let b = self.buf[self.pos];
        self.pos += 1;
        Ok(b)
    }

    fn need(&mut self, bits: u32) -> io::Result<()> {
        while self.n < bits {
            self.acc |= (self.next_byte()? as u64) << self.n;
            self.n += 8;
        }
        Ok(())
    }

    fn bits(&mut self, count: u32) -> io::Result<u32> {
        if count == 0 {
            return Ok(0);
        }
        self.need(count)?;
        let v = (self.acc & ((1u64 << count) - 1)) as u32;
        self.acc >>= count;
        self.n -= count;
        Ok(v)
    }

    fn align(&mut self) {
        let drop = self.n % 8;
        self.acc >>= drop;
        self.n -= drop;
    }
}

/// Canonical Huffman decode table indexed by the next 15 bits (LSB first): symbol << 4 | length; 0 = invalid code
struct Huffman {
    table: Vec<u32>,
}

impl Huffman {
    fn new(lengths: &[u8]) -> io::Result<Huffman> {
        let mut count = [0u16; 16];
        for &l in lengths {
            count[l as usize] += 1;
        }
        count[0] = 0;
        let mut left = 1i32;
        for c in &count[1..] {
            left = (left << 1) - *c as i32;
            if left < 0 {
                return Err(bad("压缩数据损坏（霍夫曼表）"));
            }
        }
        let mut next = [0u32; 16];
        let mut code = 0u32;
        for bits in 1..16 {
            code = (code + count[bits - 1] as u32) << 1;
            next[bits] = code;
        }
        let mut table = vec![0u32; 1 << 15];
        for (sym, &len) in lengths.iter().enumerate() {
            if len == 0 {
                continue;
            }
            let len = len as u32;
            let c = next[len as usize];
            next[len as usize] += 1;
            // Codes are stored MSB first in the stream order; the table is indexed by the bits as read (LSB first)
            let mut rev = 0u32;
            for i in 0..len {
                if c & (1 << i) != 0 {
                    rev |= 1 << (len - 1 - i);
                }
            }
            let entry = (sym as u32) << 4 | len;
            let mut i = rev;
            while i < (1 << 15) {
                table[i as usize] = entry;
                i += 1 << len;
            }
        }
        Ok(Huffman { table })
    }

    fn decode<R: Read>(&self, r: &mut BitReader<R>) -> io::Result<u32> {
        r.need(15)?;
        let e = self.table[(r.acc & 0x7FFF) as usize];
        let len = e & 15;
        if len == 0 {
            return Err(bad("压缩数据损坏（无效编码）"));
        }
        r.acc >>= len;
        r.n -= len;
        Ok(e >> 4)
    }
}

fn fixed_tables() -> io::Result<(Huffman, Huffman)> {
    let mut lit = [0u8; 288];
    lit[..144].fill(8);
    lit[144..256].fill(9);
    lit[256..280].fill(7);
    lit[280..].fill(8);
    Ok((Huffman::new(&lit)?, Huffman::new(&[5u8; 30])?))
}

/// Inflates a raw deflate stream into `out`, returning (bytes written, CRC-32). `progress` counts output bytes.
pub fn inflate<R: Read, W: Write>(src: R, out: &mut W, cancel: &AtomicBool, progress: &AtomicU64) -> io::Result<(u64, u32)> {
    const WINDOW: usize = 32768;
    let mut r = BitReader::new(src);
    let mut buf: Vec<u8> = Vec::with_capacity(4 * WINDOW + 512);
    let (mut total, mut crc) = (0u64, 0u32);
    let flush = |buf: &mut Vec<u8>, keep: usize, total: &mut u64, crc: &mut u32, out: &mut W, flushed: &mut usize| -> io::Result<()> {
        // Everything after `flushed` is new; write it, then drop all but the last `keep` bytes (the window)
        let new = &buf[*flushed..];
        out.write_all(new)?;
        *crc = crc32_update(*crc, new);
        *total += new.len() as u64;
        progress.fetch_add(new.len() as u64, Ordering::Relaxed);
        let cut = buf.len().saturating_sub(keep);
        buf.drain(..cut);
        *flushed = buf.len();
        Ok(())
    };
    let mut flushed = 0usize;
    loop {
        if cancel.load(Ordering::Relaxed) {
            return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled"));
        }
        let last = r.bits(1)? == 1;
        match r.bits(2)? {
            0 => {
                r.align();
                let len = r.bits(16)?;
                let nlen = r.bits(16)?;
                if len != !nlen & 0xFFFF {
                    return Err(bad("压缩数据损坏（存储块长度）"));
                }
                for _ in 0..len {
                    let b = r.bits(8)? as u8;
                    buf.push(b);
                }
            }
            kind @ (1 | 2) => {
                let (lit, dist) = if kind == 1 {
                    fixed_tables()?
                } else {
                    let hlit = r.bits(5)? as usize + 257;
                    let hdist = r.bits(5)? as usize + 1;
                    let hclen = r.bits(4)? as usize + 4;
                    let mut cl = [0u8; 19];
                    for &i in CL_ORDER.iter().take(hclen) {
                        cl[i] = r.bits(3)? as u8;
                    }
                    let clh = Huffman::new(&cl)?;
                    let mut lengths = vec![0u8; hlit + hdist];
                    let mut i = 0;
                    while i < lengths.len() {
                        let sym = clh.decode(&mut r)?;
                        let (value, repeat) = match sym {
                            0..=15 => (sym as u8, 1),
                            16 => {
                                if i == 0 {
                                    return Err(bad("压缩数据损坏（重复长度）"));
                                }
                                (lengths[i - 1], 3 + r.bits(2)? as usize)
                            }
                            17 => (0, 3 + r.bits(3)? as usize),
                            _ => (0, 11 + r.bits(7)? as usize),
                        };
                        if i + repeat > lengths.len() {
                            return Err(bad("压缩数据损坏（长度表）"));
                        }
                        lengths[i..i + repeat].fill(value);
                        i += repeat;
                    }
                    (Huffman::new(&lengths[..hlit])?, Huffman::new(&lengths[hlit..])?)
                };
                loop {
                    let sym = lit.decode(&mut r)?;
                    if sym < 256 {
                        buf.push(sym as u8);
                    } else if sym == 256 {
                        break;
                    } else {
                        let li = (sym - 257) as usize;
                        if li >= 29 {
                            return Err(bad("压缩数据损坏（长度码）"));
                        }
                        let len = LEN_BASE[li] as usize + r.bits(LEN_EXTRA[li] as u32)? as usize;
                        let di = dist.decode(&mut r)? as usize;
                        if di >= 30 {
                            return Err(bad("压缩数据损坏（距离码）"));
                        }
                        let d = DIST_BASE[di] as usize + r.bits(DIST_EXTRA[di] as u32)? as usize;
                        if d > buf.len() {
                            return Err(bad("压缩数据损坏（距离超出）"));
                        }
                        let start = buf.len() - d;
                        for k in 0..len {
                            let b = buf[start + k];
                            buf.push(b);
                        }
                    }
                    if buf.len() >= 3 * WINDOW {
                        flush(&mut buf, WINDOW, &mut total, &mut crc, out, &mut flushed)?;
                    }
                }
            }
            _ => return Err(bad("压缩数据损坏（块类型）")),
        }
        if buf.len() >= 3 * WINDOW {
            flush(&mut buf, WINDOW, &mut total, &mut crc, out, &mut flushed)?;
        }
        if last {
            flush(&mut buf, 0, &mut total, &mut crc, out, &mut flushed)?;
            return Ok((total, crc));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------- deflate

struct BitWriter {
    out: Vec<u8>,
    acc: u64,
    n: u32,
}

impl BitWriter {
    fn put(&mut self, value: u32, count: u32) {
        self.acc |= (value as u64) << self.n;
        self.n += count;
        while self.n >= 8 {
            self.out.push(self.acc as u8);
            self.acc >>= 8;
            self.n -= 8;
        }
    }
    /// Huffman codes go most significant bit first
    fn code(&mut self, code: u32, len: u32) {
        let mut rev = 0;
        for i in 0..len {
            if code & (1 << i) != 0 {
                rev |= 1 << (len - 1 - i);
            }
        }
        self.put(rev, len);
    }
    fn align(&mut self) {
        if self.n > 0 {
            self.out.push(self.acc as u8);
            self.acc = 0;
            self.n = 0;
        }
    }
}

fn fixed_lit(sym: u32) -> (u32, u32) {
    match sym {
        0..=143 => (0x30 + sym, 8),
        144..=255 => (0x190 + sym - 144, 9),
        256..=279 => (sym - 256, 7),
        _ => (0xC0 + sym - 280, 8),
    }
}

fn len_code(len: usize) -> usize {
    LEN_BASE.iter().rposition(|&b| b as usize <= len).unwrap()
}
fn dist_code(d: usize) -> usize {
    DIST_BASE.iter().rposition(|&b| b as usize <= d).unwrap()
}

const WSIZE: usize = 32768;
const CHUNK: usize = 65536;
const HASH_BITS: u32 = 15;
const MAX_CHAIN: usize = 48;

/// Streaming compressor: feed input with `write_input`, then `finish`. Output is raw deflate.
pub struct Deflater {
    buf: Vec<u8>,
    base: usize,
    head: Vec<i64>,
    prev: Vec<i64>,
    bits: BitWriter,
    /// buf[..pending_start] is history only (already compressed); the rest waits for the next block
    pending_start: usize,
}

impl Default for Deflater {
    fn default() -> Self {
        Self::new()
    }
}

impl Deflater {
    pub fn new() -> Self {
        Deflater {
            buf: Vec::with_capacity(WSIZE + CHUNK),
            base: 0,
            head: vec![-1; 1 << HASH_BITS],
            prev: vec![-1; WSIZE],
            bits: BitWriter { out: Vec::new(), acc: 0, n: 0 },
            pending_start: 0,
        }
    }

    fn hash(b: &[u8]) -> usize {
        let v = (b[0] as u32) | (b[1] as u32) << 8 | (b[2] as u32) << 16;
        (v.wrapping_mul(0x9E37_79B1) >> (32 - HASH_BITS)) as usize
    }

    fn insert(&mut self, i: usize) {
        if i + 3 > self.buf.len() {
            return;
        }
        let h = Self::hash(&self.buf[i..]);
        let abs = (self.base + i) as i64;
        self.prev[(abs as usize) & (WSIZE - 1)] = self.head[h];
        self.head[h] = abs;
    }

    fn longest(&self, i: usize, end: usize) -> (usize, usize) {
        let max = 258.min(end - i);
        if max < 3 {
            return (0, 0);
        }
        let abs = (self.base + i) as i64;
        let mut cand = self.head[Self::hash(&self.buf[i..])];
        let (mut best, mut best_d) = (0, 0);
        let mut chain = 0;
        while cand >= 0 && chain < MAX_CHAIN {
            let d = (abs - cand) as usize;
            if d == 0 || d > WSIZE || cand < self.base as i64 {
                break;
            }
            let j = (cand as usize) - self.base;
            if self.buf[j + best] == self.buf[i + best] {
                let mut l = 0;
                while l < max && self.buf[j + l] == self.buf[i + l] {
                    l += 1;
                }
                if l > best {
                    best = l;
                    best_d = d;
                    if l == max {
                        break;
                    }
                }
            }
            cand = self.prev[(cand as usize) & (WSIZE - 1)];
            chain += 1;
        }
        if best >= 3 { (best, best_d) } else { (0, 0) }
    }

    /// Compresses buf[start..end] as one block; output bytes are appended to the internal buffer
    fn block(&mut self, start: usize, end: usize, last: bool) {
        // Tokens: (literal byte or length, distance; 0 = literal)
        let mut tokens: Vec<(u16, u16)> = Vec::with_capacity(end - start);
        let mut cost = 3u64; // block header
        let mut i = start;
        while i < end {
            let (len, d) = self.longest(i, end);
            if len >= 3 {
                let lc = len_code(len);
                let dc = dist_code(d);
                cost += fixed_lit(257 + lc as u32).1 as u64 + LEN_EXTRA[lc] as u64 + 5 + DIST_EXTRA[dc] as u64;
                tokens.push((len as u16, d as u16));
                for k in i..i + len {
                    self.insert(k);
                }
                i += len;
            } else {
                cost += fixed_lit(self.buf[i] as u32).1 as u64;
                tokens.push((self.buf[i] as u16, 0));
                self.insert(i);
                i += 1;
            }
        }
        cost += 7; // end of block
        let stored = 8 * (end - start) as u64 + 40;
        if cost >= stored {
            let mut p = start;
            loop {
                let n = (end - p).min(65535);
                let fin = last && p + n == end;
                self.bits.put(u32::from(fin), 1);
                self.bits.put(0, 2);
                self.bits.align();
                self.bits.out.extend_from_slice(&(n as u16).to_le_bytes());
                self.bits.out.extend_from_slice(&(!(n as u16)).to_le_bytes());
                self.bits.out.extend_from_slice(&self.buf[p..p + n]);
                p += n;
                if p >= end {
                    break;
                }
            }
            return;
        }
        self.bits.put(u32::from(last), 1);
        self.bits.put(1, 2);
        for (v, d) in tokens {
            if d == 0 {
                let (c, l) = fixed_lit(v as u32);
                self.bits.code(c, l);
            } else {
                let lc = len_code(v as usize);
                let (c, l) = fixed_lit(257 + lc as u32);
                self.bits.code(c, l);
                self.bits.put(v as u32 - LEN_BASE[lc] as u32, LEN_EXTRA[lc] as u32);
                let dc = dist_code(d as usize);
                self.bits.code(dc as u32, 5);
                self.bits.put(d as u32 - DIST_BASE[dc] as u32, DIST_EXTRA[dc] as u32);
            }
        }
        let (c, l) = fixed_lit(256);
        self.bits.code(c, l);
    }

    /// Adds input; returns compressed bytes produced so far (may be empty)
    pub fn write_input(&mut self, mut data: &[u8]) -> Vec<u8> {
        while !data.is_empty() {
            let have = self.buf.len() - self.window_len();
            let take = (CHUNK - have).min(data.len());
            self.buf.extend_from_slice(&data[..take]);
            data = &data[take..];
            if self.buf.len() - self.window_len() == CHUNK {
                let start = self.window_len();
                let end = self.buf.len();
                self.block(start, end, false);
                self.slide();
            }
        }
        std::mem::take(&mut self.bits.out)
    }

    /// Bytes at the front of buf kept only as match history
    fn window_len(&self) -> usize {
        self.pending_start
    }

    fn slide(&mut self) {
        let keep = WSIZE.min(self.buf.len());
        let cut = self.buf.len() - keep;
        self.buf.drain(..cut);
        self.base += cut;
        self.pending_start = self.buf.len();
    }

    pub fn finish(mut self) -> Vec<u8> {
        let start = self.window_len();
        let end = self.buf.len();
        self.block(start, end, true);
        self.bits.align();
        self.bits.out
    }
}
