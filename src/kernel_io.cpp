// Files: maps game:\ and \Device\Cdrom0\ to the game folder on the PC
#include "kernel.h"
#include "gpu/memwatch.h"
#include "scene_assets.h"

#include <io.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace fs = std::filesystem;

#ifdef _WIN32
#  define FSEEK64 _fseeki64
#  define FTELL64 _ftelli64
#else
#  define FSEEK64 fseeko
#  define FTELL64 ftello
#endif

constexpr uint32_t STATUS_NO_SUCH_FILE_  = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE_   = 0xC0000011;
constexpr uint32_t STATUS_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_NO_MORE_FILES_ = 0x80000006;
constexpr uint32_t STATUS_INVALID_HANDLE_ = 0xC0000008;

static std::mutex g_ioMutex;

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

// Converts an Xbox path into a PC path. Returns false if the device is unknown.
bool XamResolveContentPath(const std::string& xboxPath, fs::path& out);   // kernel_xam.cpp: mounted saves (Cars:\)

static bool ResolvePath(const std::string& xboxPath, fs::path& out)
{
    if (XamResolveContentPath(xboxPath, out))
        return true;
    std::string p = xboxPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::string l = Lower(p);

    static const char* prefixes[] = {
        "/\?\?/game:/", "game:/", "/device/cdrom0/", "d:/", "/\?\?/d:/", "/device/harddisk0/partition1/",
        "/\?\?/cdrom0:/", "cdrom0:/", "/\?\?/game:", "game:", "/device/cdrom0", "d:", "/\?\?/d:",
    };
    for (const char* pre : prefixes)
    {
        const size_t n = strlen(pre);
        if (l.compare(0, n, pre) == 0)
        {
            std::string rest = p.substr(n);
            while (!rest.empty() && rest[0] == '/')
                rest.erase(0, 1);
            out = fs::path(g_gameRoot) / fs::path(rest);
            return true;
        }
    }
    // No prefix: relative to the game root
    if (l.find(':') == std::string::npos && !l.empty() && l[0] != '/')
    {
        out = fs::path(g_gameRoot) / fs::path(p);
        return true;
    }
    return false;
}

// ---- Mods folder ----
// <exe folder>\mods (or CARS_MODS) mirrors the game folder: a file that exists there is used instead of the original
// (the ISO files are never changed). Scripts: a text "X.lua" replaces "X.lua.bin" (the runtime builds the .lua.bin
// on the fly: 4 big-endian bytes with the size + the source code, which the game's own Lua parser compiles).
static const fs::path& ModsRoot()
{
    static const fs::path root = [] {
        if (const char* env = getenv("CARS_MODS"))
            return fs::path(env);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return fs::path(exe).parent_path() / L"mods";
    }();
    return root;
}

struct ModFile
{
    fs::path path;            // mod file (empty if luaSource)
    bool isLuaSource = false;
    std::string luaSource;    // Lua source text (becomes a .lua.bin)
    bool isResFolder = false; // extracted "X.res" / "X.str" folder (tools/scenes_extract.py): built by scene_assets.cpp
    bool isStrFolder = false;
    bool isLoose = false;     // scene as loose files (<folder>\_package\X.soi/.toc + geo/tex/col): scene_assets.cpp
    bool looseStream = false;
    fs::path loosePack;
    std::string looseStem;
};

// ---- Scene streams without zlib (X.str folder built by scene_assets.cpp, or a raw .str) ----
// The game's stream reader (sub_820B0398) reads each block into a buffer and the job thread (sub_820B02F8) calls
// uncompress (sub_822702F8). For a raw stream, each read is registered here and the uncompress hook (game_hooks.cpp)
// copies the block instead of decompressing it.
struct RawRead
{
    uint32_t size;
    std::string path;      // stream folder/file (so model capture knows where each block came from)
    uint64_t offset;       // position in the stream
};
static std::mutex g_rawMutex;
static std::map<uint32_t, RawRead> g_rawBuffers;   // guest address -> bytes read from a raw stream
static std::atomic<bool> g_rawUsed{ false };

static void RawStreamRegister(uint32_t buffer, uint32_t size, const std::string& path, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(g_rawMutex);
    auto it = g_rawBuffers.lower_bound(buffer);
    while (it != g_rawBuffers.end() && it->first < buffer + size)
        it = g_rawBuffers.erase(it);
    g_rawBuffers[buffer] = { size, path, offset };
}

