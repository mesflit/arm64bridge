// Guest address space: a region of the host process below 4 GiB that is
// identity-mapped into the emulated 32-bit CPU (guest address == host address).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common.h"

namespace a64b {

enum { GPROT_READ = 1, GPROT_WRITE = 2, GPROT_EXEC = 4 };

bool mem_init();
u32 mem_arena_base();
u32 mem_arena_size();
u32 mem_page_size();
bool mem_contains(u64 host_addr);

// Changes whenever translated guest code may have become stale.
u64 mem_code_epoch();
void mem_bump_code_epoch();

// Page level allocation (always zero filled, read/write on the host side).
u32 page_alloc(u32 size, bool exec = false);
// Map a file (or anonymous memory if fd < 0) at a guest address chosen by us.
u32 page_map_file(u32 size, int host_prot, int host_flags, int fd, i64 offset);
bool page_free(u32 addr, u32 size);
void page_mark_exec(u32 addr, u32 size);

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
