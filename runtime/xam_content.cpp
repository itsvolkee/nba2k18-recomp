// XAM: enumerators, storage devices, users, tasks, launch data and in-process
// messages.
//
// The runtime presents a console with one hard drive, no storage units, no
// signed-in profile and no network. Calls that create something hand out a real
// handle, and calls that answer a question fill in their outputs, so the game
// never reads memory nobody wrote.
//
// Return values follow the Xbox 360 system library as documented by the
// open-source Xenia emulator; the code is written for this runtime.
#include "kernel.h"
#include <deque>
#include <vector>

using namespace kernel;

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS              = 0;
    constexpr uint32_t X_ERROR_NO_MORE_FILES        = 0x12;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER    = 0x57;
    constexpr uint32_t X_ERROR_NOT_FOUND            = 0x490;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t X_ERROR_NO_SUCH_USER         = 0x525;
    constexpr uint32_t X_ERROR_FUNCTION_FAILED      = 0x65B;
    constexpr uint32_t X_ERROR_IO_PENDING           = 0x3E5;
    constexpr uint32_t X_E_INVALIDARG               = 0x80070057;
    constexpr uint32_t X_ONLINE_E_LOGON_NOT_LOGGED_ON = 0x80151802;

    constexpr uint32_t HResult(uint32_t win32) { return win32 ? (win32 & 0xFFFF) | 0x80070000 : 0; }

    constexpr uint32_t XCONTENT_DATA_SIZE        = 0x134;
    constexpr uint32_t DEVICE_DATA_SIZE          = 0x50;
    constexpr uint32_t ACHIEVEMENT_DETAILS_SIZE  = 36;
    constexpr uint32_t ACHIEVEMENT_STRINGS_SIZE  = 464;
    constexpr uint32_t HARD_DRIVE_ID             = 1;

    // ---- XOVERLAPPED: result, length, context, event, completion routine, completion ctx, extended error
    void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t extendedError, uint32_t length)
    {
        if (!ov) return;
        mem::W32(ov + 0x00, result);
        mem::W32(ov + 0x04, length);
        mem::W32(ov + 0x08, CurrentThread() ? CurrentThread()->handle : 0);
        mem::W32(ov + 0x18, extendedError);
        uint32_t routine = mem::R32(ov + 0x10);
        if (routine)
        {
            static std::once_flag once;
            std::call_once(once, [] { LOG_WARN("XOVERLAPPED completion routines aren't supported yet"); });
            return;
        }
        uint32_t eventHandle = mem::R32(ov + 0x0C);
        if (!eventHandle) return;
        auto obj = GetHandle(eventHandle);
        if (!obj || obj->type != ObjType::Event) return;
        {
            std::lock_guard lock(WaitLock());
            static_cast<KEvent*>(obj.get())->signaled = true;
        }
        WaitCv().notify_all();
    }

    // Finish a call that may be asynchronous: with an XOVERLAPPED the result goes there.
    uint32_t Finish(uint32_t ov, uint32_t result, uint32_t length = 0)
    {
        if (!ov) return result;
        CompleteOverlapped(ov, result ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS, HResult(result), length);
        return X_ERROR_IO_PENDING;
    }

    // ---- enumerators: a queue of fixed-size items handed out by XamEnumerate
    struct Enumerator : KObject
    {
        uint32_t itemSize;
        uint32_t perCall;
        std::deque<std::vector<uint8_t>> items;
        uint32_t extra = 0;          // private structure (XamGetPrivateEnumStructureFromHandle)
        Enumerator(uint32_t size, uint32_t count) : KObject(ObjType::Other), itemSize(size), perCall(count ? count : 1) {}
    };

    std::shared_ptr<Enumerator> GetEnumerator(uint32_t handle)
    {
        auto obj = GetHandle(handle);
        return obj && obj->type == ObjType::Other ? std::dynamic_pointer_cast<Enumerator>(obj) : nullptr;
    }

    uint32_t NewEnumerator(std::shared_ptr<Enumerator> e, uint32_t bufferSizePtr, uint32_t handlePtr)
    {
        if (bufferSizePtr) mem::W32(bufferSizePtr, e->itemSize * e->perCall);
        uint32_t h = CreateHandle(std::move(e));
        if (handlePtr) mem::W32(handlePtr, h);
        return h;
    }

    bool UserSignedIn(uint32_t) { return false; }      // no profiles yet
}