bool RawStreamActive() { return g_rawUsed.load(); }

// true if src received data from a raw stream: size = contiguous bytes read from src; path/offset = where they came from
bool RawStreamTake(uint32_t src, uint32_t& size, std::string& path, uint64_t& offset)
{
    std::lock_guard<std::mutex> lock(g_rawMutex);
    auto it = g_rawBuffers.find(src);
    if (it == g_rawBuffers.end())
        return false;
    size = 0;
    path = it->second.path;
    offset = it->second.offset;
    while (it != g_rawBuffers.end() && it->first == src + size)
    {
        size += it->second.size;
        it = g_rawBuffers.erase(it);
    }
    return true;
}

// mods\<path>.hidden hides the game's original file (e.g. the .res/.str package of a scene built only from
// loose .xng/.dds/.xnm files and the complete .scn)
static bool HiddenByMods(const fs::path& gamePath)
{
    std::error_code ec;
    static const bool hasMods = fs::is_directory(ModsRoot(), ec);
    if (!hasMods)
        return false;
    const fs::path rel = gamePath.lexically_relative(fs::path(g_gameRoot));
    if (rel.empty() || *rel.begin() == "..")
        return false;
    fs::path marker = ModsRoot() / rel;
    marker += L".hidden";
    return fs::exists(marker, ec);
}

static bool FindModOverride(const fs::path& gamePath, ModFile& out)
{
    std::error_code ec;
    static const bool hasMods = fs::is_directory(ModsRoot(), ec);
    if (!hasMods)
        return false;
    const fs::path rel = gamePath.lexically_relative(fs::path(g_gameRoot));
    if (rel.empty() || *rel.begin() == "..")
        return false;
    const fs::path direct = ModsRoot() / rel;
    if (fs::is_regular_file(direct, ec))
    {
        out.path = direct;
        return true;
    }
    const std::string lowerName = Lower(rel.filename().string());
    const bool isRes = lowerName.size() > 4 && lowerName.compare(lowerName.size() - 4, 4, ".res") == 0;
    const bool isStr = lowerName.size() > 4 && lowerName.compare(lowerName.size() - 4, 4, ".str") == 0;
    if ((isRes || isStr) && fs::is_directory(direct, ec))
    {
        out.path = direct;
        out.isResFolder = isRes;
        out.isStrFolder = isStr;
        return true;
    }
    if (isRes || isStr)
    {
        std::string stem = rel.filename().string();
        stem.resize(stem.size() - 4);
        if (LooseSceneExists(direct.parent_path(), stem))
        {
            out.isLoose = true;
            out.looseStream = isStr;
            out.loosePack = direct.parent_path();
            out.looseStem = stem;
            out.path = out.loosePack / L"_package";
            return true;
        }
    }
    const std::string name = Lower(rel.filename().string());
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".lua.bin") == 0)
    {
        fs::path src = direct;
        src.replace_extension();   // X.lua.bin -> X.lua
        if (fs::is_regular_file(src, ec))
        {
            FILE* f = _wfopen(src.c_str(), L"rb");
            if (!f)
                return false;
            fseek(f, 0, SEEK_END);
            out.luaSource.resize(size_t(ftell(f)));
            fseek(f, 0, SEEK_SET);
            fread(out.luaSource.data(), 1, out.luaSource.size(), f);
            fclose(f);
            out.isLuaSource = true;
            out.path = src;
            return true;
        }
    }
    return false;
}

static std::string ObjectAttributesPath(uint32_t objAttr)
{
    // OBJECT_ATTRIBUTES: +0 root dir, +4 pointer to the ANSI_STRING name, +8 attributes
    return KReadGuestAnsi(R32(objAttr + 4));
}

static void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (iosb)
    {
        W32(iosb + 0, status);
        W32(iosb + 4, info);
    }
}

static void SignalEvent(uint32_t eventHandle)
{
    if (!eventHandle)
        return;
    if (auto ev = KGetHandle(eventHandle))
    {
        std::lock_guard<std::mutex> lock(g_kmtx);
        ev->signaled = true;
        g_kcv.notify_all();
    }
}

