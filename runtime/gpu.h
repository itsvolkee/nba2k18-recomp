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

    // GPU counter: advanced on every vblank and every swap; EVENT_WRITE_SHD
    // packets that ask for it write its value.
    void IncrementCounter();

    // Frames presented (swap packets executed by the command processor)
    uint64_t SwapsExecuted();

    // Set the GPU registers the game reads but nothing writes (interrupt
    // status, EDRAM timing, viewport size, ...)
    void SeedRegisters();

    // Private swap packet VdSwap writes into the command stream (opcode 0x64 is
    // unused by the hardware), followed by "SWAP" and the frontbuffer address.
    constexpr uint32_t PM4_SWAP = 0x64;
    constexpr uint32_t SWAP_SIGNATURE = 0x53574150;   // "SWAP"

    // Register file the command processor maintains (type-0 writes, SET_CONSTANT, ...)
    uint32_t ReadReg(uint32_t index);

    // One line per packet type seen, with counts (for the periodic log line)
    std::string PacketSummary();
}