// ====================================================================== enumeration

IMPORT(XamEnumerate)
{
    // (u32 Handle, u32 Flags, void* Buffer, u32 BufferSize, u32* ItemsReturned, XOVERLAPPED* Ov)
    auto e = GetEnumerator(A0);
    if (!e)
    {
        if (A4) mem::W32(A4, 0);
        RETURN(Finish(A5, X_ERROR_INVALID_PARAMETER));
    }
    uint32_t fit = e->itemSize ? A3 / e->itemSize : 0;
    uint32_t n = 0;
    while (n < fit && n < e->perCall && !e->items.empty())
    {
        auto& item = e->items.front();
        memcpy(mem::Ptr(A2 + n * e->itemSize), item.data(), e->itemSize);
        e->items.pop_front();
        ++n;
    }
    uint32_t result = n ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    if (A5)
    {
        CompleteOverlapped(A5, result, HResult(result), n);
        RETURN(X_ERROR_IO_PENDING);
    }
    if (A4) mem::W32(A4, n);
    RETURN(result);
}

IMPORT(XamCreateEnumeratorHandle)
{
    // (u32 User, u32 App, u32 OpenMsg, u32 CloseMsg, u32 ExtraSize, u32 ItemCount, u32 Flags, u32* Handle)
    auto e = std::make_shared<Enumerator>(1, ctx.r8.u32);
    if (A4)
    {
        e->extra = mem::AllocVirtual(A4, 0x10);
        memset(mem::Ptr(e->extra), 0, A4);
    }
    NewEnumerator(e, 0, A7);
    RETURN(X_ERROR_SUCCESS);
}

IMPORT(XamGetPrivateEnumStructureFromHandle)
{
    // (u32 Handle, void** Out)
    auto e = GetEnumerator(A0);
    if (!e) RETURN(X_ERROR_INVALID_PARAMETER);
    if (A1) mem::W32(A1, e->extra);
    RETURN(X_ERROR_SUCCESS);
}

// ====================================================================== content and devices

IMPORT(XamContentCreateEnumerator)
{
    // (u32 User, u32 DeviceId, u32 ContentType, u32 Flags, u32 ItemsPerCall, u32* BufferSize, u32* Handle)
    if (A0 < 4 && !UserSignedIn(A0)) RETURN(X_ERROR_NO_SUCH_USER);
    NewEnumerator(std::make_shared<Enumerator>(XCONTENT_DATA_SIZE, A4), A5, A6);    // no saved content yet
    RETURN(X_ERROR_SUCCESS);
}

static void WriteHardDrive(uint32_t p)
{
    memset(mem::Ptr(p), 0, DEVICE_DATA_SIZE);
    mem::W32(p + 0x00, HARD_DRIVE_ID);
    mem::W32(p + 0x04, 1);                                  // device type: hard drive
    mem::W64(p + 0x08, 20ull << 30);                        // total bytes
    mem::W64(p + 0x10, 16ull << 30);                        // free bytes
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++) mem::W16(p + 0x18 + i * 2, uint16_t(name[i]));
}

IMPORT(XamContentCreateDeviceEnumerator)
{
    // (u32 ContentType, u32 Flags, u32 MaxCount, u32* BufferSize, u32* Handle)
    auto e = std::make_shared<Enumerator>(DEVICE_DATA_SIZE, A2);
    std::vector<uint8_t> item(DEVICE_DATA_SIZE);
    uint32_t tmp = mem::AllocSystem(DEVICE_DATA_SIZE, 0x10);
    WriteHardDrive(tmp);
    memcpy(item.data(), mem::Ptr(tmp), DEVICE_DATA_SIZE);
    e->items.push_back(std::move(item));
    NewEnumerator(e, A3, A4);
    RETURN(X_ERROR_SUCCESS);
}

IMPORT(XamContentGetDeviceState)
{
    // (u32 DeviceId, XOVERLAPPED* Ov)
    uint32_t r = A0 == HARD_DRIVE_ID ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    RETURN(Finish(A1, r));
}

IMPORT(XamContentGetDeviceData)
{
    // (u32 DeviceId, X_CONTENT_DEVICE_DATA* Data)
    if (A0 != HARD_DRIVE_ID) RETURN(X_ERROR_DEVICE_NOT_CONNECTED);
    if (A1) WriteHardDrive(A1);
    RETURN(X_ERROR_SUCCESS);
}

