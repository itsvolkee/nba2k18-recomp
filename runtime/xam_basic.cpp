// XAM basics: memory, controller input (no controller wired up yet), and
// system notifications (none sent yet).
//
// Return values follow the Xbox 360 system library as documented by the
// open-source Xenia emulator; the code is written for this runtime.
#include "kernel.h"

using namespace kernel;

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS              = 0;
    constexpr uint32_t X_ERROR_EMPTY                = 0x10D;   // no keystroke waiting
    constexpr uint32_t X_ERROR_BAD_ARGUMENTS        = 0xA0;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t XUSER_INDEX_ANY              = 0xFF;

    // Controller 1 reports as a connected gamepad with nothing pressed;
    // controllers 2-4 report as not connected. Real input comes later.
    bool Connected(uint32_t user) { return user == 0 || (user & XUSER_INDEX_ANY) == XUSER_INDEX_ANY; }
}

// ====================================================================== memory

IMPORT(XamAlloc)
{
    // (u32 Flags, u32 Size, u32* OutPtr) -> X_ERROR
    uint32_t p = mem::AllocVirtual(A1 ? A1 : 1, 0x10);
    if (A2) mem::W32(A2, p);
    RETURN(p ? X_ERROR_SUCCESS : 0x8007000E);      // E_OUTOFMEMORY
}

IMPORT(XamFree)
{
    // (void* Ptr)
    if (A0) mem::Free(A0);
    RETURN(X_ERROR_SUCCESS);
}

// ====================================================================== input

IMPORT(XamInputGetState)
{
    // (u32 UserIndex, u32 Flags, X_INPUT_STATE* State): packet number + 12-byte gamepad
    if (A2) memset(mem::Ptr(A2), 0, 16);
    if (A0 >= 4 && (A0 & XUSER_INDEX_ANY) != XUSER_INDEX_ANY) RETURN(X_ERROR_DEVICE_NOT_CONNECTED);
    RETURN(Connected(A0) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED);
}

IMPORT(XamInputSetState)
{
    // (u32 UserIndex, u32 Flags, X_INPUT_VIBRATION* Vibration)
    RETURN(Connected(A0) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED);
}

IMPORT(XamInputGetCapabilities)
{
    // (u32 UserIndex, u32 Flags, X_INPUT_CAPABILITIES* Caps): type, subtype, flags, gamepad, vibration
    if (!A2) RETURN(X_ERROR_BAD_ARGUMENTS);
    memset(mem::Ptr(A2), 0, 20);
    if (!Connected(A0)) RETURN(X_ERROR_DEVICE_NOT_CONNECTED);
    mem::W8(A2 + 0, 1);         // XINPUT_DEVTYPE_GAMEPAD
    mem::W8(A2 + 1, 1);         // XINPUT_DEVSUBTYPE_GAMEPAD
    RETURN(X_ERROR_SUCCESS);
}

IMPORT(XamInputGetKeystrokeEx)
{
    // (u32* UserIndex, u32 Flags, X_INPUT_KEYSTROKE* Keystroke)
    // No keystrokes are ever queued yet, so report "empty". Returning success
    // here made the game's keystroke-draining loop spin forever.
    if (!A2) RETURN(X_ERROR_BAD_ARGUMENTS);
    memset(mem::Ptr(A2), 0, 8);
    uint32_t user = A0 ? mem::R32(A0) : 0;
    RETURN(Connected(user) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED);
}

// ====================================================================== notifications

IMPORT(XamNotifyCreateListener)
{
    // (u64 Mask, u32 MaxVersion) -> listener handle
    auto listener = std::make_shared<KEvent>(true, false);
    uint32_t h = CreateHandle(listener);
    LOG_TRACE("XamNotifyCreateListener(mask 0x{:X}) -> 0x{:X}", ctx.r3.u64, h);
    RETURN(h);
}

IMPORT(XNotifyGetNext)
{
    // (u32 Listener, u32 MatchId, u32* Id, u32* Param) -> true if a notification was dequeued
    if (A3) mem::W32(A3, 0);
    if (A2) mem::W32(A2, 0);
    RETURN(0);
}
