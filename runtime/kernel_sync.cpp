// Kernel imports: memory, synchronisation, threads, TLS, time.
#include "kernel.h"
#include <chrono>
#include <functional>
#include <map>

using namespace kernel;

static int64_t ReadTimeout(uint32_t p, int64_t& storage)
{
    storage = int64_t(mem::R64(p));
    return storage;
}

// ====================================================================== memory

IMPORT(NtAllocateVirtualMemory)
{
    // (u32* BaseAddress, u32* RegionSize, u32 AllocationType, u32 Protect, u32 Unknown)
    uint32_t want = mem::R32(A0);
    uint32_t size = mem::R32(A1);
    uint32_t type = A2;
    uint32_t align = (type & 0x20000000) ? 0x1000 : 0x10000;   // MEM_NOZERO / 4K-page hint
    size = (size + 0xFFF) & ~0xFFFu;

    if (want != 0 && mem::SizeOf(want) != 0)
    {
        // committing inside an existing reservation
        mem::W32(A1, size);
        RETURN(STATUS_SUCCESS);
    }
    uint32_t a = mem::AllocVirtual(size, align);
    if (!a) RETURN(STATUS_NO_MEMORY);
    mem::W32(A0, a);
    mem::W32(A1, size);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtFreeVirtualMemory)
{
    // (u32* BaseAddress, u32* RegionSize, u32 FreeType, u32 Unknown)
    uint32_t a = mem::R32(A0);
    if (A2 & 0x8000)            // MEM_RELEASE
        mem::Free(a);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtQueryVirtualMemory)
{
    // (u32 BaseAddress, MEMORY_BASIC_INFORMATION* Info)
    uint32_t a = A0, info = A1;
    uint32_t page = a & ~0xFFFu;
    uint32_t remaining = mem::SizeOf(a);
    mem::W32(info + 0x00, page);
    mem::W32(info + 0x04, page);
    mem::W32(info + 0x08, 0x04);                         // PAGE_READWRITE
    mem::W32(info + 0x0C, remaining ? (remaining + 0xFFF) & ~0xFFFu : 0x1000);
    mem::W32(info + 0x10, remaining ? 0x1000 : 0x10000); // MEM_COMMIT : MEM_FREE
    mem::W32(info + 0x14, 0x04);
    mem::W32(info + 0x18, 0x20000);                      // MEM_PRIVATE
    RETURN(STATUS_SUCCESS);
}

IMPORT(MmAllocatePhysicalMemoryEx)
{
    // (u32 Flags, u32 Size, u32 Protect, u32 MinAddress, u32 MaxAddress, u32 Alignment)
    uint32_t align = std::max<uint32_t>(A5, 0x1000);
    RETURN(mem::AllocPhysical(A1, align));
}

IMPORT(MmFreePhysicalMemory)    { mem::Free(A1); RETURN(0); }
IMPORT(MmGetPhysicalAddress)    { RETURN(A0 & 0x1FFFFFFF); }
IMPORT(MmQueryAddressProtect)   { RETURN(0x04); }
IMPORT(MmQueryAllocationSize)   { RETURN(mem::SizeOf(A0)); }
IMPORT(MmMapIoSpace)            { RETURN(A0); }

IMPORT(MmQueryStatistics)
{
    // MM_STATISTICS: Length is filled by the caller; report 512 MB with plenty free
    uint32_t s = A0;
    uint32_t len = mem::R32(s);
    for (uint32_t i = 4; i < len; i += 4) mem::W32(s + i, 0);
    mem::W32(s + 0x04, 0x20000);     // TotalPhysicalPages (4 KB)
    mem::W32(s + 0x08, 0x2000);      // KernelPages
    mem::W32(s + 0x0C, 0x10000);     // TitleAvailablePages
    mem::W32(s + 0x10, 0x20000000);  // TitleTotalVirtualMemoryBytes
    RETURN(STATUS_SUCCESS);
}

IMPORT(ExAllocatePool)             { RETURN(mem::AllocVirtual(A0, 0x10)); }
IMPORT(ExAllocatePoolTypeWithTag)  { RETURN(mem::AllocVirtual(A0, 0x10)); }
IMPORT(ExFreePool)                 { mem::Free(A0); RETURN(0); }

