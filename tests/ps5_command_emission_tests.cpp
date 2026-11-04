#include "app/ps5/command_emission.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

extern "C" PPC_FUNC(__imp__sub_828714A0);
PPC_FUNC(sub_82870AB0) {std::fputs("Unexpected original ring allocation\n",stderr);std::abort();}
[[noreturn]] void PPCRecompFailure(const PPCContext&,uint32_t,const char*) {std::abort();}
namespace {
constexpr uint32_t device=0x01000000,source=0x02000000,stack=0x03020000,output=0x04000000;
struct Guest {
    uint8_t* base=static_cast<uint8_t*>(mmap(nullptr,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
    Guest() {if(base==MAP_FAILED)std::abort();}
    ~Guest() {munmap(base,0x100000000ull);}
};
uint64_t randomState=0x987654321BADC0DEull;
uint64_t nextRandom() {randomState^=randomState<<13;randomState^=randomState>>7;randomState^=randomState<<17;return randomState;}
void put(uint8_t* base,uint32_t at,uint32_t value) {value=__builtin_bswap32(value);std::memcpy(base+at,&value,4);}
PPCContext context(uint64_t mask,unsigned alignment,unsigned sourceOffset) {
    PPCContext ctx{};
#define REG(n) ctx.r##n.u64=nextRandom();
    REG(0) REG(1) REG(2) REG(3) REG(4) REG(5) REG(6) REG(7) REG(8) REG(9) REG(10) REG(11) REG(12) REG(13) REG(14) REG(15)
    REG(16) REG(17) REG(18) REG(19) REG(20) REG(21) REG(22) REG(23) REG(24) REG(25) REG(26) REG(27) REG(28) REG(29) REG(30) REG(31)
#undef REG
    ctx.lr=nextRandom();ctx.ctr.u64=nextRandom();ctx.traceIndex=uint32_t(nextRandom());ctx.lastFunction=uint32_t(nextRandom());
    for(auto& entry:ctx.trace)entry=uint32_t(nextRandom());
    ctx.r1.u64=stack;ctx.r3.u64=device;ctx.r4.u64=mask;ctx.r5.u64=nextRandom();ctx.r6.u64=source+sourceOffset;
    ctx.xer.ca=nextRandom()&1;ctx.xer.so=nextRandom()&1;
    (void)alignment;return ctx;
}
void compare(const PPCContext& expected,const PPCContext& actual,uint64_t mask,unsigned align) {
    if(std::memcmp(&expected,&actual,sizeof(expected))) {
        const auto* a=reinterpret_cast<const uint8_t*>(&expected);const auto* b=reinterpret_cast<const uint8_t*>(&actual);
        size_t at=0;while(at<sizeof(expected) && a[at]==b[at])++at;
        std::fprintf(stderr,"Context mismatch mask=%016llx align=%u offset=%zu expected=%02x actual=%02x r9=%llx/%llx r10=%llx/%llx r11=%llx/%llx\n",
            (unsigned long long)mask,align,at,a[at],b[at],(unsigned long long)expected.r9.u64,(unsigned long long)actual.r9.u64,
            (unsigned long long)expected.r10.u64,(unsigned long long)actual.r10.u64,(unsigned long long)expected.r11.u64,(unsigned long long)actual.r11.u64);
        std::abort();
    }
}
}
int main(int argc,char**) {
    Guest expected,actual;unsigned cases=0;
    auto check=[&](uint64_t mask,unsigned align,unsigned offset,bool fallback=false) {
        PPCContext original=context(mask,align,offset),native;
        std::memcpy(&native,&original,sizeof(native));
        for(auto* base:{expected.base,actual.base}) {
            std::memset(base+device,0xA7,256);std::memset(base+stack-256,0xB8,512);std::memset(base+output,0xC9,8192);
            for(unsigned i=0;i<8192;++i)base[source+i]=uint8_t(i*37+(i>>6));
            put(base,device+48,output+align);put(base,device+52,output+8192);
        }
        __imp__sub_828714A0(original,expected.base);
        if(fallback)sub_828714A0(native,actual.base);
        else if(!DarkRecomp::Ps5::tryEmitVectorConstants(native,actual.base)) {
            std::fprintf(stderr,"Valid fast case rejected mask=%llx align=%u\n",(unsigned long long)mask,align);return false;
        }
        compare(original,native,mask,align);
        for(const auto region:std::array<std::pair<uint32_t,size_t>,4>{{{device,256},{stack-256,512},{source,8192},{output,8192}}})
            if(std::memcmp(expected.base+region.first,actual.base+region.first,region.second)) {
                std::fprintf(stderr,"Guest memory differs at region=%x mask=%llx alignment=%u\n",region.first,(unsigned long long)mask,align);std::abort();
            }
        ++cases;return true;
    };
    for(uint64_t mask:{1ull,1ull<<63,~0ull,0xAAAAAAAAAAAAAAAAull,0x5555555555555555ull,0xFFFF00000000FFFFull})
        for(unsigned align:{0u,4u,8u,12u})if(!check(mask,align,16))return 1;
    for(unsigned bit=0;bit<64;++bit)for(unsigned align:{0u,4u,8u,12u})
        if(!check(uint64_t(1)<<bit,align,32))return 1;
    for(unsigned i=0;i<10000;++i)if(!check(nextRandom()|1,uint32_t(nextRandom()%4)*4,uint32_t(nextRandom()%4)*16))return 1;
    // Unaligned VMX sources use the original's round-down loads via fallback.
    for(unsigned align:{0u,4u,8u,12u})if(!check(0xF000000000000001ull,align,4,true))return 1;
    auto reject=[&](PPCContext& ctx) {
        PPCContext before;std::memcpy(&before,&ctx,sizeof(ctx));
        std::array<uint8_t,8192> bytes,sourceBytes;std::array<uint8_t,256> deviceBytes;
        std::array<uint8_t,512> stackBytes;
        std::memcpy(bytes.data(),actual.base+output,bytes.size());
        std::memcpy(sourceBytes.data(),actual.base+source,sourceBytes.size());
        std::memcpy(deviceBytes.data(),actual.base+device,deviceBytes.size());
        std::memcpy(stackBytes.data(),actual.base+stack-256,stackBytes.size());
        if(DarkRecomp::Ps5::tryEmitVectorConstants(ctx,actual.base) || std::memcmp(&before,&ctx,sizeof(ctx)) ||
           std::memcmp(bytes.data(),actual.base+output,bytes.size()) ||
           std::memcmp(sourceBytes.data(),actual.base+source,sourceBytes.size()) ||
           std::memcmp(deviceBytes.data(),actual.base+device,deviceBytes.size()) ||
           std::memcmp(stackBytes.data(),actual.base+stack-256,stackBytes.size()))std::abort();
        ++cases;
    };
    auto ctx=context(0,0,0);reject(ctx);
    ctx=context(~0ull,0,0);put(actual.base,device+48,output);put(actual.base,device+52,output+32);reject(ctx);
    put(actual.base,device+52,output+8192);ctx.r6.u64=output;reject(ctx);
    ctx.r6.u64=stack-176;reject(ctx);
    ctx.r6.u64=0x7FC00000;reject(ctx);
    ctx.r6.u64=0xFFFFFFF0;reject(ctx);
    ctx=context(~0ull,0,0);ctx.r3.u64+=0x100000000ull;reject(ctx);
    ctx=context(~0ull,0,0);ctx.r1.u64+=4;reject(ctx);
    ctx=context(~0ull,0,0);put(actual.base,device+48,device);reject(ctx);
    put(actual.base,device+48,output+1);reject(ctx);
    std::printf("Vector packet emission: %u cases match original AOT full context and guest memory, including fallback; rejected paths leave state unchanged.\n",cases);
    if(argc>1) {
        using Function=void(*)(PPCContext&,uint8_t*);
        for(uint64_t mask:{0x8000000000000000ull,0xFF00000000000000ull,~0ull,0xAAAAAAAAAAAAAAAAull}) {
            std::array<double,7> old{},now{};
            auto measure=[&](Function function) {
                auto state=context(mask,0,0);put(actual.base,device+52,output+8192);
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<100000;++i) {
                    state.r3.u64=device;state.r4.u64=mask;state.r5.u64=16384;state.r6.u64=source;
                    put(actual.base,device+48,output);function(state,actual.base);
                }
                return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/100000;
            };
            for(unsigned i=0;i<old.size();++i) {
                if(i&1) {now[i]=measure(sub_828714A0);old[i]=measure(__imp__sub_828714A0);}
                else {old[i]=measure(__imp__sub_828714A0);now[i]=measure(sub_828714A0);}
            }
            std::sort(old.begin(),old.end());std::sort(now.begin(),now.end());
            std::printf("mask=%016llx original_ns=%.2f native_ns=%.2f speedup=%.2fx\n",(unsigned long long)mask,old[3],now[3],old[3]/now[3]);
        }
    }
}
