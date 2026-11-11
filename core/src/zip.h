// Minimal ZIP reader (enough to pull native libraries out of an APK).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>

#include "common.h"

namespace a64b {
bool zip_extract(const std::string& zip_path, const std::string& entry, std::vector<u8>& out);
bool zip_list(const std::string& zip_path, std::vector<std::string>& names);
}  // namespace a64b
