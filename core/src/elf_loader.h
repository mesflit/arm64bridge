// Loader for 32-bit ARM shared objects.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace a64b {

struct Module {
    std::string name;
    u32 base = 0;       // lowest mapped address
    u32 size = 0;
    u32 bias = 0;       // load bias (address = bias + vaddr)
    u32 phdr = 0;       // guest address of the program headers
    u32 phnum = 0;
    u32 exidx = 0;      // ARM exception index table
    u32 exidx_count = 0;
    u32 init_func = 0;
    u32 init_array = 0, init_array_count = 0;
    bool symbolic = false;
    bool constructed = false;
    std::unordered_map<std::string, u32> exports;  // name -> guest address
    std::vector<std::string> needed;

    u32 find(const std::string& sym) const {
        auto it = exports.find(sym);
        return it == exports.end() ? 0 : it->second;
    }
};

// Supplies the bytes of a guest library by file name ("libfoo.so").
using LibProvider = std::function<bool(const std::string& name, std::vector<u8>& out)>;
void loader_add_provider(LibProvider p);

// Loads a library (and its dependencies), relocates it and runs its constructors.
Module* loader_load(const std::string& name, std::string* error = nullptr);
Module* loader_find(const std::string& name);
Module* loader_module_for(u32 addr);
std::vector<Module*> loader_modules();
// Guest-defined symbol lookup across all loaded modules.
u32 loader_lookup(const std::string& sym);
// True for libraries whose functions are provided by the bridge (libc.so, libm.so, ...).
bool loader_is_system_lib(const std::string& name);

struct ElfInfo {
    bool valid = false;
    bool arm32 = false;
    std::string error;
    std::string machine;
    std::vector<std::string> needed;
    std::vector<std::string> imports;   // undefined symbols
    std::vector<std::string> exports;
};
ElfInfo elf_inspect(const u8* data, size_t size);

}  // namespace a64b
