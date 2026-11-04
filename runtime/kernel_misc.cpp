// Kernel imports: executable/module info, config, debug, strings, formatting.
#include "kernel.h"
#include <cstdlib>
#include <string>

using namespace kernel;

// ====================================================================== executable / modules

IMPORT(XexCheckExecutablePrivilege) { RETURN(0); }

IMPORT(RtlImageXexHeaderField)
{
    // (void* XexHeader, u32 Key) -> value (if the key holds an inline value) or pointer to the field data
    uint32_t hdr = A0 ? A0 : Exe().guestXexHeader;
    uint32_t key = A1;
    uint32_t count = mem::R32(hdr + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t k = mem::R32(hdr + 0x18 + i * 8);
        uint32_t v = mem::R32(hdr + 0x18 + i * 8 + 4);
        if (k != key) continue;
        if ((key & 0xFF) == 0) RETURN(v);       // value stored inline
        RETURN(hdr + v);                        // offset into the header
    }
    RETURN(0);
}

IMPORT(XexGetModuleHandle)
{
    // (char* ModuleName, u32* Handle)
    std::string name = GuestString(A0);
    if (A1) mem::W32(A1, Exe().guestModule);
    if (!name.empty()) LOG_TRACE("XexGetModuleHandle(\"{}\")", name);
    RETURN(STATUS_SUCCESS);
}

IMPORT(XexGetProcedureAddress)
{
    // (u32 Handle, u32 Ordinal, u32* Address)
    LOG_WARN("XexGetProcedureAddress(handle 0x{:X}, ordinal {}) not supported", A0, A1);
    if (A2) mem::W32(A2, 0);
    RETURN(STATUS_UNSUCCESSFUL);
}

IMPORT(XexLoadImage)   { LOG_WARN("XexLoadImage(\"{}\") not supported", GuestString(A0)); RETURN(STATUS_UNSUCCESSFUL); }
IMPORT(XexUnloadImage) { RETURN(STATUS_SUCCESS); }

IMPORT(ExGetXConfigSetting)
{
    // (u16 Category, u16 Setting, void* Buffer, u16 BufferSize, u16* RequiredSize)
    uint32_t size = A3 & 0xFFFF;
    if (A2) memset(mem::Ptr(A2), 0, size);
    if (A4) mem::W16(A4, uint16_t(size));
    LOG_TRACE("ExGetXConfigSetting(category {}, setting {})", A0 & 0xFFFF, A1 & 0xFFFF);
    RETURN(STATUS_SUCCESS);
}

IMPORT(ExRegisterTitleTerminateNotification) { RETURN(0); }

// ====================================================================== debug / failure

IMPORT(DbgPrint)
{
    std::string s = GuestString(A0);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG_INFO("[game] {}", s);
    RETURN(0);
}

IMPORT(KeBugCheck)   { LOG_ERROR("game called KeBugCheck(0x{:X})", A0); std::exit(1); }
IMPORT(KeBugCheckEx) { LOG_ERROR("game called KeBugCheckEx(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X})", A0, A1, A2, A3, A4); std::exit(1); }
IMPORT(HalReturnToFirmware) { LOG_INFO("game asked to return to the dashboard (mode {})", A0); std::exit(0); }

IMPORT(RtlRaiseException)
{
    // EXCEPTION_RECORD: +0 code, +0x14 parameters
    uint32_t rec = A0;
    uint32_t code = mem::R32(rec);
    if (code == 0x406D1388)     // MSVC "set thread name"
    {
        uint32_t info = rec + 0x14;
        LOG_INFO("thread {} is named \"{}\"", mem::R32(info + 0xC), GuestString(mem::R32(info + 4)));
        RETURN(0);
    }
    LOG_WARN("RtlRaiseException code 0x{:08X} ignored", code);
    RETURN(0);
}

IMPORT(RtlNtStatusToDosError) { RETURN(A0 == 0 ? 0 : (A0 & 0xFFFF)); }
IMPORT(RtlCaptureContext)     { RETURN(0); }

// ====================================================================== memory helpers

IMPORT(RtlCompareMemoryUlong)
{
    // (void* Source, u32 Length, u32 Pattern) -> bytes that matched
    uint32_t n = 0;
    for (uint32_t i = 0; i + 4 <= A1; i += 4, n += 4)
        if (mem::R32(A0 + i) != A2) break;
    RETURN(n);
}

IMPORT(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= A1; i += 4) mem::W32(A0 + i, A2);
    RETURN(0);
}

// ====================================================================== strings

IMPORT(RtlInitAnsiString)
{
    // (ANSI_STRING* Dest, char* Source)
    uint32_t len = A1 ? uint32_t(GuestString(A1).size()) : 0;
    mem::W16(A0, uint16_t(len));
    mem::W16(A0 + 2, uint16_t(A1 ? len + 1 : 0));
    mem::W32(A0 + 4, A1);
    RETURN(0);
}

