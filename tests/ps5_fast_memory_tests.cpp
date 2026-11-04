#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

extern "C" void* __wrap_memcpy(void*,const void*,size_t);
extern "C" void* __wrap_memmove(void*,const void*,size_t);
extern "C" void* __wrap_memset(void*,int,size_t);

int main() {
    unsigned checks=0;
    std::vector<size_t> lengths;
    for(size_t n=0;n<=160;++n)lengths.push_back(n);
    for(size_t n=256;n<=8192;n*=2)for(int delta=-1;delta<=1;++delta)lengths.push_back(n+delta);
    std::array<uint8_t,17000> source{},actual{},expected{};
    for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*73+(i>>5));
    for(auto n:lengths)for(unsigned from=0;from<32;++from)for(unsigned to=0;to<32;++to) {
        actual.fill(0xA5);expected=actual;
        auto* target=actual.data()+64+to;
        std::memcpy(expected.data()+64+to,source.data()+64+from,n);
        assert(__wrap_memcpy(target,source.data()+64+from,n)==target && actual==expected);++checks;
        for(int byte:{0,0x91,0xFF,0x1FF,-1}) {
            actual.fill(0xA5);expected=actual;
            std::memset(expected.data()+64+to,byte,n);
            assert(__wrap_memset(target,byte,n)==target && actual==expected);++checks;
        }
    }
    for(auto n:lengths)for(int distance=-129;distance<=129;++distance)for(unsigned offset=0;offset<16;++offset) {
        actual=source;expected=source;
        const size_t from=4000+offset,to=from+distance;
        std::memmove(expected.data()+to,expected.data()+from,n);
        assert(__wrap_memmove(actual.data()+to,actual.data()+from,n)==actual.data()+to && actual==expected);++checks;
    }
    const size_t page=size_t(sysconf(_SC_PAGESIZE)), span=4*page;
    auto* a=static_cast<uint8_t*>(mmap(nullptr,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    auto* b=static_cast<uint8_t*>(mmap(nullptr,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(a!=MAP_FAILED && b!=MAP_FAILED);
    assert(mprotect(a+page,2*page,PROT_READ|PROT_WRITE)==0);
    assert(mprotect(b+page,2*page,PROT_READ|PROT_WRITE)==0);
    for(auto n:lengths)if(n<=2*page)for(unsigned ends=0;ends<4;++ends) {
        auto* from=ends&1?a+3*page-n:a+page;
        auto* to=ends&2?b+3*page-n:b+page;
        std::memset(from,0x79,n);
        assert(__wrap_memcpy(to,from,n)==to && std::memcmp(from,to,n)==0);++checks;
        assert(__wrap_memset(to,0x91,n)==to);++checks;
    }
    // Overlapping move ending exactly at either inaccessible guard page.
    for(size_t n=1;n<2*page-32;n+=13)for(unsigned distance=1;distance<=17;++distance) {
        auto* end=b+3*page;
        std::memset(b+page,0x37,2*page);
        __wrap_memmove(end-n,end-n-distance,n);++checks;
        __wrap_memmove(b+page,b+page+distance,n);++checks;
    }
    munmap(a,span);munmap(b,span);
    assert(__wrap_memcpy(nullptr,nullptr,0)==nullptr);
    assert(__wrap_memmove(nullptr,nullptr,0)==nullptr);
    assert(__wrap_memset(nullptr,0,0)==nullptr);
    std::printf("Fast memory: %u host-oracle, overlap, alignment, canary and guard-page checks pass.\n",checks);
}