// ====================================================================== critical sections

struct CritSec
{
    std::recursive_mutex m;
};
static std::mutex s_csLock;
static std::unordered_map<uint32_t, std::unique_ptr<CritSec>> s_cs;

static CritSec& GetCs(uint32_t p)
{
    std::lock_guard lock(s_csLock);
    auto& slot = s_cs[p];
    if (!slot) slot = std::make_unique<CritSec>();
    return *slot;
}

// RTL_CRITICAL_SECTION: header(0x10) LockCount(+0x10) RecursionCount(+0x14) OwningThread(+0x18)
static void CsInit(uint32_t p)
{
    mem::W8(p, 1);
    mem::W32(p + 0x10, uint32_t(-1));
    mem::W32(p + 0x14, 0);
    mem::W32(p + 0x18, 0);
    GetCs(p);
}

IMPORT(RtlInitializeCriticalSection)             { CsInit(A0); RETURN(0); }
IMPORT(RtlInitializeCriticalSectionAndSpinCount) { CsInit(A0); RETURN(STATUS_SUCCESS); }

IMPORT(RtlEnterCriticalSection)
{
    uint32_t p = A0;
    GetCs(p).m.lock();
    mem::W32(p + 0x18, CurrentThread() ? CurrentThread()->kthread : 0);
    mem::W32(p + 0x14, mem::R32(p + 0x14) + 1);
    RETURN(0);
}

IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t p = A0;
    if (!GetCs(p).m.try_lock()) RETURN(0);
    mem::W32(p + 0x18, CurrentThread() ? CurrentThread()->kthread : 0);
    mem::W32(p + 0x14, mem::R32(p + 0x14) + 1);
    RETURN(1);
}

IMPORT(RtlLeaveCriticalSection)
{
    uint32_t p = A0;
    uint32_t rec = mem::R32(p + 0x14);
    if (rec) mem::W32(p + 0x14, rec - 1);
    if (rec <= 1) mem::W32(p + 0x18, 0);
    GetCs(p).m.unlock();
    RETURN(0);
}

// ====================================================================== events / semaphores / mutants

static void Signal(const std::function<void()>& f)
{
    {
        std::lock_guard lock(WaitLock());
        f();
    }
    WaitCv().notify_all();
}

IMPORT(NtCreateEvent)
{
    // (u32* Handle, OBJECT_ATTRIBUTES* Attr, u32 EventType, u32 InitialState)
    auto e = std::make_shared<KEvent>(A2 == 0, A3 != 0);
    mem::W32(A0, CreateHandle(e));
    RETURN(STATUS_SUCCESS);
}

template <typename T> static std::shared_ptr<T> As(const std::shared_ptr<KObject>& o, ObjType t)
{
    return (o && o->type == t) ? std::static_pointer_cast<T>(o) : nullptr;
}

IMPORT(NtSetEvent)
{
    auto e = As<KEvent>(GetHandle(A0), ObjType::Event);
    if (!e) RETURN(STATUS_INVALID_HANDLE);
    bool prev = false;
    Signal([&] { prev = e->signaled; e->signaled = true; });
    if (A1) mem::W32(A1, prev);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtClearEvent)
{
    auto e = As<KEvent>(GetHandle(A0), ObjType::Event);
    if (!e) RETURN(STATUS_INVALID_HANDLE);
    std::lock_guard lock(WaitLock());
    e->signaled = false;
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtPulseEvent)
{
    auto e = As<KEvent>(GetHandle(A0), ObjType::Event);
    if (!e) RETURN(STATUS_INVALID_HANDLE);
    Signal([&] { e->signaled = true; });
    std::this_thread::yield();
    std::lock_guard lock(WaitLock());
    e->signaled = false;
    RETURN(STATUS_SUCCESS);
}

IMPORT(KeSetEvent)
{
    // (KEVENT* Event, s32 Increment, u8 Wait) -> previous state
    auto e = As<KEvent>(ObjectFromGuest(A0), ObjType::Event);
    if (!e) RETURN(0);
    bool prev = false;
    Signal([&] { prev = e->signaled; e->signaled = true; });
    mem::W32(A0 + 4, 1);
    RETURN(prev);
}

