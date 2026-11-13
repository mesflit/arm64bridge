//! Minimal ZIP reader (enough to pull native libraries out of an APK).
// SPDX-License-Identifier: GPL-2.0-or-later
use crate::zsys;

fn rd16(d: &[u8], p: usize) -> u16 {
    u16::from_le_bytes([d[p], d[p + 1]])
}
fn rd32(d: &[u8], p: usize) -> u32 {
    u32::from_le_bytes([d[p], d[p + 1], d[p + 2], d[p + 3]])
}

struct Mapped {
    data: *const u8,
    size: usize,
}
impl Drop for Mapped {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.data as *mut _, self.size) };
    }
}
impl Mapped {
    fn open(path: &str) -> Option<Mapped> {
        unsafe {
            let cpath = crate::util::cstring(path);
            let fd = libc::open(cpath.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC);
            if fd < 0 {
                return None;
            }
            let mut st: libc::stat = std::mem::zeroed();
            if libc::fstat(fd, &mut st) != 0 || st.st_size < 22 {
                libc::close(fd);
                return None;
            }
            let p = libc::mmap(std::ptr::null_mut(), st.st_size as usize, libc::PROT_READ, libc::MAP_PRIVATE, fd, 0);
            libc::close(fd);
            if p == libc::MAP_FAILED {
                return None;
            }
            Some(Mapped { data: p as *const u8, size: st.st_size as usize })
        }
    }
    fn bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.data, self.size) }
    }
}

struct Entry<'a> {
    name: &'a [u8],
    method: u16,
    csize: u32,
    usize: u32,
    lho: u32,
}

// Calls f for each entry; stops when f returns true. False if the archive is malformed.
fn for_each_entry(z: &[u8], mut f: impl FnMut(&Entry) -> bool) -> bool {
    let min_pos = z.len().saturating_sub(65557);
    let mut eocd = None;
    let mut i = z.len() - 22;
    loop {
        if rd32(z, i) == 0x06054b50 {
            eocd = Some(i);
            break;
        }
        if i == min_pos {
            break;
        }
        i -= 1;
    }
    let Some(eocd) = eocd else { return false };
    let count = rd16(z, eocd + 10) as u32;
    let mut p = rd32(z, eocd + 16) as usize;
    for _ in 0..count {
        if p + 46 > z.len() || rd32(z, p) != 0x02014b50 {
            return false;
        }
        let (nlen, elen, clen) = (rd16(z, p + 28) as usize, rd16(z, p + 30) as usize, rd16(z, p + 32) as usize);
        if p + 46 + nlen > z.len() {
            return false;
        }
        let e = Entry { name: &z[p + 46..p + 46 + nlen], method: rd16(z, p + 10), csize: rd32(z, p + 20), usize: rd32(z, p + 24), lho: rd32(z, p + 42) };
        if f(&e) {
            return true;
        }
        p += 46 + nlen + elen + clen;
    }
    true
}

/// Names of all entries of the archive.
pub fn zip_list(zip_path: &str) -> Option<Vec<String>> {
    let z = Mapped::open(zip_path)?;
    let mut names = Vec::new();
    if !for_each_entry(z.bytes(), |e| {
        names.push(String::from_utf8_lossy(e.name).into_owned());
        false
    }) {
        return None;
    }
    Some(names)
}

/// Contents of one entry (stored or deflated).
pub fn zip_extract(zip_path: &str, entry: &str) -> Option<Vec<u8>> {
    let m = Mapped::open(zip_path)?;
    let z = m.bytes();
    let mut result = None;
    for_each_entry(z, |e| {
        if e.name != entry.as_bytes() {
            return false;
        }
        let lho = e.lho as usize;
        if lho + 30 > z.len() || rd32(z, lho) != 0x04034b50 {
            return true;
        }
        let data_off = lho + 30 + rd16(z, lho + 26) as usize + rd16(z, lho + 28) as usize;
        let (csize, usize) = (e.csize as usize, e.usize as usize);
        if data_off + csize > z.len() {
            return true;
        }
        if e.method == 0 {
            if csize == usize {
                result = Some(z[data_off..data_off + usize].to_vec());
            }
        } else if usize == 0 {
            result = Some(Vec::new()); // empty file (zlib would reject the empty output buffer)
        } else if e.method == 8 {
            result = zsys::inflate_raw(&z[data_off..data_off + csize], usize);
        }
        true
    });
    result
}
