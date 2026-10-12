// GPU: vblank interrupt (60 Hz) + PM4 command processor (Xenia semantics).
// Consumes the ring buffer, keeps the Xenos register file, performs memory writes/fences
// and hands shaders, draws, resolves and swaps to the D3D12 backend (gpu/renderer.cpp).
#include "kernel.h"
#include "gpu/renderer.h"
#include "gpu/xenos_regs.h"

#include <atomic>
#include <chrono>

#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

static std::atomic<uint32_t> g_gfxCallback{ 0 };
static std::atomic<uint32_t> g_gfxContext{ 0 };
static std::atomic<bool> g_vblankStarted{ false };
static std::recursive_mutex g_callbackMutex;   // the interrupt callback never runs on two threads at once (reentrant: GpuImmediateVblank)

// GPU registers are mapped at 0x7FC80000 (index * 4)
constexpr uint32_t GPU_MMIO_BASE = 0x7FC80000;
constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;

static uint32_t g_regs[xreg::kRegisterCount];
static uint32_t g_rbBase = 0;          // guest (virtual) address of the ring buffer
static uint32_t g_rbSizeDwords = 0;
static uint32_t g_rptr = 0;
static std::atomic<uint32_t> g_rptrWriteback{ 0 };
static std::atomic<bool> g_cpStarted{ false };
static std::atomic<uint64_t> g_swapCount{ 0 };
static uint32_t g_swapCounter = 0;     // GPU counter (EVENT_WRITE_SHD with bit 31)
static std::atomic<uint64_t> g_cpSwapsSeen{ 0 }, g_cpBatches{ 0 };
static std::atomic<uint32_t> g_cpLastOpcode{ 0 }, g_cpLastAddr{ 0 }, g_cpStage{ 0 };   // hang diagnostics
static std::atomic<uint64_t> g_cpLoops{ 0 };
static uint64_t g_binMask = ~0ull, g_binSelect = ~0ull;   // packet predication (tiling)
static uint32_t ReadWptr();

uint64_t GpuSwapCount() { return g_swapCount.load(); }
uint32_t* GpuRegisters() { return g_regs; }

static uint32_t PhysToGuest(uint32_t p) { return p >= 0xA0000000 ? p : 0xA0000000 | (p & 0x1FFFFFFF); }

// Display/EDRAM registers that D3D reads via MMIO (values from Xenia's GraphicsSystem::ReadRegister)
static void SetMmioDisplayRegisters()
{
    W32(GPU_MMIO_BASE + 0x0F00 * 4, 0x08100748);   // RB_EDRAM_TIMING
    W32(GPU_MMIO_BASE + 0x0F01 * 4, 0x0000200E);   // RB_BC_CONTROL
    W32(GPU_MMIO_BASE + 0x0F2D * 4, 0x00BBBBBB);   // RB_HSIO_INTERFACE_ALIGNER_VALUE
    W32(GPU_MMIO_BASE + 0x1951 * 4, 1);            // interrupt status: vblank
    W32(GPU_MMIO_BASE + 0x1961 * 4, 0x050002D0);   // AVIVO_D1MODE_VIEWPORT_SIZE 1280x720
}

static void CallInterrupt(PPCContext& ctx, uint32_t source, uint32_t cpu = 2)
{
    const uint32_t cb = g_gfxCallback.load();
    if (!cb)
        return;
    std::lock_guard<std::recursive_mutex> lock(g_callbackMutex);
    SetMmioDisplayRegisters();
    W8(ctx.r13.u32 + 0x10C, uint8_t(cpu));   // PCR: current processor
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_gfxContext.load();
    try { CallGuest(ctx, g_mem.base, cb); } catch (const GuestThreadExit&) {}
}

// Immediate vblank (60 fps mode): called from inside the swap interrupt callback (sub_822C4708, GPU thread) so the
// queued flip happens right away, without the GPU waiting for the next 60 Hz vblank; pacing is left to the host vsync.
// Uses a copy of the context and preserves the processor in the PCR (the outer callback uses that number right after).
void GpuImmediateVblank(const PPCContext& caller)
{
    PPCContext tmp = caller;
    const uint8_t cpu = *(g_mem.base + caller.r13.u32 + 0x10C);
    CallInterrupt(tmp, 0, cpu);
    W8(caller.r13.u32 + 0x10C, cpu);
}

