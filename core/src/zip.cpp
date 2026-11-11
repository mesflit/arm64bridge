// SPDX-License-Identifier: GPL-2.0-or-later
#include "zip.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <functional>

namespace a64b {

namespace {

u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

struct Mapped {
    const u8* data = nullptr;
    size_t size = 0;
    ~Mapped() {
        if (data) munmap((void*)data, size);
    }
    bool open(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < 22) {
            close(fd);
            return false;
        }
        void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return false;
        data = (const u8*)p;
        size = (size_t)st.st_size;
        return true;
    }
};

// Calls fn(name, method, compressed size, size, local header offset) for each entry; stop when fn returns true.
bool for_each_entry(const Mapped& z, const std::function<bool(const std::string&, u16, u32, u32, u32)>& fn) {
    size_t min_pos = z.size > 65557 ? z.size - 65557 : 0;
    size_t eocd = 0;
    bool found = false;
    for (size_t i = z.size - 22;; i--) {
        if (rd32(z.data + i) == 0x06054b50) {
            eocd = i;
            found = true;
            break;
        }
        if (i == min_pos) break;
    }
    if (!found) return false;
    u32 count = rd16(z.data + eocd + 10), cd_off = rd32(z.data + eocd + 16);
    size_t p = cd_off;
    for (u32 i = 0; i < count; i++) {
        if (p + 46 > z.size || rd32(z.data + p) != 0x02014b50) return false;
        u16 method = rd16(z.data + p + 10);
        u32 csize = rd32(z.data + p + 20), usize = rd32(z.data + p + 24);
        u16 nlen = rd16(z.data + p + 28), elen = rd16(z.data + p + 30), clen = rd16(z.data + p + 32);
        u32 lho = rd32(z.data + p + 42);
        if (p + 46 + nlen > z.size) return false;
        std::string name((const char*)z.data + p + 46, nlen);
        if (fn(name, method, csize, usize, lho)) return true;
        p += 46 + nlen + elen + clen;
    }
    return true;
}

}  // namespace

bool zip_list(const std::string& zip_path, std::vector<std::string>& names) {
    Mapped z;
    if (!z.open(zip_path)) return false;
    return for_each_entry(z, [&](const std::string& n, u16, u32, u32, u32) {
        names.push_back(n);
        return false;
    });
}

bool zip_extract(const std::string& zip_path, const std::string& entry, std::vector<u8>& out) {
    Mapped z;
    if (!z.open(zip_path)) return false;
    bool ok = false;
    for_each_entry(z, [&](const std::string& n, u16 method, u32 csize, u32 usize, u32 lho) {
        if (n != entry) return false;
        if ((size_t)lho + 30 > z.size || rd32(z.data + lho) != 0x04034b50) return true;
        size_t data_off = (size_t)lho + 30 + rd16(z.data + lho + 26) + rd16(z.data + lho + 28);
        if (data_off + csize > z.size) return true;
        out.resize(usize);
        if (method == 0) {
            if (csize != usize) return true;
            memcpy(out.data(), z.data + data_off, usize);
            ok = true;
        } else if (method == 8) {
            z_stream zs;
            memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return true;
            zs.next_in = const_cast<Bytef*>(z.data + data_off);
            zs.avail_in = csize;
            zs.next_out = out.data();
            zs.avail_out = usize;
            int r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            ok = (r == Z_STREAM_END && zs.total_out == usize);
        }
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

}  // namespace a64b