IMPORT(KeResetEvent)
{
    auto e = As<KEvent>(ObjectFromGuest(A0), ObjType::Event);
    if (!e) RETURN(0);
    std::lock_guard lock(WaitLock());
    bool prev = e->signaled;
    e->signaled = false;
    mem::W32(A0 + 4, 0);
    RETURN(prev);
}

IMPORT(NtCreateSemaphore)
{
    // (u32* Handle, OBJECT_ATTRIBUTES* Attr, s32 Initial, s32 Maximum)
    auto s = std::make_shared<KSemaphore>(int32_t(A2), int32_t(A3));
    mem::W32(A0, CreateHandle(s));
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtReleaseSemaphore)
{
    auto s = As<KSemaphore>(GetHandle(A0), ObjType::Semaphore);
    if (!s) RETURN(STATUS_INVALID_HANDLE);
    int32_t prev = 0;
    bool over = false;
    Signal([&] { prev = s->count; if (s->count + int32_t(A1) > s->limit) over = true; else s->count += int32_t(A1); });
    if (A2) mem::W32(A2, uint32_t(prev));
    RETURN(over ? STATUS_SEMAPHORE_LIMIT_EXCEEDED : STATUS_SUCCESS);
}

IMPORT(KeInitializeSemaphore)
{
    // (KSEMAPHORE* Sem, s32 Count, s32 Limit)
    mem::W8(A0, 5);
    mem::W32(A0 + 4, A1);
    mem::W32(A0 + 0x10, A2);
    RegisterGuestObject(A0, std::make_shared<KSemaphore>(int32_t(A1), int32_t(A2)));
    RETURN(0);
}

IMPORT(KeReleaseSemaphore)
{
    // (KSEMAPHORE* Sem, s32 Increment, s32 Adjustment, u8 Wait) -> previous count
    auto s = As<KSemaphore>(ObjectFromGuest(A0), ObjType::Semaphore);
    if (!s) RETURN(0);
    int32_t prev = 0;
    Signal([&] { prev = s->count; s->count = std::min(s->count + int32_t(A2), s->limit); });
    RETURN(uint32_t(prev));
}

IMPORT(NtCreateMutant)
{
    // (u32* Handle, OBJECT_ATTRIBUTES* Attr, u32 InitialOwner)
    auto m = std::make_shared<KMutant>();
    if (A2) { m->owner = CurrentThread(); m->recursion = 1; }
    mem::W32(A0, CreateHandle(m));
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtReleaseMutant)
{
    auto m = As<KMutant>(GetHandle(A0), ObjType::Mutant);
    if (!m) RETURN(STATUS_INVALID_HANDLE);
    bool notOwned = false;
    Signal([&]
    {
        if (m->owner != CurrentThread()) { notOwned = true; return; }
        if (--m->recursion == 0) m->owner = nullptr;
    });
    RETURN(notOwned ? STATUS_MUTANT_NOT_OWNED : STATUS_SUCCESS);
}

// ====================================================================== waits

IMPORT(NtWaitForSingleObjectEx)
{
    // (u32 Handle, u32 WaitMode, u32 Alertable, s64* Timeout)
    auto o = GetHandle(A0);
    if (!o) RETURN(STATUS_INVALID_HANDLE);
    int64_t t;
    RETURN(Wait(&o, 1, false, A3 ? &(t = ReadTimeout(A3, t)) : nullptr));
}

IMPORT(NtWaitForMultipleObjectsEx)
{
    // (u32 Count, u32* Handles, u32 WaitType, u32 WaitMode, u32 Alertable, s64* Timeout)
    uint32_t n = std::min<uint32_t>(A0, 64);
    std::shared_ptr<KObject> objs[64];
    for (uint32_t i = 0; i < n; i++)
    {
        objs[i] = GetHandle(mem::R32(A1 + i * 4));
        if (!objs[i]) RETURN(STATUS_INVALID_HANDLE);
    }
    int64_t t;
    RETURN(Wait(objs, n, A2 == 0, A5 ? &(t = ReadTimeout(A5, t)) : nullptr));
}

IMPORT(KeWaitForSingleObject)
{
    // (void* Object, u32 Reason, u32 Mode, u32 Alertable, s64* Timeout)
    auto o = ObjectFromGuest(A0);
    int64_t t;
    RETURN(Wait(&o, 1, false, A4 ? &(t = ReadTimeout(A4, t)) : nullptr));
}

