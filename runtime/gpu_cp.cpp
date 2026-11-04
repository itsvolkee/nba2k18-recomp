// Minimal PM4 command processor for the GPU stand-in.
//
// Walks the command stream the game submits and carries out only the packets
// that signal the CPU or chain more commands. Drawing and state packets are
// decoded and skipped. Every packet type is counted and logged the first
// time it appears, so the log shows what the game is sending.
//
// Packet header (big-endian dword):
//   type 0: write `count` registers starting at `base` (bit 15: all to one register)
//   type 1: write two registers
//   type 2: filler
//   type 3: opcode + `count` payload dwords
//
// Opcode values follow the Xenos/R500 PM4 set as documented by open-source
// Xbox 360 emulators; the less certain ones are only logged, not acted on.
#include "gpu.h"
#include "kernel.h"
#include <chrono>
#include <map>
#include <set>

namespace gpu
{
    namespace
    {
        uint32_t g_regs[0x10000];
        std::atomic<uint32_t> g_vblank{ 0 };

        // log the first few occurrences of something, then go quiet
        bool LogFew(const char* key, int limit = 8)
        {
            static std::mutex m;
            static std::map<std::string, int> counts;
            std::lock_guard lock(m);
            return ++counts[key] <= limit;
        }
        std::mutex g_statLock;
        std::map<std::string, uint64_t> g_counts;
        std::set<std::string> g_seen;

        const char* OpcodeName(uint32_t op)
        {
            switch (op)
            {
            case 0x10: return "NOP";
            case 0x21: return "REG_RMW";
            case 0x22: return "DRAW_INDX";
            case 0x23: return "VIZ_QUERY";
            case 0x26: return "WAIT_FOR_IDLE";
            case 0x27: return "IM_LOAD";
            case 0x2B: return "IM_LOAD_IMMEDIATE";
            case 0x2D: return "SET_CONSTANT";
            case 0x2F: return "LOAD_ALU_CONSTANT";
            case 0x36: return "DRAW_INDX_2";
            case 0x37: return "INDIRECT_BUFFER_PFD";
            case 0x3B: return "INVALIDATE_STATE";
            case 0x3C: return "WAIT_REG_MEM";
            case 0x3D: return "MEM_WRITE";
            case 0x3F: return "INDIRECT_BUFFER";
            case 0x45: return "COND_WRITE";
            case 0x46: return "EVENT_WRITE";
            case 0x48: return "ME_INIT";
            case 0x54: return "INTERRUPT";
            case 0x55: return "SET_CONSTANT2";
            case 0x56: return "SET_SHADER_CONSTANTS";
            case 0x58: return "EVENT_WRITE_SHD";
            case 0x59: return "EVENT_WRITE_EXT";
            case 0x5B: return "EVENT_WRITE_ZPD";
            case 0x5E: return "CONTEXT_UPDATE";
            case 0x60: return "SET_BIN_MASK_LO";
            case 0x61: return "SET_BIN_MASK_HI";
            case 0x62: return "SET_BIN_SELECT_LO";
            case 0x63: return "SET_BIN_SELECT_HI";
            default:   return nullptr;
            }
        }

        void Count(const std::string& what, uint32_t detail)
        {
            std::lock_guard lock(g_statLock);
            g_counts[what]++;
            if (g_seen.insert(what).second)
                LOG_INFO("GPU command first seen: {} (0x{:X})", what, detail);
        }

        // GPU memory writes carry an endian mode in the low 2 address bits.
        // 2 (8-in-32) is what makes the value read back correctly from the CPU.
        void GpuStore(uint32_t addressWithEndian, uint32_t value)
        {
            uint32_t endian = addressWithEndian & 3;
            uint32_t a = PhysToVirt(addressWithEndian & ~3u);
            switch (endian)
            {
            case 0: memcpy(mem::Ptr(a), &value, 4); break;                                 // no swap
            case 1: { uint32_t v = ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); memcpy(mem::Ptr(a), &v, 4); break; }
            case 2: mem::W32(a, value); break;                                             // 8-in-32
            case 3: { uint32_t v = (value << 16) | (value >> 16); mem::W32(a, v); break; } // 16-in-32
            }
        }

