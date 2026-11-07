// Graphics stand-in: just enough of the Xbox 360 GPU interface for the game to
// run its frame loop. Nothing is drawn yet.
//
//  - vsync thread: 60 Hz, advances the GPU counter and delivers the vblank
//    interrupt (source 0) on CPU 2
//  - command-processor thread: runs what the game submits through a minimal
//    PM4 command processor (gpu_cp.cpp) and delivers command-stream
//    interrupts (source 1) to the CPUs in their mask. Kept separate from vsync
//    so a blocking wait in the command stream can't stop vblanks.
//  - interrupts run as a DPC would: IRQL 2 and "DPC active" set in the PCR
//  - VdSwap fills the slot the game reserves in its command stream with the
//    frontbuffer fetch constant and a private swap packet the command
//    processor counts as a presented frame
//  - video-mode queries return 1280x720, widescreen, 60 Hz
//  - DPCs queued with KeInsertQueueDpc run on a dedicated guest context
//
// Behaviour follows what the open-source Xenia emulator does for the same
// interfaces; the code here is written for this runtime.
#include "kernel.h"
#include "gpu.h"
#include <chrono>
#include <deque>

using namespace kernel;

namespace
{
    using gpu::PhysToVirt;
    using gpu::GPU_REGS;
    constexpr uint32_t REG_CP_RB_WPTR    = 0x01C5;      // ring buffer write pointer (dwords)
    constexpr uint32_t PCR_IRQL          = 0x018;
    constexpr uint32_t PCR_DPC_ACTIVE    = 0x150;       // KPRCB (at 0x100) + 0x50

    std::mutex g_lock;
    uint32_t g_interruptCallback = 0, g_interruptData = 0;
    uint32_t g_ringBase = 0, g_ringSizeLog2 = 0;
    uint32_t g_rptrWriteback = 0;
    std::atomic<uint64_t> g_vsyncs{ 0 }, g_swapCalls{ 0 };
    std::atomic<uint32_t> g_rptr{ 0 }, g_wptr{ 0 };
    std::once_flag g_threadStarted;

    void WriteVideoMode(uint32_t p)
    {
        // X_VIDEO_MODE
        mem::W32(p + 0x00, 1280);           // display width
        mem::W32(p + 0x04, 720);            // display height
        mem::W32(p + 0x08, 0);              // interlaced
        mem::W32(p + 0x0C, 1);              // widescreen
        mem::W32(p + 0x10, 1);              // high definition
        float hz = 60.0f; uint32_t bits; memcpy(&bits, &hz, 4);
        mem::W32(p + 0x14, bits);           // refresh rate
        mem::W32(p + 0x18, 1);              // video standard: NTSC-M
        mem::W32(p + 0x1C, 0x4A);
        mem::W32(p + 0x20, 0x01);
        mem::W32(p + 0x24, 0);
        mem::W32(p + 0x28, 0);
        mem::W32(p + 0x2C, 0);
    }

    // Call the game's graphics interrupt callback the way the kernel does:
    // from a DPC (IRQL 2, DPC active) on the given context's CPU.
    void DispatchInterrupt(GuestThread& ctx, uint32_t source)
    {
        uint32_t cb, data;
        {
            std::lock_guard lock(g_lock);
            cb = g_interruptCallback; data = g_interruptData;
        }
        if (!cb) return;
        uint8_t irql = mem::R8(ctx.pcr + PCR_IRQL);
        uint32_t dpc = mem::R32(ctx.pcr + PCR_DPC_ACTIVE);
        mem::W8(ctx.pcr + PCR_IRQL, 2);
        mem::W32(ctx.pcr + PCR_DPC_ACTIVE, 1);
        CallOnContext(ctx, cb, { source, data });
        mem::W8(ctx.pcr + PCR_IRQL, irql);
        mem::W32(ctx.pcr + PCR_DPC_ACTIVE, dpc);
    }

