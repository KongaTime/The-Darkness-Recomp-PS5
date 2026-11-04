#include "ppc_context.h"
PPC_EXTERN_FUNC(fixture_override);
__attribute__((alias("__imp__fixture_override"))) PPC_WEAK_FUNC(fixture_override);
PPC_FUNC_IMPL(__imp__fixture_override) {ctx.r3.u64+=7;ctx.r4.u64^=3;base[0]+=1;}
PPC_FUNC_IMPL(fixture_chain) {fixture_override(ctx,base);ctx.r5.u64+=ctx.r3.u64;}
using Function=void(*)(PPCContext&,uint8_t*);
Function fixture_address() {return fixture_override;}
