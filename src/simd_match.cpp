#include "find4w/simd_match.hpp"
#include <immintrin.h>
#include <cstring>

namespace f4w {

static const char* scalar_find(const char* h, size_t hlen, const char* n, size_t nlen, bool ci) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;

    size_t limit = hlen - nlen;
    if (ci) {
        for (size_t i = 0; i <= limit; ++i) {
            bool match = true;
            for (size_t j = 0; j < nlen; ++j) {
                char a = h[i + j];
                char b = n[j];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match) return h + i;
        }
    } else {
        for (size_t i = 0; i <= limit; ++i) {
            if (memcmp(h + i, n, nlen) == 0) return h + i;
        }
    }
    return nullptr;
}

static const char* sse2_find(const char* h, size_t hlen, const char* n, size_t nlen, bool ci) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    if (hlen < 16) return scalar_find(h, hlen, n, nlen, ci);

    char first = ci ? ((n[0] >= 'A' && n[0] <= 'Z') ? n[0] + 32 : n[0]) : n[0];
    __m128i first_vec = _mm_set1_epi8(first);
    __m128i case_bit  = _mm_set1_epi8(0x20);

    size_t limit = hlen - nlen;
    size_t i = 0;

    for (; i + 15 <= limit; i += 16) {
        __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i));

        if (ci) {
            __m128i lower = _mm_or_si128(block, case_bit);
            __m128i alpha_lo = _mm_cmpgt_epi8(block, _mm_set1_epi8('A' - 1));
            __m128i alpha_hi = _mm_cmpgt_epi8(_mm_set1_epi8('Z' + 1), block);
            __m128i is_upper = _mm_and_si128(alpha_lo, alpha_hi);
            block = _mm_or_si128(_mm_andnot_si128(is_upper, block), _mm_and_si128(is_upper, lower));
        }

        __m128i cmp = _mm_cmpeq_epi8(block, first_vec);
        int mask = _mm_movemask_epi8(cmp);

        while (mask) {
            int bit = _tzcnt_u32(mask);
            size_t pos = i + bit;
            if (pos > limit) return nullptr;

            bool found = true;
            if (ci) {
                for (size_t j = 1; j < nlen; ++j) {
                    char a = h[pos + j];
                    char b = n[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { found = false; break; }
                }
            } else {
                if (memcmp(h + pos + 1, n + 1, nlen - 1) != 0)
                    found = false;
            }

            if (found) return h + pos;
            mask &= mask - 1;
        }
    }

    if (i <= limit)
        return scalar_find(h + i, hlen - i, n, nlen, ci);
    return nullptr;
}

static const char* avx2_find(const char* h, size_t hlen, const char* n, size_t nlen, bool ci) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    if (hlen < 32) return sse2_find(h, hlen, n, nlen, ci);

    char first = ci ? ((n[0] >= 'A' && n[0] <= 'Z') ? n[0] + 32 : n[0]) : n[0];
    __m256i first_vec = _mm256_set1_epi8(first);
    __m256i case_bit  = _mm256_set1_epi8(0x20);

    size_t limit = hlen - nlen;
    size_t i = 0;

    for (; i + 31 <= limit; i += 32) {
        __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h + i));

        if (ci) {
            __m256i lower = _mm256_or_si256(block, case_bit);
            __m256i alpha_lo = _mm256_cmpgt_epi8(block, _mm256_set1_epi8('A' - 1));
            __m256i alpha_hi = _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), block);
            __m256i is_upper = _mm256_and_si256(alpha_lo, alpha_hi);
            block = _mm256_or_si256(_mm256_andnot_si256(is_upper, block), _mm256_and_si256(is_upper, lower));
        }

        __m256i cmp = _mm256_cmpeq_epi8(block, first_vec);
        int mask = _mm256_movemask_epi8(cmp);

        while (mask) {
            int bit = _tzcnt_u32(mask);
            size_t pos = i + bit;
            if (pos > limit) { _mm256_zeroupper(); return nullptr; }

            bool found = true;
            if (ci) {
                for (size_t j = 1; j < nlen; ++j) {
                    char a = h[pos + j];
                    char b = n[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { found = false; break; }
                }
            } else {
                if (memcmp(h + pos + 1, n + 1, nlen - 1) != 0)
                    found = false;
            }

            if (found) { _mm256_zeroupper(); return h + pos; }
            mask &= mask - 1;
        }
    }

    _mm256_zeroupper();
    if (i <= limit)
        return sse2_find(h + i, hlen - i, n, nlen, ci);
    return nullptr;
}

const char* simd_find(const char* haystack, size_t haystack_len,
                      const char* needle, size_t needle_len,
                      bool case_insensitive, bool use_avx2) {
    if (use_avx2)
        return avx2_find(haystack, haystack_len, needle, needle_len, case_insensitive);
    return sse2_find(haystack, haystack_len, needle, needle_len, case_insensitive);
}

size_t simd_count(const char* haystack, size_t haystack_len,
                  const char* needle, size_t needle_len,
                  bool case_insensitive, bool use_avx2) {
    size_t count = 0;
    const char* p = haystack;
    size_t remaining = haystack_len;

    while (remaining >= needle_len) {
        const char* found = simd_find(p, remaining, needle, needle_len, case_insensitive, use_avx2);
        if (!found) break;
        ++count;
        size_t offset = (found - p) + 1;
        p = found + 1;
        remaining = haystack_len - (p - haystack);
    }
    return count;
}

} // namespace f4w
