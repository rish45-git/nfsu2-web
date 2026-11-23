//! The file-system calls the server makes that Unix and Windows spell differently (the Windows build: a server a
//! player runs on their own PC for their friends).  On Unix these are std's own (`std::os::unix::fs`); on Windows the
//! same names with what Windows has: a file's mode is not set (the state folder is the user's own, under their
//! profile's rights), a file has no device and inode numbers to tell (0, 0), a positioned read is `seek_read`, and
//! nothing is a socket file.

#[cfg(unix)]
pub use std::os::unix::fs::{DirBuilderExt, FileExt, FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt};

/// `open(2)`'s O_NOFOLLOW and O_NONBLOCK for `OpenOptionsExt::custom_flags` (0 on Windows: no such flags)
#[cfg(unix)]
pub const O_NOFOLLOW: i32 = libc::O_NOFOLLOW;
#[cfg(unix)]
pub const O_NONBLOCK: i32 = libc::O_NONBLOCK;
#[cfg(windows)]
pub const O_NOFOLLOW: i32 = 0;
#[cfg(windows)]
pub const O_NONBLOCK: i32 = 0;

#[cfg(windows)]
mod win {
    use std::io;
    use std::time::UNIX_EPOCH;

    pub trait OpenOptionsExt {
        fn mode(&mut self, mode: u32) -> &mut Self;
        fn custom_flags(&mut self, flags: i32) -> &mut Self;
    }
    impl OpenOptionsExt for std::fs::OpenOptions {
        fn mode(&mut self, _mode: u32) -> &mut Self {
            self
        }
        fn custom_flags(&mut self, _flags: i32) -> &mut Self {
            self
        }
    }

    pub trait DirBuilderExt {
        fn mode(&mut self, mode: u32) -> &mut Self;
    }
    impl DirBuilderExt for std::fs::DirBuilder {
        fn mode(&mut self, _mode: u32) -> &mut Self {
            self
        }
    }

    pub trait MetadataExt {
        fn dev(&self) -> u64;
        fn ino(&self) -> u64;
        fn uid(&self) -> u32;
        fn mode(&self) -> u32;
        fn size(&self) -> u64;
        fn mtime(&self) -> i64;
        fn mtime_nsec(&self) -> i64;
    }
    impl MetadataExt for std::fs::Metadata {
        fn dev(&self) -> u64 {
            0
        }
        fn ino(&self) -> u64 {
            0
        }
        fn uid(&self) -> u32 {
            0
        }
        fn mode(&self) -> u32 {
            if self.is_dir() { 0o700 } else { 0o600 }
        }
        fn size(&self) -> u64 {
            self.len()
        }
        fn mtime(&self) -> i64 {
            match self.modified().ok().map(|t| t.duration_since(UNIX_EPOCH)) {
                Some(Ok(d)) => d.as_secs() as i64,
                _ => 0,
            }
        }
        fn mtime_nsec(&self) -> i64 {
            match self.modified().ok().map(|t| t.duration_since(UNIX_EPOCH)) {
                Some(Ok(d)) => d.subsec_nanos() as i64,
                _ => 0,
            }
        }
    }

    pub trait FileExt {
        fn read_at(&self, buf: &mut [u8], offset: u64) -> io::Result<usize>;
    }
    impl FileExt for std::fs::File {
        fn read_at(&self, buf: &mut [u8], offset: u64) -> io::Result<usize> {
            std::os::windows::fs::FileExt::seek_read(self, buf, offset)
        }
    }

    pub trait FileTypeExt {
        fn is_socket(&self) -> bool;
    }
    impl FileTypeExt for std::fs::FileType {
        fn is_socket(&self) -> bool {
            false
        }
    }
}
#[cfg(windows)]
pub use win::{DirBuilderExt, FileExt, FileTypeExt, MetadataExt, OpenOptionsExt};

/// The bytes free for this user on the file system `dir` is on; None where that cannot be told.
pub fn disk_free(dir: &std::path::Path) -> Option<u64> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        let p = std::ffi::CString::new(dir.as_os_str().as_bytes()).ok()?;
        // SAFETY: statvfs only writes the struct it is given, and the path is a C string
        let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
        (unsafe { libc::statvfs(p.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as u64 * s.f_frsize as u64)
    }
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetDiskFreeSpaceExW(dir: *const u16, free_to_caller: *mut u64, total: *mut u64, free: *mut u64) -> i32;
        }
        let w: Vec<u16> = dir.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
        let mut free = 0u64;
        // SAFETY: a NUL-terminated wide path, and the call only writes the u64 it is given
        (unsafe { GetDiskFreeSpaceExW(w.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut()) } != 0).then_some(free)
    }
}
