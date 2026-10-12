#include "kernel.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>

static thread_local GuestThread* t_thread = nullptr;
static std::atomic<uint32_t> g_nextThreadId{ 1 };

GuestThread* CurrentThread() { return t_thread; }

// Kernel structure fields the game actually reads:
//   r13 -> PCR:  +0x070 stack base, +0x074 stack limit, +0x100 current KTHREAD,
//                +0x10C current CPU, +0x150 KPRCB (first field: current KTHREAD)
//   KTHREAD:     +0x058 stack limit, +0x05C stack base, +0x14C thread id
static std::mutex g_threadMapMutex;
static std::unordered_map<uint32_t, GuestThread*> g_threadsByKthread;

// "Current" processor of the thread (0..5): PCR+0x10C (prcb.current_cpu) and KTHREAD+0xBF, as in Xenia.
// The game's XAudio2 and job system use this number to index busy-wait barriers: if two threads
// have the same number (or a different number than the game asked for in the affinity) they spin forever.
void KSetThreadCpu(GuestThread* gt, uint32_t cpu)
{
    W8(gt->pcr + 0x10C, uint8_t(cpu));
    W8(gt->kthread + 0xBF, uint8_t(cpu));
}

uint32_t KGetThreadCpu(const GuestThread* gt) { return gt->pcr ? (*(g_mem.base + gt->pcr + 0x10C)) : 0; }

GuestThread* KFindThreadByKthread(uint32_t kthread)
{
    std::lock_guard<std::mutex> lock(g_threadMapMutex);
    auto it = g_threadsByKthread.find(kthread);
    return it != g_threadsByKthread.end() ? it->second : nullptr;
}

// Affinity mask -> processor (lowest bit); no mask: the same processor as the creating thread
static uint32_t CpuFromAffinity(uint32_t mask)
{
    if (mask & 0x3F)
        return uint32_t(__builtin_ctz(mask & 0x3F));
    const GuestThread* parent = CurrentThread();
    return parent ? KGetThreadCpu(parent) : 0;
}

GuestThread* KSetupGuestThread(uint32_t stackSize, uint32_t affinityMask)
{
    auto* gt = new GuestThread();
    gt->threadId = g_nextThreadId++;

    if (stackSize < 0x40000)
        stackSize = 0x40000;
    stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;

    const uint32_t stack = g_virtualHeap.Alloc(stackSize, 0x10000);
    gt->stackLimit = stack;
    gt->stackBase = stack + stackSize;

    gt->pcr = g_poolHeap.Alloc(0x1000, 0x1000);
    gt->kthread = g_poolHeap.Alloc(0x1000, 0x1000);
    const uint32_t tlsBlock = g_poolHeap.Alloc(0x1000, 0x1000);

    W32(gt->pcr + 0x000, tlsBlock);
    W32(gt->pcr + 0x010, gt->pcr);
    W32(gt->pcr + 0x070, gt->stackBase);
    W32(gt->pcr + 0x074, gt->stackLimit);
    W32(gt->pcr + 0x100, gt->kthread);
    W32(gt->pcr + 0x150, gt->kthread);

    W8 (gt->kthread + 0x00, 6);
    W32(gt->kthread + 0x058, gt->stackLimit);
    W32(gt->kthread + 0x05C, gt->stackBase);
    W32(gt->kthread + 0x14C, gt->threadId);
    KSetThreadCpu(gt, CpuFromAffinity(affinityMask));
    {
        std::lock_guard<std::mutex> lock(g_threadMapMutex);
        g_threadsByKthread[gt->kthread] = gt;
    }
    return gt;
}