static void VBlankThread()
{
    SetThreadDescription(GetCurrentThread(), L"vblank");
    GuestThread* gt = KSetupGuestThread(0x40000);
    auto ctx = std::make_unique<PPCContext>();
    KInitContext(*ctx, gt);
    KSetCurrentThread(gt);
    auto next = std::chrono::steady_clock::now();
    // CARS_VBLANK_HZ: frequency of the emulated vblank (diagnostics; default 60 Hz)
    const int hz = getenv("CARS_VBLANK_HZ") ? std::max(1, atoi(getenv("CARS_VBLANK_HZ"))) : 60;
    const auto period = std::chrono::microseconds(1000000 / hz);
    for (;;)
    {
        next += period;
        std::this_thread::sleep_until(next);
        CallInterrupt(*ctx, 0);   // source 0 = vblank
    }
}

// ---------------------------------------------------------------------------------------------
// PM4 command processor

static uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(value);
    case 3: return (value << 16) | (value >> 16);
    default: return value;
    }
}

// The 2 low bits of the address give the swap (Xenia: GpuSwap + little-endian store)
static void WriteWithEndian(uint32_t addr, uint32_t value)
{
    *(uint32_t*)(g_mem.base + PhysToGuest(addr & ~3u)) = GpuSwap(value, addr & 3);
}

static uint32_t ReadWithEndian(uint32_t addr)
{
    return GpuSwap(*(uint32_t*)(g_mem.base + PhysToGuest(addr & ~3u)), addr & 3);
}

