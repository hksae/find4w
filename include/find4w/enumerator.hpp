#pragma once

#include "types.hpp"
#include <functional>

namespace f4w {

using FileCallback = std::function<void(FileEntry&&)>;

void enumerate_files(const SearchConfig& config, const FileCallback& callback);

} // namespace f4w
