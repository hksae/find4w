#pragma once

#include "types.hpp"
#include <functional>

namespace f4w {

using MftFileCallback = std::function<void(FileEntry&&)>;

bool mft_scan_volume(wchar_t drive_letter, const SearchConfig& config, const MftFileCallback& callback);

} // namespace f4w