// Session log: every file the game opens (up to 10 times per path) shows which part of the game it was in
static void LogFileOpen(const std::string& xpath, bool exists, bool write)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, int> counts;
    std::lock_guard<std::mutex> lock(mutex);
    const int n = ++counts[xpath];
    if (n <= 10)
        fprintf(stderr, "[file] %s%s%s%s\n", xpath.c_str(), write ? " (write)" : "", exists ? "" : " (missing)",
                n == 10 ? " (further opens of this file are not shown)" : "");
}

static uint32_t DoOpen(PPCContext& ctx, uint32_t handleOut, uint32_t access, uint32_t objAttr, uint32_t iosb, uint32_t disposition,
                       uint32_t options)
{
    const std::string xpath = ObjectAttributesPath(objAttr);
    fs::path path;
    if (!ResolvePath(xpath, path))
    {
        if (g_traceImports)
            fprintf(stderr, "[io] unknown device: %s\n", xpath.c_str());
        SetIoStatus(iosb, STATUS_NAME_NOT_FOUND, 0);
        return STATUS_NAME_NOT_FOUND;
    }

    std::error_code ec;
    const bool wantsWriteAccess = (access & (0x40000000 | 0x2 | 0x4 | 0x10000000)) != 0;
    // Mod: only for files the game opens for reading (OPEN / OPEN_IF)
    ModFile mod;
    if (!wantsWriteAccess && HiddenByMods(path))
    {
        fprintf(stderr, "[mods] %s hidden (.hidden)\n", xpath.c_str());
        SetIoStatus(iosb, STATUS_NAME_NOT_FOUND, 0);
        return STATUS_NAME_NOT_FOUND;
    }
    const bool modded = !wantsWriteAccess && (disposition == 1 || disposition == 3) && FindModOverride(path, mod);
    if (modded)
    {
        fprintf(stderr, "[mods] %s -> %s%s\n", xpath.c_str(), mod.path.string().c_str(), mod.isLuaSource ? " (Lua source)" : "");
        auto obj = std::make_shared<KObject>();
        obj->type = KType::File;
        if (mod.isLoose)
        {
            bool raw = false;
            obj->file = LooseSceneOpen(ModsRoot(), mod.loosePack, mod.looseStem, mod.looseStream, obj->size, raw);
            obj->path = (mod.loosePack / (mod.looseStem + (mod.looseStream ? ".str" : ".res"))).string();
            if (raw)
            {
                obj->rawStream = true;
                g_rawUsed = true;
            }
        }
        else if (mod.isResFolder || mod.isStrFolder)
        {
            obj->file = SceneFolderOpen(mod.path, mod.isStrFolder, obj->size);
            obj->path = mod.path.string();
            if (mod.isStrFolder)
            {
                obj->rawStream = true;
                g_rawUsed = true;
            }
        }
        else if (mod.isLuaSource)
        {
            obj->file = tmpfile();
            if (obj->file)
            {
                const uint32_t n = uint32_t(mod.luaSource.size());
                const uint8_t prefix[4] = { uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n) };
                fwrite(prefix, 1, 4, obj->file);
                fwrite(mod.luaSource.data(), 1, n, obj->file);
                FSEEK64(obj->file, 0, SEEK_SET);
                obj->size = 4 + uint64_t(n);
            }
            obj->path = path.string();
        }
        else
        {
            obj->file = _wfopen(mod.path.c_str(), L"rb");
            if (obj->file)
            {
                FSEEK64(obj->file, 0, SEEK_END);
                obj->size = (uint64_t)FTELL64(obj->file);
                FSEEK64(obj->file, 0, SEEK_SET);
                // extracted .str (raw blocks): has the X.res folder next to it (tools/scenes_extract.py) or does not start with zlib
                const std::string ext = Lower(mod.path.extension().string());
                uint8_t head[2] = {};
                fs::path resFolder = mod.path;
                resFolder.replace_extension(L".res");
                if (ext == ".str" && fread(head, 1, 2, obj->file) == 2 &&
                    (fs::is_directory(resFolder, ec) || !(head[0] == 0x78 && ((head[0] << 8) | head[1]) % 31 == 0)))
                {
                    obj->rawStream = true;
                    g_rawUsed = true;
                    fprintf(stderr, "[mods] raw stream (no zlib): %s\n", mod.path.string().c_str());
                }
                FSEEK64(obj->file, 0, SEEK_SET);
            }
            obj->path = mod.path.string();
        }
        if (obj->file)
        {
            W32(handleOut, KNewHandle(obj));
            SetIoStatus(iosb, STATUS_SUCCESS, 1);
            return STATUS_SUCCESS;
        }
    }

    const bool exists = fs::exists(path, ec);
    const bool isDir = exists && fs::is_directory(path, ec);
    if (g_traceImports)
        fprintf(stderr, "[io] open '%s' -> %s (%s) disp=%u access=%08X\n", xpath.c_str(), path.string().c_str(),
                exists ? (isDir ? "directory" : "exists") : "missing", disposition, access);
    else if (!isDir)
        LogFileOpen(xpath, exists, wantsWriteAccess);

    auto obj = std::make_shared<KObject>();
    obj->type = KType::File;
    obj->path = path.string();

    uint32_t info = 1;   // FILE_OPENED
    // FILE_DIRECTORY_FILE: create/open a folder (saves)
    if (!exists && (options & 1) && disposition != 1)
    {
        if (!fs::create_directories(path, ec) && !fs::is_directory(path, ec))
        {
            SetIoStatus(iosb, STATUS_NAME_NOT_FOUND, 0);
            return STATUS_NAME_NOT_FOUND;
        }
        obj->isDirectory = true;
        info = 2;   // FILE_CREATED
    }
    else if (isDir)
    {
        obj->isDirectory = true;
    }
    else
    {
        const bool wantsWrite = (access & (0x40000000 | 0x2 | 0x4 | 0x10000000)) != 0;
        const wchar_t* mode = nullptr;
        switch (disposition)
        {
        case 1: // OPEN
            if (!exists) { SetIoStatus(iosb, STATUS_NAME_NOT_FOUND, 0); return STATUS_NAME_NOT_FOUND; }
            mode = wantsWrite ? L"r+b" : L"rb";
            break;
        case 2: // CREATE
            if (exists) { SetIoStatus(iosb, STATUS_NAME_COLLISION, 0); return STATUS_NAME_COLLISION; }
            mode = L"w+b"; info = 2;
            break;
        case 3: // OPEN_IF
            mode = exists ? (wantsWrite ? L"r+b" : L"rb") : L"w+b";
            info = exists ? 1 : 2;
            break;
        default: // SUPERSEDE(0), OVERWRITE(4), OVERWRITE_IF(5)
            mode = L"w+b"; info = exists ? 3 : 2;
            break;
        }
        if (!exists || mode[0] == L'w')
            fs::create_directories(path.parent_path(), ec);
        obj->file = _wfopen(path.c_str(), mode);   // wide path: folders with accented characters in the user name
        if (!obj->file)
        {
            SetIoStatus(iosb, STATUS_NAME_NOT_FOUND, 0);
            return STATUS_NAME_NOT_FOUND;
        }
        FSEEK64(obj->file, 0, SEEK_END);
        obj->size = (uint64_t)FTELL64(obj->file);
        FSEEK64(obj->file, 0, SEEK_SET);
    }

    W32(handleOut, KNewHandle(obj));
    SetIoStatus(iosb, STATUS_SUCCESS, info);
    return STATUS_SUCCESS;
}

