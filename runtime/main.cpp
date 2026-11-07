// NBA 2K18 recompiled: host entry point.
//
// Usage: NBA2K18 [game folder]
//   The game folder holds the extracted disc (default.xex and the data files)
//   and default.img, the decrypted executable image written by XexDump.
#include "kernel.h"
#include <exception>
#include <filesystem>
#include <fstream>
#include <typeinfo>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

static std::vector<uint8_t> ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t BE32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Indirect calls (bctrl) go through CTR, so the target is still in ctx.ctr here.
static void UnmappedCall(PPCContext& ctx, uint8_t*)
{
    LOG_ERROR("indirect call to 0x{:08X}, which isn't a known function start (called from around 0x{:08X})",
              ctx.ctr.u32, uint32_t(ctx.lr));
    std::abort();
}

// ---- crash reporting: say which guest thread died and where, before the process goes away ----
static std::string GuestWhere()
{
    GuestThread* t = kernel::CurrentThread();
    if (!t || !t->ctx) return "on a host thread";
    return std::format("on guest thread {} (last guest call returned to 0x{:08X}, r3=0x{:X})",
                       t->id, uint32_t(t->ctx->lr), t->ctx->r3.u32);
}

static void OnTerminate()
{
    std::string what = "unknown exception";
    if (auto e = std::current_exception())
    {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) { what = std::format("{}: {}", typeid(ex).name(), ex.what()); }
        catch (...) {}
    }
    else what = "std::terminate called without an exception";
    LOG_ERROR("crash: {} {}", what, GuestWhere());
    std::abort();
}

#ifdef _WIN32
static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
{
    auto* r = info->ExceptionRecord;
    std::string extra;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
    {
        uintptr_t addr = r->ExceptionInformation[1];
        uintptr_t base = reinterpret_cast<uintptr_t>(mem::base);
        extra = std::format(" ({} host 0x{:X}", r->ExceptionInformation[0] ? "writing" : "reading", addr);
        if (addr >= base && addr - base < 0x100000000ull)
            extra += std::format(" = guest 0x{:08X}", uint32_t(addr - base));
        extra += ")";
    }
    LOG_ERROR("crash: exception 0x{:08X} at host 0x{:X}{} {}", uint32_t(r->ExceptionCode),
              reinterpret_cast<uintptr_t>(r->ExceptionAddress), extra, GuestWhere());
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static fs::path FindGameFolder(int argc, char** argv)
{
    if (argc > 1) return argv[1];
    for (fs::path c : { fs::path("private/game"), fs::path("../private/game"), fs::path("game") })
        if (fs::exists(c / "default.xex")) return c;
    return "private/game";
}

int main(int argc, char** argv)
{
    logging::OpenFile("nba2k18.log");
    std::set_terminate(OnTerminate);
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnUnhandledException);
#endif
    fs::path game = FindGameFolder(argc, argv);
    kernel::SetGameDirectory(fs::absolute(game).string());
    LOG_INFO("game folder: {}", kernel::GameDirectory());

    // ---- XEX header (unencrypted) ----
    auto xex = ReadAll(game / "default.xex");
    if (xex.size() < 0x1000 || memcmp(xex.data(), "XEX2", 4) != 0)
    {
        LOG_ERROR("{} is missing or not an Xbox 360 executable", (game / "default.xex").string());
        return 1;
    }
    auto& exe = kernel::Exe();
    uint32_t headerSize = BE32(&xex[8]);
    uint32_t secInfo = BE32(&xex[0x10]);
    uint32_t optCount = BE32(&xex[0x14]);
    for (uint32_t i = 0; i < optCount; i++)
    {
        uint32_t key = BE32(&xex[0x18 + i * 8]);
        uint32_t val = BE32(&xex[0x18 + i * 8 + 4]);
        switch (key)
        {
        case 0x00010100: exe.entry = val; break;
        case 0x00010201: exe.imageBase = val; break;
        case 0x00020200: exe.defaultStackSize = val; break;
        case 0x00020104:
            exe.tlsSlotCount  = BE32(&xex[val + 0]);
            exe.tlsRawAddress = BE32(&xex[val + 4]);
            exe.tlsDataSize   = BE32(&xex[val + 8]);
            exe.tlsRawSize    = BE32(&xex[val + 12]);
            break;
        }
    }
    exe.imageSize = BE32(&xex[secInfo + 4]);
    LOG_INFO("entry 0x{:08X}, image 0x{:08X}+0x{:X}, stack 0x{:X}, TLS {} slots / 0x{:X} bytes",
             exe.entry, exe.imageBase, exe.imageSize, exe.defaultStackSize, exe.tlsSlotCount, exe.tlsDataSize);

    if (exe.imageBase != PPC_IMAGE_BASE || exe.imageSize != PPC_IMAGE_SIZE)
    {
        LOG_ERROR("this default.xex doesn't match the one the code was recompiled from");
        return 1;
    }

    // ---- decrypted image ----
    auto image = ReadAll(game / "default.img");
    if (image.size() != exe.imageSize)
    {
        LOG_ERROR("{} is missing or the wrong size; run the setup script to create it", (game / "default.img").string());
        return 1;
    }

    if (!mem::Init()) return 1;
    memcpy(mem::Ptr(exe.imageBase), image.data(), image.size());

    // copy of the XEX header + a module entry pointing at it (for RtlImageXexHeaderField / XexGetModuleHandle)
    exe.guestXexHeader = mem::AllocSystem(headerSize, 0x10);
    memcpy(mem::Ptr(exe.guestXexHeader), xex.data(), headerSize);
    exe.guestModule = mem::AllocSystem(0x100, 0x10);
    mem::W32(exe.guestModule + 0x18, exe.imageBase);     // DllBase
    mem::W32(exe.guestModule + 0x20, exe.imageSize);     // SizeOfImage
    mem::W32(exe.guestModule + 0x58, exe.guestXexHeader);

    // ---- function table for indirect calls ----
    // Every slot starts out pointing at a trap, so a call to an address that
    // isn't a known function start reports where it went instead of crashing.
    for (uint64_t a = PPC_CODE_BASE; a < PPC_CODE_BASE + PPC_CODE_SIZE; a += 4)
        PPC_LOOKUP_FUNC(mem::base, a) = UnmappedCall;
    size_t mapped = 0;
    for (auto* m = PPCFuncMappings; m->host; ++m, ++mapped)
        PPC_LOOKUP_FUNC(mem::base, m->guest) = m->host;
    LOG_INFO("{} functions mapped", mapped);

    kernel::TscFrequency();
    LOG_INFO("starting the game");
    kernel::StartWatchdog();
    kernel::RunMainThread(exe.entry, exe.defaultStackSize);
    LOG_INFO("main thread returned");
    return 0;
}