static bool Compare(uint32_t func, uint32_t value, uint32_t ref)
{
    switch (func & 7)
    {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

// ALU constant banks written since the last draw (bit 0: VS c0..c255, bit 1: PS): the renderer only re-copies those
uint32_t g_aluDirty = 3;

static void WriteRegister(uint32_t index, uint32_t value)
{
    if (index >= xreg::kRegisterCount)
        return;
    g_regs[index] = value;
    if (const uint32_t alu = index - xreg::ALU_CONSTANTS; alu < 2048)
        g_aluDirty |= alu < 1024 ? 1u : 2u;
    else if (index - xreg::SCRATCH_REG0 < 8)
    {
        // Writeback of the scratch registers to memory (used by D3D for fences)
        const uint32_t n = index - xreg::SCRATCH_REG0;
        if ((1u << n) & g_regs[xreg::SCRATCH_UMSK])
            W32(PhysToGuest(g_regs[xreg::SCRATCH_ADDR] + n * 4), value);
    }
    else if (index == xreg::COHER_STATUS_HOST)
    {
        g_regs[index] = 0;   // "immediate" coherency
    }
}

struct CmdReader
{
    uint32_t base;      // guest address
    uint32_t sizeDw;    // size in dwords
    uint32_t pos;       // in dwords
    bool ring;          // ring buffer (with wrap) or indirect buffer
    uint32_t Next()
    {
        const uint32_t v = R32(base + pos * 4);
        pos = ring ? (pos + 1) % sizeDw : pos + 1;
        return v;
    }
    uint32_t Address() const { return base + pos * 4; }
    void Skip(uint32_t n) { pos = ring ? (pos + n) % sizeDw : pos + n; }
};

static void ExecuteCommands(CmdReader& rd, uint32_t endPos, PPCContext& ctx, int depth);

// Busy time of the command processor and time waiting for the CPU (WAIT_REG_MEM), for the statistics
static std::atomic<uint64_t> g_cpBusyUs{ 0 }, g_cpWaitUs{ 0 };
uint64_t GpuTakeCpBusyUs() { return g_cpBusyUs.exchange(0); }
uint64_t GpuTakeCpWaitUs() { return g_cpWaitUs.exchange(0); }

static void ExecuteDraw(CmdReader& rd, uint32_t remaining, uint32_t& used)
{
    if (remaining == 0)
        return;
    const uint32_t initiator = rd.Next();
    used++;
    g_regs[xreg::VGT_DRAW_INITIATOR] = initiator;
    DrawCall d{};
    d.primType = initiator & 0x3F;
    const uint32_t source = (initiator >> 6) & 3;
    d.index32 = (initiator >> 11) & 1;
    d.numIndices = initiator >> 16;
    if (source == 0)   // DMA: indices in memory
    {
        if (remaining < 3)
            return;
        d.indexed = true;
        d.indexAddress = rd.Next() & 0x1FFFFFFF;
        const uint32_t dmaSize = rd.Next();
        used += 2;
        d.indexEndian = dmaSize >> 30;
        g_regs[xreg::VGT_DMA_BASE] = d.indexAddress;
        g_regs[xreg::VGT_DMA_SIZE] = dmaSize;
    }
    else if (source == 1)
    {
        return;   // immediate indices: not used by the 360's D3D9
    }
    RendererDraw(d);
}

static void ExecuteType3(CmdReader& rd, uint32_t header, PPCContext& ctx, int depth)
{
    const uint32_t opcode = (header >> 8) & 0x7F;
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    uint32_t used = 0;
    auto next = [&] { used++; return rd.Next(); };
    g_cpLastOpcode = opcode | (uint32_t(depth) << 8);
    g_cpLastAddr = rd.Address();
    g_cpStage = 1;

    // Predication (D3D predicated tiling): a packet with bit 0 only executes if the current bin is selected
    if ((header & 1) && ((g_binSelect & g_binMask) == 0 || opcode == 0x64))
    {
        rd.Skip(count);
        g_cpStage = 2;
        return;
    }

    switch (opcode)
    {
    case 0x37:   // INDIRECT_BUFFER_PFD
    case 0x3F:   // INDIRECT_BUFFER
    {
        const uint32_t addr = next();
        const uint32_t size = next() & 0xFFFFF;
        if (depth < 4 && size)
        {
            CmdReader ib{ PhysToGuest(addr), size, 0, false };
            ExecuteCommands(ib, size, ctx, depth + 1);
        }
        break;
    }
    case 0x3D:   // MEM_WRITE
    {
        uint32_t addr = next();
        for (uint32_t i = 1; i < count; i++, addr += 4)
            WriteWithEndian(addr, next());
        break;
    }
    case 0x3E:   // REG_TO_MEM
    {
        const uint32_t reg = next();
        const uint32_t addr = next();
        WriteWithEndian(addr, reg < xreg::kRegisterCount ? g_regs[reg] : 0);
        break;
    }
    case 0x58:   // EVENT_WRITE_SHD
    {
        const uint32_t initiator = next();
        const uint32_t addr = next();
        uint32_t value = next();
        if (initiator >> 31)
            value = g_swapCounter;
        WriteWithEndian(addr, value);
        break;
    }
    case 0x5A:   // EVENT_WRITE_EXT (screen extents)
    {
        next();
        const uint32_t addr = next();
        const uint16_t ext[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
        const uint32_t a = PhysToGuest(addr & ~3u);
        for (int i = 0; i < 6; i++)
            W16(a + i * 2, ext[i]);
        break;
    }
    case 0x45:   // COND_WRITE
    {
        const uint32_t info = next();
        const uint32_t pollAddr = next();
        const uint32_t ref = next();
        const uint32_t mask = next();
        const uint32_t writeAddr = next();
        const uint32_t data = next();
        const uint32_t value = (info & 0x10) ? ReadWithEndian(pollAddr) : g_regs[pollAddr & 0x7FFF];
        if (Compare(info, value & mask, ref))
        {
            if (info & 0x100)
                WriteWithEndian(writeAddr, data);
            else
                WriteRegister(writeAddr & 0x7FFF, data);
        }
        break;
    }
    case 0x3C:   // WAIT_REG_MEM
    {
        const uint32_t info = next();
        const uint32_t pollAddr = next();
        const uint32_t ref = next();
        const uint32_t mask = next();
        next();  // interval
        static const bool logWaits = getenv("CARS_LOG_WAITS") != nullptr;
        static int waitLogs = 0;
        if (logWaits && waitLogs < 60)
        {
            waitLogs++;
            fprintf(stderr, "[wait] swap %llu: %s %08X func %u ref %08X mask %08X current value %08X\n",
                    (unsigned long long)g_swapCount.load(), (info & 0x10) ? "mem" : "reg", pollAddr, info & 7, ref, mask,
                    (info & 0x10) ? ReadWithEndian(pollAddr) : g_regs[pollAddr & 0x7FFF]);
        }
        if (info & 0x10)
        {
            // memory wait: the CPU writes the value; wait a bit and move on (avoids deadlock)
            const auto waitStart = std::chrono::steady_clock::now();
            const auto limit = waitStart + std::chrono::milliseconds(100);
            const uint32_t before = ReadWithEndian(pollAddr);
            while (!Compare(info, ReadWithEndian(pollAddr) & mask, ref) && std::chrono::steady_clock::now() < limit)
                std::this_thread::yield();
            const uint64_t waitedUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count());
            g_cpWaitUs += waitedUs;
            // CARS_LOG_SLOW_WAITS=N: logs waits longer than 1 ms starting at swap N
            static const uint64_t slowFrom = getenv("CARS_LOG_SLOW_WAITS") ? strtoull(getenv("CARS_LOG_SLOW_WAITS"), nullptr, 10) : 0;
            static int slowLogs = 0;
            if (slowFrom && g_swapCount >= slowFrom && waitedUs > 1000 && slowLogs < 80)
            {
                slowLogs++;
                fprintf(stderr, "[slow-wait] swap %llu: mem %08X func %u ref %08X mask %08X before %08X after %08X: %.2f ms\n",
                        (unsigned long long)g_swapCount.load(), pollAddr, info & 7, ref, mask, before, ReadWithEndian(pollAddr),
                        waitedUs / 1000.0);
            }
            static int timeouts = 0;
            if (!Compare(info, ReadWithEndian(pollAddr) & mask, ref) && timeouts++ < 8)
                fprintf(stderr, "[cp] WAIT_REG_MEM timed out: mem %08X value %08X mask %08X func %u ref %08X\n", pollAddr,
                        ReadWithEndian(pollAddr), mask, info & 7, ref);
        }
        break;
    }
    case 0x21:   // REG_RMW
    {
        const uint32_t info = next();
        const uint32_t andv = next();
        const uint32_t orv = next();
        const uint32_t reg = info & 0x1FFF;
        uint32_t v = g_regs[reg];
        v &= (info & 0x80000000) ? g_regs[andv & 0x1FFF] : andv;
        v |= (info & 0x40000000) ? g_regs[orv & 0x1FFF] : orv;
        WriteRegister(reg, v);
        break;
    }
    case 0x50:   // SET_BIN_MASK (hi, lo)
    {
        const uint64_t hi = next(), lo = next();
        g_binMask = (hi << 32) | lo;
        break;
    }
    case 0x51:   // SET_BIN_SELECT (hi, lo)
    {
        const uint64_t hi = next(), lo = next();
        g_binSelect = (hi << 32) | lo;
        break;
    }
    case 0x60: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | next(); break;
    case 0x61: g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(next()) << 32); break;
    case 0x62: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | next(); break;
    case 0x63: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(next()) << 32); break;
    case 0x54:   // INTERRUPT
    {
        const uint32_t cpuMask = next();
        for (uint32_t n = 0; n < 6; n++)
            if (cpuMask & (1u << n))
                CallInterrupt(ctx, 1, n);
        break;
    }

    // ---- constants and state ----
    case 0x2D:   // SET_CONSTANT
    {
        const uint32_t offsetType = next();
        const uint32_t index = offsetType & 0x7FF;
        static const uint32_t kBases[5] = { xreg::ALU_CONSTANTS, xreg::FETCH_CONSTANTS, xreg::BOOL_CONSTANTS, xreg::LOOP_CONSTANTS, 0x2000 };
        const uint32_t type = (offsetType >> 16) & 0xFF;
        if (type < 5)
            for (uint32_t i = 1; i < count; i++)
                WriteRegister(kBases[type] + index + i - 1, next());
        break;
    }
    case 0x55:   // SET_CONSTANT2
    case 0x56:   // SET_SHADER_CONSTANTS
    {
        const uint32_t index = next() & 0xFFFF;
        for (uint32_t i = 1; i < count; i++)
            WriteRegister(index + i - 1, next());
        break;
    }
    case 0x2F:   // LOAD_ALU_CONSTANT
    {
        const uint32_t addr = next() & 0x3FFFFFFF;
        const uint32_t offsetType = next();
        const uint32_t sizeDwords = next() & 0xFFF;
        static const uint32_t kBases[5] = { xreg::ALU_CONSTANTS, xreg::FETCH_CONSTANTS, xreg::BOOL_CONSTANTS, xreg::LOOP_CONSTANTS, 0x2000 };
        const uint32_t type = (offsetType >> 16) & 0xFF;
        const uint32_t index = offsetType & 0x7FF;
        if (type < 5)
        {
            const uint32_t src = PhysToGuest(addr);
            for (uint32_t i = 0; i < sizeDwords; i++)
                WriteRegister(kBases[type] + index + i, R32(src + i * 4));
        }
        break;
    }
    case 0x27:   // IM_LOAD
    {
        const uint32_t addrType = next();
        const uint32_t startSize = next();
        const uint32_t sizeDwords = startSize & 0xFFFF;
        RendererSetShader((addrType & 3) == 1, g_mem.base + PhysToGuest(addrType & ~3u), sizeDwords);
        break;
    }
    case 0x2B:   // IM_LOAD_IMMEDIATE
    {
        const uint32_t type = next();
        const uint32_t startSize = next();
        const uint32_t sizeDwords = std::min(startSize & 0xFFFF, count - 2);
        if (rd.ring && rd.pos + sizeDwords > rd.sizeDw)
        {
            // microcode crossing the end of the ring: linear copy
            std::vector<uint32_t> tmp(sizeDwords);
            for (uint32_t i = 0; i < sizeDwords; i++)
                tmp[i] = __builtin_bswap32(next());
            RendererSetShader((type & 3) == 1, reinterpret_cast<const uint8_t*>(tmp.data()), sizeDwords);
        }
        else
        {
            RendererSetShader((type & 3) == 1, g_mem.base + rd.Address(), sizeDwords);
        }
        break;
    }
    case 0x22:   // DRAW_INDX
        next();  // viz query token
        ExecuteDraw(rd, count - used, used);
        break;
    case 0x36:   // DRAW_INDX_2
        ExecuteDraw(rd, count - used, used);
        break;
    case 0x64:   // XE_SWAP (written by our VdSwap)
    {
        const uint32_t magic = next();
        const uint32_t fb = next();
        const uint32_t w = next();
        const uint32_t h = next();
        if (magic == 0x53574150)   // 'SWAP'
        {
            g_cpSwapsSeen++;
            RendererSwap(fb, w, h);
            g_swapCounter++;
        }
        break;
    }
    default:
        break;
    }
    g_cpStage = 2;
    if (used < count)
        rd.Skip(count - used);
}