KERNEL_IMPORT(RtlInitAnsiString)
{
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    const uint32_t len = src ? (uint32_t)strlen((const char*)(g_mem.base + src)) : 0;
    W16(dst + 0, (uint16_t)len);
    W16(dst + 2, (uint16_t)(src ? len + 1 : 0));
    W32(dst + 4, src);
}

KERNEL_IMPORT(NtCreateFile)
{
    // (handle*, access, objattr*, iosb*, allocSize*, attributes, share, disposition, [stack] options)
    RetStatus(ctx, DoOpen(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, R32(ctx.r1.u32 + 0x54)));
}

KERNEL_IMPORT(NtOpenFile)
{
    // (handle*, access, objattr*, iosb*, share, options)
    RetStatus(ctx, DoOpen(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32));
}

KERNEL_IMPORT(NtReadFile)
{
    // (handle, event, apcRoutine, apcContext, iosb*, buffer, length, byteOffset*)
    auto obj = KGetHandle(ctx.r3.u32);
    const uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offPtr = ctx.r10.u32;
    if (!obj || !obj->file)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE_, 0);
        RetStatus(ctx, STATUS_INVALID_HANDLE_);
        return;
    }
    size_t got;
    int64_t where;
    {
        std::lock_guard<std::mutex> lock(g_ioMutex);
        if (offPtr)
        {
            const int64_t off = (int64_t)R64(offPtr);
            if (off >= 0)
                FSEEK64(obj->file, off, SEEK_SET);
        }
        where = FTELL64(obj->file);
        // Windows ReadFile raises no exception on a protected page: unprotect the pages watched by the GPU cache first
        MemWatchInvalidate(buffer, length);
        got = fread(g_mem.base + buffer, 1, length, obj->file);
    }
    if (obj->rawStream && got)
        RawStreamRegister(buffer, uint32_t(got), obj->path, uint64_t(where));
    // CARS_LOG_READS=text[;text]: logs reads of files whose path contains one of the texts (offset, size, caller)
    static const std::vector<std::string> logReads = [] {
        std::vector<std::string> v;
        if (const char* s = getenv("CARS_LOG_READS"))
            for (std::string all = s; !all.empty();)
            {
                const size_t sep = all.find(';');
                v.push_back(all.substr(0, sep));
                all = sep == std::string::npos ? "" : all.substr(sep + 1);
            }
        return v;
    }();
    for (const auto& pattern : logReads)
        if (obj->path.find(pattern) != std::string::npos)
        {
            char stack[160];
            int n = 0;
            uint32_t sp = ctx.r1.u32;
            for (int i = 0; i < 8 && n < int(sizeof(stack)) - 10; i++)
            {
                const uint32_t next = R32(sp);
                if (next <= sp || next - sp > 0x100000)
                    break;
                n += snprintf(stack + n, sizeof(stack) - n, " %08X", R32(next - 8));
                sp = next;
            }
            stack[n] = 0;
            fprintf(stderr, "[read] %s off %08llX size %X -> %zX at %08X lr=%08X |%s\n", obj->path.c_str(), (long long)where, length,
                    got, buffer, uint32_t(ctx.lr), stack);
            break;
        }
    const uint32_t status = (got == 0 && length > 0) ? STATUS_END_OF_FILE_ : STATUS_SUCCESS;
    SetIoStatus(iosb, status, (uint32_t)got);
    // Completion routine (ReadFileEx): delivered as an APC on the next alertable wait (bit 0 = do not use an I/O port)
    if ((ctx.r5.u32 & ~1u) && ctx.r6.u32 && status == STATUS_SUCCESS)
        KQueueApc(ctx.r5.u32 & ~1u, ctx.r6.u32, iosb, 0);
    SignalEvent(ctx.r4.u32);
    RetStatus(ctx, status);
}

