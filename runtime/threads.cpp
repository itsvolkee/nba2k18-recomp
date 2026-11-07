// Guest threads: each one gets a host thread, a PPCContext, a stack, a PCR and TLS in guest memory.
#include "kernel.h"
#include <map>
#include <chrono>

namespace kernel
{
    // PCR / KTHREAD offsets (only the fields the game is expected to read)
    constexpr uint32_t PCR_SIZE            = 0x2D8;
    constexpr uint32_t PCR_TLS_PTR         = 0x000;
    constexpr uint32_t PCR_SELF            = 0x030;
    constexpr uint32_t PCR_STACK_BASE      = 0x070;
    constexpr uint32_t PCR_STACK_LIMIT     = 0x074;
    constexpr uint32_t PCR_CURRENT_THREAD  = 0x100;
    constexpr uint32_t PCR_CURRENT_CPU     = 0x10C;
    constexpr uint32_t KTHREAD_SIZE        = 0xAB0;
    constexpr uint32_t KTHREAD_STACK_BASE  = 0x05C;
    constexpr uint32_t KTHREAD_STACK_LIMIT = 0x060;
    constexpr uint32_t KTHREAD_THREAD_ID   = 0x14C;

    static thread_local std::shared_ptr<GuestThread> t_current;
    static std::mutex s_threadLock;
    static std::map<uint32_t, std::shared_ptr<GuestThread>> s_threads;   // by id
    static std::atomic<uint32_t> s_nextThreadId{ 1 };
    static std::atomic<uint32_t> s_nextCpu{ 0 };

    struct ThreadExit { uint32_t code; };

    GuestThread* CurrentThread() { return t_current.get(); }
    std::shared_ptr<GuestThread> CurrentThreadShared() { return t_current; }

    static void SetupGuestState(GuestThread& t, uint32_t stackSize)
    {
        if (stackSize < 0x10000) stackSize = 0x10000;
        stackSize = (stackSize + 0xFFF) & ~0xFFFu;
        t.stackSize = stackSize;
        t.stackBase = mem::AllocSystem(stackSize, 0x1000);          // lowest address
        uint32_t stackTop = t.stackBase + stackSize;

        // thread-local storage: copy the executable's TLS template
        auto& exe = Exe();
        uint32_t tlsSize = std::max(exe.tlsDataSize, exe.tlsRawSize) + 0x10;
        t.tlsStatic = mem::AllocSystem(tlsSize, 0x10);
        if (exe.tlsRawAddress && exe.tlsRawSize)
            memcpy(mem::Ptr(t.tlsStatic), mem::Ptr(exe.tlsRawAddress), exe.tlsRawSize);

        t.kthread = mem::AllocSystem(KTHREAD_SIZE, 0x10);
        mem::W8(t.kthread, 6);                                      // dispatcher type: thread
        mem::W32(t.kthread + KTHREAD_STACK_BASE, stackTop);
        mem::W32(t.kthread + KTHREAD_STACK_LIMIT, t.stackBase);
        mem::W32(t.kthread + KTHREAD_THREAD_ID, t.id);
        t.guestPtr = t.kthread;

        t.pcr = mem::AllocSystem(PCR_SIZE, 0x1000);
        mem::W32(t.pcr + PCR_TLS_PTR, t.tlsStatic);
        mem::W32(t.pcr + PCR_SELF, t.pcr);
        mem::W32(t.pcr + PCR_STACK_BASE, stackTop);
        mem::W32(t.pcr + PCR_STACK_LIMIT, t.stackBase);
        mem::W32(t.pcr + PCR_CURRENT_THREAD, t.kthread);
        mem::W8(t.pcr + PCR_CURRENT_CPU, uint8_t(s_nextCpu++ % 6));

        t.ctx = std::make_unique<PPCContext>();
        memset(t.ctx.get(), 0, sizeof(PPCContext));
        t.ctx->r1.u64 = (stackTop - 0x100) & ~0xFull;
        t.ctx->r13.u64 = t.pcr;
        t.ctx->fpscr.loadFromHost();
    }

