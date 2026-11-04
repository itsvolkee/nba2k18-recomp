#include "memory.h"
#include "log.h"
#include <map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace mem
{
    uint8_t* base = nullptr;

    struct Region
    {
        const char* name;
        uint32_t begin, end, next;
    };

    static Region s_virtual  { "virtual",  0x40000000, 0x7F000000, 0x40000000 };
    static Region s_system   { "system",   0x7F000000, 0x80000000, 0x7F000000 };
    static Region s_physical { "physical", 0xA0000000, 0xC0000000, 0xA0000000 };

    static std::mutex s_lock;
    static std::map<uint32_t, uint32_t> s_allocs;               // address -> size (live)
    static std::multimap<uint32_t, uint32_t> s_freeBySize;      // size -> address (freed, reusable)

    bool Init()
    {
        constexpr size_t size = 0x100000000ull;
#ifdef _WIN32
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
        if (!base)
        {
            LOG_ERROR("could not reserve 4 GB of guest memory");
            return false;
        }
        LOG_INFO("guest memory at {}", (void*)base);
        return true;
    }

    static uint32_t Alloc(Region& r, uint32_t size, uint32_t align)
    {
        if (size == 0) size = 1;
        if (align < 0x10) align = 0x10;
        size = (size + 0xF) & ~0xFu;

        std::lock_guard lock(s_lock);

        // reuse a freed block of the same region if one is big enough and aligned
        for (auto it = s_freeBySize.lower_bound(size); it != s_freeBySize.end(); ++it)
        {
            uint32_t a = it->second;
            if (a >= r.begin && a < r.end && (a % align) == 0)
            {
                uint32_t blockSize = it->first;
                s_freeBySize.erase(it);
                s_allocs[a] = blockSize;
                memset(base + a, 0, blockSize);
                return a;
            }
        }

        uint32_t a = (r.next + (align - 1)) & ~(align - 1);
        if (uint64_t(a) + size > r.end)
        {
            LOG_ERROR("out of guest {} memory (asked for 0x{:X})", r.name, size);
            return 0;
        }
        r.next = a + size;
        s_allocs[a] = size;
        memset(base + a, 0, size);
        return a;
    }

    uint32_t AllocVirtual(uint32_t size, uint32_t align)  { return Alloc(s_virtual, size, align); }
    uint32_t AllocPhysical(uint32_t size, uint32_t align) { return Alloc(s_physical, size, align); }
    uint32_t AllocSystem(uint32_t size, uint32_t align)   { return Alloc(s_system, size, align); }

    void Free(uint32_t address)
    {
        std::lock_guard lock(s_lock);
        auto it = s_allocs.find(address);
        if (it == s_allocs.end()) return;
        s_freeBySize.emplace(it->second, it->first);
        s_allocs.erase(it);
    }

    uint32_t SizeOf(uint32_t address)
    {
        std::lock_guard lock(s_lock);
        auto it = s_allocs.upper_bound(address);
        if (it == s_allocs.begin()) return 0;
        --it;
        if (address >= it->first && address < it->first + it->second)
            return it->second - (address - it->first);
        return 0;
    }
}
