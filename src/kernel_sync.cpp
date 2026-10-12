// Threads, synchronization, TLS, spinlocks and critical sections
#include "kernel.h"

#include <chrono>

static std::mutex g_lockTableMutex;
using LockMap = std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>>;
static LockMap g_csTable;
static LockMap g_spinTable;

static std::recursive_mutex& LockFor(LockMap& table, uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_lockTableMutex);
    auto& slot = table[addr];
    if (!slot)
        slot = std::make_unique<std::recursive_mutex>();
    return *slot;
}

static void InitCriticalSection(uint32_t p)
{
    W8(p, 1);
    W32(p + 0x10, 0xFFFFFFFF);
    LockFor(g_csTable, p);
}

KERNEL_IMPORT(RtlInitializeCriticalSection)              { InitCriticalSection(ctx.r3.u32); }
KERNEL_IMPORT(RtlInitializeCriticalSectionAndSpinCount)  { InitCriticalSection(ctx.r3.u32); RetStatus(ctx, STATUS_SUCCESS); }
KERNEL_IMPORT(RtlEnterCriticalSection)                   { LockFor(g_csTable, ctx.r3.u32).lock(); }
KERNEL_IMPORT(RtlLeaveCriticalSection)                   { LockFor(g_csTable, ctx.r3.u32).unlock(); }

KERNEL_IMPORT(KfAcquireSpinLock)                 { LockFor(g_spinTable, ctx.r3.u32).lock(); ctx.r3.u64 = 0; }
KERNEL_IMPORT(KfReleaseSpinLock)                 { LockFor(g_spinTable, ctx.r3.u32).unlock(); }
KERNEL_IMPORT(KeAcquireSpinLockAtRaisedIrql)     { LockFor(g_spinTable, ctx.r3.u32).lock(); }
KERNEL_IMPORT(KeReleaseSpinLockFromRaisedIrql)   { LockFor(g_spinTable, ctx.r3.u32).unlock(); }
KERNEL_IMPORT(KfLowerIrql)                       { }
KERNEL_IMPORT(KeRaiseIrqlToDpcLevel)             { ctx.r3.u64 = 0; }
KERNEL_IMPORT(KeEnterCriticalRegion)             { }
KERNEL_IMPORT(KeLeaveCriticalRegion)             { }
KERNEL_IMPORT(KeLockL2)                          { }
KERNEL_IMPORT(KeUnlockL2)                        { }
KERNEL_IMPORT(KeEnableFpuExceptions)             { ctx.r3.u64 = 0; }
KERNEL_IMPORT(KeGetCurrentProcessType)           { ctx.r3.u64 = 1; }

// Dynamic TLS: 64 slots, values per guest thread
static std::mutex g_tlsMutex;
static bool g_tlsUsed[64];

KERNEL_IMPORT(KeTlsAlloc)
{
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    for (uint32_t i = 0; i < 64; i++)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

KERNEL_IMPORT(KeTlsFree)
{
    const uint32_t i = ctx.r3.u32;
    if (i < 64)
    {
        std::lock_guard<std::mutex> lock(g_tlsMutex);
        g_tlsUsed[i] = false;
    }
    ctx.r3.u64 = 1;
}

KERNEL_IMPORT(KeTlsGetValue)
{
    const uint32_t i = ctx.r3.u32;
    ctx.r3.u64 = (i < 64 && CurrentThread()) ? CurrentThread()->tls[i] : 0;
}

KERNEL_IMPORT(KeTlsSetValue)
{
    const uint32_t i = ctx.r3.u32;
    if (i < 64 && CurrentThread())
        CurrentThread()->tls[i] = ctx.r4.u32;
    ctx.r3.u64 = 1;
}

// Events
static KObjectPtr NewEvent(bool manual, bool initial)
{
    auto o = std::make_shared<KObject>();
    o->type = KType::Event;
    o->manualReset = manual;
    o->signaled = initial;
    return o;
}

KERNEL_IMPORT(NtCreateEvent)
{
    // (PHANDLE, ATTRIBUTES, 0=notification 1=synchronization, initial state)
    const bool manual = ctx.r5.u32 == 0;
    const bool initial = (ctx.r6.u32 & 0xFF) != 0;
    const uint32_t h = KNewHandle(NewEvent(manual, initial));
    if (ctx.r3.u32)
        W32(ctx.r3.u32, h);
    RetStatus(ctx, STATUS_SUCCESS);
}

static uint32_t SignalEvent(const KObjectPtr& o)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    const uint32_t prev = o->signaled ? 1 : 0;
    o->signaled = true;
    g_kcv.notify_all();
    return prev;
}