void KInitContext(PPCContext& ctx, GuestThread* gt)
{
    ctx = PPCContext{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = gt->stackBase - 0x100;
    ctx.r13.u64 = gt->pcr;
}

void RunGuestMain(PPCFunc* entry, uint32_t stackSize)
{
    GuestThread* gt = KSetupGuestThread(stackSize);
    t_thread = gt;
    auto ctx = std::make_unique<PPCContext>();
    KInitContext(*ctx, gt);
    gt->ctx = ctx.get();
    gt->entry = 0x822FD290;
    KRegisterThreadForSampling(gt);
    try
    {
        entry(*ctx, g_mem.base);
    }
    catch (const GuestThreadExit&)
    {
    }
}

static void GuestThreadMain(KObjectPtr obj, GuestThread* gt, uint32_t startup, uint32_t startRoutine, uint32_t startContext)
{
    t_thread = gt;
    {
        std::unique_lock<std::mutex> lock(g_kmtx);
        g_kcv.wait(lock, [&] { return !obj->suspended; });
    }

    auto ctx = std::make_unique<PPCContext>();
    KInitContext(*ctx, gt);

    uint32_t entry;
    if (startup)
    {
        entry = startup;                 // XapiThreadStartup(startRoutine, startContext)
        ctx->r3.u64 = startRoutine;
        ctx->r4.u64 = startContext;
    }
    else
    {
        entry = startRoutine;
        ctx->r3.u64 = startContext;
    }
    gt->ctx = ctx.get();
    gt->entry = startRoutine;
    KRegisterThreadForSampling(gt);

    try
    {
        CallGuest(*ctx, g_mem.base, entry);
        obj->exitCode = ctx->r3.u32;
    }
    catch (const GuestThreadExit&)
    {
    }

    std::lock_guard<std::mutex> lock(g_kmtx);
    obj->finished = true;
    g_kcv.notify_all();
}

uint32_t KCreateGuestThread(uint32_t stackSize, uint32_t startup, uint32_t startRoutine, uint32_t startContext,
                            bool suspended, uint32_t* handleOut, uint32_t* tidOut, uint32_t creationFlags)
{
    // High byte of the creation flags = processor mask (XDK XSetThreadProcessor / CreateThread)
    GuestThread* gt = KSetupGuestThread(stackSize, creationFlags >> 24);

    auto obj = std::make_shared<KObject>();
    obj->type = KType::Thread;
    obj->suspended = suspended;
    obj->kthread = gt->kthread;
    obj->threadId = gt->threadId;

    const uint32_t handle = KNewHandle(obj);
    KRegisterGuestObject(gt->kthread, obj);
    if (handleOut) *handleOut = handle;
    if (tidOut) *tidOut = gt->threadId;

    std::thread(GuestThreadMain, obj, gt, startup, startRoutine, startContext).detach();
    return STATUS_SUCCESS;
}

void KSetCurrentThread(GuestThread* gt) { t_thread = gt; }

// ---- Diagnostics: guest stack sampling (CARS_STACKS=1) ----
static std::mutex g_sampleMutex;
static std::vector<GuestThread*> g_sampleThreads;

void KRegisterThreadForSampling(GuestThread* gt)
{
    char name[64];
    snprintf(name, sizeof(name), "guest %u (%08X)", gt->threadId, gt->entry);
    extern void HostProfilerRegisterCurrentThread(const char* name);
    HostProfilerRegisterCurrentThread(name);   // thread name + native profiler (CARS_HOST_SAMPLE)
    std::lock_guard<std::mutex> lock(g_sampleMutex);
    g_sampleThreads.push_back(gt);
}

static bool InStack(const GuestThread* gt, uint32_t sp) { return sp >= gt->stackLimit && sp + 8 <= gt->stackBase; }

void KDumpGuestStacks()
{
    std::lock_guard<std::mutex> lock(g_sampleMutex);
    for (GuestThread* gt : g_sampleThreads)
    {
        if (!gt->ctx)
            continue;
        const PPCContext& c = *gt->ctx;
        fprintf(stderr, "[stack] thread %u (entry %08X): lr=%08X r3=%08X |", gt->threadId, gt->entry, uint32_t(c.lr), c.r3.u32);
        uint32_t sp = c.r1.u32;
        for (int i = 0; i < 14 && InStack(gt, sp); i++)
        {
            const uint32_t next = R32(sp);
            if (!InStack(gt, next) || next <= sp)
                break;
            fprintf(stderr, " %08X", R32(next - 8));   // LR saved by the prologue (r12 at -8 of the caller's frame)
            sp = next;
        }
        fprintf(stderr, "\n");
    }
}
// Reads a thread's guest stack (guarded: the thread may be switching stacks or may have exited)
static bool SampleThread(GuestThread* gt, char* buf, size_t size)
{
    __try
    {
        int n = snprintf(buf, size, "t%u lr=%08X |", gt->threadId, uint32_t(gt->ctx->lr));
        uint32_t sp = gt->ctx->r1.u32;
        for (int i = 0; i < 6 && InStack(gt, sp) && n < 200; i++)
        {
            const uint32_t next = R32(sp);
            if (!InStack(gt, next) || next <= sp)
                break;
            n += snprintf(buf + n, size - n, " %08X", R32(next - 8));
            sp = next;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Statistical profiler (CARS_SAMPLE=1): samples the guest stacks every 10 ms and shows the most frequent ones
void KStartSamplingProfiler()
{
    std::thread([] {
        std::unordered_map<std::string, uint32_t> hist;
        uint32_t samples = 0;
        auto last = std::chrono::steady_clock::now();
        char buf[256];
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            {
                std::lock_guard<std::mutex> lock(g_sampleMutex);
                for (GuestThread* gt : g_sampleThreads)
                {
                    if (!gt->ctx)
                        continue;
                    if (SampleThread(gt, buf, sizeof(buf)))
                        hist[buf]++;
                }
                samples++;
            }
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(20))
            {
                std::vector<std::pair<std::string, uint32_t>> v(hist.begin(), hist.end());
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                fprintf(stderr, "[samples] %u samples:\n", samples);
                for (size_t i = 0; i < v.size() && i < 25; i++)
                    fprintf(stderr, "[samples] %5.1f%% %s\n", 100.0 * v[i].second / samples, v[i].first.c_str());
                hist.clear();
                samples = 0;
                last = std::chrono::steady_clock::now();
            }
        }
    }).detach();
}

// ---- User APCs ----
void KQueueApc(uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2)
{
    if (GuestThread* gt = CurrentThread())
        gt->apcs.push_back({ routine, context, arg1, arg2 });
}

bool KDeliverApcs(PPCContext& ctx)
{
    GuestThread* gt = CurrentThread();
    if (!gt || gt->apcs.empty())
        return false;
    auto list = std::move(gt->apcs);
    gt->apcs.clear();
    auto c = std::make_unique<PPCContext>();
    for (const auto& a : list)
    {
        // Run the routine on a copied context, with the stack below the caller's
        *c = ctx;
        c->r1.u64 = (ctx.r1.u32 - 0x1000) & ~0xFu;
        c->r3.u64 = a.context;
        c->r4.u64 = a.arg1;
        c->r5.u64 = a.arg2;
        CallGuest(*c, g_mem.base, a.routine);
    }
    return true;
}
