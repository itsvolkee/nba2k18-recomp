// Guest memory: one 4 GB block that the recompiled code addresses through `base`.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mem
{
    extern uint8_t* base;

    bool Init();                                 // reserve + commit the 4 GB guest space

    // Guest address ranges (mirroring the Xbox 360 layout loosely):
    //   0x40000000-0x7EFFFFFF  virtual allocations (NtAllocateVirtualMemory, heaps)
    //   0x7F000000-0x7FBFFFFF  thread stacks, PCRs, TLS
    //   0x7FC80000-0x7FFFFFFF  GPU registers (the game's MMIO writes land here)
    //   0x82000000-...         executable image, then the function table
    //   0xA0000000-0xBFFFFFFF  physical allocations (MmAllocatePhysicalMemoryEx)
    uint32_t AllocVirtual(uint32_t size, uint32_t align = 0x10000);
    uint32_t AllocPhysical(uint32_t size, uint32_t align = 0x1000);
    uint32_t AllocSystem(uint32_t size, uint32_t align = 0x1000);
    void Free(uint32_t address);
    uint32_t SizeOf(uint32_t address);           // size of an allocation, 0 if unknown

    inline void* Ptr(uint32_t a) { return base + a; }
    template <typename T> inline T* Ptr(uint32_t a) { return reinterpret_cast<T*>(base + a); }

    // Big-endian accessors
    inline uint8_t  R8 (uint32_t a) { return base[a]; }
    inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, base + a, 2); return __builtin_bswap16(v); }
    inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, base + a, 4); return __builtin_bswap32(v); }
    inline uint64_t R64(uint32_t a) { uint64_t v; memcpy(&v, base + a, 8); return __builtin_bswap64(v); }
    inline void W8 (uint32_t a, uint8_t v)  { base[a] = v; }
    inline void W16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(base + a, &v, 2); }
    inline void W32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(base + a, &v, 4); }
    inline void W64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(base + a, &v, 8); }

    inline uint32_t HostToGuest(const void* p) { return uint32_t(static_cast<const uint8_t*>(p) - base); }
}
