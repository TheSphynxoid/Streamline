#pragma once

// SimdPort: minimal SSE2 <-> NEON shim for the handful of intrinsics CMap uses.
// Not a general-purpose translator. Lives in Detail — internal plumbing.
//
// x86-64: <immintrin.h> gives us SSE2 (baseline on x86-64).
// ARM64: <arm_neon.h>. Four ops are direct NEON one-liners; sl_movemask_epi8
//        is adapted from sse2neon v1.9.1 (MIT) — the canonical tested form.
//        See attribution in include/third_party/sse2neon-attribution.txt.

#include <cstdint>

#if defined(SL_X64) || (defined(_M_X64) || defined(__x86_64__))

#include <immintrin.h>

namespace StreamLine::Detail {

    using sl_m128i = __m128i;

    inline sl_m128i sl_loadu_si128(const void* p) noexcept {
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    }
    inline sl_m128i sl_set1_epi8(int8_t v) noexcept {
        return _mm_set1_epi8(v);
    }
    inline sl_m128i sl_cmpeq_epi8(sl_m128i a, sl_m128i b) noexcept {
        return _mm_cmpeq_epi8(a, b);
    }
    inline int sl_movemask_epi8(sl_m128i a) noexcept {
        return _mm_movemask_epi8(a);
    }
    inline void sl_pause() noexcept {
        _mm_pause();
    }

} // namespace StreamLine::Detail

#elif defined(SL_ARM64) || defined(__aarch64__)

#include <arm_neon.h>

namespace StreamLine::Detail {

    using sl_m128i = int8x16_t;

    inline sl_m128i sl_loadu_si128(const void* p) noexcept {
        return vld1q_s8(static_cast<const int8_t*>(p));
    }
    inline sl_m128i sl_set1_epi8(int8_t v) noexcept {
        return vdupq_n_s8(v);
    }
    inline sl_m128i sl_cmpeq_epi8(sl_m128i a, sl_m128i b) noexcept {
        return vceqq_s8(a, b);
    }

    // _mm_movemask_epi8: pack the MSB of each of 16 bytes into the low 16 bits
    // (byte i's sign bit -> bit i).
    //
    // Algorithm adapted from sse2neon v1.9.1 (MIT, https://github.com/DLTcollab/sse2neon),
    // function _mm_movemask_epi8, AArch64 path. Attribution preserved per MIT terms.
    //
    // Why this and not a hand-rolled version: the four other intrinsics above are
    // direct NEON one-liners, but movemask_epi8 has multiple plausible-but-wrong
    // forms (e.g. a naive pairwise-add collapses two bytes per sum and drops sign
    // bits). sse2neon's version is tested across all 16 lanes and is the de-facto
    // reference; adapting it is safer than inventing a new one.
    //
    // Method:
    //   1. vshrq #7      -> each byte's MSB becomes bit 0 (0 or 1)
    //   2. vshlq by 0..7 -> byte i's bit lands at position (i mod 8) within its byte
    //   3. vaddv_u8      -> horizontal sum of each 8-byte half into one byte.
    //                        Within a byte each bit is at a distinct position, so the
    //                        sum cannot carry — it is effectively an OR into one byte.
    //   Low 8 bytes -> bits [7:0], high 8 bytes -> bits [15:8].
    //
    // (C) sse2neon contributors. See include/third_party/sse2neon-attribution.txt
    // for the full MIT notice. The other four intrinsics are trivial and original.
    inline int sl_movemask_epi8(sl_m128i a) noexcept {
        uint8x16_t input = vreinterpretq_u8_s8(a);
        uint8x16_t msbs = vshrq_n_u8(input, 7);
        static const int8_t shift_table[16] = {0, 1, 2, 3, 4, 5, 6, 7,
                                               0, 1, 2, 3, 4, 5, 6, 7};
        int8x16_t shifts = vld1q_s8(shift_table);
        uint8x16_t positioned = vshlq_u8(msbs, shifts);
        return vaddv_u8(vget_low_u8(positioned)) |
               (vaddv_u8(vget_high_u8(positioned)) << 8);
    }

    inline void sl_pause() noexcept {
        __yield();
    }

} // namespace StreamLine::Detail

#else
#error "SimdPort: unsupported architecture. Define SL_X64 or SL_ARM64, or target x86-64/aarch64."
#endif
