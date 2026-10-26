use std::fs;
use std::io::{self, Write};
use std::path::Path;

pub fn read(path: &Path) -> io::Result<Vec<u8>> {
    fs::read(path)
}

/// Write to `<path>.tmp`, flush to disk, then rename over the target so a crash never leaves a half-written file.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = Path::new(&tmp);
    {
        let mut file = fs::File::create(tmp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
    }
    fs::rename(tmp, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("fastfiles-core-test-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn roundtrip_creates_parent_and_replaces() {
        let dir = temp_dir("roundtrip");
        let file = dir.join("nested").join("session.txt");
        write_atomic(&file, b"first").unwrap();
        write_atomic(&file, "tab=中文\n".as_bytes()).unwrap();
        assert_eq!(read(&file).unwrap(), "tab=中文\n".as_bytes());
        assert!(!file.with_extension("txt.tmp").exists());
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn missing_file_is_error() {
        let dir = temp_dir("missing");
        assert!(read(&dir.join("nope.txt")).is_err());
    }
}