static void ExecuteCommands(CmdReader& rd, uint32_t endPos, PPCContext& ctx, int depth)
{
    while (rd.pos != endPos)
    {
        if (!rd.ring && rd.pos >= endPos)
            break;
        const uint32_t header = rd.Next();
        switch (header >> 30)
        {
        case 0:
        {
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t reg = header & 0x7FFF;
            const bool oneReg = (header >> 15) & 1;
            for (uint32_t i = 0; i < count; i++)
                WriteRegister(oneReg ? reg : reg + i, rd.Next());
            break;
        }
        case 1:
        {
            const uint32_t a = rd.Next(), b = rd.Next();
            WriteRegister(header & 0x7FF, a);
            WriteRegister((header >> 11) & 0x7FF, b);
            break;
        }
        case 2:
            break;
        case 3:
            ExecuteType3(rd, header, ctx, depth);
            break;
        }
    }
}

static uint32_t ReadWptr()
{
    // D3D writes the register byte-swapped or not; accept whichever fits in the ring
    const uint32_t raw = *(uint32_t*)(g_mem.base + GPU_MMIO_BASE + REG_CP_RB_WPTR * 4);
    const uint32_t swapped = __builtin_bswap32(raw);
    if (swapped < g_rbSizeDwords) return swapped;
    if (raw < g_rbSizeDwords) return raw;
    return g_rptr;
}