IMPORT(KeWaitForMultipleObjects)
{
    // (u32 Count, void** Objects, u32 WaitType, u32 Reason, u32 Mode, u32 Alertable, s64* Timeout, void* WaitBlocks)
    uint32_t n = std::min<uint32_t>(A0, 64);
    std::shared_ptr<KObject> objs[64];
    for (uint32_t i = 0; i < n; i++) objs[i] = ObjectFromGuest(mem::R32(A1 + i * 4));
    int64_t t;
    RETURN(Wait(objs, n, A2 == 0, A6 ? &(t = ReadTimeout(A6, t)) : nullptr));
}

// ====================================================================== handles / objects

IMPORT(NtClose) { RETURN(CloseHandle(A0) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE); }

IMPORT(NtDuplicateObject)
{
    // (u32 Handle, u32* NewHandle, u32 Options)
    auto o = GetHandle(A0);
    if (!o) RETURN(STATUS_INVALID_HANDLE);
    mem::W32(A1, CreateHandle(o));
    if (A2 & 1) CloseHandle(A0);      // DUPLICATE_CLOSE_SOURCE
    RETURN(STATUS_SUCCESS);
}

IMPORT(ObReferenceObjectByHandle)
{
    // (u32 Handle, u32 ObjectType, void** Object)
    auto o = GetHandle(A0);
    if (!o) RETURN(STATUS_INVALID_HANDLE);
    if (A2) mem::W32(A2, GuestPtrFor(o));
    RETURN(STATUS_SUCCESS);
}

IMPORT(ObReferenceObject)   { RETURN(0); }
IMPORT(ObDereferenceObject) { RETURN(0); }
IMPORT(ObIsTitleObject)     { RETURN(1); }

// ====================================================================== threads

IMPORT(ExCreateThread)
{
    // (u32* Handle, u32 StackSize, u32* ThreadId, u32 XapiStartup, u32 StartAddress, u32 StartContext, u32 Flags)
    bool suspended = A6 & 1;
    auto t = CreateThread(A1, A4, A5, A3, suspended);
    t->handle = CreateHandle(t);
    if (A0) mem::W32(A0, t->handle);
    if (A2) mem::W32(A2, t->id);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtResumeThread)
{
    auto t = As<GuestThread>(GetHandle(A0), ObjType::Thread);
    if (!t) RETURN(STATUS_INVALID_HANDLE);
    if (A1) mem::W32(A1, t->suspendCount);
    ResumeThread(t.get());
    RETURN(STATUS_SUCCESS);
}

IMPORT(ExTerminateThread) { ExitCurrentThread(A0); }

IMPORT(KeDelayExecutionThread)
{
    // (u32 Mode, u32 Alertable, s64* Interval)
    int64_t interval = int64_t(mem::R64(A2));
    if (interval < 0)
        std::this_thread::sleep_for(std::chrono::nanoseconds(-interval * 100));
    else
        std::this_thread::yield();
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtYieldExecution)          { std::this_thread::yield(); RETURN(STATUS_SUCCESS); }
IMPORT(KeSetAffinityThread)       { if (A2) mem::W32(A2, 0x3F); RETURN(0); }
IMPORT(KeSetBasePriorityThread)   { RETURN(0); }
IMPORT(KeQueryBasePriorityThread) { RETURN(0); }
IMPORT(KeSetDisableBoostThread)   { RETURN(0); }
IMPORT(KeGetCurrentProcessType)   { RETURN(1); }          // title process
IMPORT(KeSetCurrentProcessType)   { RETURN(0); }
IMPORT(KeEnterCriticalRegion)     { RETURN(0); }
IMPORT(KeLeaveCriticalRegion)     { RETURN(0); }
IMPORT(KeEnableFpuExceptions)     { RETURN(0); }
IMPORT(KeLockL2)                  { RETURN(0); }
IMPORT(KeUnlockL2)                { RETURN(0); }

// ====================================================================== spin locks

