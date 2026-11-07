// Host-side implementation of the Xbox 360 kernel/XAM calls the game imports.
#pragma once
#include "memory.h"
#include "log.h"
#include <ppc_recomp_shared.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

// ---- calling-convention helpers for import implementations ----
#define IMPORT(name) PPC_FUNC(__imp__##name)
#define A0 ctx.r3.u32
#define A1 ctx.r4.u32
#define A2 ctx.r5.u32
#define A3 ctx.r6.u32
#define A4 ctx.r7.u32
#define A5 ctx.r8.u32
#define A6 ctx.r9.u32
#define A7 ctx.r10.u32
#define RETURN(v) do { ctx.r3.u64 = uint64_t(v); return; } while (0)

constexpr uint32_t STATUS_SUCCESS             = 0x00000000;
constexpr uint32_t STATUS_TIMEOUT             = 0x00000102;
constexpr uint32_t STATUS_PENDING             = 0x00000103;
constexpr uint32_t STATUS_ABANDONED           = 0x00000080;
constexpr uint32_t STATUS_UNSUCCESSFUL        = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED     = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE      = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER   = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE        = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE         = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY           = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED       = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED    = 0xC0000046;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_NO_MORE_FILES       = 0x80000006;

// ---- kernel objects ----
enum class ObjType { Event, Semaphore, Mutant, Thread, Timer, File, Other };

struct GuestThread;

struct KObject
{
    ObjType type;
    uint32_t guestPtr = 0;        // guest dispatcher object, if any
    explicit KObject(ObjType t) : type(t) {}
    virtual ~KObject() = default;

    // All waitable state is protected by one global lock, which keeps
    // multi-object waits simple. Called with kernel::WaitLock() held.
    virtual bool IsSignaled() const { return false; }
    virtual void Acquire(GuestThread*) {}        // consume the signal (auto-reset, count--, ownership)
};

struct KEvent : KObject
{
    bool manualReset, signaled;
    KEvent(bool manual, bool initial) : KObject(ObjType::Event), manualReset(manual), signaled(initial) {}
    bool IsSignaled() const override { return signaled; }
    void Acquire(GuestThread*) override { if (!manualReset) signaled = false; }
};

struct KSemaphore : KObject
{
    int32_t count, limit;
    KSemaphore(int32_t c, int32_t l) : KObject(ObjType::Semaphore), count(c), limit(l) {}
    bool IsSignaled() const override { return count > 0; }
    void Acquire(GuestThread*) override { --count; }
};

struct KMutant : KObject
{
    GuestThread* owner = nullptr;
    uint32_t recursion = 0;
    KMutant() : KObject(ObjType::Mutant) {}
    bool IsSignaled() const override;
    void Acquire(GuestThread* t) override { owner = t; ++recursion; }
};

struct GuestThread : KObject
{
    uint32_t id = 0, handle = 0;
    uint32_t stackBase = 0, stackSize = 0;
    uint32_t pcr = 0, kthread = 0, tlsStatic = 0;
    uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;
    std::unique_ptr<PPCContext> ctx;
    std::thread host;
    int suspendCount = 0;
    bool finished = false;
    uint32_t exitCode = 0;
    uint32_t tlsSlots[64] = {};

    // diagnostics for the watchdog: what this thread is waiting on, if anything
    std::atomic<int64_t> waitSince{ 0 };    // steady-clock ms; 0 = not waiting
    uint32_t waitCaller = 0;                // guest return address of the wait call
    char waitWhat[96] = {};

    GuestThread() : KObject(ObjType::Thread) {}
    bool IsSignaled() const override { return finished; }
};

namespace kernel
{
    std::mutex& WaitLock();
    std::condition_variable& WaitCv();       // notified whenever any object becomes signaled

    uint32_t CreateHandle(std::shared_ptr<KObject> obj);
    std::shared_ptr<KObject> GetHandle(uint32_t handle);
    bool CloseHandle(uint32_t handle);

    // Objects that live in guest memory (KEVENT etc. passed by pointer)
    std::shared_ptr<KObject> ObjectFromGuest(uint32_t dispatcherPtr);
    void RegisterGuestObject(uint32_t dispatcherPtr, std::shared_ptr<KObject> obj);   // replaces any existing one
    uint32_t GuestPtrFor(const std::shared_ptr<KObject>& obj);   // allocate a dispatcher header if needed

    // Wait for one or all objects. timeout100ns: nullptr = infinite, else relative (negative) or absolute.
    uint32_t Wait(std::shared_ptr<KObject>* objs, uint32_t count, bool waitAll, const int64_t* timeout100ns);

    GuestThread* CurrentThread();
    std::shared_ptr<GuestThread> CreateThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                                              uint32_t xapiStartup, bool suspended);
    void ResumeThread(GuestThread* t);
    [[noreturn]] void ExitCurrentThread(uint32_t code);

    // A guest context (stack, PCR, TLS) for host-driven work such as GPU
    // interrupts and DPCs. CallOnContext must always be used from the same
    // host thread for a given context.
    std::shared_ptr<GuestThread> CreateServiceContext(uint32_t cpu);
    void CallOnContext(GuestThread& t, uint32_t function, std::initializer_list<uint32_t> args);
    std::shared_ptr<GuestThread> CurrentThreadShared();

    // Reads a guest string (ANSI or UTF-16BE) into a host std::string
    std::string GuestString(uint32_t p, uint32_t maxLen = 0xFFFF);
    std::string GuestWString(uint32_t p, uint32_t maxChars = 0xFFFF);
    std::string AnsiStringStruct(uint32_t ansiStringPtr);     // ANSI_STRING { u16 len, u16 max, u32 buf }
    void RunMainThread(uint32_t entry, uint32_t stackSize);
    void StartWatchdog();       // logs what each guest thread is waiting on, every 15 s

    // Calls a guest function from host code.
    void CallGuest(PPCContext& ctx, uint32_t address);

    // Executable info (from the XEX header)
    struct ExeInfo
    {
        uint32_t entry = 0, imageBase = 0, imageSize = 0;
        uint32_t defaultStackSize = 0x40000;
        uint32_t tlsSlotCount = 0, tlsRawAddress = 0, tlsDataSize = 0, tlsRawSize = 0;
        uint32_t guestXexHeader = 0;      // copy of the XEX header in guest memory
        uint32_t guestModule = 0;         // fake LDR_DATA_TABLE_ENTRY for XexGetModuleHandle
    };
    ExeInfo& Exe();

    // Performance counter: the recompiled mftb reads the host TSC, so the
    // frequency we report must be the host TSC frequency.
    uint64_t TscFrequency();

    std::string GameDirectory();
    void SetGameDirectory(const std::string& dir);
}