IMPORT(XamUserCreateAchievementEnumerator)
{
    // (u32 TitleId, u32 User, u64 Xuid, u32 Flags, u32 Offset, u32 Count, u32* BufferSize, u32* Handle)
    // (the 64-bit Xuid takes one register: r5)
    uint32_t flags = ctx.r6.u32, count = ctx.r8.u32, bufferSizePtr = ctx.r9.u32, handlePtr = ctx.r10.u32;
    if (!count || !bufferSizePtr || !handlePtr || A1 >= 4) RETURN(X_ERROR_INVALID_PARAMETER);
    uint32_t size = ACHIEVEMENT_DETAILS_SIZE + ((flags & 7) ? ACHIEVEMENT_STRINGS_SIZE : 0);
    NewEnumerator(std::make_shared<Enumerator>(size, count), bufferSizePtr, handlePtr);
    RETURN(X_ERROR_SUCCESS);
}

// ====================================================================== users (none signed in)

IMPORT(XamUserGetXUID)
{
    // (u32 User, u32 TypeMask, u64* Xuid)
    if (!A2) RETURN(X_E_INVALIDARG);
    mem::W64(A2, 0);
    if (A0 >= 4) RETURN(X_E_INVALIDARG);
    RETURN(HResult(X_ERROR_NO_SUCH_USER));
}

IMPORT(XamUserGetName)
{
    // (u32 User, char* Buffer, u32 Length)
    if (A1 && A2) mem::W8(A1, 0);
    RETURN(A0 >= 4 ? X_ERROR_INVALID_PARAMETER : X_ERROR_NO_SUCH_USER);
}

IMPORT(XamUserGetSigninInfo)
{
    // (u32 User, u32 Flags, X_USER_SIGNIN_INFO* Info)  -- 40 bytes
    if (!A2) RETURN(X_E_INVALIDARG);
    memset(mem::Ptr(A2), 0, 40);
    RETURN(A0 >= 4 ? X_E_INVALIDARG : HResult(X_ERROR_NO_SUCH_USER));
}

IMPORT(XamUserReadProfileSettings)
{
    // (u32 TitleId, u32 User, u32 XuidCount, u64* Xuids, u32 SettingCount, u32* Ids,
    //  u32* BufferSize, void* Buffer, XOVERLAPPED* Ov)
    //
    // Games call this twice: once with no buffer to learn the size (answer 122,
    // "insufficient buffer", plus the size), then again to read. With no saved
    // profile every setting comes back as "default" with an empty value.
    uint32_t user = A1, xuidCount = A2, xuids = A3, count = A4, ids = A5, sizePtr = A6, buffer = A7;
    uint32_t ov = mem::R32(ctx.r1.u32 + 0x54);          // 9th argument: on the caller's stack
    if (count < 1 || count > 32 || !sizePtr) RETURN(X_ERROR_INVALID_PARAMETER);
    uint32_t bufferSize = mem::R32(sizePtr);
    if (bufferSize && !buffer) RETURN(X_ERROR_INVALID_PARAMETER);

    constexpr uint32_t HEADER = 8, SETTING = 40;
    uint32_t settingsSize = 0, dataSize = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t key = mem::R32(ids + i * 4);
        uint32_t type = key >> 28;
        settingsSize += SETTING;
        if (type == 4 || type == 6) dataSize += (key >> 16) & 0xFFF;     // wide string / binary
    }
    if (xuids) { settingsSize *= xuidCount; dataSize *= xuidCount; }
    uint32_t needed = HEADER + settingsSize + dataSize;
    if (!buffer || bufferSize < needed)
    {
        if (!bufferSize) mem::W32(sizePtr, needed);
        RETURN(122);                                     // ERROR_INSUFFICIENT_BUFFER
    }

    memset(mem::Ptr(buffer), 0, needed);
    uint32_t settings = buffer + HEADER;
    mem::W32(buffer + 0, count);
    mem::W32(buffer + 4, settings);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t key = mem::R32(ids + i * 4);
        uint32_t s = settings + i * SETTING;
        mem::W32(s + 0x00, 1);                           // source: default value
        mem::W32(s + 0x08, user);                        // user index (xuid -1 for local reads)
        mem::W32(s + 0x0C, 0xFFFFFFFF);
        mem::W32(s + 0x10, key);
        mem::W8(s + 0x18, uint8_t(key >> 28));           // data type; value left zero
    }
    LOG_TRACE("XamUserReadProfileSettings(title 0x{:08X}, user {}, {} settings) -> defaults", A0, user, count);
    RETURN(Finish(ov, X_ERROR_SUCCESS));
}