KERNEL_IMPORT(NtSetEvent)
{
    auto o = KGetHandle(ctx.r3.u32);
    if (!o) { RetStatus(ctx, STATUS_INVALID_HANDLE); return; }
    const uint32_t prev = SignalEvent(o);
    if (ctx.r4.u32)
        W32(ctx.r4.u32, prev);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(KeSetEvent)   { ctx.r3.u64 = SignalEvent(KGetGuestObject(ctx.r3.u32)); }

KERNEL_IMPORT(KeResetEvent)
{
    auto o = KGetGuestObject(ctx.r3.u32);
    std::lock_guard<std::mutex> lock(g_kmtx);
    ctx.r3.u64 = o->signaled ? 1 : 0;
    o->signaled = false;
}

// Semaphores
KERNEL_IMPORT(KeInitializeSemaphore)
{
    const uint32_t p = ctx.r3.u32;
    W8(p, 5);
    W32(p + 4, ctx.r4.u32);
    W32(p + 0x10, ctx.r5.u32);
    auto o = std::make_shared<KObject>();
    o->type = KType::Semaphore;
    o->count = (int32_t)ctx.r4.u32;
    o->limit = (int32_t)ctx.r5.u32;
    KRegisterGuestObject(p, o);
}

KERNEL_IMPORT(KeReleaseSemaphore)
{
    auto o = KGetGuestObject(ctx.r3.u32);
    std::lock_guard<std::mutex> lock(g_kmtx);
    const int32_t prev = o->count;
    o->count += (int32_t)ctx.r5.u32;
    if (o->limit > 0 && o->count > o->limit)
        o->count = o->limit;
    g_kcv.notify_all();
    ctx.r3.u64 = (uint32_t)prev;
}

// Mutants
KERNEL_IMPORT(NtCreateMutant)
{
    auto o = std::make_shared<KObject>();
    o->type = KType::Mutant;
    if (ctx.r5.u32 & 0xFF)
    {
        o->owner = CurrentThread() ? CurrentThread()->threadId : 0xFFFF;
        o->recursion = 1;
    }
    const uint32_t h = KNewHandle(o);
    if (ctx.r3.u32)
        W32(ctx.r3.u32, h);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(NtReleaseMutant)
{
    auto o = KGetHandle(ctx.r3.u32);
    if (!o) { RetStatus(ctx, STATUS_INVALID_HANDLE); return; }
    std::lock_guard<std::mutex> lock(g_kmtx);
    if (ctx.r4.u32)
        W32(ctx.r4.u32, (uint32_t)o->recursion);
    if (o->recursion > 0 && --o->recursion == 0)
        o->owner = 0;
    g_kcv.notify_all();
    RetStatus(ctx, STATUS_SUCCESS);
}

// Waits
KERNEL_IMPORT(NtWaitForSingleObjectEx)
{
    // (handle, mode, alertable, timeout*)
    if (ctx.r5.u32 && KDeliverApcs(ctx)) { RetStatus(ctx, STATUS_USER_APC); return; }
    auto o = KGetHandle(ctx.r3.u32);
    if (!o) { RetStatus(ctx, STATUS_INVALID_HANDLE); return; }
    const uint32_t st = KWait({ o }, false, KTimeoutFromGuest(ctx.r6.u32));
    if (g_traceImports)
    {
        // Wait histogram (polling diagnostics)
        static std::mutex m;
        static std::unordered_map<uint64_t, uint32_t> hist;
        static auto last = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m);
        hist[(uint64_t(ctx.r3.u32) << 32) | st]++;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10))
        {
            for (auto& [k, n] : hist)
                if (n > 20)
                    fprintf(stderr, "[waits] handle %08X status %08X: %u times in 10s (type %d, timeout %lld ms, lr %08X)\n",
                            uint32_t(k >> 32), uint32_t(k), n, (int)o->type, (long long)KTimeoutFromGuest(ctx.r6.u32), uint32_t(ctx.lr));
            hist.clear();
            last = std::chrono::steady_clock::now();
        }
    }
    RetStatus(ctx, st);
}

KERNEL_IMPORT(KeWaitForSingleObject)
{
    if (ctx.r6.u32 && KDeliverApcs(ctx)) { RetStatus(ctx, STATUS_USER_APC); return; }
    RetStatus(ctx, KWait({ KGetGuestObject(ctx.r3.u32) }, false, KTimeoutFromGuest(ctx.r7.u32)));
}

KERNEL_IMPORT(NtWaitForMultipleObjectsEx)
{
    // (n, handles, type, mode, alertable, timeout*)
    if (ctx.r7.u32 && KDeliverApcs(ctx)) { RetStatus(ctx, STATUS_USER_APC); return; }
    std::vector<KObjectPtr> objs;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto o = KGetHandle(R32(ctx.r4.u32 + i * 4));
        if (!o) { RetStatus(ctx, STATUS_INVALID_HANDLE); return; }
        objs.push_back(o);
    }
    RetStatus(ctx, KWait(objs, ctx.r5.u32 == 0, KTimeoutFromGuest(ctx.r8.u32)));
}