KERNEL_IMPORT(NtWriteFile)
{
    auto obj = KGetHandle(ctx.r3.u32);
    const uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offPtr = ctx.r10.u32;
    if (!obj || !obj->file)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE_, 0);
        RetStatus(ctx, STATUS_INVALID_HANDLE_);
        return;
    }
    size_t put;
    {
        std::lock_guard<std::mutex> lock(g_ioMutex);
        if (offPtr)
        {
            const int64_t off = (int64_t)R64(offPtr);
            if (off >= 0)
                FSEEK64(obj->file, off, SEEK_SET);
        }
        put = fwrite(g_mem.base + buffer, 1, length, obj->file);
        fflush(obj->file);
    }
    SetIoStatus(iosb, STATUS_SUCCESS, (uint32_t)put);
    if ((ctx.r5.u32 & ~1u) && ctx.r6.u32)
        KQueueApc(ctx.r5.u32 & ~1u, ctx.r6.u32, iosb, 0);
    SignalEvent(ctx.r4.u32);
    RetStatus(ctx, STATUS_SUCCESS);
}

static void FillNetworkOpenInfo(uint32_t p, uint64_t size, bool dir)
{
    for (uint32_t i = 0; i < 0x38; i += 4)
        W32(p + i, 0);
    W64(p + 0x20, (size + 0xFFF) & ~0xFFFull);   // AllocationSize
    W64(p + 0x28, size);                          // EndOfFile
    W32(p + 0x30, dir ? 0x10 : 0x80);             // attributes
}

