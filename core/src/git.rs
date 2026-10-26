//! Git repository detection for the status bar (Files shows the branch of the folder you're in). Reads `.git/HEAD`
//! directly, so it costs a few small file reads and needs no git installation.

use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, PartialEq)]
pub struct Repo {
    /// Working tree root (the folder that holds `.git`)
    pub root: PathBuf,
    /// Branch name, or the abbreviated commit when HEAD is detached
    pub head: String,
    pub detached: bool,
    /// The `.git` folder (for worktrees: the worktree's own git dir)
    pub git_dir: PathBuf,
}

/// Repository containing `path`, looking up through its parents.
pub fn find(path: &Path) -> Option<Repo> {
    for dir in path.ancestors().take(64) {
        let dot = dir.join(".git");
        let git_dir = if dot.is_dir() {
            dot
        } else if dot.is_file() {
            // Worktrees and submodules: ".git" is a file with "gitdir: <path>"
            let text = fs::read_to_string(&dot).ok()?;
            let target = text.lines().find_map(|l| l.strip_prefix("gitdir:"))?.trim();
            let target = Path::new(target);
            if target.is_absolute() { target.to_path_buf() } else { dir.join(target) }
        } else {
            continue;
        };
        let (head, detached) = read_head(&git_dir)?;
        return Some(Repo { root: dir.to_path_buf(), head, detached, git_dir });
    }
    None
}

fn read_head(git_dir: &Path) -> Option<(String, bool)> {
    let text = fs::read_to_string(git_dir.join("HEAD")).ok()?;
    let line = text.lines().next()?.trim();
    if let Some(name) = line.strip_prefix("ref:") {
        let name = name.trim();
        let short = name.strip_prefix("refs/heads/").unwrap_or(name);
        return (!short.is_empty()).then(|| (short.to_string(), false));
    }
    (line.len() >= 7 && line.bytes().all(|b| b.is_ascii_hexdigit())).then(|| (line[..7].to_string(), true))
}

/// Local branch names (loose refs and packed-refs), sorted and de-duplicated.
pub fn branches(repo: &Repo) -> Vec<String> {
    // Worktrees keep refs in the main repository: "commondir" points there
    let common = fs::read_to_string(repo.git_dir.join("commondir"))
        .ok()
        .map(|c| {
            let p = Path::new(c.trim());
            if p.is_absolute() { p.to_path_buf() } else { repo.git_dir.join(p) }
        })
        .unwrap_or_else(|| repo.git_dir.clone());
    let mut out = Vec::new();
    collect_loose(&common.join("refs").join("heads"), "", &mut out);
    if let Ok(packed) = fs::read_to_string(common.join("packed-refs")) {
        for line in packed.lines() {
            if let Some((_, name)) = line.split_once(' ') {
                if let Some(branch) = name.trim().strip_prefix("refs/heads/") {
                    out.push(branch.to_string());
                }
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

fn collect_loose(dir: &Path, prefix: &str, out: &mut Vec<String>) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let full = if prefix.is_empty() { name } else { format!("{prefix}/{name}") };
        match entry.file_type() {
            Ok(t) if t.is_dir() => collect_loose(&entry.path(), &full, out),
            Ok(t) if t.is_file() => out.push(full),
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ff-git-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn finds_branch_from_a_subfolder() {
        let root = temp_dir("branch");
        fs::create_dir_all(root.join(".git")).unwrap();
        fs::write(root.join(".git").join("HEAD"), "ref: refs/heads/feature/x\n").unwrap();
        let sub = root.join("src").join("deep");
        fs::create_dir_all(&sub).unwrap();
        let heads = root.join(".git").join("refs").join("heads");
        fs::create_dir_all(heads.join("feature")).unwrap();
        fs::write(heads.join("feature").join("x"), "0123456789abcdef0123456789abcdef01234567\n").unwrap();
        fs::write(heads.join("main"), "0123456789abcdef0123456789abcdef01234567\n").unwrap();
        fs::write(root.join(".git").join("packed-refs"), "# pack-refs\nabc refs/heads/old\nabc refs/heads/main\nabc refs/tags/v1\n").unwrap();
        let repo = find(&sub).unwrap();
        assert_eq!(repo, Repo { root: root.clone(), head: "feature/x".into(), detached: false, git_dir: root.join(".git") });
        assert_eq!(branches(&repo), vec!["feature/x", "main", "old"]);
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn detached_head_and_gitdir_file() {
        let root = temp_dir("detached");
        let real = root.join("real-git");
        fs::create_dir_all(&real).unwrap();
        fs::write(real.join("HEAD"), "0123456789abcdef0123456789abcdef01234567\n").unwrap();
        let tree = root.join("tree");
        fs::create_dir_all(&tree).unwrap();
        fs::write(tree.join(".git"), "gitdir: ../real-git\n").unwrap();
        let repo = find(&tree).unwrap();
        assert_eq!(repo.head, "0123456");
        assert!(repo.detached);
        assert!(find(&root).is_none() || find(&root).unwrap().root != root);
        fs::remove_dir_all(&root).unwrap();
    }
}