    static void ThreadMain(std::shared_ptr<GuestThread> t)
    {
        t_current = t;
        {
            std::unique_lock lock(WaitLock());
            WaitCv().wait(lock, [&] { return t->suspendCount <= 0; });
        }
        LOG_INFO("thread {} starting at 0x{:08X}", t->id, t->xapiStartup ? t->xapiStartup : t->startAddress);
        try
        {
            PPCContext& ctx = *t->ctx;
            if (t->xapiStartup)
            {
                ctx.r3.u64 = t->startAddress;
                ctx.r4.u64 = t->startContext;
                CallGuest(ctx, t->xapiStartup);
            }
            else
            {
                ctx.r3.u64 = t->startContext;
                CallGuest(ctx, t->startAddress);
            }
            t->exitCode = ctx.r3.u32;
        }
        catch (const ThreadExit& e)
        {
            t->exitCode = e.code;
        }
        LOG_INFO("thread {} exited with 0x{:X}", t->id, t->exitCode);
        {
            std::lock_guard lock(WaitLock());
            t->finished = true;
            mem::W32(t->kthread + 4, 1);       // SignalState
        }
        WaitCv().notify_all();
    }

    std::shared_ptr<GuestThread> CreateThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                                              uint32_t xapiStartup, bool suspended)
    {
        auto t = std::make_shared<GuestThread>();
        t->id = s_nextThreadId++;
        t->startAddress = startAddress;
        t->startContext = startContext;
        t->xapiStartup = xapiStartup;
        t->suspendCount = suspended ? 1 : 0;
        SetupGuestState(*t, stackSize ? stackSize : Exe().defaultStackSize);
        {
            std::lock_guard lock(s_threadLock);
            s_threads[t->id] = t;
        }
        RegisterGuestObject(t->kthread, t);
        t->host = std::thread(ThreadMain, t);
        t->host.detach();
        return t;
    }

    void ResumeThread(GuestThread* t)
    {
        {
            std::lock_guard lock(WaitLock());
            if (t->suspendCount > 0) --t->suspendCount;
        }
        WaitCv().notify_all();
    }

    void StartWatchdog()
    {
        std::thread([]
        {
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::string report;
                std::lock_guard lock(s_threadLock);
                for (auto& [id, t] : s_threads)
                {
                    if (t->finished) continue;
                    int64_t since = t->waitSince.load();
                    const char* kind = t->startAddress ? "" : " (service)";
                    if (since)
                        report += std::format("\n    thread {}{}: waiting {:.1f}s on {} (called from 0x{:08X})",
                                              id, kind, (now - since) / 1000.0, t->waitWhat, t->waitCaller);
                    else
                        report += std::format("\n    thread {}{}: running", id, kind);
                }
                LOG_INFO("watchdog:{}", report);
            }
        }).detach();
    }

    void RunMainThread(uint32_t entry, uint32_t stackSize)
    {
        auto t = std::make_shared<GuestThread>();
        t->id = s_nextThreadId++;
        t->startAddress = entry;
        SetupGuestState(*t, stackSize);
        {
            std::lock_guard lock(s_threadLock);
            s_threads[t->id] = t;
        }
        RegisterGuestObject(t->kthread, t);
        t->handle = CreateHandle(t);
        ThreadMain(t);
    }

    std::shared_ptr<GuestThread> CreateServiceContext(uint32_t cpu)
    {
        auto t = std::make_shared<GuestThread>();
        t->id = s_nextThreadId++;
        SetupGuestState(*t, 0x40000);
        mem::W8(t->pcr + PCR_CURRENT_CPU, uint8_t(cpu));
        {
            std::lock_guard lock(s_threadLock);
            s_threads[t->id] = t;
        }
        RegisterGuestObject(t->kthread, t);
        return t;
    }

    void CallOnContext(GuestThread& t, uint32_t function, std::initializer_list<uint32_t> args)
    {
        // make CurrentThread() report this context while the guest code runs
        auto previous = t_current;
        {
            std::lock_guard lock(s_threadLock);
            auto it = s_threads.find(t.id);
            if (it != s_threads.end()) t_current = it->second;
        }
        PPCContext& ctx = *t.ctx;
        PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        size_t i = 0;
        for (uint32_t a : args) if (i < 8) regs[i++]->u64 = a;
        uint64_t sp = ctx.r1.u64;
        CallGuest(ctx, function);
        ctx.r1.u64 = sp;               // keep the stack balanced across repeated calls
        t_current = previous;
    }

    [[noreturn]] void ExitCurrentThread(uint32_t code)
    {
        throw ThreadExit{ code };
    }
}
