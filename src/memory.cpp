#include "runtime.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

GuestMemory g_mem;
uint8_t* GuestMemoryBase() { return g_mem.base; }
GuestHeap g_virtualHeap;
GuestHeap g_physicalHeap;
GuestHeap g_poolHeap;

static constexpr uint64_t kGuestSize = 0x100000000ull;
static constexpr uint32_t kPage = 0x1000;

static uint32_t AlignUp(uint32_t v, uint32_t a)   { return (v + a - 1) & ~(a - 1); }
static uint32_t AlignDown(uint32_t v, uint32_t a) { return v & ~(a - 1); }

// Xbox 360 physical memory (512 MB) seen through three windows: 0xA0000000 (64 KB pages), 0xC0000000 (16 MB)
// and 0xE0000000 (4 KB). All three are views of the same section, as on the hardware.
static constexpr uint32_t kPhysicalViews[3] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };
static constexpr uint32_t kPhysicalSize = 0x20000000u;

bool GuestMemory::Init()
{
#ifdef _WIN32
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, kPhysicalSize, nullptr);
    if (!section)
        return false;
    for (int attempt = 0; attempt < 16; attempt++)
    {
        // Find 4 free GB, release and remap: low part reserved + 3 views of physical memory
        uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, kGuestSize, MEM_RESERVE, PAGE_READWRITE);
        if (!p)
            return false;
        VirtualFree(p, 0, MEM_RELEASE);
        if (!VirtualAlloc(p, kPhysicalViews[0], MEM_RESERVE, PAGE_READWRITE))
            continue;
        bool ok = true;
        void* views[3] = {};
        for (int i = 0; i < 3 && ok; i++)
        {
            views[i] = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, p + kPhysicalViews[i]);
            ok = views[i] == p + kPhysicalViews[i];
        }
        if (ok)
        {
            base = p;
            return true;
        }
        for (void* v : views)
            if (v)
                UnmapViewOfFile(v);
        VirtualFree(p, 0, MEM_RELEASE);
    }
    return false;
#else
    void* p = mmap(nullptr, kGuestSize, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return false;
    base = (uint8_t*)p;
    return true;
#endif
}

bool GuestMemory::Commit(uint32_t addr, uint32_t size)
{
    if (size == 0)
        return true;
    uint64_t start = AlignDown(addr, kPage);
    uint64_t end = (uint64_t(addr) + size + kPage - 1) & ~uint64_t(kPage - 1);
    if (end > kGuestSize)
        end = kGuestSize;
#ifdef _WIN32
    if (start >= kPhysicalViews[0])
        return true;   // physical memory: already committed (section)
    if (end > kPhysicalViews[0])
        end = kPhysicalViews[0];
    return VirtualAlloc(base + start, end - start, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(base + start, end - start, PROT_READ | PROT_WRITE) == 0;
#endif
}

struct GuestHeap::Impl
{
    std::mutex mutex;
    std::map<uint32_t, uint32_t> freeList;            // start -> size
    std::unordered_map<uint32_t, uint32_t> allocs;    // start -> size
};

void GuestHeap::Init(uint32_t lo, uint32_t hi)
{
    m_lo = lo;
    m_hi = hi;
    m_impl = new Impl();
    m_impl->freeList[lo] = hi - lo;
}

uint32_t GuestHeap::Alloc(uint32_t size, uint32_t alignment, bool zero)
{
    if (size == 0)
        size = 1;
    size = AlignUp(size, kPage);
    if (alignment < kPage)
        alignment = kPage;

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (auto it = m_impl->freeList.begin(); it != m_impl->freeList.end(); ++it)
    {
        const uint32_t start = it->first;
        const uint32_t blockSize = it->second;
        const uint64_t aligned = (uint64_t(start) + alignment - 1) & ~uint64_t(alignment - 1);
        if (aligned + size > uint64_t(start) + blockSize)
            continue;

        const uint32_t a = (uint32_t)aligned;
        const uint32_t tailStart = a + size;
        const uint32_t tailSize = (start + blockSize) - tailStart;
        m_impl->freeList.erase(it);
        if (a > start)
            m_impl->freeList[start] = a - start;
        if (tailSize)
            m_impl->freeList[tailStart] = tailSize;

        m_impl->allocs[a] = size;
        g_mem.Commit(a, size);
        if (zero)
            memset(g_mem.base + a, 0, size);
        return a;
    }
    return 0;
}

bool GuestHeap::AllocFixed(uint32_t addr, uint32_t size)
{
    addr = AlignDown(addr, kPage);
    size = AlignUp(size ? size : 1, kPage);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (auto it = m_impl->freeList.begin(); it != m_impl->freeList.end(); ++it)
    {
        const uint32_t start = it->first;
        const uint32_t blockSize = it->second;
        if (addr >= start && uint64_t(addr) + size <= uint64_t(start) + blockSize)
        {
            const uint32_t tailStart = addr + size;
            const uint32_t tailSize = (start + blockSize) - tailStart;
            m_impl->freeList.erase(it);
            if (addr > start)
                m_impl->freeList[start] = addr - start;
            if (tailSize)
                m_impl->freeList[tailStart] = tailSize;
            m_impl->allocs[addr] = size;
            g_mem.Commit(addr, size);
            return true;
        }
    }
    return false;
}

bool GuestHeap::Free(uint32_t addr, uint32_t* sizeOut)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto it = m_impl->allocs.find(addr);
    if (it == m_impl->allocs.end())
        return false;

    const uint32_t size = it->second;
    m_impl->allocs.erase(it);
    if (sizeOut)
        *sizeOut = size;

    auto next = m_impl->freeList.lower_bound(addr);
    uint32_t start = addr;
    uint32_t total = size;
    if (next != m_impl->freeList.end() && addr + size == next->first)
    {
        total += next->second;
        next = m_impl->freeList.erase(next);
    }
    if (next != m_impl->freeList.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr)
        {
            start = prev->first;
            total += prev->second;
            m_impl->freeList.erase(prev);
        }
    }
    m_impl->freeList[start] = total;
    return true;
}

void GuestHeap::Snapshot(std::vector<std::pair<uint32_t, uint32_t>>& out) const
{
    if (!m_impl)
        return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const size_t first = out.size();
    for (const auto& [a, s] : m_impl->allocs)
        out.emplace_back(a, s);
    std::sort(out.begin() + first, out.end());
}

// All ranges allocated in the guest heaps (GPU shader scan)
void GuestAllocatedRanges(std::vector<std::pair<uint32_t, uint32_t>>& out)
{
    g_virtualHeap.Snapshot(out);
    g_poolHeap.Snapshot(out);
    g_physicalHeap.Snapshot(out);
}