    void VsyncThread()
    {
        auto ctx = CreateServiceContext(2);    // vblank interrupts are delivered on CPU 2
        auto next = std::chrono::steady_clock::now();
        auto lastReport = next;
        uint64_t lastSwaps = 0;
        while (true)
        {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
            gpu::IncrementCounter();
            g_vsyncs++;
            DispatchInterrupt(*ctx, 0);

            auto now = std::chrono::steady_clock::now();
            if (now - lastReport >= std::chrono::seconds(5))
            {
                uint64_t swaps = gpu::SwapsExecuted();
                LOG_INFO("graphics: {} vsyncs, {} VdSwap calls, {} frames presented ({:.1f}/s), ring read/write {}/{}; commands: {}",
                         g_vsyncs.load(), g_swapCalls.load(), swaps, double(swaps - lastSwaps) / 5.0,
                         g_rptr.load(), g_wptr.load(), gpu::PacketSummary());
                lastSwaps = swaps;
                lastReport = now;
            }
        }
    }

    void CommandProcessorThread()
    {
        // one guest context per hardware thread, created when first needed
        std::shared_ptr<GuestThread> cpus[6];
        uint32_t rptr = 0;
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            uint32_t wb, ring, ringLog2;
            {
                std::lock_guard lock(g_lock);
                wb = g_rptrWriteback; ring = g_ringBase; ringLog2 = g_ringSizeLog2;
            }
            if (!ring) continue;
            uint32_t ringDwords = (1u << (ringLog2 + 3)) / 4;
            uint32_t wptr = mem::R32(GPU_REGS + REG_CP_RB_WPTR * 4) % ringDwords;
            g_wptr = wptr;
            if (wptr == rptr) continue;
            rptr = gpu::ExecuteRing(PhysToVirt(ring), ringDwords, rptr, wptr,
                [&](uint32_t cpuMask)
                {
                    for (uint32_t n = 0; n < 6; n++)
                    {
                        if (!(cpuMask & (1u << n))) continue;
                        if (!cpus[n]) cpus[n] = CreateServiceContext(n);
                        DispatchInterrupt(*cpus[n], 1);
                    }
                });
            g_rptr = rptr;
            if (wb) mem::W32(PhysToVirt(wb), rptr);
        }
    }

    void StartGpuThread()
    {
        std::call_once(g_threadStarted, []
        {
            gpu::SeedRegisters();
            std::thread(VsyncThread).detach();
            std::thread(CommandProcessorThread).detach();
            LOG_INFO("graphics stand-in running (60 Hz vsync, minimal command processor)");
        });
    }

    // ---- DPCs ----
    struct Dpc { uint32_t dpc, routine, context, arg1, arg2; };
    std::mutex g_dpcLock;
    std::condition_variable g_dpcCv;
    std::deque<Dpc> g_dpcs;
    std::once_flag g_dpcStarted;

    void DpcThread()
    {
        auto ctx = CreateServiceContext(0);
        while (true)
        {
            Dpc d;
            {
                std::unique_lock lock(g_dpcLock);
                g_dpcCv.wait(lock, [] { return !g_dpcs.empty(); });
                d = g_dpcs.front();
                g_dpcs.pop_front();
            }
            // DPC routine(KDPC* Dpc, void* Context, void* Arg1, void* Arg2), at IRQL 2
            mem::W8(ctx->pcr + PCR_IRQL, 2);
            mem::W32(ctx->pcr + PCR_DPC_ACTIVE, 1);
            CallOnContext(*ctx, d.routine, { d.dpc, d.context, d.arg1, d.arg2 });
            mem::W8(ctx->pcr + PCR_IRQL, 0);
            mem::W32(ctx->pcr + PCR_DPC_ACTIVE, 0);
        }
    }
}

// ====================================================================== video mode

IMPORT(VdQueryVideoMode)    { WriteVideoMode(A0); RETURN(0); }
IMPORT(XGetVideoMode)       { WriteVideoMode(A0); RETURN(0); }
IMPORT(VdQueryVideoFlags)   { RETURN(0x7); }                  // widescreen | 1280x720 | hi-def
IMPORT(VdGetCurrentDisplayGamma)
{
    // (u32* Type, float* Power)
    mem::W32(A0, 2);
    float g = 2.22222233f; uint32_t bits; memcpy(&bits, &g, 4);
    mem::W32(A1, bits);
    RETURN(0);
}

// ====================================================================== engine / EDRAM setup

