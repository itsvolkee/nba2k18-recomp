// Kernel imports: file system. Xbox paths are mapped onto host folders:
//   game:\  d:\  \Device\Cdrom0\         -> the extracted disc (read-only)
//   cache:\ z:\  \Device\Harddisk0\Cache  -> <game folder>/../cache   (writable)
//   anything the game links with ObCreateSymbolicLink is resolved first.
#include "kernel.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace fs = std::filesystem;
using namespace kernel;

// ====================================================================== path mapping

static std::mutex s_linkLock;
static std::map<std::string, std::string> s_links;      // lower-case "name:" -> target

static std::string Lower(std::string s)
{
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}

static fs::path CacheDir()
{
    fs::path p = fs::path(GameDirectory()).parent_path() / "cache";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// Case-insensitive lookup of each path component (the Xbox file system ignores case).
static fs::path ResolveCase(const fs::path& root, const std::string& rel)
{
    fs::path cur = root;
    size_t start = 0;
    while (start < rel.size())
    {
        size_t end = rel.find_first_of("\\/", start);
        std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? rel.size() : end + 1;
        if (part.empty() || part == ".") continue;
        fs::path direct = cur / part;
        std::error_code ec;
        if (fs::exists(direct, ec)) { cur = direct; continue; }
        bool found = false;
        if (fs::is_directory(cur, ec))
        {
            std::string want = Lower(part);
            for (auto& e : fs::directory_iterator(cur, ec))
                if (Lower(e.path().filename().string()) == want) { cur = e.path(); found = true; break; }
        }
        if (!found) cur = direct;
    }
    return cur;
}

struct Mapped
{
    fs::path host;
    bool writable = false;
};

static Mapped MapPath(std::string guest)
{
    std::replace(guest.begin(), guest.end(), '/', '\\');
    std::string low = Lower(guest);

    {
        std::lock_guard lock(s_linkLock);
        for (auto& [name, target] : s_links)
            if (low.rfind(name, 0) == 0)
            {
                guest = target + guest.substr(name.size());
                low = Lower(guest);
                break;
            }
    }

    static const struct { const char* prefix; bool writable; } roots[] = {
        { "game:", false }, { "d:", false }, { "\\device\\cdrom0", false }, { "media:", false },
        { "cache:", true }, { "z:", true }, { "\\device\\harddisk0\\cache0", true }, { "\\device\\harddisk0\\partition1", true },
        { "update:", true }, { "devkit:", true },
    };
    for (auto& r : roots)
    {
        size_t n = strlen(r.prefix);
        if (low.rfind(r.prefix, 0) == 0)
        {
            std::string rest = guest.substr(n);
            fs::path root = r.writable ? CacheDir() : fs::path(GameDirectory());
            return { ResolveCase(root, rest), r.writable };
        }
    }
    // relative paths: treat as the game folder
    return { ResolveCase(GameDirectory(), guest), false };
}

static std::string ObjectAttributesPath(uint32_t attr)
{
    // OBJECT_ATTRIBUTES { u32 RootDirectory; ANSI_STRING* ObjectName; u32 Attributes }
    if (!attr) return {};
    std::string name = AnsiStringStruct(mem::R32(attr + 4));
    uint32_t root = mem::R32(attr);
    if (root && root != 0xFFFFFFFD)
    {
        extern std::string FilePathForHandle(uint32_t h);
        std::string dir = FilePathForHandle(root);
        if (!dir.empty()) name = dir + "\\" + name;
    }
    return name;
}

// ====================================================================== file objects

struct KFile : KObject
{
    std::string guestPath;
    fs::path hostPath;
    std::fstream stream;
    bool isDirectory = false, writable = false;
    uint64_t position = 0;
    std::vector<fs::directory_entry> listing;
    size_t listIndex = 0;
    bool listed = false;
    KFile() : KObject(ObjType::File) {}
};

std::string FilePathForHandle(uint32_t h)
{
    auto o = GetHandle(h);
    return (o && o->type == ObjType::File) ? static_cast<KFile*>(o.get())->guestPath : std::string();
}

static std::shared_ptr<KFile> FileFromHandle(uint32_t h)
{
    auto o = GetHandle(h);
    return (o && o->type == ObjType::File) ? std::static_pointer_cast<KFile>(o) : nullptr;
}

static void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (!iosb) return;
    mem::W32(iosb, status);
    mem::W32(iosb + 4, info);
}