KERNEL_IMPORT(NtQueryInformationFile)
{
    // (handle, iosb*, info*, length, class)
    auto obj = KGetHandle(ctx.r3.u32);
    const uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32;
    if (!obj)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE_, 0);
        RetStatus(ctx, STATUS_INVALID_HANDLE_);
        return;
    }
    uint32_t written = 0;
    switch (cls)
    {
    case 5: // FileStandardInformation
        W64(info + 0x00, (obj->size + 0xFFF) & ~0xFFFull);
        W64(info + 0x08, obj->size);
        W32(info + 0x10, 1);
        W8 (info + 0x14, 0);
        W8 (info + 0x15, obj->isDirectory ? 1 : 0);
        written = 0x18;
        break;
    case 34: // FileNetworkOpenInformation
        FillNetworkOpenInfo(info, obj->size, obj->isDirectory);
        written = 0x38;
        break;
    case 14: // FilePositionInformation
    {
        std::lock_guard<std::mutex> lock(g_ioMutex);
        W64(info, obj->file ? (uint64_t)FTELL64(obj->file) : 0);
        written = 8;
        break;
    }
    case 4: // FileBasicInformation
        for (uint32_t i = 0; i < 0x28; i += 4)
            W32(info + i, 0);
        W32(info + 0x20, obj->isDirectory ? 0x10 : 0x80);
        written = 0x28;
        break;
    default:
        if (g_traceImports)
            fprintf(stderr, "[io] NtQueryInformationFile class %u not supported\n", cls);
        SetIoStatus(iosb, 0xC0000003, 0);   // STATUS_INVALID_INFO_CLASS
        RetStatus(ctx, 0xC0000003);
        return;
    }
    (void)len;
    SetIoStatus(iosb, STATUS_SUCCESS, written);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(NtSetInformationFile)
{
    // (handle, iosb*, info*, length, class)
    auto obj = KGetHandle(ctx.r3.u32);
    const uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!obj)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE_, 0);
        RetStatus(ctx, STATUS_INVALID_HANDLE_);
        return;
    }
    if (cls == 14 && obj->file)   // FilePositionInformation
    {
        std::lock_guard<std::mutex> lock(g_ioMutex);
        FSEEK64(obj->file, (int64_t)R64(info), SEEK_SET);
    }
    else if ((cls == 20 || cls == 19) && obj->file)   // FileEndOfFileInformation / FileAllocationInformation
    {
        std::lock_guard<std::mutex> lock(g_ioMutex);
        const int64_t size = (int64_t)R64(info);
        if (cls == 20 || size < (int64_t)obj->size)
        {
            fflush(obj->file);
            if (_chsize_s(_fileno(obj->file), size) == 0)
                obj->size = uint64_t(size);
        }
    }
    else if (cls == 13)   // FileDispositionInformation: delete when closed
        obj->deleteOnClose = R8(info) != 0;
    else if (cls == 10)   // FileRenameInformation {u32 replace, u32 root, ANSI_STRING name}
    {
        fs::path to;
        if (ResolvePath(KReadGuestAnsi(info + 8), to))
        {
            std::error_code ec;
            std::lock_guard<std::mutex> lock(g_ioMutex);
            if (R8(info) && fs::exists(to, ec))
                fs::remove(to, ec);
            if (obj->file)
            {
                fclose(obj->file);
                obj->file = nullptr;
            }
            fs::rename(fs::path(obj->path), to, ec);
            if (!ec)
                obj->path = to.string();
            if (!obj->isDirectory)
                obj->file = _wfopen(fs::path(obj->path).c_str(), L"r+b");
        }
    }
    SetIoStatus(iosb, STATUS_SUCCESS, 0);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(NtQueryFullAttributesFile)
{
    // (objattr*, out FileNetworkOpenInformation*)
    fs::path path;
    if (!ResolvePath(ObjectAttributesPath(ctx.r3.u32), path))
    {
        RetStatus(ctx, STATUS_NAME_NOT_FOUND);
        return;
    }
    std::error_code ec;
    ModFile mod;
    uint64_t size0 = 0;
    if (HiddenByMods(path))
    {
        RetStatus(ctx, STATUS_NAME_NOT_FOUND);
        return;
    }
    if (FindModOverride(path, mod) && (!mod.isLoose || LooseSceneSize(ModsRoot(), mod.loosePack, mod.looseStem, mod.looseStream, size0)))
    {
        uint64_t size = size0;   // loose scene: size of the built file
        if (mod.isLoose)
        {
        }
        else if (mod.isLuaSource)
            size = 4 + uint64_t(mod.luaSource.size());
        else if (mod.isResFolder || mod.isStrFolder)
            SceneFolderSize(mod.path, mod.isStrFolder, size);
        else
            size = (uint64_t)fs::file_size(mod.path, ec);
        FillNetworkOpenInfo(ctx.r4.u32, size, false);
        RetStatus(ctx, STATUS_SUCCESS);
        return;
    }
    if (!fs::exists(path, ec))
    {
        RetStatus(ctx, STATUS_NAME_NOT_FOUND);
        return;
    }
    const bool dir = fs::is_directory(path, ec);
    FillNetworkOpenInfo(ctx.r4.u32, dir ? 0 : (uint64_t)fs::file_size(path, ec), dir);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(NtQueryVolumeInformationFile)
{
    // (handle, iosb*, info*, length, class)
    const uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32;
    for (uint32_t i = 0; i + 4 <= len && i < 0x40; i += 4)
        W32(info + i, 0);
    if (cls == 3)   // FileFsSizeInformation
    {
        W64(info + 0x00, 0x400000);   // total units
        W64(info + 0x08, 0x200000);   // free units
        W32(info + 0x10, 8);          // sectors per unit
        W32(info + 0x14, 0x800);      // bytes per sector
    }
    SetIoStatus(iosb, STATUS_SUCCESS, len < 0x18 ? len : 0x18);
    RetStatus(ctx, STATUS_SUCCESS);
}

static bool WildcardMatch(const char* pat, const char* str)
{
    if (!*pat) return !*str;
    if (*pat == '*')
        return WildcardMatch(pat + 1, str) || (*str && WildcardMatch(pat, str + 1));
    if (*str && (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*str)))
        return WildcardMatch(pat + 1, str + 1);
    return false;
}

