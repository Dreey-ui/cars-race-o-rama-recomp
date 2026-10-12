// Profiler for the game's D3D functions (hooks generated in d3d_profile_gen.cpp).
// With --profile, counts calls and external callers and writes d3d_profile.txt every 10 s.
// Used to identify Draw*/SetTexture/SetShader/Present in the symbol-less binary.
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

extern const uint32_t g_d3dProfCount;
extern const uint32_t g_d3dProfAddrs[];
uint64_t GpuSwapCount();

static bool g_profiling = false;
static std::atomic<uint64_t> g_calls[4096];
static std::mutex g_callerMutex;
static std::unordered_map<uint64_t, uint64_t> g_callers;   // (index << 32 | lr) -> calls

static uint32_t D3DLo() { return g_d3dProfAddrs[0]; }
static uint32_t D3DHi() { return g_d3dProfAddrs[g_d3dProfCount - 1] + 0x1000; }

// Candidate functions whose arguments are logged on the first calls
// (sampled on the 1st, 2nd, 50th, 500th... call, to catch different game phases)
static const uint32_t g_logAddrs[] = { 0x822CFAD0 };
static std::atomic<int> g_logged[sizeof(g_logAddrs) / sizeof(g_logAddrs[0])];
static bool Sampled(int n) { return n < 2 || n == 50 || n == 500 || n == 2000 || n == 5000 || n == 10000 || n == 20000; }

static void LogArgs(uint32_t addr, PPCContext& ctx)
{
    for (size_t k = 0; k < sizeof(g_logAddrs) / sizeof(g_logAddrs[0]); k++)
    {
        if (g_logAddrs[k] != addr)
            continue;
        if (!Sampled(g_logged[k]++))
            return;
        const uint32_t sp = ctx.r1.u32;
        fprintf(stderr, "[args] %08X lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X"
                        " | stack %08X %08X %08X | f1=%g f2=%g\n",
                addr, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                ctx.r9.u32, ctx.r10.u32, R32(sp + 0x54), R32(sp + 0x5C), R32(sp + 0x64), ctx.f1.f64, ctx.f2.f64);
        return;
    }
}

void D3DProfHit(uint32_t index, PPCContext& ctx)
{
    if (!g_profiling)
        return;
    const uint32_t lr = uint32_t(ctx.lr);
    g_calls[index].fetch_add(1, std::memory_order_relaxed);
    LogArgs(g_d3dProfAddrs[index], ctx);
    if (lr < D3DLo() || lr >= D3DHi())
    {
        std::lock_guard<std::mutex> lock(g_callerMutex);
        g_callers[(uint64_t(index) << 32) | lr]++;
    }
}

// Names already known (from the imports they call)
static const char* KnownName(uint32_t addr)
{
    switch (addr)
    {
    case 0x822BC0F8: return "create device / VdInitializeRingBuffer";
    case 0x822BBF50: return "GPU interrupt callback";
    case 0x822C4DF0: return "Swap (VdGetSystemCommandBuffer + VdSwap)";
    case 0x822CA610: return "VdInitializeEngines";
    case 0x822D22B0: return "VdRetrainEDRAM";
    default: return "";
    }
}

static void WriteReport(uint64_t frames, const std::vector<uint64_t>& delta)
{
    FILE* f = fopen("d3d_profile.txt", "w");
    if (!f)
        return;

    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint64_t>>> callers;
    {
        std::lock_guard<std::mutex> lock(g_callerMutex);
        for (auto& [key, n] : g_callers)
            callers[uint32_t(key >> 32)].push_back({ uint32_t(key), n });
    }

    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < g_d3dProfCount; i++)
        if (g_calls[i].load())
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return delta[a] != delta[b] ? delta[a] > delta[b] : g_calls[a].load() > g_calls[b].load();
    });

    fprintf(f, "frames (VdSwap) total: %llu, in this 10 s window: %llu\n", (unsigned long long)GpuSwapCount(), (unsigned long long)frames);
    fprintf(f, "functions called: %zu of %u\n\n", order.size(), g_d3dProfCount);
    fprintf(f, "%-10s %12s %10s  %s\n", "function", "total", "per frame", "external callers (lr: times)");
    for (uint32_t i : order)
    {
        const double perFrame = frames ? double(delta[i]) / double(frames) : 0.0;
        fprintf(f, "%08X   %12llu %10.2f ", g_d3dProfAddrs[i], (unsigned long long)g_calls[i].load(), perFrame);
        auto& c = callers[i];
        std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.second > b.second; });
        fprintf(f, " [%zu]", c.size());
        for (size_t k = 0; k < c.size() && k < 5; k++)
            fprintf(f, " %08X:%llu", c[k].first, (unsigned long long)c[k].second);
        if (*KnownName(g_d3dProfAddrs[i]))
            fprintf(f, "   <- %s", KnownName(g_d3dProfAddrs[i]));
        fprintf(f, "\n");
    }
    fclose(f);
}

static void ReportThread()
{
    std::vector<uint64_t> last(g_d3dProfCount, 0), delta(g_d3dProfCount, 0);
    uint64_t lastFrames = 0;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        for (uint32_t i = 0; i < g_d3dProfCount; i++)
        {
            const uint64_t n = g_calls[i].load();
            delta[i] = n - last[i];
            last[i] = n;
        }
        const uint64_t frames = GpuSwapCount();
        WriteReport(frames - lastFrames, delta);
        lastFrames = frames;
        fprintf(stderr, "[profile] d3d_profile.txt updated (%llu frames)\n", (unsigned long long)frames);
    }
}

void D3DProfileStart()
{
    g_profiling = true;
    std::thread(ReportThread).detach();
}
