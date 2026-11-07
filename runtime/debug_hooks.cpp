// Debugging aid: wrap individual recompiled game functions to log their
// arguments and results.
//
// Every recompiled function `sub_XXXXXXXX` is a weak alias of the generated
// `__imp__sub_XXXXXXXX`, so defining `sub_XXXXXXXX` here replaces it for every
// direct and indirect call while the original stays callable.
#include "kernel.h"

#define TRACE_RESULT(addr, maxCalls)                                                              \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                       \
    PPC_FUNC(sub_##addr)                                                                          \
    {                                                                                             \
        static std::atomic<uint32_t> calls{ 0 };                                                  \
        uint32_t n = ++calls;                                                                     \
        uint32_t a0 = ctx.r3.u32, a1 = ctx.r4.u32, a2 = ctx.r5.u32, a3 = ctx.r6.u32;              \
        __imp__sub_##addr(ctx, base);                                                             \
        if (n <= (maxCalls))                                                                      \
            LOG_INFO("trace sub_" #addr "(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}) -> 0x{:X}  [call {}]",  \
                     a0, a1, a2, a3, ctx.r3.u32, n);                                              \
    }

// The system-notification object the game's main loop uses (stored at 0x83B3CD28)
// comes out of this factory. On the last run it was never created.
TRACE_RESULT(82EA50E0, 4)
TRACE_RESULT(82EA78E0, 4)
TRACE_RESULT(82EA63D0, 4)
TRACE_RESULT(82EA7768, 4)
TRACE_RESULT(82EA8C28, 4)
TRACE_RESULT(82EAC320, 4)
TRACE_RESULT(82EDA000, 4)
TRACE_RESULT(82E42EA8, 4)
TRACE_RESULT(82E42FE0, 4)
TRACE_RESULT(82E444C8, 4)