static void CommandProcessorThread()
{
    GuestThread* gt = KSetupGuestThread(0x40000);
    auto ctx = std::make_unique<PPCContext>();
    KInitContext(*ctx, gt);
    KSetCurrentThread(gt);
    extern void HostProfilerRegisterCurrentThread(const char* name);
    HostProfilerRegisterCurrentThread("gpu cp");
    // Low latency: the game's D3D waits for GPU fences in sequence (texture loading etc.).
    // After recent work, spin with yield for ~4 ms before going back to sleep.
    timeBeginPeriod(1);
    auto lastWork = std::chrono::steady_clock::now();
    for (;;)
    {
        g_cpLoops++;
        const uint32_t wptr = ReadWptr();
        if (wptr == g_rptr)
        {
            if (std::chrono::steady_clock::now() - lastWork < std::chrono::milliseconds(4))
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        lastWork = std::chrono::steady_clock::now();
        CmdReader rd{ g_rbBase, g_rbSizeDwords, g_rptr, true };
        ExecuteCommands(rd, wptr, *ctx, 0);
        g_cpBusyUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lastWork).count());
        g_cpBatches++;
        g_rptr = wptr;
        W32(GPU_MMIO_BASE + REG_CP_RB_RPTR * 4, g_rptr);
        if (const uint32_t wb = g_rptrWriteback.load())
            W32(wb, g_rptr);
    }
}

