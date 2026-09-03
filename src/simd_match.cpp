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
                char a = h[i + j], b = n[j];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match) return h + i;
        }
    } else {
        for (size_t i = 0; i <= limit; ++i)
            if (memcmp(h + i, n, nlen) == 0) return h + i;
    }
    return nullptr;
}

static inline char to_lower_c(char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

// SSE2: compare first AND last char simultaneously (Horspool-style)
static const char* sse2_find(const char* h, size_t hlen, const char* n, size_t nlen, bool ci) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    if (hlen < 16) return scalar_find(h, hlen, n, nlen, ci);

    char first = ci ? to_lower_c(n[0]) : n[0];
    char last  = ci ? to_lower_c(n[nlen - 1]) : n[nlen - 1];
    __m128i first_vec = _mm_set1_epi8(first);
    __m128i last_vec  = _mm_set1_epi8(last);
    __m128i case_bit  = _mm_set1_epi8(0x20);
    __m128i az_lo     = _mm_set1_epi8('A' - 1);
    __m128i az_hi     = _mm_set1_epi8('Z' + 1);

    size_t limit = hlen - nlen;
    size_t i = 0;

    auto lower128 = [&](__m128i v) -> __m128i {
        __m128i lo = _mm_cmpgt_epi8(v, az_lo);
        __m128i hi = _mm_cmpgt_epi8(az_hi, v);
        __m128i up = _mm_and_si128(lo, hi);
        return _mm_or_si128(_mm_andnot_si128(up, v), _mm_and_si128(up, _mm_or_si128(v, case_bit)));
    };

    for (; i + 15 + (nlen - 1) <= hlen; i += 16) {
        __m128i bf = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i));
        __m128i bl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i + nlen - 1));
        if (ci) { bf = lower128(bf); bl = lower128(bl); }

        int mask = _mm_movemask_epi8(_mm_and_si128(
            _mm_cmpeq_epi8(bf, first_vec),
            _mm_cmpeq_epi8(bl, last_vec)));

        while (mask) {
            int bit = _tzcnt_u32(mask);
            size_t pos = i + bit;
            if (pos > limit) return nullptr;

            bool ok = true;
            if (nlen > 2) {
                if (ci) {
                    for (size_t j = 1; j < nlen - 1; ++j) {
                        if (to_lower_c(h[pos + j]) != to_lower_c(n[j])) { ok = false; break; }
                    }
                } else {
                    ok = (memcmp(h + pos + 1, n + 1, nlen - 2) == 0);
                }
            }
            if (ok) return h + pos;
            mask &= mask - 1;
        }
    }

    if (i <= limit)
        return scalar_find(h + i, hlen - i, n, nlen, ci);
    return nullptr;
}

// AVX2: compare first AND last char simultaneously across 32 bytes
static const char* avx2_find(const char* h, size_t hlen, const char* n, size_t nlen, bool ci) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    if (hlen < 32) return sse2_find(h, hlen, n, nlen, ci);

    char first = ci ? to_lower_c(n[0]) : n[0];
    char last  = ci ? to_lower_c(n[nlen - 1]) : n[nlen - 1];
    __m256i first_vec = _mm256_set1_epi8(first);
    __m256i last_vec  = _mm256_set1_epi8(last);
    __m256i case_bit  = _mm256_set1_epi8(0x20);
    __m256i az_lo     = _mm256_set1_epi8('A' - 1);
    __m256i az_hi     = _mm256_set1_epi8('Z' + 1);

    size_t limit = hlen - nlen;
    size_t i = 0;

    auto lower256 = [&](__m256i v) -> __m256i {
        __m256i lo = _mm256_cmpgt_epi8(v, az_lo);
        __m256i hi = _mm256_cmpgt_epi8(az_hi, v);
        __m256i up = _mm256_and_si256(lo, hi);
        return _mm256_or_si256(_mm256_andnot_si256(up, v), _mm256_and_si256(up, _mm256_or_si256(v, case_bit)));
    };

    for (; i + 31 + (nlen - 1) <= hlen; i += 32) {
        __m256i bf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h + i));
        __m256i bl = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h + i + nlen - 1));
        if (ci) { bf = lower256(bf); bl = lower256(bl); }

        int mask = _mm256_movemask_epi8(_mm256_and_si256(
            _mm256_cmpeq_epi8(bf, first_vec),
            _mm256_cmpeq_epi8(bl, last_vec)));

        while (mask) {
            int bit = _tzcnt_u32(mask);
            size_t pos = i + bit;
            if (pos > limit) { _mm256_zeroupper(); return nullptr; }

            bool ok = true;
            if (nlen > 2) {
                if (ci) {
                    for (size_t j = 1; j < nlen - 1; ++j) {
                        if (to_lower_c(h[pos + j]) != to_lower_c(n[j])) { ok = false; break; }
                    }
                } else {
                    ok = (memcmp(h + pos + 1, n + 1, nlen - 2) == 0);
                }
            }
            if (ok) { _mm256_zeroupper(); return h + pos; }
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
        p = found + 1;
        remaining = haystack_len - (p - haystack);
    }
    return count;
}

} // namespace f4w