IMPORT(XamUserWriteProfileSettings)
{
    // (u32 TitleId, u32 User, u32 SettingCount, void* Settings, XOVERLAPPED* Ov)
    RETURN(Finish(A4, X_ERROR_NO_SUCH_USER));
}

IMPORT(XamUserCheckPrivilege)
{
    // (u32 User, u32 Privilege, u32* Result)
    if (A2) mem::W32(A2, 0);
    RETURN(A0 >= 4 && A0 != 0xFF ? X_ERROR_INVALID_PARAMETER : X_ERROR_NO_SUCH_USER);
}

IMPORT(XamUserGetCachedUserFlags) { RETURN(0); }

// ====================================================================== tasks

IMPORT(XamTaskSchedule)
{
    // (void* Callback, void* Message, X_TASK_ARGS* Optional, u32* Handle): runs Callback(Message) on its own thread
    auto t = CreateThread(0, A0, A1, 0, false);
    uint32_t h = CreateHandle(t);
    if (A3) mem::W32(A3, h);
    LOG_TRACE("XamTaskSchedule(0x{:08X}, 0x{:08X}) -> thread {}", A0, A1, t->id);
    RETURN(X_ERROR_SUCCESS);
}

IMPORT(XamTaskCloseHandle) { CloseHandle(A0); RETURN(X_ERROR_SUCCESS); }
IMPORT(XamTaskShouldExit)  { RETURN(0); }

// ====================================================================== launch data / title

IMPORT(XamLoaderGetLaunchDataSize)
{
    // (u32* Size)
    if (!A0) RETURN(X_ERROR_INVALID_PARAMETER);
    mem::W32(A0, 0);
    RETURN(X_ERROR_NOT_FOUND);
}

IMPORT(XamLoaderGetLaunchData) { RETURN(X_ERROR_NOT_FOUND); }

IMPORT(XamGetCurrentTitleId)
{
    // title ID from the XEX execution-info header (key 0x00040006, title ID at +0x0C)
    uint32_t h = Exe().guestXexHeader;
    uint32_t count = mem::R32(h + 0x14);
    for (uint32_t i = 0; i < count; i++)
        if (mem::R32(h + 0x18 + i * 8) == 0x00040006)
            RETURN(mem::R32(h + mem::R32(h + 0x18 + i * 8 + 4) + 0x0C));
    RETURN(0);
}

// ====================================================================== in-process messages (no Live)

IMPORT(XMsgInProcessCall)
{
    // (u32 App, u32 Message, u32 Arg1, u32 Arg2)
    if (A0 == 0xFC && A1 == 0x58006) RETURN(X_ONLINE_E_LOGON_NOT_LOGGED_ON);    // NAT type: not online
    static std::mutex lock;
    static std::unordered_map<uint64_t, uint32_t> seen;
    {
        std::lock_guard g(lock);
        if (++seen[(uint64_t(A0) << 32) | A1] == 1)
            LOG_WARN("XMsgInProcessCall(app 0x{:X}, message 0x{:X}) not handled", A0, A1);
    }
    RETURN(X_ERROR_NOT_FOUND);
}

IMPORT(XMsgStartIORequest)
{
    // (u32 App, u32 Message, XOVERLAPPED* Ov, void* Buffer, u32 Length)
    LOG_TRACE("XMsgStartIORequest(app 0x{:X}, message 0x{:X})", A0, A1);
    RETURN(Finish(A2, X_ERROR_NOT_FOUND));
}

IMPORT(XMsgStartIORequestEx)
{
    // (u32 App, u32 Message, XOVERLAPPED* Ov, void* Buffer, u32 Length, void* Extra)
    LOG_TRACE("XMsgStartIORequestEx(app 0x{:X}, message 0x{:X})", A0, A1);
    RETURN(Finish(A2, X_ERROR_NOT_FOUND));
}
