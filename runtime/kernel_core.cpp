// Handles, waits, guest threads and guest calls.
#include "kernel.h"
#include <chrono>
#include <map>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace kernel
{
    static std::mutex s_waitLock;
    static std::condition_variable s_waitCv;
    std::mutex& WaitLock() { return s_waitLock; }
    std::condition_variable& WaitCv() { return s_waitCv; }

    // ---------------- handles ----------------
    static std::mutex s_handleLock;
    static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_handles;
    static uint32_t s_nextHandle = 0x1000;

    uint32_t CreateHandle(std::shared_ptr<KObject> obj)
    {
        std::lock_guard lock(s_handleLock);
        uint32_t h = s_nextHandle;
        s_nextHandle += 4;
        s_handles[h] = std::move(obj);
        return h;
    }

    std::shared_ptr<KObject> GetHandle(uint32_t handle)
    {
        if (handle == 0xFFFFFFFE) // NtCurrentThread()
        {
            // the thread object is kept alive by the thread table
            extern std::shared_ptr<GuestThread> CurrentThreadShared();
            return CurrentThreadShared();
        }
        std::lock_guard lock(s_handleLock);
        auto it = s_handles.find(handle);
        return it == s_handles.end() ? nullptr : it->second;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(s_handleLock);
        return s_handles.erase(handle) != 0;
    }

    // ---------------- objects living in guest memory ----------------
    static std::mutex s_guestObjLock;
    static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_guestObjects;

    std::shared_ptr<KObject> ObjectFromGuest(uint32_t p)
    {
        std::lock_guard lock(s_guestObjLock);
        auto it = s_guestObjects.find(p);
        if (it != s_guestObjects.end()) return it->second;

        // DISPATCHER_HEADER: +0 type (u8), +4 SignalState (s32)
        uint8_t type = mem::R8(p);
        int32_t signal = int32_t(mem::R32(p + 4));
        std::shared_ptr<KObject> obj;
        switch (type)
        {
        case 0: obj = std::make_shared<KEvent>(true, signal != 0); break;    // NotificationEvent
        case 1: obj = std::make_shared<KEvent>(false, signal != 0); break;   // SynchronizationEvent
        case 2: obj = std::make_shared<KMutant>(); break;
        case 5: obj = std::make_shared<KSemaphore>(signal, int32_t(mem::R32(p + 0x10))); break;
        default:
            LOG_WARN("unknown dispatcher object type {} at 0x{:08X}, treating as event", type, p);
            obj = std::make_shared<KEvent>(false, signal != 0);
            break;
        }
        obj->guestPtr = p;
        s_guestObjects[p] = obj;
        return obj;
    }

    void RegisterGuestObject(uint32_t p, std::shared_ptr<KObject> obj)
    {
        std::lock_guard lock(s_guestObjLock);
        obj->guestPtr = p;
        s_guestObjects[p] = std::move(obj);
    }

    std::string GuestString(uint32_t p, uint32_t maxLen)
    {
        std::string s;
        if (!p) return s;
        for (uint32_t i = 0; i < maxLen; i++)
        {
            char c = char(mem::R8(p + i));
            if (!c) break;
            s += c;
        }
        return s;
    }

    std::string GuestWString(uint32_t p, uint32_t maxChars)
    {
        std::string s;
        if (!p) return s;
        for (uint32_t i = 0; i < maxChars; i++)
        {
            uint16_t c = mem::R16(p + i * 2);
            if (!c) break;
            s += c < 0x80 ? char(c) : '?';
        }
        return s;
    }

    std::string AnsiStringStruct(uint32_t a)
    {
        if (!a) return {};
        uint16_t len = mem::R16(a);
        uint32_t buf = mem::R32(a + 4);
        return std::string(static_cast<const char*>(mem::Ptr(buf)), len);
    }

    uint32_t GuestPtrFor(const std::shared_ptr<KObject>& obj)
    {
        if (obj->guestPtr) return obj->guestPtr;
        if (obj->type == ObjType::Thread) return static_cast<GuestThread*>(obj.get())->kthread;
        uint32_t p = mem::AllocSystem(0x20, 0x10);
        uint8_t type = 1;
        if (obj->type == ObjType::Event) type = static_cast<KEvent*>(obj.get())->manualReset ? 0 : 1;
        else if (obj->type == ObjType::Mutant) type = 2;
        else if (obj->type == ObjType::Semaphore) type = 5;
        mem::W8(p, type);
        std::lock_guard lock(s_guestObjLock);
        obj->guestPtr = p;
        s_guestObjects[p] = obj;
        return p;
    }

    // ---------------- waits ----------------
    uint32_t Wait(std::shared_ptr<KObject>* objs, uint32_t count, bool waitAll, const int64_t* timeout100ns)
    {
        GuestThread* self = CurrentThread();
        std::unique_lock lock(s_waitLock);

        auto ready = [&]() -> int
        {
            if (waitAll)
            {
                for (uint32_t i = 0; i < count; i++)
                    if (!objs[i]->IsSignaled()) return -1;
                for (uint32_t i = 0; i < count; i++) objs[i]->Acquire(self);
                return 0;
            }
            for (uint32_t i = 0; i < count; i++)
                if (objs[i]->IsSignaled()) { objs[i]->Acquire(self); return int(i); }
            return -1;
        };

        int r = ready();
        if (r >= 0) return STATUS_SUCCESS + r;

        if (timeout100ns)
        {
            int64_t t = *timeout100ns;
            if (t == 0) return STATUS_TIMEOUT;
            std::chrono::steady_clock::time_point deadline;
            if (t < 0)
                deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(-t * 100);
            else
            {
                // absolute FILETIME: convert relative to the current system time
                auto nowFt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
                deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(std::max<int64_t>(0, t - nowFt) * 100);
            }
            while (true)
            {
                if (s_waitCv.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    r = ready();
                    return r >= 0 ? STATUS_SUCCESS + r : STATUS_TIMEOUT;
                }
                r = ready();
                if (r >= 0) return STATUS_SUCCESS + r;
            }
        }

        while (true)
        {
            s_waitCv.wait(lock);
            r = ready();
            if (r >= 0) return STATUS_SUCCESS + r;
        }
    }

    // ---------------- executable info / time ----------------
    static ExeInfo s_exe;
    ExeInfo& Exe() { return s_exe; }

    static std::string s_gameDir;
    std::string GameDirectory() { return s_gameDir; }
    void SetGameDirectory(const std::string& dir) { s_gameDir = dir; }

    uint64_t TscFrequency()
    {
        static uint64_t freq = []
        {
            auto t0 = std::chrono::steady_clock::now();
            uint64_t c0 = __rdtsc();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            uint64_t c1 = __rdtsc();
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            uint64_t f = uint64_t((c1 - c0) / secs);
            LOG_INFO("host TSC frequency {} MHz", f / 1000000);
            return f;
        }();
        return freq;
    }

    // ---------------- calling guest code ----------------
    void CallGuest(PPCContext& ctx, uint32_t address)
    {
        if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            LOG_ERROR("call to 0x{:08X} is outside the game's code", address);
            return;
        }
        PPCFunc* fn = PPC_LOOKUP_FUNC(mem::base, address);
        if (!fn)
        {
            LOG_ERROR("no recompiled function at 0x{:08X}", address);
            return;
        }
        fn(ctx, mem::base);
    }
}

bool KMutant::IsSignaled() const
{
    return owner == nullptr || owner == kernel::CurrentThread();
}