IMPORT(VdInitializeEngines)                        { gpu::SeedRegisters(); RETURN(1); }
IMPORT(VdShutdownEngines)                          { RETURN(0); }
IMPORT(VdIsHSIOTrainingSucceeded)                  { RETURN(1); }
IMPORT(VdRetrainEDRAM)                             { RETURN(0); }
IMPORT(VdRetrainEDRAMWorker)                       { RETURN(0); }
IMPORT(VdCallGraphicsNotificationRoutines)         { RETURN(0); }
IMPORT(VdEnableDisableClockGating)                 { RETURN(0); }
IMPORT(VdPersistDisplay)                           { if (A1) mem::W32(A1, 0); RETURN(1); }
IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { RETURN(0); }

IMPORT(VdSetGraphicsInterruptCallback)
{
    // (void* Callback, void* UserData)
    {
        std::lock_guard lock(g_lock);
        g_interruptCallback = A0;
        g_interruptData = A1;
    }
    LOG_INFO("graphics interrupt callback 0x{:08X} (data 0x{:08X})", A0, A1);
    StartGpuThread();
    RETURN(0);
}

IMPORT(VdInitializeRingBuffer)
{
    // (u32 PhysicalAddress, u32 SizeLog2)
    std::lock_guard lock(g_lock);
    g_ringBase = A0;
    g_ringSizeLog2 = A1;
    LOG_INFO("ring buffer at physical 0x{:08X}, size log2 {}", A0, A1);
    RETURN(0);
}

IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    // (u32 PhysicalAddress, u32 BlockSizeLog2)
    std::lock_guard lock(g_lock);
    g_rptrWriteback = A0;
    LOG_INFO("ring buffer read-pointer writeback at physical 0x{:08X}", A0);
    RETURN(0);
}

IMPORT(VdGetSystemCommandBuffer)
{
    // (u32* Buffer, u32* Size)
    static uint32_t buf = mem::AllocPhysical(0x94000, 0x1000);
    if (A0) mem::W32(A0, buf);
    if (A1) mem::W32(A1, 0x94000);
    RETURN(0);
}

IMPORT(VdSwap)
{
    // (u32* Buffer, u32* FetchConstant, u32* SystemWriteback, u32* SystemBuffer, u32 Unk,
    //  u32* Frontbuffer, u32* TextureFormat, u32* ColorSpace, u32* Width, u32* Height)
    // The game reserves 64 dwords in its command stream for the kernel to fill.
    // We put the frontbuffer fetch constant and our private swap packet there;
    // the command processor counts it as a presented frame when it gets to it.
    uint32_t buf = A0, fetch = A1, frontbuffer = A5 ? mem::R32(A5) : 0;
    uint64_t n = ++g_swapCalls;
    if (n == 1) LOG_INFO("first frame finished (VdSwap), frontbuffer 0x{:08X}", frontbuffer);
    if (!buf) RETURN(0);
    uint32_t o = 0;
    auto put = [&](uint32_t v) { mem::W32(buf + (o++) * 4, v); };
    put((5u << 16) | 0x4800);                                   // type 0: 6 registers at SHADER_CONSTANT_FETCH_00_0
    for (uint32_t i = 0; i < 6; i++) put(fetch ? mem::R32(fetch + i * 4) : 0);
    put((3u << 30) | (3u << 16) | (gpu::PM4_SWAP << 8));        // type 3: swap, 4 payload dwords
    put(gpu::SWAP_SIGNATURE);
    put(frontbuffer);
    put(1280);
    put(720);
    while (o < 64) put(2u << 30);                               // type 2 filler
    RETURN(0);
}

// ====================================================================== DPCs

IMPORT(KeInitializeDpc)
{
    // (KDPC* Dpc, void* Routine, void* Context)
    uint32_t d = A0;
    mem::W16(d + 0x00, 19);       // DpcObject
    mem::W32(d + 0x0C, A1);       // DeferredRoutine
    mem::W32(d + 0x10, A2);       // DeferredContext
    RETURN(0);
}

IMPORT(KeInsertQueueDpc)
{
    // (KDPC* Dpc, void* Arg1, void* Arg2) -> queued
    uint32_t d = A0;
    Dpc dpc{ d, mem::R32(d + 0x0C), mem::R32(d + 0x10), A1, A2 };
    if (!dpc.routine) RETURN(0);
    std::call_once(g_dpcStarted, [] { std::thread(DpcThread).detach(); });
    {
        std::lock_guard lock(g_dpcLock);
        g_dpcs.push_back(dpc);
    }
    g_dpcCv.notify_one();
    RETURN(1);
}

IMPORT(KiApcNormalRoutineNop) { RETURN(0); }
