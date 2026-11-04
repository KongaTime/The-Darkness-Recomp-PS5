// Local bulk-memory routines for the title and linked driver. Captures showed
// substantial time in imported copies (8% of the producer for one extra 4 KiB
// snapshot, plus render uniform uploads). Keep ordinary cached stores and exact
// bounds: these addresses also include mapped guest and GPU upload memory.
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>

#if defined(__clang__)
#define DARK_NO_MEMORY_BUILTIN __attribute__((no_builtin("memcpy", "memmove", "memset")))
#else
#define DARK_NO_MEMORY_BUILTIN
#endif

extern "C" DARK_NO_MEMORY_BUILTIN void* __wrap_memcpy(void* destination,const void* source,size_t size) {
    auto* d=static_cast<uint8_t*>(destination);
    const auto* s=static_cast<const uint8_t*>(source);
    while(size>=64) {
        const auto a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        const auto b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+16));
        const auto c=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+32));
        const auto e=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+48));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d),a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+16),b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+32),c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+48),e);
        s+=64;d+=64;size-=64;
    }
    while(size>=16) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d),_mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
        s+=16;d+=16;size-=16;
    }
    for(size_t i=0;i<size;++i)d[i]=s[i];
    return destination;
}

extern "C" DARK_NO_MEMORY_BUILTIN void* __wrap_memmove(void* destination,const void* source,size_t size) {
    const auto to=reinterpret_cast<uintptr_t>(destination),from=reinterpret_cast<uintptr_t>(source);
    if(to==from || !size)return destination;
    if(to<from || to-from>=size)return __wrap_memcpy(destination,source,size);
    auto* d=static_cast<uint8_t*>(destination);
    const auto* s=static_cast<const uint8_t*>(source);
    while(size>=64) {
        size-=64;
        const auto a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+size));
        const auto b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+size+16));
        const auto c=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+size+32));
        const auto e=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+size+48));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+size),a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+size+16),b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+size+32),c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+size+48),e);
    }
    while(size>=16) {
        size-=16;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+size),_mm_loadu_si128(reinterpret_cast<const __m128i*>(s+size)));
    }
    while(size) {--size;d[size]=s[size];}
    return destination;
}

extern "C" DARK_NO_MEMORY_BUILTIN void* __wrap_memset(void* destination,int value,size_t size) {
    auto* d=static_cast<uint8_t*>(destination);
    const auto bytes=_mm_set1_epi8(static_cast<char>(value));
    while(size>=64) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d),bytes);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+16),bytes);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+32),bytes);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d+48),bytes);
        d+=64;size-=64;
    }
    while(size>=16) {_mm_storeu_si128(reinterpret_cast<__m128i*>(d),bytes);d+=16;size-=16;}
    for(size_t i=0;i<size;++i)d[i]=static_cast<uint8_t>(value);
    return destination;
}
