// Shared pieces of the GPU stand-in.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace gpu
{
    constexpr uint32_t GPU_REGS = 0x7FC80000;      // where the game's GPU register writes land

    // physical (GPU) address -> guest virtual address in the physical mapping
    inline uint32_t PhysToVirt(uint32_t phys) { return (phys & 0x1FFFFFFF) | 0xA0000000; }

    // Minimal PM4 command processor. Executes the packets between `rptr` and
    // `wptr` (dword indices into the ring) and returns the new read pointer.
    // `interrupt(cpuMask)` is called for PM4 INTERRUPT packets.
    uint32_t ExecuteRing(uint32_t ringVirt, uint32_t ringDwords, uint32_t rptr, uint32_t wptr,
                         const std::function<void(uint32_t)>& interrupt);

    // Register file the command processor maintains (type-0 writes, SET_CONSTANT, ...)
    uint32_t ReadReg(uint32_t index);

    // One line per packet type seen, with counts (for the periodic log line)
    std::string PacketSummary();
}