KERNEL_IMPORT(NtQueryDirectoryFile)
{
    // (handle, event, apcRoutine, apcContext, iosb*, info*, length, fileName(ANSI_STRING*), restartScan)
    auto obj = KGetHandle(ctx.r3.u32);
    const uint32_t iosb = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32;
    if (!obj || !obj->isDirectory)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE_, 0);
        RetStatus(ctx, STATUS_INVALID_HANDLE_);
        return;
    }
    if (!obj->dirLoaded || ctx.r11.u32)
    {
        std::string pattern = ctx.r10.u32 ? KReadGuestAnsi(ctx.r10.u32) : "*";
        if (pattern.empty()) pattern = "*";
        obj->dirEntries.clear();
        obj->dirPos = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(obj->path, ec))
        {
            const std::string name = e.path().filename().string();
            if (WildcardMatch(pattern.c_str(), name.c_str()))
                obj->dirEntries.push_back(name);
        }
        obj->dirLoaded = true;
    }
    if (obj->dirPos >= obj->dirEntries.size())
    {
        SetIoStatus(iosb, STATUS_NO_MORE_FILES_, 0);
        RetStatus(ctx, STATUS_NO_MORE_FILES_);
        return;
    }
    const std::string name = obj->dirEntries[obj->dirPos++];
    const uint32_t need = 0x40 + (uint32_t)name.size();
    if (need > length)
    {
        obj->dirPos--;
        SetIoStatus(iosb, 0xC0000023, 0);   // STATUS_BUFFER_TOO_SMALL
        RetStatus(ctx, 0xC0000023);
        return;
    }
    std::error_code ec;
    const fs::path full = fs::path(obj->path) / name;
    const bool dir = fs::is_directory(full, ec);
    const uint64_t size = dir ? 0 : (uint64_t)fs::file_size(full, ec);
    for (uint32_t i = 0; i < 0x40; i += 4)
        W32(info + i, 0);
    W64(info + 0x28, size);
    W64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    W32(info + 0x38, dir ? 0x10 : 0x80);
    W32(info + 0x3C, (uint32_t)name.size());
    memcpy(g_mem.base + info + 0x40, name.data(), name.size());
    SetIoStatus(iosb, STATUS_SUCCESS, need);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(NtFlushBuffersFile)
{
    SetIoStatus(ctx.r4.u32, STATUS_SUCCESS, 0);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(ObCreateSymbolicLink)  { RetStatus(ctx, STATUS_SUCCESS); }
KERNEL_IMPORT(ObDeleteSymbolicLink)  { RetStatus(ctx, STATUS_SUCCESS); }