KERNEL_IMPORT(KeWaitForMultipleObjects)
{
    std::vector<KObjectPtr> objs;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objs.push_back(KGetGuestObject(R32(ctx.r4.u32 + i * 4)));
    RetStatus(ctx, KWait(objs, ctx.r5.u32 == 0, KTimeoutFromGuest(ctx.r9.u32)));
}

KERNEL_IMPORT(KeDelayExecutionThread)
{
    // (mode, alertable, interval*)
    if (ctx.r4.u32 && KDeliverApcs(ctx)) { RetStatus(ctx, STATUS_USER_APC); return; }
    const int64_t ms = KTimeoutFromGuest(ctx.r5.u32);
    if (ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    else
        GuestYield();
    RetStatus(ctx, STATUS_SUCCESS);
}

// Threads
KERNEL_IMPORT(ExCreateThread)
{
    // (PHANDLE, StackSize, PThreadId, XapiThreadStartup, StartRoutine, StartContext, CreationFlags)
    uint32_t handle = 0, tid = 0;
    KCreateGuestThread(ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, (ctx.r9.u32 & 1) != 0, &handle, &tid, ctx.r9.u32);
    if (ctx.r3.u32) W32(ctx.r3.u32, handle);
    if (ctx.r5.u32) W32(ctx.r5.u32, tid);
    RetStatus(ctx, STATUS_SUCCESS);
}

static uint32_t ResumeThreadObj(const KObjectPtr& o)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    const uint32_t prev = o->suspended ? 1 : 0;
    o->suspended = false;
    g_kcv.notify_all();
    return prev;
}

KERNEL_IMPORT(NtResumeThread)
{
    auto o = KGetHandle(ctx.r3.u32);
    if (!o || o->type != KType::Thread) { RetStatus(ctx, STATUS_INVALID_HANDLE); return; }
    const uint32_t prev = ResumeThreadObj(o);
    if (ctx.r4.u32) W32(ctx.r4.u32, prev);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(KeResumeThread)
{
    auto o = KGetGuestObject(ctx.r3.u32);
    ctx.r3.u64 = o->type == KType::Thread ? ResumeThreadObj(o) : 0;
}

// (KTHREAD*, affinity mask, previous mask*): the "current" processor becomes the lowest bit of the mask
KERNEL_IMPORT(KeSetAffinityThread)
{
    const uint32_t mask = ctx.r4.u32 & 0x3F;
    if (!mask)
    {
        RetStatus(ctx, 0xC000000D);   // STATUS_INVALID_PARAMETER
        return;
    }
    GuestThread* gt = KFindThreadByKthread(ctx.r3.u32);
    if (!gt)
    {
        RetStatus(ctx, STATUS_INVALID_HANDLE);
        return;
    }
    if (ctx.r5.u32)
        W32(ctx.r5.u32, 1u << KGetThreadCpu(gt));
    KSetThreadCpu(gt, uint32_t(__builtin_ctz(mask)));
    static std::atomic<int> logged{ 0 };
    if (logged++ < 40)
        fprintf(stderr, "[cpu] thread %u (entry %08X) -> processor %u\n", gt->threadId, gt->entry, KGetThreadCpu(gt));
    RetStatus(ctx, STATUS_SUCCESS);
}
KERNEL_IMPORT(KeSetBasePriorityThread)  { ctx.r3.u64 = 0; }

KERNEL_IMPORT(ExTerminateThread)
{
    GuestThread* gt = CurrentThread();
    if (gt && gt->threadId == 1)
    {
        printf("[kernel] the main thread called ExTerminateThread(%u): exiting\n", ctx.r3.u32);
        fflush(stdout);
        exit(0);
    }
    throw GuestThreadExit{};
}

KERNEL_IMPORT(ObReferenceObjectByHandle)
{
    auto o = KGetHandle(ctx.r3.u32);
    if (!o) { RetStatus(ctx, STATUS_INVALID_HANDLE); return; }

    uint32_t ptr = o->guestPtr;
    if (ptr == 0)
    {
        ptr = GuestAllocSmall(0x40);
        switch (o->type)
        {
        case KType::Event:     W8(ptr, o->manualReset ? 0 : 1); break;
        case KType::Mutant:    W8(ptr, 2); break;
        case KType::Semaphore: W8(ptr, 5); break;
        default:               break;
        }
        KRegisterGuestObject(ptr, o);
    }
    if (ctx.r5.u32)
        W32(ctx.r5.u32, ptr);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(ObReferenceObject)    { }
KERNEL_IMPORT(ObDereferenceObject)  { }

KERNEL_IMPORT(NtClose)
{
    RetStatus(ctx, KCloseHandle(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
}
