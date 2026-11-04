// Graphics stand-in: just enough of the Xbox 360 GPU interface for the game to
// get past graphics setup. Nothing is drawn yet.
//
//  - a 60 Hz "vsync" that calls the game's graphics interrupt callback
//  - commands the game submits run through a minimal PM4 command processor
//    (gpu_cp.cpp): memory/event writes, interrupts, indirect buffers, waits;
//    drawing is skipped. The read-pointer writeback follows.
//  - video-mode queries return 1280x720, widescreen, 60 Hz
//  - VdSwap (one call per finished frame) is counted and logged
//  - DPCs queued with KeInsertQueueDpc run on a dedicated guest context
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

    std::mutex g_lock;
    uint32_t g_interruptCallback = 0, g_interruptData = 0;
    uint32_t g_ringBase = 0, g_ringSizeLog2 = 0;
    uint32_t g_rptrWriteback = 0;
    std::atomic<uint64_t> g_vsyncs{ 0 }, g_swaps{ 0 };
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

    void GpuThread()
    {
        auto ctx = CreateServiceContext(2);    // interrupts are delivered on CPU 2
        LOG_INFO("graphics stand-in running (60 Hz vsync, minimal command processor)");
        auto next = std::chrono::steady_clock::now();
        auto lastReport = next;
        uint64_t lastSwaps = 0;
        uint32_t rptr = 0;
        while (true)
        {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);

            uint32_t cb, data, wb, ring, ringLog2;
            {
                std::lock_guard lock(g_lock);
                cb = g_interruptCallback; data = g_interruptData; wb = g_rptrWriteback;
                ring = g_ringBase; ringLog2 = g_ringSizeLog2;
            }

            // run what the game has submitted, then report how far we got
            uint32_t wptr = mem::R32(GPU_REGS + REG_CP_RB_WPTR * 4);
            if (ring)
            {
                uint32_t ringDwords = (1u << (ringLog2 + 3)) / 4;
                rptr = gpu::ExecuteRing(PhysToVirt(ring), ringDwords, rptr, wptr % ringDwords,
                    [&](uint32_t cpuMask)
                    {
                        // command-processor interrupt: callback(source 1, user data)
                        if (cb) CallOnContext(*ctx, cb, { 1, data });
                    });
                if (wb) mem::W32(PhysToVirt(wb), rptr);
            }

            // vsync interrupt: callback(source 0 = vblank, user data)
            if (cb)
            {
                g_vsyncs++;
                CallOnContext(*ctx, cb, { 0, data });
            }

            auto now = std::chrono::steady_clock::now();
            if (now - lastReport >= std::chrono::seconds(5))
            {
                uint64_t swaps = g_swaps.load();
                LOG_INFO("graphics: {} vsyncs, {} frames finished ({:.1f}/s), ring read/write {}/{}; commands: {}",
                         g_vsyncs.load(), swaps, double(swaps - lastSwaps) / 5.0, rptr, wptr, gpu::PacketSummary());
                lastSwaps = swaps;
                lastReport = now;
            }
        }
    }

    void StartGpuThread()
    {
        std::call_once(g_threadStarted, [] { std::thread(GpuThread).detach(); });
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
            // DPC routine(KDPC* Dpc, void* Context, void* Arg1, void* Arg2)
            CallOnContext(*ctx, d.routine, { d.dpc, d.context, d.arg1, d.arg2 });
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

IMPORT(VdInitializeEngines)                        { RETURN(1); }
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
    // One call per finished frame. Real presentation comes with the renderer.
    uint64_t n = ++g_swaps;
    if (n == 1) LOG_INFO("first frame finished (VdSwap)");
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
