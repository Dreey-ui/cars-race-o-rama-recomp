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

// Campos das estruturas do kernel que o jogo realmente lê:
//   r13 -> PCR:  +0x070 base da pilha, +0x074 limite da pilha, +0x100 KTHREAD atual,
//                +0x10C CPU atual, +0x150 KPRCB (primeiro campo: KTHREAD atual)
//   KTHREAD:     +0x058 limite da pilha, +0x05C base da pilha, +0x14C id da thread
static std::mutex g_threadMapMutex;
static std::unordered_map<uint32_t, GuestThread*> g_threadsByKthread;

// Processador "atual" da thread (0..5): PCR+0x10C (prcb.current_cpu) e KTHREAD+0xBF, como no Xenia.
// O XAudio2 e o sistema de jobs do jogo usam esse numero para indexar barreiras de espera ativa: se duas threads
// tiverem o mesmo numero (ou um numero diferente do que o jogo pediu na afinidade) elas giram sem fim.
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

// Mascara de afinidade -> processador (o menor bit); sem mascara: o mesmo processador da thread que criou
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
    // Byte alto dos flags de criacao = mascara de processadores (XSetThreadProcessor / CreateThread do XDK)
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

// ---- Diagnostico: amostragem das pilhas guest (CARS_STACKS=1) ----
static std::mutex g_sampleMutex;
static std::vector<GuestThread*> g_sampleThreads;

void KRegisterThreadForSampling(GuestThread* gt)
{
    char name[64];
    snprintf(name, sizeof(name), "guest %u (%08X)", gt->threadId, gt->entry);
    extern void HostProfilerRegisterCurrentThread(const char* name);
    HostProfilerRegisterCurrentThread(name);   // nome da thread + profiler nativo (CARS_HOST_SAMPLE)
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
        fprintf(stderr, "[pilha] thread %u (entrada %08X): lr=%08X r3=%08X |", gt->threadId, gt->entry, uint32_t(c.lr), c.r3.u32);
        uint32_t sp = c.r1.u32;
        for (int i = 0; i < 14 && InStack(gt, sp); i++)
        {
            const uint32_t next = R32(sp);
            if (!InStack(gt, next) || next <= sp)
                break;
            fprintf(stderr, " %08X", R32(next - 8));   // LR salvo pelo prologo (r12 em -8 do frame do chamador)
            sp = next;
        }
        fprintf(stderr, "\n");
    }
}
// Le a pilha guest de uma thread (protegido: a thread pode estar mudando de pilha ou ter terminado)
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

// Profiler estatistico (CARS_SAMPLE=1): amostra as pilhas guest a cada 10 ms e mostra as mais frequentes
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
                fprintf(stderr, "[amostras] %u amostras:\n", samples);
                for (size_t i = 0; i < v.size() && i < 25; i++)
                    fprintf(stderr, "[amostras] %5.1f%% %s\n", 100.0 * v[i].second / samples, v[i].first.c_str());
                hist.clear();
                samples = 0;
                last = std::chrono::steady_clock::now();
            }
        }
    }).detach();
}

// ---- APCs de usuario ----
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
        // Roda a rotina num contexto copiado, com a pilha abaixo da do chamador
        *c = ctx;
        c->r1.u64 = (ctx.r1.u32 - 0x1000) & ~0xFu;
        c->r3.u64 = a.context;
        c->r4.u64 = a.arg1;
        c->r5.u64 = a.arg2;
        CallGuest(*c, g_mem.base, a.routine);
    }
    return true;
}
