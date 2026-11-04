#pragma once
// The console's libc rejects <malloc.h>; the runtime only wants the C
// allocator from it.
#include <stdlib.h>
