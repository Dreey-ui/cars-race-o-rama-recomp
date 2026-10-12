// Entry point: loads the decrypted XEX into guest memory and runs _xstart
#include "kernel.h"
#include "video.h"
#include "gpu/renderer.h"
#include "gpu/memwatch.h"
#include "settings.h"
#include "session_log.h"
#include "scene_assets.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <file.h>
#include <image.h>

#include "ppc_recomp_shared.h"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

// CARS_TRAP=address,size (hex, physical memory): diagnostics -- protects the range (in the 3 physical views) and logs the first
// CPU access (who reads/writes), then unprotects the range
static uint32_t g_trapPhys = 0, g_trapSize = 0;
static void SetTrapProtection(DWORD prot)
{
    for (uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
    {
        DWORD old;
        VirtualProtect(g_mem.base + view + g_trapPhys, g_trapSize, prot, &old);
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* e)
{
    const auto code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && g_trapSize && e->ExceptionRecord->NumberParameters > 1 && g_mem.base)
    {
        const uint64_t f = e->ExceptionRecord->ExceptionInformation[1];
        const uint64_t g = f - (uint64_t)g_mem.base;
        if (f >= (uint64_t)g_mem.base && g >= 0xA0000000ull && ((g & 0x1FFFFFFF) - g_trapPhys) < g_trapSize)
        {
            const GuestThread* gt = CurrentThread();
            fprintf(stderr, "[trap] %s of %08llX (physical %08llX) by guest thread %u lr=%08X | stack:",
                    e->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read", (unsigned long long)g,
                    (unsigned long long)(g & 0x1FFFFFFF), gt ? gt->threadId : 0, gt && gt->ctx ? uint32_t(gt->ctx->lr) : 0);
            if (gt && gt->ctx)
            {
                uint32_t sp = gt->ctx->r1.u32;
                for (int i = 0; i < 8; i++)
                {
                    const uint32_t next = R32(sp);
                    if (next <= sp || next - sp > 0x100000)
                        break;
                    fprintf(stderr, " %08X", R32(next - 8));
                    sp = next;
                }
            }
            fprintf(stderr, "\n");
            fflush(stderr);
            SetTrapProtection(PAGE_READWRITE);
            g_trapSize = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters > 1 && g_mem.base)
    {
        // Protected frontbuffer (only exists on the host GPU): the game's CPU wants to read it -> copy from the GPU and continue
        const uint64_t fa = e->ExceptionRecord->ExceptionInformation[1];
        if (fa >= (uint64_t)g_mem.base && fa < (uint64_t)g_mem.base + 0x100000000ull &&
            RendererHandleFrontbufferAccess(uint32_t(fa - (uint64_t)g_mem.base)))
            return EXCEPTION_CONTINUE_EXECUTION;
        // Write to a page watched by the GPU VB/IB cache: mark dirty, unprotect and retry the instruction
        if (e->ExceptionRecord->ExceptionInformation[0] == 1 && fa >= (uint64_t)g_mem.base && fa < (uint64_t)g_mem.base + 0x100000000ull &&
            MemWatchHandleWriteFault(uint32_t(fa - (uint64_t)g_mem.base)))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters > 1 && g_mem.base)
    {
        // Guest page not mapped yet: map it zeroed and go on (exploration only; up to 512x)
        const uint64_t f = e->ExceptionRecord->ExceptionInformation[1];
        static LONG autoCount = 0;
        if (f >= (uint64_t)g_mem.base && f < (uint64_t)g_mem.base + 0x100000000ull && autoCount < 512)
        {
            const uint32_t g = uint32_t(f - (uint64_t)g_mem.base);
            InterlockedIncrement(&autoCount);
            fprintf(stderr, "[auto-map] %s guest 0x%08X (page mapped with zeros)\n",
                    e->ExceptionRecord->ExceptionInformation[0] == 1 ? "write to" : "read from", g);
            fflush(stderr);
            if (g_mem.Commit(g & ~0xFFFu, 0x1000))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_BREAKPOINT && code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000409 /* fastfail */ &&
        code != 0xC0000374 /* heap corrompido */ && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t fault = e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    const uint64_t rip = (uint64_t)e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod);
    fprintf(stderr, "\n[CRASH] code=0x%08lX type=%s\n", code,
            code == EXCEPTION_ACCESS_VIOLATION ? (e->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : e->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" : "read") : "-");
    fprintf(stderr, "[CRASH] host address=0x%016llX  rip=0x%016llX  (rip - module = 0x%llX)\n",
            (unsigned long long)fault, (unsigned long long)rip, (unsigned long long)(rip - (uint64_t)mod));
    if (g_mem.base && fault >= (uint64_t)g_mem.base && fault < (uint64_t)g_mem.base + 0x100000000ull)
        fprintf(stderr, "[CRASH] GUEST address = 0x%08llX\n", (unsigned long long)(fault - (uint64_t)g_mem.base));
    {
        const CONTEXT* c = e->ContextRecord;
        const uint8_t* ip = reinterpret_cast<const uint8_t*>(rip);
        if (rip < 0x10000)
        {
            // Call through a null pointer: the return address is at the top of the stack
            const uint64_t ret = *reinterpret_cast<const uint64_t*>(c->Rsp);
            HMODULE m2 = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m2);
            fprintf(stderr, "[CRASH] call to a null address; return = 0x%016llX (return - module = 0x%llX)\n",
                    (unsigned long long)ret, (unsigned long long)(ret - (uint64_t)m2));
        }
        if (GuestThread* gt = CurrentThread(); gt && gt->ctx)
        {
            fprintf(stderr, "[CRASH] guest thread %u: lr=%08X ctr=%08X r3=%08X r1=%08X | guest stack:", gt->threadId, uint32_t(gt->ctx->lr),
                    gt->ctx->ctr.u32, gt->ctx->r3.u32, gt->ctx->r1.u32);
            uint32_t sp = gt->ctx->r1.u32;
            for (int i = 0; i < 12 && sp >= 0x10000 && g_mem.base; i++)
            {
                const uint32_t next = R32(sp);
                if (next <= sp || next - sp > 0x100000)
                    break;
                fprintf(stderr, " %08X", R32(next - 8));
                sp = next;
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[CRASH] bytes:");
        for (int i = 0; i < 12 && rip >= 0x10000; i++)
            fprintf(stderr, " %02X", ip[i]);
        fprintf(stderr, "\n[CRASH] rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX rsp=%llX\n",
                c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
        fprintf(stderr, "[CRASH] r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX\n",
                c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
    }
    // the log has to reach the file before the process dies (if someone handles the exception, the session goes on)
    fprintf(stderr, "[session] end (CRASH)\n");
    SessionLogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#define STEP(msg) do { fprintf(stderr, "[boot] %s\n", msg); fflush(stderr); } while (0)

bool g_traceImports = false;
std::string g_gameRoot;

static std::mutex g_logMutex;
static std::unordered_map<std::string, uint64_t> g_importCounts;

void LogUnimplementedImport(const char* name, PPCContext& ctx)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    const uint64_t n = ++g_importCounts[name];
    if (n <= 3)
    {
        fprintf(stderr, "[unimplemented import] %s(r3=%08X r4=%08X r5=%08X r6=%08X)%s\n",
                name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, n == 3 ? "  (silencing)" : "");
    }
}

// Loop detection: every 3 s shows the imports called many times since the last check. Also warns when the picture
// stops moving (no swap for 6 s): logs the guest stacks of all threads once per hang
static void MonitorThread()
{
    std::unordered_map<std::string, uint64_t> last;
    const bool stacks = getenv("CARS_STACKS") != nullptr;
    bool frozen = false;
    for (uint64_t tick = 1;; tick++)
    {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (stacks && tick % 4 == 0)
            KDumpGuestStacks();
        if (const uint64_t lastSwap = RendererLastSwapTick())
        {
            const double idle = double(GetTickCount64() - lastSwap) / 1000.0;
            if (idle >= 6.0 && !frozen)
            {
                frozen = true;
                fprintf(stderr, "[hang?] no new frame for %.0f s; stacks of the game threads:\n", idle);
                KDumpGuestStacks();
                SessionLogFlush();
            }
            else if (idle < 6.0 && frozen)
            {
                frozen = false;
                fprintf(stderr, "[hang?] the picture is moving again\n");
            }
        }
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (auto& [name, n] : g_importCounts)
        {
            const uint64_t d = n - last[name];
            if (d >= 100)
                fprintf(stderr, "[monitor] %s called %llu times in the last 3s\n", name.c_str(), (unsigned long long)d);
            last[name] = n;
        }
    }
}

// Fatal error: in the log and in a message box (the exe has no console when opened with a double click)
static int Fatal(const char* msg)
{
    fprintf(stderr, "%s\n", msg);
    SessionLogEnd("fatal error");
    MessageBoxA(nullptr, msg, "Cars Race-O-Rama (recomp)", MB_OK | MB_ICONERROR);
    return 1;
}

static std::filesystem::path ExeDir()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

// XEX embedded in the exe (RCDATA resource 101, src/xex_image.rc): the default_dec.xex file is no longer needed
static std::vector<uint8_t> LoadEmbeddedXex()
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res)
        return {};
    HGLOBAL h = LoadResource(nullptr, res);
    const auto* p = static_cast<const uint8_t*>(LockResource(h));
    const DWORD n = SizeofResource(nullptr, res);
    return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>{};
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, CrashHandler);
#endif
    // Tool (does not start the game and writes no session log, so game sessions are not pushed out of the rotation):
    // CarsRecomp --build-folder <folder X.x360.str or X.x360.res> <output file> builds an extracted scene folder
    if (argc == 4 && !strcmp(argv[1], "--build-folder"))
    {
        const std::filesystem::path folder = argv[2];
        uint64_t size = 0;
        const std::string ext = folder.extension().string();
        FILE* in = SceneFolderOpen(folder, ext == ".str" || ext == ".STR", size);
        FILE* out = in ? fopen(argv[3], "wb") : nullptr;
        std::vector<uint8_t> buf(1 << 20);
        for (size_t n; out && (n = fread(buf.data(), 1, buf.size(), in)) > 0;)
            fwrite(buf.data(), 1, n, out);
        if (out)
            fclose(out);
        if (in)
            fclose(in);
        return out ? 0 : 1;
    }
    // CarsRecomp --build-loose <mods folder> <C\Scenes\X\X.x360.res> <output>: builds a scene from loose files
    // (mods\C\Scenes\X\_package + geo/tex/col) into <output>.res and <output>.str, without starting the game
    if (argc == 5 && !strcmp(argv[1], "--build-loose"))
    {
        const std::filesystem::path root = argv[2], rel = argv[3], out = argv[4];
        std::string stem = rel.filename().string();
        if (stem.size() > 4)
            stem.resize(stem.size() - 4);
        std::filesystem::path res = out, str = out;
        res += ".res";
        str += ".str";
        return LooseSceneBuildFiles(root, (root / rel).parent_path(), stem, res, str) ? 0 : 1;
    }
    // logs\<date and time>\game.log next to the exe (session_log.cpp): everything the runtime writes, with the time
    SessionLogStart();
    // Usage: CarsRecomp [<default_dec.xex> <game_folder>] [--trace] [--novsync] [--nowindow] [--profile]
    // Without paths: embedded XEX and game folder = exe folder (the exe lives inside the game folder)
    std::vector<const char*> positional;
    bool vsync = SettingsGetInt(L"Video", L"VSync", 1) != 0, window = true, profile = false;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--trace"))
            g_traceImports = true;
        else if (!strcmp(argv[i], "--novsync"))
            vsync = false;
        else if (!strcmp(argv[i], "--nowindow"))
            window = false;
        else if (!strcmp(argv[i], "--profile"))
            profile = true;
        else if (argv[i][0] != '-')
            positional.push_back(argv[i]);
    }
    std::vector<uint8_t> file;
    STEP("reading the XEX");
    if (positional.size() >= 2)
    {
        g_gameRoot = positional[1];
        file = LoadFile(positional[0]);
        if (file.empty())
        {
            char msg[600];
            snprintf(msg, sizeof(msg), "Could not open %s", positional[0]);
            return Fatal(msg);
        }
    }
    else
    {
        g_gameRoot = positional.size() == 1 ? positional[0] : ExeDir().string();
        file = LoadEmbeddedXex();
        if (file.empty())
            return Fatal("Embedded XEX not found in the executable.");
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(g_gameRoot) / "C", ec))
        {
            char msg[900];
            snprintf(msg, sizeof(msg),
                     "The game files were not found in:\n%s\n\nPut this executable inside the game folder "
                     "(the folder that has C, E, F, G, S, default.xex...).", g_gameRoot.c_str());
            return Fatal(msg);
        }
    }
    fprintf(stderr, "[boot] game folder: %s\n", g_gameRoot.c_str());
    STEP("ParseImage");
    Image image = Image::ParseImage(file.data(), file.size());
    if (image.sections.empty())
    {
        fprintf(stderr, "Invalid XEX (use the decrypted default_dec.xex)\n");
        return 1;
    }

    STEP("reserving 4 GB");
    if (!g_mem.Init())
    {
        fprintf(stderr, "Failed to reserve 4 GB of virtual memory\n");
        return 1;
    }

    // Guest heaps, outside the image (0x82000000..~0x83125000, including the function table)
    g_virtualHeap.Init(0x20000000, 0x7E000000);
    g_poolHeap.Init(0x90000000, 0xA0000000);
    g_physicalHeap.Init(0xA0000000, 0xC0000000);

    STEP("mapping sections (main.cpp v6)");
    fprintf(stderr, "[boot] image.base=0x%zX image.size=0x%X PPC_IMAGE_SIZE=0x%llX\n", image.base, image.size, (unsigned long long)PPC_IMAGE_SIZE);
    // Map the sections
    const uint32_t imageEnd = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
    g_mem.Commit(uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE));
    for (const auto& s : image.sections)
        fprintf(stderr, "[boot] section %-8s base=0x%08zX size=0x%X\n", s.name.c_str(), s.base, s.size);
    for (const auto& s : image.sections)
    {
        // clamp to the real image size (sections such as .reloc can go past the end)
        if (s.base < PPC_IMAGE_BASE || s.base >= imageEnd)
            continue;
        if (s.name.compare(0, 6, ".reloc") == 0) // PE relocations only; the XEX buffer ends before the declared end
            continue;
        uint64_t size = s.size;
        if (s.base + size > imageEnd)
            size = imageEnd - s.base;
        fprintf(stderr, "[boot] copying 0x%08zX + 0x%llX\n", s.base, (unsigned long long)size);
        memcpy(g_mem.base + s.base, s.data, size);
    }

    STEP("function table");
    // Function table: one 8-byte pointer per guest instruction
    g_mem.Commit(imageEnd, uint32_t(PPC_CODE_SIZE * 2));
    size_t count = 0;
    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
        {
            PPC_LOOKUP_FUNC(g_mem.base, PPCFuncMappings[i].guest) = PPCFuncMappings[i].host;
            count++;
        }
    }
    printf("[boot] image 0x%08X, %zu functions registered, entry 0x%08X\n",
           uint32_t(PPC_IMAGE_BASE), count, uint32_t(image.entry_point));
    fflush(stdout);

    std::thread(MonitorThread).detach();
    extern void HostThreadUsageStart();
    HostThreadUsageStart();
    atexit([] { SessionLogEnd("process exited"); });
    if (getenv("CARS_SAMPLE"))
    {
        extern void KStartSamplingProfiler();
        KStartSamplingProfiler();
    }
    if (window)
        RendererInit(vsync, GpuRegisters(), g_mem.base + uint32_t(PPC_IMAGE_BASE), size_t(PPC_IMAGE_SIZE));
    if (profile)
        D3DProfileStart();
    extern void XmaInit();
    XmaInit();   // XMA context table + registers (the game's XAudio2 reads them at boot)
    if (const char* trap = getenv("CARS_TRAP"))
    {
        char* end = nullptr;
        const uint32_t a = uint32_t(strtoul(trap, &end, 16));
        const uint32_t n = (end && *end == ',') ? uint32_t(strtoul(end + 1, nullptr, 16)) : 0x1000;
        g_trapPhys = (a & 0x1FFFFFFF) & ~0xFFFu;
        const uint32_t size = (n + 0xFFF) & ~0xFFFu;
        // CARS_TRAP_DELAY=ms: arm after this delay (skips the boot accesses)
        const int delay = getenv("CARS_TRAP_DELAY") ? atoi(getenv("CARS_TRAP_DELAY")) : 0;
        std::thread([size, delay] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            g_trapSize = size;
            SetTrapProtection(PAGE_NOACCESS);
            fprintf(stderr, "[trap] protecting physical %08X + %X\n", g_trapPhys, g_trapSize);
        }).detach();
    }
    STEP("starting _xstart");
    RunGuestMain(_xstart, 0x40000);
    printf("[boot] _xstart returned\n");
    SessionLogEnd("_xstart returned");
    return 0;
}