        uint32_t GpuLoad(uint32_t addressWithEndian)
        {
            uint32_t endian = addressWithEndian & 3;
            uint32_t a = PhysToVirt(addressWithEndian & ~3u);
            uint32_t raw;
            memcpy(&raw, mem::Ptr(a), 4);
            switch (endian)
            {
            case 0: return raw;
            case 1: return ((raw & 0x00FF00FF) << 8) | ((raw >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(raw);
            default: { uint32_t v = __builtin_bswap32(raw); return (v << 16) | (v >> 16); }
            }
        }

        void WriteReg(uint32_t index, uint32_t value)
        {
            index &= 0xFFFF;
            g_regs[index] = value;
            // Scratch registers (0x578-0x57F) are mirrored to memory when their
            // bit is set in SCRATCH_UMSK (0x1DC), at SCRATCH_ADDR (0x1DD) + n*4.
            // Some games use this as a fence.
            if (index >= 0x578 && index <= 0x57F)
            {
                uint32_t n = index - 0x578;
                if ((g_regs[0x1DC] >> n) & 1)
                    GpuStore(g_regs[0x1DD] + n * 4 | 2, value);
            }
            // keep the MMIO view in sync so CPU reads of GPU registers see the value
            if (index != 0x01C5)            // never overwrite the game's ring write pointer
                mem::W32(GPU_REGS + index * 4, value);
        }

        bool Compare(uint32_t func, uint32_t value, uint32_t ref)
        {
            switch (func & 7)
            {
            case 0: return false;
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            default: return true;
            }
        }

        struct Stream
        {
            uint32_t base, sizeDwords, index;
            uint32_t Read() { uint32_t v = mem::R32(base + (index % sizeDwords) * 4); index++; return v; }
        };

        void ExecuteStream(Stream& s, uint32_t end, const std::function<void(uint32_t)>& interrupt, int depth);

        // Returns false when a wait isn't satisfied yet: the stream stops at that
        // packet and is retried on the next tick (like a real command processor stalling).
        bool ExecutePacket(Stream& s, const std::function<void(uint32_t)>& interrupt, int depth)
        {
            uint32_t packetStart = s.index;
            uint32_t h = s.Read();
            switch (h >> 30)
            {
            case 0:
            {
                uint32_t count = ((h >> 16) & 0x3FFF) + 1;
                uint32_t reg = h & 0x7FFF;
                bool oneReg = h & 0x8000;
                Count("type0 register write", reg);
                for (uint32_t i = 0; i < count; i++) WriteReg(oneReg ? reg : reg + i, s.Read());
                return true;
            }
            case 1:
            {
                Count("type1 register write", h & 0x7FF);
                WriteReg(h & 0x7FF, s.Read());
                WriteReg((h >> 11) & 0x7FF, s.Read());
                return true;
            }
            case 2:
                return true;
            }

            uint32_t op = (h >> 8) & 0x7F;
            uint32_t count = ((h >> 16) & 0x3FFF) + 1;
            uint32_t payloadStart = s.index;
            const char* name = OpcodeName(op);
            Count(name ? name : std::format("unknown opcode 0x{:02X}", op), count);

            auto arg = [&](uint32_t i) { return mem::R32(s.base + ((payloadStart + i) % s.sizeDwords) * 4); };

            switch (op)
            {
            case 0x3D:  // MEM_WRITE: address, then data dwords
            {
                uint32_t addr = arg(0);
                if (LogFew("memwrite")) LOG_INFO("GPU MEM_WRITE to 0x{:08X}: {} dwords, first 0x{:X}", addr, count - 1, count > 1 ? arg(1) : 0);
                for (uint32_t i = 1; i < count; i++) GpuStore(addr + (i - 1) * 4, arg(i));
                break;
            }
            case 0x58:  // EVENT_WRITE_SHD: initiator, address, value
            case 0x59:  // EVENT_WRITE_EXT
                if (count >= 3)
                {
                    // initiator bit 31 asks for the GPU's vblank counter instead of the value
                    bool counter = arg(0) >> 31;
                    uint32_t value = counter ? g_vblank.load() : arg(2);
                    GpuStore(arg(1), value);
                    if (LogFew("eventwrite"))
                        LOG_INFO("GPU EVENT_WRITE_SHD to 0x{:08X} = 0x{:X}{} (initiator 0x{:08X})",
                                 arg(1), value, counter ? " (vblank counter)" : "", arg(0));
                }
                break;
            case 0x54:  // INTERRUPT: cpu mask
                if (LogFew("interrupt")) LOG_INFO("GPU INTERRUPT (cpu mask 0x{:X})", count >= 1 ? arg(0) : 1);
                interrupt(count >= 1 ? arg(0) : 1);
                break;
            case 0x3F:  // INDIRECT_BUFFER: address, length (dwords)
            case 0x37:
                if (depth < 4 && count >= 2)
                {
                    uint32_t len = arg(1) & 0xFFFFF;
                    Stream sub{ PhysToVirt(arg(0) & ~3u), len ? len : 1, 0 };
                    ExecuteStream(sub, len, interrupt, depth + 1);
                }
                break;
            case 0x3C:  // WAIT_REG_MEM: info, address/register, reference, mask, wait
            {
                uint32_t info = arg(0), where = arg(1), ref = arg(2), mask = arg(3);
                bool memory = info & 0x10;
                auto read = [&] { return (memory ? GpuLoad(where) : g_regs[where & 0xFFFF]) & mask; };
                if (!Compare(info, read(), ref))
                {
                    if (depth == 0)
                    {
                        // stall: retry this packet on the next tick
                        if (LogFew("waitstall", 4))
                            LOG_INFO("GPU stalled on WAIT_REG_MEM ({} 0x{:08X} & 0x{:X} func {} ref 0x{:X}, now 0x{:X})",
                                     memory ? "memory" : "register", where, mask, info & 7, ref, read());
                        s.index = packetStart;
                        return false;
                    }
                    // inside an indirect buffer we can't resume mid-buffer: wait briefly
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                    while (!Compare(info, read(), ref) && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    if (!Compare(info, read(), ref) && LogFew("waitgiveup"))
                        LOG_WARN("GPU WAIT_REG_MEM in indirect buffer gave up ({} 0x{:08X} & 0x{:X} func {} ref 0x{:X}, value 0x{:X})",
                                 memory ? "memory" : "register", where, mask, info & 7, ref, read());
                }
                break;
            }
            case 0x45:  // COND_WRITE: info, poll address, ref, mask, write address, data
            {
                uint32_t info = arg(0), poll = arg(1), ref = arg(2), mask = arg(3);
                uint32_t value = ((info & 0x10) ? GpuLoad(poll) : g_regs[poll & 0xFFFF]) & mask;
                if (Compare(info, value, ref))
                {
                    if (info & 0x100) GpuStore(arg(4), arg(5));
                    else WriteReg(arg(4), arg(5));
                }
                break;
            }
            case 0x21:  // REG_RMW: register, AND mask, OR mask
            {
                uint32_t reg = arg(0) & 0x1FFF;
                WriteReg(reg, (g_regs[reg] & arg(1)) | arg(2));
                break;
            }
            case 0x2D:  // SET_CONSTANT: offset/type, values
            case 0x55:
            {
                uint32_t p0 = arg(0);
                uint32_t type = (p0 >> 16) & 0xFF, index = p0 & 0x7FF;
                static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                uint32_t baseReg = type < 5 ? bases[type] : 0x2000;
                for (uint32_t i = 1; i < count; i++) WriteReg(baseReg + index + (i - 1), arg(i));
                break;
            }
            default:
                break;      // decoded and skipped
            }
            s.index = payloadStart + count;
            return true;
        }

        void ExecuteStream(Stream& s, uint32_t end, const std::function<void(uint32_t)>& interrupt, int depth)
        {
            uint32_t guard = 0;
            while (s.index != end && guard++ < 0x100000)
                if (!ExecutePacket(s, interrupt, depth)) break;
        }
    }

    uint32_t ExecuteRing(uint32_t ringVirt, uint32_t ringDwords, uint32_t rptr, uint32_t wptr,
                         const std::function<void(uint32_t)>& interrupt)
    {
        if (!ringDwords || rptr == wptr) return wptr;
        Stream s{ ringVirt, ringDwords, rptr };
        uint32_t end = wptr < rptr ? wptr + ringDwords : wptr;      // unwrap
        ExecuteStream(s, end, interrupt, 0);
        return s.index % ringDwords;
    }

    uint32_t ReadReg(uint32_t index) { return g_regs[index & 0xFFFF]; }
    void SetVblankCount(uint32_t n) { g_vblank = n; }

    std::string PacketSummary()
    {
        std::lock_guard lock(g_statLock);
        std::string s;
        for (auto& [k, v] : g_counts)
        {
            if (!s.empty()) s += ", ";
            s += std::format("{} x{}", k, v);
        }
        return s.empty() ? "none" : s;
    }
}