// ---------------------------------------------------------------------------------------------
// Imports Vd*

KERNEL_IMPORT(VdSetGraphicsInterruptCallback)
{
    g_gfxCallback = ctx.r3.u32;
    g_gfxContext = ctx.r4.u32;
    if (g_traceImports)
        fprintf(stderr, "[gpu] interrupt callback = %08X, context = %08X\n", ctx.r3.u32, ctx.r4.u32);
    if (ctx.r3.u32 && !g_vblankStarted.exchange(true))
        std::thread(VBlankThread).detach();
}

KERNEL_IMPORT(VdInitializeEngines)
{
    g_mem.Commit(GPU_MMIO_BASE, 0x10000);
    SetMmioDisplayRegisters();
    ctx.r3.u64 = 1;
}

KERNEL_IMPORT(VdInitializeRingBuffer)
{
    g_mem.Commit(GPU_MMIO_BASE, 0x10000);
    g_rbBase = PhysToGuest(ctx.r3.u32);
    g_rbSizeDwords = (1u << (ctx.r4.u32 + 3)) / 4;
    g_rptr = 0;
    if (g_traceImports)
        fprintf(stderr, "[gpu] ring buffer at %08X, %u dwords\n", g_rbBase, g_rbSizeDwords);
    if (!g_cpStarted.exchange(true))
        std::thread(CommandProcessorThread).detach();
}

KERNEL_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    g_rptrWriteback = PhysToGuest(ctx.r3.u32);
    if (g_traceImports)
        fprintf(stderr, "[gpu] RPTR writeback at %08X\n", g_rptrWriteback.load());
}

KERNEL_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// If it returns 0, D3D enters a degraded mode ("HSIO training failed") and stops kicking the ring buffer
KERNEL_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
KERNEL_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
KERNEL_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
KERNEL_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
KERNEL_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
KERNEL_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 1 | 2; }   // widescreen, >= 1280 wide