static void SpinAcquire(uint32_t p)
{
    auto* word = reinterpret_cast<std::atomic<uint32_t>*>(mem::Ptr(p));
    uint32_t self = __builtin_bswap32(CurrentThread() ? CurrentThread()->kthread : 1);
    uint32_t expected = 0;
    while (!word->compare_exchange_weak(expected, self, std::memory_order_acquire))
    {
        expected = 0;
        std::this_thread::yield();
    }
}

static void SpinRelease(uint32_t p)
{
    reinterpret_cast<std::atomic<uint32_t>*>(mem::Ptr(p))->store(0, std::memory_order_release);
}

IMPORT(KfAcquireSpinLock)               { SpinAcquire(A0); RETURN(0); }
IMPORT(KfReleaseSpinLock)               { SpinRelease(A0); RETURN(0); }
IMPORT(KeAcquireSpinLockAtRaisedIrql)   { SpinAcquire(A0); RETURN(0); }
IMPORT(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(A0); RETURN(0); }

// ====================================================================== TLS

static std::mutex s_tlsLock;
static uint64_t s_tlsUsed = 0;

IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(s_tlsLock);
    for (uint32_t i = 0; i < 64; i++)
        if (!(s_tlsUsed & (1ull << i))) { s_tlsUsed |= 1ull << i; RETURN(i); }
    RETURN(0xFFFFFFFF);
}

IMPORT(KeTlsFree)
{
    if (A0 >= 64) RETURN(0);
    std::lock_guard lock(s_tlsLock);
    s_tlsUsed &= ~(1ull << A0);
    RETURN(1);
}

IMPORT(KeTlsGetValue) { RETURN(A0 < 64 && CurrentThread() ? CurrentThread()->tlsSlots[A0] : 0); }
IMPORT(KeTlsSetValue)
{
    if (A0 >= 64 || !CurrentThread()) RETURN(0);
    CurrentThread()->tlsSlots[A0] = A1;
    RETURN(1);
}

// ====================================================================== time

static int64_t NowFiletime()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
}

IMPORT(KeQueryPerformanceFrequency) { RETURN(TscFrequency()); }
IMPORT(KeQuerySystemTime)           { if (A0) mem::W64(A0, uint64_t(NowFiletime())); RETURN(0); }

IMPORT(RtlTimeToTimeFields)
{
    // (s64* Time, TIME_FIELDS* Fields): Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (s16 each)
    int64_t ft = int64_t(mem::R64(A0));
    int64_t ms = ft / 10000;
    int64_t days = ms / 86400000;
    int64_t msOfDay = ms % 86400000;
    using namespace std::chrono;
    // days since 1601-01-01 -> civil date
    sys_days d = sys_days(year{ 1601 } / January / 1) + std::chrono::days(days);
    year_month_day ymd{ d };
    weekday wd{ d };
    uint32_t f = A1;
    mem::W16(f + 0, uint16_t(int(ymd.year())));
    mem::W16(f + 2, uint16_t(unsigned(ymd.month())));
    mem::W16(f + 4, uint16_t(unsigned(ymd.day())));
    mem::W16(f + 6, uint16_t(msOfDay / 3600000));
    mem::W16(f + 8, uint16_t(msOfDay / 60000 % 60));
    mem::W16(f + 10, uint16_t(msOfDay / 1000 % 60));
    mem::W16(f + 12, uint16_t(msOfDay % 1000));
    mem::W16(f + 14, uint16_t(wd.c_encoding()));
    RETURN(0);
}

IMPORT(RtlTimeFieldsToTime)
{
    // (TIME_FIELDS* Fields, s64* Time) -> bool
    using namespace std::chrono;
    uint32_t f = A0;
    year_month_day ymd{ year{ int16_t(mem::R16(f)) }, month{ mem::R16(f + 2) }, day{ mem::R16(f + 4) } };
    if (!ymd.ok()) RETURN(0);
    int64_t days = (sys_days(ymd) - sys_days(year{ 1601 } / January / 1)).count();
    int64_t ms = days * 86400000 + int64_t(mem::R16(f + 6)) * 3600000 + int64_t(mem::R16(f + 8)) * 60000 +
                 int64_t(mem::R16(f + 10)) * 1000 + mem::R16(f + 12);
    mem::W64(A1, uint64_t(ms * 10000));
    RETURN(1);
}
