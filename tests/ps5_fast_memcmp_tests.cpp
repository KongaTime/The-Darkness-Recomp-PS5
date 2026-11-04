// app/ps5/fast_memcmp.cpp replaces the console libc's memcmp and bcmp through
// the link. Every caller depends on the sign of memcmp and on bcmp's zero, so
// both are checked against the host libc for every length up to 80 bytes, every
// source alignment within a block, every mismatch position and both orderings.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

extern "C" int __wrap_memcmp(const void*, const void*, size_t);
extern "C" int __wrap_bcmp(const void*, const void*, size_t);

namespace {
int sign(int value) { return (value > 0) - (value < 0); }
}

int main() {
    std::vector<uint8_t> a(160), b(160);
    unsigned checks = 0;
    for (size_t length = 0; length <= 80; ++length) {
        for (size_t offsetA = 0; offsetA < 16; offsetA += 3) {
            for (size_t offsetB = 0; offsetB < 16; offsetB += 5) {
                for (size_t i = 0; i < a.size(); ++i) a[i] = b[i] = uint8_t(i * 37 + 11);
                std::memmove(b.data() + offsetB, a.data() + offsetA, length);
                const uint8_t* x = a.data() + offsetA;
                uint8_t* y = b.data() + offsetB;
                if (__wrap_memcmp(x, y, length) != 0 || __wrap_bcmp(x, y, length) != 0) {
                    std::fprintf(stderr, "FAILED: equal ranges differ (length %zu)\n", length);
                    return 1;
                }
                ++checks;
                for (size_t at = 0; at < length; ++at) {
                    for (const uint8_t value : {uint8_t(0x00), uint8_t(0x7F), uint8_t(0x80), uint8_t(0xFF)}) {
                        const uint8_t saved = y[at];
                        if (value == x[at]) continue;
                        y[at] = value;
                        const int expected = sign(std::memcmp(x, y, length));
                        if (sign(__wrap_memcmp(x, y, length)) != expected ||
                            sign(__wrap_memcmp(y, x, length)) != -expected ||
                            __wrap_bcmp(x, y, length) == 0) {
                            std::fprintf(stderr, "FAILED: length %zu, mismatch at %zu, value %02X\n", length, at,
                                         unsigned(value));
                            return 1;
                        }
                        y[at] = saved;
                        ++checks;
                    }
                }
            }
        }
    }
    // A 4 KiB block equal but for its last byte, as the constant compares see.
    std::vector<uint8_t> big(4096, 0x3F), other(4096, 0x3F);
    other.back() = 0x40;
    if (__wrap_memcmp(big.data(), other.data(), 4096) >= 0 || __wrap_bcmp(big.data(), big.data(), 4096) != 0) {
        std::fprintf(stderr, "FAILED: 4 KiB block\n");
        return 1;
    }
    // Exercise every mismatch position across the unrolled blocks and tails,
    // including simultaneous mismatches whose address order determines sign.
    std::vector<size_t> lengths{0,1,15,16,17,31,32,63,64,65,79,80,81,127,128,129,
                               144,255,256,257,1023,1024,1025,4095,4096,4097};
    std::vector<uint8_t> x(4200,0x80),y(4200,0x80);
    auto check=[&](const uint8_t* p,const uint8_t* q,size_t n) {
        const int expected=sign(std::memcmp(p,q,n));
        if(sign(__wrap_memcmp(p,q,n))!=expected ||
           sign(__wrap_memcmp(q,p,n))!=-expected ||
           (__wrap_bcmp(p,q,n)==0)!=(expected==0))std::abort();
        ++checks;
    };
    for(size_t n:lengths)for(unsigned ax=0;ax<32;ax+=3)for(unsigned ay=0;ay<32;ay+=5) {
        std::fill(y.begin(),y.end(),0x80);
        check(x.data()+ax,y.data()+ay,n);
        for(size_t at=0;at<n;++at) {
            y[ay+at]=0xFF;
            check(x.data()+ax,y.data()+ay,n);
            if(at+1<n) {
                y[ay+at+1]=0;check(x.data()+ax,y.data()+ay,n);
                y[ay+at+1]=0x80;
            }
            y[ay+at]=0x80;
        }
    }
    // A comparison may read exactly its range, never into either guard page.
    const size_t page=size_t(sysconf(_SC_PAGESIZE)),span=4*page;
    auto* p=static_cast<uint8_t*>(mmap(nullptr,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    auto* q=static_cast<uint8_t*>(mmap(nullptr,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(p==MAP_FAILED || q==MAP_FAILED || mprotect(p+page,2*page,PROT_READ|PROT_WRITE) ||
       mprotect(q+page,2*page,PROT_READ|PROT_WRITE))std::abort();
    for(size_t n:lengths)for(unsigned ends=0;ends<4;++ends) {
        if(n>2*page)continue;
        auto* left=ends&1?p+3*page-n:p+page;
        auto* right=ends&2?q+3*page-n:q+page;
        std::memset(left,0x80,n);std::memset(right,0x80,n);check(left,right,n);
        if(n) {right[n-1]=0xFF;check(left,right,n);right[0]=0;check(left,right,n);}
    }
    munmap(p,span);munmap(q,span);
    std::printf("Fast memcmp/bcmp: %u comparisons match the host libc's sign and equality.\n", checks + 2);
    return 0;
}
