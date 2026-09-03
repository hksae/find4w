#pragma once

#include "types.hpp"

namespace f4w {

const char* simd_find(const char* haystack, size_t haystack_len,
                      const char* needle, size_t needle_len,
                      bool case_insensitive, bool use_avx2);

size_t simd_count(const char* haystack, size_t haystack_len,
                  const char* needle, size_t needle_len,
                  bool case_insensitive, bool use_avx2);

} // namespace f4w
