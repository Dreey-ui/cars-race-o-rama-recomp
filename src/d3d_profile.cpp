// Profiler das funcoes do D3D do jogo (hooks gerados em d3d_profile_gen.cpp).
// Com --profile, conta chamadas e chamadores externos e grava d3d_profile.txt a cada 10 s.
// Serve para identificar Draw*/SetTexture/SetShader/Present no binario sem simbolos.
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
static std::unordered_map<uint64_t, uint64_t> g_callers;   // (indice << 32 | lr) -> chamadas

static uint32_t D3DLo() { return g_d3dProfAddrs[0]; }
static uint32_t D3DHi() { return g_d3dProfAddrs[g_d3dProfCount - 1] + 0x1000; }

// Funcoes candidatas cujos argumentos sao registrados nas primeiras chamadas
// (amostradas na 1a, 2a, 50a, 500a... chamada, para pegar fases diferentes do jogo)
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
                        " | pilha %08X %08X %08X | f1=%g f2=%g\n",
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

// Nomes ja conhecidos (pelos imports que chamam)
static const char* KnownName(uint32_t addr)
{
    switch (addr)
    {
    case 0x822BC0F8: return "cria device / VdInitializeRingBuffer";
    case 0x822BBF50: return "callback de interrupcao da GPU";
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

    fprintf(f, "frames (VdSwap) total: %llu, nesta janela de 10 s: %llu\n", (unsigned long long)GpuSwapCount(), (unsigned long long)frames);
    fprintf(f, "funcoes chamadas: %zu de %u\n\n", order.size(), g_d3dProfCount);
    fprintf(f, "%-10s %12s %10s  %s\n", "funcao", "total", "por frame", "chamadores externos (lr: vezes)");
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
        fprintf(stderr, "[profile] d3d_profile.txt atualizado (%llu frames)\n", (unsigned long long)frames);
    }
}

void D3DProfileStart()
{
    g_profiling = true;
    std::thread(ReportThread).detach();
}
