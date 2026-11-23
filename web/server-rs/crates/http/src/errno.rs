//! The error numbers `io::Error::raw_os_error` gives for what the server tells apart: errno on Unix, and on Windows
//! the Winsock code for a socket call's and the Win32 code for a file's (EISDIR: reading a folder is "access denied"
//! there, so nothing says it; EPERM: Winsock's own "permission denied", a bind to a port the system keeps).

#[cfg(unix)]
pub use libc::{EACCES, EADDRINUSE, EADDRNOTAVAIL, EAFNOSUPPORT, EINVAL, EISDIR, EMFILE, ENFILE, ENOBUFS, ENOENT, ENOMEM, EPERM};

#[cfg(windows)]
mod win {
    pub const ENOENT: i32 = 2; // ERROR_FILE_NOT_FOUND
    pub const ENFILE: i32 = 4; // ERROR_TOO_MANY_OPEN_FILES
    pub const EACCES: i32 = 5; // ERROR_ACCESS_DENIED
    pub const ENOMEM: i32 = 8; // ERROR_NOT_ENOUGH_MEMORY
    pub const EISDIR: i32 = -1;
    pub const EPERM: i32 = 10013; // WSAEACCES
    pub const EINVAL: i32 = 10022; // WSAEINVAL
    pub const EMFILE: i32 = 10024; // WSAEMFILE
    pub const EAFNOSUPPORT: i32 = 10047; // WSAEAFNOSUPPORT
    pub const EADDRINUSE: i32 = 10048; // WSAEADDRINUSE
    pub const EADDRNOTAVAIL: i32 = 10049; // WSAEADDRNOTAVAIL
    pub const ENOBUFS: i32 = 10055; // WSAENOBUFS
}
#[cfg(windows)]
pub use win::*;
