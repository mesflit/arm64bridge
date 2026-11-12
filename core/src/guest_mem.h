// Guest address space: a region of the host process below 4 GiB that is
// identity-mapped into the emulated 32-bit CPU (guest address == host address).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common.h"

#include <utility>
#include <vector>

namespace a64b {

enum { GPROT_READ = 1, GPROT_WRITE = 2, GPROT_EXEC = 4 };

bool mem_init();
u32 mem_arena_base();
u32 mem_arena_size();
// Second arena below 2 GiB (size 0 if there is none).
u32 mem_low_arena_base();
u32 mem_low_arena_size();
u32 mem_page_size();
bool mem_contains(u64 host_addr);
// True if the address lies in pages that are currently allocated (safe to read). Never blocks.
bool mem_is_mapped(u32 addr, u32 len = 1);

// Changes whenever translated guest code may have become stale.
u64 mem_code_epoch();
void mem_bump_code_epoch();

// Page level allocation (always zero filled, read/write on the host side).
u32 page_alloc(u32 size, bool exec = false);
// Map a file (or anonymous memory if fd < 0) at a guest address chosen by us.
u32 page_map_file(u32 size, int host_prot, int host_flags, int fd, i64 offset);
// Map at the guest address the guest asked for (anonymous if fd < 0). With `fixed` an existing guest
// mapping there is replaced (MAP_FIXED); without it the address is only used when it is free.
// Addresses outside the arena are used when the host has nothing there. Returns 0 if not possible.
u32 page_map_at(u32 addr, u32 size, int host_prot, int host_flags, int fd, i64 offset, bool fixed);
bool page_free(u32 addr, u32 size);
void page_mark_exec(u32 addr, u32 size);
// Allocated arena regions (start, size).
std::vector<std::pair<u32, u32>> mem_used_regions();

// malloc family living inside the arena.
u32 g_malloc(u32 size);
u32 g_calloc(u32 n, u32 size);
u32 g_realloc(u32 ptr, u32 size);
u32 g_memalign(u32 align, u32 size);
void g_free(u32 ptr);
u32 g_usable_size(u32 ptr);
u32 g_strdup(const char* s);

template <class T = void>
static inline T* g2h(u32 addr) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(addr));
}
// Convert a host pointer to a guest address. Pointers outside the low 4 GiB
// cannot be represented; they are reported and become NULL.
u32 h2g(const void* p);

}  // namespace a64b