IMPORT(RtlInitUnicodeString)
{
    uint32_t chars = 0;
    if (A1) while (mem::R16(A1 + chars * 2)) chars++;
    mem::W16(A0, uint16_t(chars * 2));
    mem::W16(A0 + 2, uint16_t(A1 ? chars * 2 + 2 : 0));
    mem::W32(A0 + 4, A1);
    RETURN(0);
}

IMPORT(RtlFreeAnsiString) { mem::Free(mem::R32(A0 + 4)); mem::W32(A0 + 4, 0); RETURN(0); }

IMPORT(RtlCopyString)
{
    // (ANSI_STRING* Dest, ANSI_STRING* Source)
    uint16_t len = A1 ? mem::R16(A1) : 0;
    uint16_t max = mem::R16(A0 + 2);
    uint16_t n = std::min(len, max);
    if (n) memcpy(mem::Ptr(mem::R32(A0 + 4)), mem::Ptr(mem::R32(A1 + 4)), n);
    mem::W16(A0, n);
    RETURN(0);
}

IMPORT(RtlCompareStringN)
{
    // (char* A, u32 LenA, char* B, u32 LenB, u32 CaseInsensitive) -> <0 / 0 / >0
    uint32_t n = std::min(A1, A3);
    for (uint32_t i = 0; i < n; i++)
    {
        int a = mem::R8(A0 + i), b = mem::R8(A2 + i);
        if (A4) { a = toupper(a); b = toupper(b); }
        if (a != b) RETURN(uint32_t(a - b));
    }
    RETURN(uint32_t(int32_t(A1) - int32_t(A3)));
}

IMPORT(RtlUnicodeStringToAnsiString)
{
    // (ANSI_STRING* Dest, UNICODE_STRING* Source, u32 Allocate)
    uint16_t bytes = mem::R16(A1);
    uint32_t src = mem::R32(A1 + 4);
    uint32_t chars = bytes / 2;
    uint32_t dst;
    if (A2) { dst = mem::AllocVirtual(chars + 1, 0x10); mem::W32(A0 + 4, dst); mem::W16(A0 + 2, uint16_t(chars + 1)); }
    else dst = mem::R32(A0 + 4);
    uint32_t max = mem::R16(A0 + 2);
    uint32_t n = std::min(chars, max ? max - 1 : 0);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = mem::R16(src + i * 2);
        mem::W8(dst + i, c < 0x100 ? uint8_t(c) : '?');
    }
    mem::W8(dst + n, 0);
    mem::W16(A0, uint16_t(n));
    RETURN(STATUS_SUCCESS);
}

IMPORT(RtlMultiByteToUnicodeN)
{
    // (u16* Dest, u32 DestBytes, u32* Written, char* Src, u32 SrcBytes)
    uint32_t n = std::min(A1 / 2, A4);
    for (uint32_t i = 0; i < n; i++) mem::W16(A0 + i * 2, mem::R8(A3 + i));
    if (A2) mem::W32(A2, n * 2);
    RETURN(STATUS_SUCCESS);
}

