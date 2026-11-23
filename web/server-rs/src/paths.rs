//! Node's `path` (posix) on the strings the server keeps its paths in: `resolve`, `join`, `dirname`, `basename`,
//! `extname`, all lexical (no file system), as the Node server used them.  Paths are strings here: they go into
//! console lines, die messages and ETags (a file's ETag hashes its real path) as the Node server wrote them.

/// `path.posix.normalize` of an absolute path: "." and ".." resolved, repeated and trailing slashes gone ("/" stays)
pub fn normalize_abs(p: &str) -> String {
    #[cfg(windows)]
    {
        let p = native(p);
        if let Some(d) = drive(&p) {
            return format!("{d}{}", normalize_posix(&p[2..]));
        }
        normalize_posix(&p)
    }
    #[cfg(not(windows))]
    normalize_posix(p)
}

/// Windows: a path of the system's as the server keeps its paths: "C:/a/b" (forward slashes, which Windows takes
/// too; no "\\?\" prefix of a canonical path).  Elsewhere: as it is.
pub fn native(p: &str) -> String {
    #[cfg(windows)]
    {
        let p = p.strip_prefix(r"\\?\").unwrap_or(p);
        p.replace('\\', "/")
    }
    #[cfg(not(windows))]
    p.to_string()
}

/// `fs.realpathSync(p)`, as the server keeps its paths
pub fn real(p: &str) -> std::io::Result<String> {
    std::fs::canonicalize(p).map(|r| native(&r.to_string_lossy()))
}

/// "C:" of "C:/a" (Windows)
#[cfg(windows)]
fn drive(p: &str) -> Option<&str> {
    let b = p.as_bytes();
    (b.len() >= 2 && b[0].is_ascii_alphabetic() && b[1] == b':').then(|| &p[..2])
}

/// whether `resolve` takes p as it is: from the root (Windows: or from a drive, "C:/a" or "C:\a")
fn is_abs(p: &str) -> bool {
    #[cfg(windows)]
    if drive(p).is_some() || p.starts_with('\\') {
        return true;
    }
    p.starts_with('/')
}

fn normalize_posix(p: &str) -> String {
    let mut parts: Vec<&str> = Vec::new();
    for seg in p.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            s => parts.push(s),
        }
    }
    let mut out = String::with_capacity(p.len());
    for s in &parts {
        out.push('/');
        out.push_str(s);
    }
    if out.is_empty() {
        out.push('/');
    }
    out
}

/// `path.resolve(base, p)` with an absolute base: p itself when it is absolute
pub fn resolve(base: &str, p: &str) -> String {
    if is_abs(p) { normalize_abs(p) } else { normalize_abs(&format!("{base}/{p}")) }
}

/// `path.join(a, b)` for an absolute a
pub fn join(a: &str, b: &str) -> String {
    normalize_abs(&format!("{a}/{b}"))
}

/// `path.posix.dirname(p)` of a normalized path
pub fn dirname(p: &str) -> String {
    let t = p.trim_end_matches('/');
    if t.is_empty() {
        return if p.starts_with('/') { "/".into() } else { ".".into() };
    }
    match t.rfind('/') {
        None => ".".into(),
        Some(i) => {
            let d = t[..i].trim_end_matches('/');
            #[cfg(windows)]
            if drive(d).is_some() && d.len() == 2 {
                return format!("{d}/");
            }
            if d.is_empty() { "/".into() } else { d.to_string() }
        }
    }
}

/// `path.posix.basename(p)`
pub fn basename(p: &str) -> String {
    let t = p.trim_end_matches('/');
    match t.rfind('/') {
        Some(i) => t[i + 1..].to_string(),
        None => t.to_string(),
    }
}

/// `path.posix.extname(p)`: from the last "." of the last part, "" when that part has none, starts with it, or is ".."
pub fn extname(p: &str) -> &str {
    let t = p.trim_end_matches('/');
    let base = match t.rfind('/') {
        Some(i) => &t[i + 1..],
        None => t,
    };
    if base == ".." {
        return "";
    }
    match base.rfind('.') {
        Some(0) | None => "",
        Some(i) => &base[i..],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn as_node_path_does_it() {
        assert_eq!(normalize_abs("/a//b/./c/../d/"), "/a/b/d");
        assert_eq!(normalize_abs("/../.."), "/");
        assert_eq!(resolve("/home/x", "web/../out"), "/home/x/out");
        assert_eq!(resolve("/home/x", "/etc/"), "/etc");
        assert_eq!(resolve("/home/x", ""), "/home/x");
        assert_eq!(join("/", "x"), "/x");
        assert_eq!(join("/a", ""), "/a");
        assert_eq!(dirname("/a/b/c"), "/a/b");
        assert_eq!(dirname("/a/b/"), "/a");
        assert_eq!(dirname("/a"), "/");
        assert_eq!(dirname("/"), "/");
        assert_eq!(basename("/a/b.txt"), "b.txt");
        assert_eq!(basename("/a/b/"), "b");
        assert_eq!(extname("/a/b.HTML"), ".HTML");
        assert_eq!(extname("/a/.bashrc"), "");
        assert_eq!(extname("/a/b."), ".");
        assert_eq!(extname("/a.b/c"), "");
        assert_eq!(extname("/x/a.tar.gz"), ".gz");
        assert_eq!(extname("/x/.."), "");
    }
}
