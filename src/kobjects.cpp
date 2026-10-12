#include "kernel.h"

#include <chrono>

std::mutex g_kmtx;
std::condition_variable g_kcv;

static std::unordered_map<uint32_t, KObjectPtr> g_handles;
static std::unordered_map<uint32_t, KObjectPtr> g_guestObjects;
static uint32_t g_nextHandle = 0x80000100;

uint32_t KNewHandle(const KObjectPtr& obj)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    const uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = obj;
    return h;
}

KObjectPtr KGetHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool KCloseHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    return g_handles.erase(handle) != 0;
}

void KRegisterGuestObject(uint32_t guestPtr, const KObjectPtr& obj)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    obj->guestPtr = guestPtr;
    g_guestObjects[guestPtr] = obj;
}

// Objetos que o jogo monta sozinho na memória guest. O 1º byte é o tipo, +4 é o SignalState.
KObjectPtr KGetGuestObject(uint32_t guestPtr)
{
    std::lock_guard<std::mutex> lock(g_kmtx);
    auto it = g_guestObjects.find(guestPtr);
    if (it != g_guestObjects.end())
        return it->second;

    auto obj = std::make_shared<KObject>();
    obj->guestPtr = guestPtr;
    switch (R8(guestPtr))
    {
    case 1:  // evento de sincronização (auto-reset)
        obj->type = KType::Event;
        obj->manualReset = false;
        obj->signaled = R32(guestPtr + 4) != 0;
        break;
    case 2:
        obj->type = KType::Mutant;
        break;
    case 5:
        obj->type = KType::Semaphore;
        obj->count = (int32_t)R32(guestPtr + 4);
        obj->limit = (int32_t)R32(guestPtr + 0x10);
        break;
    default: // 0: evento de notificação (manual-reset)
        obj->type = KType::Event;
        obj->manualReset = true;
        obj->signaled = R32(guestPtr + 4) != 0;
        break;
    }
    g_guestObjects[guestPtr] = obj;
    return obj;
}

static bool CanAcquire(const KObject& o, uint32_t me)
{
    switch (o.type)
    {
    case KType::Event:     return o.signaled;
    case KType::Semaphore: return o.count > 0;
    case KType::Mutant:    return o.owner == 0 || o.owner == me;
    case KType::Thread:    return o.finished;
    default:               return true;
    }
}

static void Acquire(KObject& o, uint32_t me)
{
    switch (o.type)
    {
    case KType::Event:     if (!o.manualReset) o.signaled = false; break;
    case KType::Semaphore: o.count--; break;
    case KType::Mutant:    o.owner = me; o.recursion++; break;
    default: break;
    }
}

uint32_t KWait(const std::vector<KObjectPtr>& objects, bool waitAll, int64_t timeoutMs)
{
    GuestThread* gt = CurrentThread();
    const uint32_t me = gt ? gt->threadId : 0xFFFF;

    std::unique_lock<std::mutex> lock(g_kmtx);

    auto poll = [&]() -> int64_t
    {
        if (waitAll)
        {
            for (auto& o : objects)
                if (!CanAcquire(*o, me))
                    return -1;
            for (auto& o : objects)
                Acquire(*o, me);
            return 0;
        }
        for (size_t i = 0; i < objects.size(); i++)
        {
            if (CanAcquire(*objects[i], me))
            {
                Acquire(*objects[i], me);
                return (int64_t)i;
            }
        }
        return -1;
    };

    int64_t r = poll();
    if (r >= 0)
        return STATUS_SUCCESS + (uint32_t)r;
    if (timeoutMs == 0)
        return STATUS_TIMEOUT;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;)
    {
        if (timeoutMs < 0)
        {
            if (g_kcv.wait_for(lock, std::chrono::seconds(3)) == std::cv_status::timeout && g_traceImports)
            {
                fprintf(stderr, "[espera] thread %u bloqueada ha mais de 3s esperando %zu objeto(s) (tipo %d)\n",
                        me, objects.size(), objects.empty() ? -1 : (int)objects[0]->type);
                fflush(stderr);
            }
        }
        else if (g_kcv.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            r = poll();
            return r >= 0 ? STATUS_SUCCESS + (uint32_t)r : STATUS_TIMEOUT;
        }
        r = poll();
        if (r >= 0)
            return STATUS_SUCCESS + (uint32_t)r;
    }
}

int64_t KTimeoutFromGuest(uint32_t timeoutPtr)
{
    if (timeoutPtr == 0)
        return -1;
    const int64_t v = (int64_t)R64(timeoutPtr);
    if (v == 0)
        return 0;
    if (v < 0)
    {
        const int64_t ms = (-v) / 10000;
        return ms > 0 ? ms : 1;
    }
    return 0;   // tempos absolutos ainda não suportados: age como poll
}

std::string KReadGuestAnsi(uint32_t ansiStringPtr)
{
    if (ansiStringPtr == 0)
        return {};
    const uint16_t length = R16(ansiStringPtr);
    const uint32_t buffer = R32(ansiStringPtr + 4);
    if (buffer == 0)
        return {};
    return std::string((const char*)(g_mem.base + buffer), length);
}

uint32_t GuestAllocSmall(uint32_t size)
{
    static std::mutex m;
    static uint32_t cur = 0, end = 0;
    std::lock_guard<std::mutex> lock(m);
    size = (size + 0x3F) & ~0x3Fu;
    if (cur == 0 || cur + size > end)
    {
        cur = g_poolHeap.Alloc(0x100000, 0x1000);
        end = cur + 0x100000;
    }
    const uint32_t r = cur;
    cur += size;
    return r;
}
