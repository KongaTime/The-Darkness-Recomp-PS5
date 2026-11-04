#include "command_emission.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cstring>

namespace DarkRecomp::Ps5 {
namespace {
bool overlaps(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) {
    return a<b+bn && b<a+an;
}
bool guestRange(uint64_t address,uint64_t size) {
    return address>=0x10000 && address<0x100000000ull && size<=0x100000000ull-address &&
        !overlaps(address,size,0x7FC00000,0x100000) && !overlaps(address,size,0x7FE00000,0x100000);
}
uint64_t shift(uint64_t value,unsigned count) {return count==64?0:value<<count;}
unsigned padding(uint32_t cursor) {
    // Original makes the payload at (header+4) sixteen-byte aligned.
    return ((8u-(cursor&15u))&15u)/4;
}
}
bool tryEmitVectorConstants(PPCContext& ctx,uint8_t* base) noexcept {
    const uint64_t device=ctx.r3.u64,source=ctx.r6.u64,stack=ctx.r1.u64,mask=ctx.r4.u64;
    if(!base || !mask || !guestRange(device,56) || !guestRange(stack,1) || stack<0x10000+176 ||
       (device&3) || (source&15) || (stack&15))return false;
    const uint64_t sourceBytes=uint64_t(64-std::countr_zero(mask))*64;
    if(!guestRange(source,sourceBytes))return false;
    const uint32_t first=PPCLoadU32(base,uint32_t(device+48));
    const uint32_t limit=PPCLoadU32(base,uint32_t(device+52));
    if(first&3)return false;
    uint64_t end=first,bits=mask;
    // Complete every capacity/alias proof before touching the original state.
    do {
        const unsigned skip=std::countl_zero(bits);bits=shift(bits,skip);
        const unsigned run=std::countl_one(bits);
        if(end+uint64_t(run)*64+12>=limit)return false;
        end+=padding(uint32_t(end))*4+4+uint64_t(run)*64;
        bits=shift(bits,run);
    }while(bits);
    const uint64_t outputBytes=end-first;
    if(!guestRange(uint64_t(first)+4,outputBytes) || end+4>limit ||
       overlaps(first+4,outputBytes,source,sourceBytes) ||
       overlaps(first+4,outputBytes,device,56) ||
       overlaps(first+4,outputBytes,stack-176,176) ||
       overlaps(source,sourceBytes,stack-176,176) ||
       overlaps(device,56,stack-176,176))return false;

    PPC_TRACE_ENTRY(ctx,0x828714A0);
    ctx.r12.u64=ctx.lr;ctx.lr=0x828714A8;
    __savegprlr_21(ctx,base);
    PPCStoreU32(base,uint32_t(stack-176),uint32_t(stack));ctx.r1.u32=uint32_t(stack-176);
    uint64_t registerBase=ctx.r5.u64,sourceOffset=0;
    uint32_t cursor=first;
    unsigned lastSkip=0,lastPadding=0;
    uint64_t lastHeader=0,lastSource=0;
    bits=mask;
    do {
        const unsigned skip=std::countl_zero(bits);bits=shift(bits,skip);
        const unsigned run=std::countl_one(bits);
        sourceOffset+=uint64_t(skip)*64;registerBase+=uint64_t(skip)*16;
        const unsigned pads=padding(cursor);
        for(unsigned i=0;i<pads;++i) {cursor+=4;PPCStoreU32(base,cursor,0x80000000u);}
        const uint64_t header=(uint64_t((run*16-1)<<16)&0xFFFF0000ull)|registerBase;
        cursor+=4;PPCStoreU32(base,cursor,uint32_t(header));
        std::memcpy(base+cursor+4,base+source+sourceOffset,size_t(run)*64);
        lastSource=source+sourceOffset+uint64_t(run-1)*64;
        cursor+=run*64;sourceOffset+=uint64_t(run)*64;registerBase+=uint64_t(run)*16;
        bits=shift(bits,run);
        lastSkip=skip;lastPadding=pads;lastHeader=header;
    }while(bits);
    // Original's caller-visible volatile registers, flags and last vector
    // registers are part of the contract too, not just the emitted bytes.
    ctx.r4.u64=cursor;ctx.r5.u64=registerBase;ctx.r8.u64=lastSkip;
    ctx.r9.u64=lastPadding==3?0xFFFFFFFF80000000ull:limit;
    ctx.r10.u64=lastPadding>=2?0xFFFFFFFF80000000ull:uint64_t(lastSkip)*64;
    ctx.r11.u64=lastHeader;
    const auto endian=simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL));
    auto take=[&](PPCVRegister& out,unsigned offset) {
        simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out.u8),simde_mm_shuffle_epi8(
            simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(base+lastSource+offset)),endian));
    };
    take(ctx.v0,0);take(ctx.v13,16);take(ctx.v12,32);take(ctx.v11,48);
    ctx.xer.ca=1;ctx.cr0.compare<int32_t>(0,0,ctx.xer);ctx.cr6.compare<uint64_t>(0,0,ctx.xer);
    PPCStoreU32(base,uint32_t(device+48),cursor);
    ctx.r1.s64+=176;__restgprlr_21(ctx,base);
    return true;
}
}
extern "C" PPC_FUNC(__imp__sub_828714A0);
PPC_FUNC(sub_828714A0) {
    if(!DarkRecomp::Ps5::tryEmitVectorConstants(ctx,base))__imp__sub_828714A0(ctx,base);
}
