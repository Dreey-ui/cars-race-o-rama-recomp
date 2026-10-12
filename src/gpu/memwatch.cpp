// Vigilancia de escrita em paginas da memoria fisica guest (veja memwatch.h)
#include "memwatch.h"
#include "runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace
{
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPages = 0x20000000u >> kPageShift;   // 512 MB fisicos
constexpr uint32_t kViews[3] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };
// Escrita de novo em menos de kDynamicWindow frames -> dinamica por kDynamicHold frames
constexpr uint32_t kDynamicWindow = 30, kDynamicHold = 600;

std::atomic<uint64_t> g_clean[kPages / 64];   // bit = pagina vigiada e sem escrita
uint32_t g_lastFault[kPages];
uint32_t g_dynamicUntil[kPages];
std::atomic<uint32_t> g_frame{ 0 };
std::atomic<uint32_t> g_epoch{ 1 };           // muda a cada escrita detectada (checagem rapida dos caches)
std::atomic<uint32_t> g_faults{ 0 };
std::mutex g_mutex;   // mudancas de protecao

void SetProtection(uint32_t firstPage, uint32_t count, DWORD prot)
{
    for (uint32_t view : kViews)
    {
        DWORD old;
        VirtualProtect(g_mem.base + view + (size_t(firstPage) << kPageShift), size_t(count) << kPageShift, prot, &old);
    }
}

bool IsCleanPage(uint32_t p) { return (g_clean[p >> 6].load(std::memory_order_acquire) >> (p & 63)) & 1; }
void SetClean(uint32_t p, bool on)
{
    if (on)
        g_clean[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_release);
    else
        g_clean[p >> 6].fetch_and(~(1ull << (p & 63)), std::memory_order_release);
}

bool InRange(uint32_t phys, uint32_t size) { return size && phys < 0x20000000u && size <= 0x20000000u - phys; }
}  // namespace

void MemWatchSetFrame(uint64_t frame) { g_frame = uint32_t(frame); }
uint32_t MemWatchTakeFaults() { return g_faults.exchange(0); }
uint32_t MemWatchEpoch() { return g_epoch.load(std::memory_order_acquire); }

bool MemWatchIsClean(uint32_t phys, uint32_t size)
{
    if (!InRange(phys, size))
        return false;
    uint32_t p = phys >> kPageShift;
    const uint32_t e = (phys + size - 1) >> kPageShift;
    while (p <= e)
    {
        const uint32_t bit = p & 63, n = std::min<uint32_t>(64 - bit, e - p + 1);
        const uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit;
        if ((g_clean[p >> 6].load(std::memory_order_acquire) & mask) != mask)
            return false;
        p += n;
    }
    return true;
}

bool MemWatchIsDynamic(uint32_t phys, uint32_t size)
{
    if (!InRange(phys, size))
        return true;
    const uint32_t now = g_frame.load();
    for (uint32_t p = phys >> kPageShift, e = (phys + size - 1) >> kPageShift; p <= e; p++)
        if (g_dynamicUntil[p] > now)
            return true;
    return false;
}

bool MemWatchProtect(uint32_t phys, uint32_t size)
{
    if (!InRange(phys, size) || MemWatchIsDynamic(phys, size))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t first = phys >> kPageShift, last = (phys + size - 1) >> kPageShift;
    // protege so as sequencias de paginas ainda nao vigiadas
    uint32_t runStart = UINT32_MAX;
    for (uint32_t p = first; p <= last + 1; p++)
    {
        const bool need = p <= last && !IsCleanPage(p);
        if (need && runStart == UINT32_MAX)
            runStart = p;
        if (!need && runStart != UINT32_MAX)
        {
            SetProtection(runStart, p - runStart, PAGE_READONLY);
            for (uint32_t q = runStart; q < p; q++)
                SetClean(q, true);
            runStart = UINT32_MAX;
        }
    }
    return true;
}

bool MemWatchHandleWriteFault(uint32_t guestAddr)
{
    if (guestAddr < 0xA0000000u)
        return false;
    const uint32_t page = (guestAddr & 0x1FFFFFFFu) >> kPageShift;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!IsCleanPage(page))
    {
        // outra thread ja liberou a pagina entre a excecao e o lock: so repete a instrucao
        MEMORY_BASIC_INFORMATION mbi;
        return VirtualQuery(g_mem.base + guestAddr, &mbi, sizeof(mbi)) && (mbi.Protect & PAGE_READWRITE);
    }
    SetProtection(page, 1, PAGE_READWRITE);
    SetClean(page, false);
    g_epoch++;
    const uint32_t now = g_frame.load();
    if (now - g_lastFault[page] < kDynamicWindow)
        g_dynamicUntil[page] = now + kDynamicHold;
    g_lastFault[page] = now;
    g_faults++;
    return true;
}

void MemWatchInvalidate(uint32_t guestAddr, uint32_t size)
{
    if (guestAddr < 0xA0000000u || !size)
        return;
    const uint32_t phys = guestAddr & 0x1FFFFFFFu;
    if (!InRange(phys, size))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    bool any = false;
    for (uint32_t p = phys >> kPageShift, e = (phys + size - 1) >> kPageShift; p <= e; p++)
        if (IsCleanPage(p))
        {
            SetProtection(p, 1, PAGE_READWRITE);
            SetClean(p, false);
            any = true;
        }
    if (any)
        g_epoch++;
}
