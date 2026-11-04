// The console's libc compares memory one byte per loop iteration. That loop was
// the single hottest instruction in the title (work/framerate-host-profile2.txt,
// 2026-10-29): a fifth of the render thread, comparing each draw's 4 KiB
// constant block with the last (world_draw.cpp uniformBlock), and a tenth of
// the engine thread, verifying each draw's guest constant bank
// (render_memory.h equalRenderMemory). The link wraps memcmp and bcmp
// (CMakeLists.txt) so every caller in the title, RADV's archive included, gets
// this vector comparison with the same results. Matching long ranges consume
// 64 bytes per loop; short ranges and early mismatches keep a 16-byte path.
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>

// The compiler must not turn these loops back into calls to memcmp or bcmp,
// which the link redirects here.
#if defined(__clang__)
#define DARK_NO_COMPARE_BUILTIN __attribute__((no_builtin("memcmp", "bcmp")))
#else
#define DARK_NO_COMPARE_BUILTIN
#endif

namespace {
// Offset of the first differing byte within a 16-byte block, or 16 if equal.
inline unsigned firstDifference(const uint8_t* a, const uint8_t* b) {
    const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
    const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
    const unsigned equal = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(x, y)));
    return equal == 0xFFFFu ? 16u : unsigned(__builtin_ctz(~equal));
}
inline bool equal64(const uint8_t* a, const uint8_t* b) {
    const auto eq = [](const uint8_t* x, const uint8_t* y) {
        return _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x)),
                             _mm_loadu_si128(reinterpret_cast<const __m128i*>(y)));
    };
    const auto low = _mm_and_si128(eq(a,b),eq(a+16,b+16));
    const auto high = _mm_and_si128(eq(a+32,b+32),eq(a+48,b+48));
    return unsigned(_mm_movemask_epi8(_mm_and_si128(low,high))) == 0xFFFFu;
}
} // namespace

extern "C" DARK_NO_COMPARE_BUILTIN int __wrap_memcmp(const void* left, const void* right, size_t size) {
    const auto* a = static_cast<const uint8_t*>(left);
    const auto* b = static_cast<const uint8_t*>(right);
    // Different object transforms commonly differ immediately. Check the first
    // vector before issuing the four loads used by a long matching range.
    if (size >= 16) {
        const unsigned at = firstDifference(a,b);
        if (at != 16) return int(a[at])-int(b[at]);
        a += 16; b += 16; size -= 16;
    }
    for (; size >= 64; a += 64, b += 64, size -= 64) {
        if (!equal64(a,b)) {
            // The aggregate mask proves inequality but loses byte order.
            // Recover the first difference in address order for memcmp's sign.
            for (unsigned block=0;block<4;++block) {
                const unsigned at=firstDifference(a+block*16,b+block*16);
                if (at != 16) return int(a[block*16+at])-int(b[block*16+at]);
            }
        }
    }
    for (; size >= 16; a += 16, b += 16, size -= 16) {
        const unsigned at = firstDifference(a, b);
        if (at != 16) return int(a[at]) - int(b[at]);
    }
    for (; size; ++a, ++b, --size)
        if (*a != *b) return int(*a) - int(*b);
    return 0;
}

extern "C" DARK_NO_COMPARE_BUILTIN int __wrap_bcmp(const void* left, const void* right, size_t size) {
    const auto* a = static_cast<const uint8_t*>(left);
    const auto* b = static_cast<const uint8_t*>(right);
    if (size >= 16) {
        if (firstDifference(a,b) != 16) return 1;
        a += 16; b += 16; size -= 16;
    }
    for (; size >= 64; a += 64, b += 64, size -= 64)
        if (!equal64(a,b)) return 1;
    for (; size >= 16; a += 16, b += 16, size -= 16)
        if (firstDifference(a, b) != 16) return 1;
    for (; size; ++a, ++b, --size)
        if (*a != *b) return 1;
    return 0;
}