static int64_t ToFiletime(fs::file_time_type t)
{
    // portable file_clock -> system_clock conversion
    auto sys = std::chrono::system_clock::now() +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count() / 100 + 116444736000000000ll;
}

static uint32_t OpenCommon(uint32_t handlePtr, uint32_t access, uint32_t attr, uint32_t iosb, uint32_t disposition)
{
    std::string guest = ObjectAttributesPath(attr);
    Mapped m = MapPath(guest);
    std::error_code ec;
    bool exists = fs::exists(m.host, ec);
    bool isDir = exists && fs::is_directory(m.host, ec);
    bool wantWrite = access & (0x40000000 | 0x00000002 | 0x00000004 | 0x00000100);   // GENERIC_WRITE, FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_ATTRIBUTES

    // dispositions: 0 SUPERSEDE, 1 OPEN, 2 CREATE, 3 OPEN_IF, 4 OVERWRITE, 5 OVERWRITE_IF
    if (!exists && (disposition == 1 || disposition == 4))
    {
        LOG_TRACE("open \"{}\" -> not found ({})", guest, m.host.string());
        SetIoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (exists && disposition == 2)
    {
        SetIoStatus(iosb, 0xC0000035, 0);      // STATUS_OBJECT_NAME_COLLISION
        return 0xC0000035;
    }
    if ((wantWrite || !exists) && !m.writable && !isDir)
    {
        LOG_WARN("write access to read-only \"{}\" refused", guest);
        SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }

    auto f = std::make_shared<KFile>();
    f->guestPath = guest;
    f->hostPath = m.host;
    f->writable = m.writable;
    f->isDirectory = isDir;
    if (!isDir)
    {
        if (!exists || disposition == 0 || disposition == 4 || disposition == 5)
        {
            fs::create_directories(m.host.parent_path(), ec);
            std::ofstream(m.host, std::ios::binary | std::ios::trunc);
        }
        auto mode = std::ios::binary | std::ios::in;
        if (m.writable) mode |= std::ios::out;
        f->stream.open(m.host, mode);
        if (!f->stream)
        {
            SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
    }
    uint32_t h = CreateHandle(f);
    mem::W32(handlePtr, h);
    SetIoStatus(iosb, STATUS_SUCCESS, exists ? 1 : 2);     // FILE_OPENED : FILE_CREATED
    LOG_TRACE("open \"{}\" -> handle 0x{:X}", guest, h);
    return STATUS_SUCCESS;
}

IMPORT(NtCreateFile)
{
    // (u32* Handle, u32 Access, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*, s64* AllocSize, u32 Attributes, u32 Share, u32 Disposition, u32 Options)
    RETURN(OpenCommon(A0, A1, A2, A3, A7));
}

IMPORT(NtOpenFile)
{
    // (u32* Handle, u32 Access, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*, u32 Share, u32 Options)
    RETURN(OpenCommon(A0, A1, A2, A3, 1));
}

static uint64_t ResolveOffset(KFile& f, uint32_t offsetPtr)
{
    if (!offsetPtr) return f.position;
    uint64_t off = mem::R64(offsetPtr);
    // -2 (low) / -1 (high) means "use the current position"
    if (off == 0xFFFFFFFFFFFFFFFEull) return f.position;
    return off;
}

static void FinishIo(uint32_t eventHandle, uint32_t iosb, uint32_t status, uint32_t bytes)
{
    SetIoStatus(iosb, status, bytes);
    if (eventHandle)
    {
        auto o = GetHandle(eventHandle);
        if (o && o->type == ObjType::Event)
        {
            { std::lock_guard lock(WaitLock()); static_cast<KEvent*>(o.get())->signaled = true; }
            WaitCv().notify_all();
        }
    }
}

IMPORT(NtReadFile)
{
    // (u32 Handle, u32 Event, u32 ApcRoutine, u32 ApcContext, IO_STATUS_BLOCK*, void* Buffer, u32 Length, s64* ByteOffset)
    auto f = FileFromHandle(A0);
    if (!f || f->isDirectory) RETURN(STATUS_INVALID_HANDLE);
    uint64_t off = ResolveOffset(*f, A7);
    f->stream.clear();
    f->stream.seekg(std::streamoff(off));
    f->stream.read(static_cast<char*>(mem::Ptr(A5)), A6);
    uint32_t got = uint32_t(f->stream.gcount());
    f->position = off + got;
    uint32_t status = (got == 0 && A6 != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    FinishIo(A1, A4, status, got);
    RETURN(status);
}

IMPORT(NtWriteFile)
{
    auto f = FileFromHandle(A0);
    if (!f || f->isDirectory || !f->writable) RETURN(STATUS_ACCESS_DENIED);
    uint64_t off = ResolveOffset(*f, A7);
    f->stream.clear();
    f->stream.seekp(std::streamoff(off));
    f->stream.write(static_cast<const char*>(mem::Ptr(A5)), A6);
    f->position = off + A6;
    FinishIo(A1, A4, STATUS_SUCCESS, A6);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtFlushBuffersFile)
{
    auto f = FileFromHandle(A0);
    if (f && !f->isDirectory) f->stream.flush();
    SetIoStatus(A1, STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

static void WriteNetworkOpenInfo(uint32_t p, const fs::path& host)
{
    std::error_code ec;
    bool dir = fs::is_directory(host, ec);
    int64_t t = ToFiletime(fs::last_write_time(host, ec));
    uint64_t size = dir ? 0 : fs::file_size(host, ec);
    for (int i = 0; i < 4; i++) mem::W64(p + i * 8, uint64_t(t));
    mem::W64(p + 0x20, (size + 0xFFF) & ~0xFFFull);
    mem::W64(p + 0x28, size);
    mem::W32(p + 0x30, dir ? 0x10 : 0x80);
}

IMPORT(NtQueryFullAttributesFile)
{
    // (OBJECT_ATTRIBUTES*, FILE_NETWORK_OPEN_INFORMATION*)
    std::string guest = ObjectAttributesPath(A0);
    Mapped m = MapPath(guest);
    std::error_code ec;
    if (!fs::exists(m.host, ec))
    {
        LOG_TRACE("attributes \"{}\" -> not found", guest);
        RETURN(STATUS_OBJECT_NAME_NOT_FOUND);
    }
    WriteNetworkOpenInfo(A1, m.host);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtQueryInformationFile)
{
    // (u32 Handle, IO_STATUS_BLOCK*, void* Info, u32 Length, u32 Class)
    auto f = FileFromHandle(A0);
    if (!f) RETURN(STATUS_INVALID_HANDLE);
    std::error_code ec;
    uint64_t size = f->isDirectory ? 0 : fs::file_size(f->hostPath, ec);
    uint32_t p = A2;
    memset(mem::Ptr(p), 0, A3);
    switch (A4)
    {
    case 4:   // FileBasicInformation
    {
        int64_t t = ToFiletime(fs::last_write_time(f->hostPath, ec));
        for (int i = 0; i < 4; i++) mem::W64(p + i * 8, uint64_t(t));
        mem::W32(p + 0x20, f->isDirectory ? 0x10 : 0x80);
        break;
    }
    case 5:   // FileStandardInformation
        mem::W64(p + 0, (size + 0xFFF) & ~0xFFFull);
        mem::W64(p + 8, size);
        mem::W32(p + 0x10, 1);
        mem::W8(p + 0x15, f->isDirectory);
        break;
    case 14:  // FilePositionInformation
        mem::W64(p, f->position);
        break;
    case 34:  // FileNetworkOpenInformation
        WriteNetworkOpenInfo(p, f->hostPath);
        break;
    default:
        LOG_WARN("NtQueryInformationFile class {} not handled", A4);
        break;
    }
    SetIoStatus(A1, STATUS_SUCCESS, A3);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtSetInformationFile)
{
    // (u32 Handle, IO_STATUS_BLOCK*, void* Info, u32 Length, u32 Class)
    auto f = FileFromHandle(A0);
    if (!f) RETURN(STATUS_INVALID_HANDLE);
    switch (A4)
    {
    case 14: f->position = mem::R64(A2); break;            // FilePositionInformation
    case 20:                                                // FileEndOfFileInformation
    {
        std::error_code ec;
        f->stream.flush();
        fs::resize_file(f->hostPath, mem::R64(A2), ec);
        break;
    }
    default:
        LOG_WARN("NtSetInformationFile class {} not handled", A4);
        break;
    }
    SetIoStatus(A1, STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

IMPORT(NtQueryVolumeInformationFile)
{
    // (u32 Handle, IO_STATUS_BLOCK*, void* Info, u32 Length, u32 Class)
    uint32_t p = A2;
    memset(mem::Ptr(p), 0, A3);
    switch (A4)
    {
    case 3:   // FileFsSizeInformation
        mem::W64(p + 0, 0x100000);       // total allocation units
        mem::W64(p + 8, 0x80000);        // available
        mem::W32(p + 0x10, 32);          // sectors per unit
        mem::W32(p + 0x14, 512);         // bytes per sector
        break;
    case 5:   // FileFsAttributeInformation
        mem::W32(p + 0, 0x2);            // case preserving
        mem::W32(p + 4, 255);
        mem::W32(p + 8, 8);
        for (int i = 0; i < 4; i++) mem::W16(p + 12 + i * 2, uint16_t("FATX"[i]));
        break;
    default:
        LOG_WARN("NtQueryVolumeInformationFile class {} not handled", A4);
        break;
    }
    SetIoStatus(A1, STATUS_SUCCESS, A3);
    RETURN(STATUS_SUCCESS);
}

static bool WildcardMatch(const char* pat, const char* s)
{
    if (!*pat) return !*s;
    if (*pat == '*') return WildcardMatch(pat + 1, s) || (*s && WildcardMatch(pat, s + 1));
    if (*s && (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s))) return WildcardMatch(pat + 1, s + 1);
    return false;
}

IMPORT(NtQueryDirectoryFile)
{
    // (u32 Handle, u32 Event, u32 Apc, u32 ApcCtx, IO_STATUS_BLOCK*, FILE_DIRECTORY_INFORMATION*, u32 Length, ANSI_STRING* Mask, u8 Restart)
    auto f = FileFromHandle(A0);
    if (!f || !f->isDirectory) RETURN(STATUS_INVALID_HANDLE);
    std::string mask = A7 ? AnsiStringStruct(A7) : "*";
    if (!f->listed)
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->hostPath, ec))
            if (WildcardMatch(mask.c_str(), e.path().filename().string().c_str())) f->listing.push_back(e);
        f->listed = true;
    }
    if (f->listIndex >= f->listing.size())
    {
        SetIoStatus(A4, STATUS_NO_MORE_FILES, 0);
        RETURN(STATUS_NO_MORE_FILES);
    }
    auto& e = f->listing[f->listIndex++];
    std::string name = e.path().filename().string();
    uint32_t p = A5;
    uint32_t need = 0x40 + uint32_t(name.size());
    if (need > A6) { SetIoStatus(A4, 0x80000005, 0); RETURN(0x80000005); }   // STATUS_BUFFER_OVERFLOW
    memset(mem::Ptr(p), 0, need);
    std::error_code ec;
    bool dir = e.is_directory(ec);
    int64_t t = ToFiletime(e.last_write_time(ec));
    uint64_t size = dir ? 0 : e.file_size(ec);
    mem::W32(p + 0x00, 0);                         // NextEntryOffset (one entry per call)
    mem::W32(p + 0x04, uint32_t(f->listIndex));
    for (int i = 0; i < 4; i++) mem::W64(p + 0x08 + i * 8, uint64_t(t));
    mem::W64(p + 0x28, size);
    mem::W64(p + 0x30, (size + 0xFFF) & ~0xFFFull);
    mem::W32(p + 0x38, dir ? 0x10 : 0x80);
    mem::W32(p + 0x3C, uint32_t(name.size()));
    memcpy(mem::Ptr(p + 0x40), name.data(), name.size());
    FinishIo(A1, A4, STATUS_SUCCESS, need);
    RETURN(STATUS_SUCCESS);
}

// ====================================================================== symbolic links

IMPORT(ObCreateSymbolicLink)
{
    // (ANSI_STRING* Name, ANSI_STRING* Target)
    std::string name = Lower(AnsiStringStruct(A0));
    std::string target = AnsiStringStruct(A1);
    // "\??\game:" style names
    if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
    if (!name.empty() && name.back() != ':' && name.back() != '\\') name += "";
    LOG_INFO("symbolic link {} -> {}", name, target);
    std::lock_guard lock(s_linkLock);
    s_links[name] = target;
    RETURN(STATUS_SUCCESS);
}

IMPORT(ObDeleteSymbolicLink)
{
    std::string name = Lower(AnsiStringStruct(A0));
    if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
    std::lock_guard lock(s_linkLock);
    s_links.erase(name);
    RETURN(STATUS_SUCCESS);
}