KERNEL_IMPORT(VdGetSystemCommandBuffer)
{
    if (ctx.r3.u32)
    {
        memset(g_mem.base + ctx.r3.u32, 0, 0x94);
        W32(ctx.r3.u32, 0xBEEF0000);
    }
    if (ctx.r4.u32)
        W32(ctx.r4.u32, 0xBEEF0001);
}

KERNEL_IMPORT(VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32) W32(ctx.r3.u32, 2);   // TV (BT.709)
    if (ctx.r4.u32) { float g = 2.22222f; uint32_t v; memcpy(&v, &g, 4); W32(ctx.r4.u32, v); }
}

KERNEL_IMPORT(VdGetCurrentDisplayInformation)
{
    const uint32_t p = ctx.r3.u32;
    if (!p)
        return;
    memset(g_mem.base + p, 0, 0x58);
    W16(p + 0x00, 1280);
    W16(p + 0x02, 720);
    W32(p + 0x08 + 0x08, 1280);   // scaler_source_rect.x2
    W32(p + 0x08 + 0x0C, 720);    // scaler_source_rect.y2
    W32(p + 0x08 + 0x10, 1280);   // scaled_output_width
    W32(p + 0x08 + 0x14, 720);
    W32(p + 0x08 + 0x18, 1);      // vertical_filter_type
    W32(p + 0x08 + 0x28, 1);      // horizontal_filter_type
    W16(p + 0x40, 320);
    W16(p + 0x42, 180);
    W16(p + 0x44, 320);
    W16(p + 0x46, 180);
    W16(p + 0x48, 1280);
    W16(p + 0x4A, 720);
    float hz = 60.0f; uint32_t v; memcpy(&v, &hz, 4);
    W32(p + 0x4C, v);
    W16(p + 0x56, 1280);
}

// VdPersistDisplay(?, out*): the kernel keeps the current image on screen while the game loads (no swaps). *out receives
// a physical block the game later returns with MmFreePhysicalMemory (like Xenia)
KERNEL_IMPORT(VdPersistDisplay)
{
    static std::atomic<int> n{ 0 };
    if (n++ < 20)
        fprintf(stderr, "[vd] VdPersistDisplay(%08X, %08X) at swap %llu\n", ctx.r3.u32, ctx.r4.u32, (unsigned long long)g_swapCount.load());
    if (ctx.r4.u32)
        W32(ctx.r4.u32, g_physicalHeap.Alloc(64, 32));
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(VdSwap)
{
    // VdSwap(buffer, frontbuffer fetch, ...): D3D reserves 64 dwords in the command buffer for the kernel.
    // We write an XE_SWAP packet (like Xenia) so the command processor presents the frame in the right order.
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t fetch = ctx.r4.u32;
    uint32_t fb = 0, w = 0, h = 0;
    if (fetch)
    {
        fb = (R32(fetch + 4) & 0xFFFFF000) & 0x1FFFFFFF;
        const uint32_t size = R32(fetch + 8);
        w = (size & 0x1FFF) + 1;
        h = ((size >> 13) & 0x1FFF) + 1;
    }
    W32(buffer, 0xC0000000u | (62u << 16) | (0x64u << 8));
    W32(buffer + 4, 0x53574150);   // 'SWAP'
    W32(buffer + 8, fb);
    W32(buffer + 12, w);
    W32(buffer + 16, h);
    const uint64_t n = ++g_swapCount;
    if (n == 1)
        fprintf(stderr, "[gpu] frontbuffer %08X %ux%u\n", fb, w, h);
    if (n % 300 == 0)
        fprintf(stderr, "[cp] VdSwap %llu, XE_SWAP processed %llu, ring batches %llu, rptr %u, last opcode %03X at %08X (stage %u)\n",
                (unsigned long long)n, (unsigned long long)g_cpSwapsSeen.load(), (unsigned long long)g_cpBatches.load(), g_rptr,
                g_cpLastOpcode.load(), g_cpLastAddr.load(), g_cpStage.load());
    if (n % 300 == 0)
        fprintf(stderr, "[cp] current wptr %u, loop iterations %llu\n", ReadWptr(), (unsigned long long)g_cpLoops.load());
    ctx.r3.u64 = 0;
}
