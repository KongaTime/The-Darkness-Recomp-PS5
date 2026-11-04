#include "ppc_context.h"
extern "C" PPC_FUNC(__imp__fixture_override);
PPC_FUNC(fixture_override) {ctx.r6.u64+=1;__imp__fixture_override(ctx,base);ctx.r3.u64+=100;}