IMPORT(RtlUnicodeToMultiByteN)
{
    // (char* Dest, u32 DestBytes, u32* Written, u16* Src, u32 SrcBytes)
    uint32_t n = std::min(A1, A4 / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = mem::R16(A3 + i * 2);
        mem::W8(A0 + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (A2) mem::W32(A2, n);
    RETURN(STATUS_SUCCESS);
}

IMPORT(RtlUpcaseUnicodeChar)   { RETURN(A0 < 0x80 ? uint32_t(toupper(int(A0))) : A0); }
IMPORT(RtlDowncaseUnicodeChar) { RETURN(A0 < 0x80 ? uint32_t(tolower(int(A0))) : A0); }

// ====================================================================== printf family
//
// Guest varargs: integer/pointer arguments arrive in r3..r10, then on the stack.
// A guest va_list points at 8-byte slots (32-bit values in the low half).

namespace
{
    struct ArgSource
    {
        PPCContext* ctx = nullptr;
        int nextReg = 0;        // index into r3..r10
        uint32_t list = 0;      // va_list pointer, if formatting from one

        uint64_t Next()
        {
            if (list)
            {
                uint64_t v = mem::R64(list);
                list += 8;
                return v;
            }
            if (nextReg < 8)
            {
                PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
                return regs[nextReg++]->u64;
            }
            // stack arguments follow the 8 register slots in the caller's parameter area
            // NOTE: offset still to be confirmed against real calls
            static bool warned = false;
            if (!warned) { LOG_WARN("printf with more than 8 arguments: stack argument offset unverified"); warned = true; }
            uint32_t slot = ctx->r1.u32 + 0x50 + uint32_t(nextReg++ - 8) * 8;
            return mem::R64(slot);
        }
    };

    std::string Format(const std::string& fmt, ArgSource& args, bool wide)
    {
        std::string out;
        for (size_t i = 0; i < fmt.size(); i++)
        {
            char c = fmt[i];
            if (c != '%') { out += c; continue; }
            if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out += '%'; i++; continue; }

            std::string spec = "%";
            size_t j = i + 1;
            while (j < fmt.size() && strchr("-+ #0", fmt[j])) spec += fmt[j++];
            if (j < fmt.size() && fmt[j] == '*') { spec += std::to_string(int32_t(args.Next())); j++; }
            else while (j < fmt.size() && isdigit((unsigned char)fmt[j])) spec += fmt[j++];
            if (j < fmt.size() && fmt[j] == '.')
            {
                spec += fmt[j++];
                if (j < fmt.size() && fmt[j] == '*') { spec += std::to_string(int32_t(args.Next())); j++; }
                else while (j < fmt.size() && isdigit((unsigned char)fmt[j])) spec += fmt[j++];
            }
            int longs = 0; bool h = false, wideStr = false, i64 = false;
            while (j < fmt.size() && strchr("lhwLIjzt", fmt[j]))
            {
                if (fmt[j] == 'l') longs++;
                else if (fmt[j] == 'h') h = true;
                else if (fmt[j] == 'w') wideStr = true;
                else if (fmt[j] == 'I' && fmt.compare(j, 3, "I64") == 0) { i64 = true; j += 2; }
                j++;
            }
            if (j >= fmt.size()) break;
            char conv = fmt[j];
            i = j;

            char buf[512];
            switch (conv)
            {
            case 'd': case 'i':
            {
                int64_t v = (longs >= 2 || i64) ? int64_t(args.Next()) : int64_t(int32_t(args.Next()));
                snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)v);
                out += buf;
                break;
            }
            case 'u': case 'x': case 'X': case 'o':
            {
                uint64_t v = (longs >= 2 || i64) ? args.Next() : uint64_t(uint32_t(args.Next()));
                if (h) v &= 0xFFFF;
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)v);
                out += buf;
                break;
            }
            case 'p':
                snprintf(buf, sizeof buf, "%08X", uint32_t(args.Next()));
                out += buf;
                break;
            case 'c': case 'C':
                out += char(args.Next() & 0xFF);
                break;
            case 's': case 'S':
            {
                uint32_t p = uint32_t(args.Next());
                bool w = (conv == 'S') != wide || longs || wideStr;
                if (h) w = false;
                std::string s = !p ? "(null)" : w ? GuestWString(p) : GuestString(p);
                snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                uint64_t bits = args.Next();
                double d;
                memcpy(&d, &bits, 8);
                snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                out += buf;
                break;
            }
            case 'n':
                args.Next();
                break;
            default:
                out += spec + conv;
                break;
            }
        }
        return out;
    }

    uint32_t WriteNarrow(uint32_t dst, uint32_t cap, const std::string& s)
    {
        // returns the length written, or -1 (as _snprintf does) when truncated
        if (cap == 0) return s.size() ? 0xFFFFFFFF : 0;
        uint32_t n = std::min<uint32_t>(uint32_t(s.size()), cap);
        memcpy(mem::Ptr(dst), s.data(), n);
        if (n < cap) mem::W8(dst + n, 0);
        return n < s.size() ? 0xFFFFFFFF : n;
    }
}

IMPORT(sprintf)
{
    // (char* Buffer, char* Format, ...)
    ArgSource args{ &ctx, 2 };
    std::string s = Format(GuestString(A1), args, false);
    memcpy(mem::Ptr(A0), s.c_str(), s.size() + 1);
    RETURN(s.size());
}

IMPORT(_snprintf)
{
    // (char* Buffer, u32 Count, char* Format, ...)
    ArgSource args{ &ctx, 3 };
    RETURN(WriteNarrow(A0, A1, Format(GuestString(A2), args, false)));
}

IMPORT(_vsnprintf)
{
    // (char* Buffer, u32 Count, char* Format, va_list Args)
    ArgSource args{ &ctx, 0, A3 };
    RETURN(WriteNarrow(A0, A1, Format(GuestString(A2), args, false)));
}

IMPORT(_snwprintf)
{
    // (u16* Buffer, u32 Count, u16* Format, ...)
    ArgSource args{ &ctx, 3 };
    std::string s = Format(GuestWString(A2), args, true);
    uint32_t n = std::min<uint32_t>(uint32_t(s.size()), A1);
    for (uint32_t i = 0; i < n; i++) mem::W16(A0 + i * 2, uint8_t(s[i]));
    if (n < A1) mem::W16(A0 + n * 2, 0);
    RETURN(n < s.size() ? 0xFFFFFFFF : n);
}
