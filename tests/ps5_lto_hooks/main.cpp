#include "ppc_context.h"
#include <cassert>
#include <cstdio>
extern "C" PPC_FUNC(fixture_chain);
using Function=void(*)(PPCContext&,uint8_t*);
Function fixture_address();
int main() {
    PPCContext ctx{};uint8_t bytes[16]{};
    for(unsigned i=0;i<10000;++i) {
        fixture_chain(ctx,bytes);fixture_address()(ctx,bytes);
        assert(ctx.r3.u64==uint64_t(i+1)*(EXPECT_OVERRIDE?214:14));
        assert(ctx.r6.u64==uint64_t(i+1)*(EXPECT_OVERRIDE?2:0));
        assert(bytes[0]==uint8_t((i+1)*2));
        const uint64_t step=EXPECT_OVERRIDE?214:14;
        assert(ctx.r5.u64==step*uint64_t(i)*(i+1)/2+(EXPECT_OVERRIDE?107:7)*uint64_t(i+1));
    }
    std::printf("Guest aliases: direct and address-taken calls, %s strong override, original ABI preserved.\n",
        EXPECT_OVERRIDE?"with":"without");
}
