// Hooks on the game's own functions. CARS_LOG_PAUSE=1 logs who pauses the game (diagnostics for the pause menu that kept
// opening by itself: it was an automatic test Start press, since removed)
#include "runtime.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <string>

// Guest stack of the current thread: LR saved by each frame's prologue (r12 at -8 of the caller's frame)
static void LogGuestBacktrace(PPCContext& ctx)
{
    fprintf(stderr, " lr=%08X |", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 10; i++)
    {
        const uint32_t next = R32(sp);
        if (next <= sp || next - sp > 0x100000)
            break;
        fprintf(stderr, " %08X", R32(next - 8));
        sp = next;
    }
    fprintf(stderr, "\n");
}

// sub_82162350(manager, pause: 1 / -1 unpause, reason): the game's native pause
extern "C" PPC_FUNC(__imp__sub_82162350);
PPC_FUNC(sub_82162350)
{
    static const bool log = getenv("CARS_LOG_PAUSE") != nullptr;
    static std::atomic<int> n{ 0 };
    if (log && n++ < 200)
    {
        fprintf(stderr, "[pause] sub_82162350(%d, %d)", ctx.r4.s32, ctx.r5.s32);
        LogGuestBacktrace(ctx);
    }
    __imp__sub_82162350(ctx, base);
}

// sub_822C4708(value) in D3D: queues the frame flip. value = frontbuffer address (bits 12-31) | presentation
// interval in vblanks (bits 8-11; the game uses 2 = 30 fps) | tearing limit (bits 0-7). The flip target is
// "last target + interval" (sub_822C45F0 presents on the vblank). FPS=60 in the .ini changes interval 2 to 1.
extern "C" PPC_FUNC(__imp__sub_822C4708);
PPC_FUNC(sub_822C4708)
{
    static const bool unlock60 = SettingsGetInt(L"Video", L"FPS", 60) >= 60;
    if (unlock60 && ((ctx.r3.u32 >> 8) & 0xF) > 1)
        ctx.r3.u64 = (ctx.r3.u32 & ~0xF00u) | 0x100u;
    __imp__sub_822C4708(ctx, base);
    // Without waiting for the next 60 Hz vblank: the flip happens now (the GPU does not sit idle waiting) and the 60 fps
    // pacing is left to the host vsync/limiter (RendererSwap). Before, a frame longer than 16.7 ms lost a whole vblank.
    if (unlock60)
    {
        extern void GpuImmediateVblank(const PPCContext& caller);
        GpuImmediateVblank(ctx);
    }
}

// sub_82284D80 = luaX_lexerror(LexState*, msg, token) of the game's Lua 5.1: logs script syntax errors (text mods)
// LexState: +4 current line, +44 TString* of the chunk name (text at TString+16)
extern "C" PPC_FUNC(__imp__sub_82284D80);
PPC_FUNC(sub_82284D80)
{
    const uint32_t ls = ctx.r3.u32, msg = ctx.r4.u32;
    const uint32_t source = R32(ls + 44);
    fprintf(stderr, "[lua] syntax error in %s, line %d: %s (token %d)\n", source ? (const char*)(base + source + 16) : "?",
            int32_t(R32(ls + 4)), msg ? (const char*)(base + msg) : "?", ctx.r5.s32);
    fflush(stderr);
    __imp__sub_82284D80(ctx, base);
}

// The game's zlib 1.0: inflate(z_stream*, flush) in two copies. CARS_LOG_INFLATE=1 logs each call (z_stream: +0 next_in,
// +4 avail_in, +8 total_in, +12 next_out, +16 avail_out, +20 total_out) with the guest stack
static void LogInflate(const char* which, PPCContext& ctx, uint32_t z, bool after)
{
    fprintf(stderr, "[inflate] %s %s z=%08X in=%08X avail_in=%X total_in=%X out=%08X avail_out=%X total_out=%X flush=%d%s", which,
            after ? "<-" : "->", z, R32(z), R32(z + 4), R32(z + 8), R32(z + 12), R32(z + 16), R32(z + 20), ctx.r4.s32,
            after ? "" : " |");
    if (after)
        fprintf(stderr, " ret=%d\n", ctx.r3.s32);
    else
        LogGuestBacktrace(ctx);
}

extern "C" PPC_FUNC(__imp__sub_822731D8);
PPC_FUNC(sub_822731D8)
{
    static const bool log = getenv("CARS_LOG_INFLATE") != nullptr;
    static std::atomic<int> n{ 0 };
    const uint32_t z = ctx.r3.u32;
    const bool on = log && n++ < 4000;
    if (on)
        LogInflate("A", ctx, z, false);
    __imp__sub_822731D8(ctx, base);
    if (on)
        LogInflate("A", ctx, z, true);
}

extern "C" PPC_FUNC(__imp__sub_823CF728);
PPC_FUNC(sub_823CF728)
{
    static const bool log = getenv("CARS_LOG_INFLATE") != nullptr;
    static std::atomic<int> n{ 0 };
    const uint32_t z = ctx.r3.u32;
    const bool on = log && n++ < 4000;
    if (on)
        LogInflate("B", ctx, z, false);
    __imp__sub_823CF728(ctx, base);
    if (on)
        LogInflate("B", ctx, z, true);
}

// sub_822702F8(dest, source, size, opaque) = the game's uncompress (z_stream with avail_in = avail_out = size; returns
// the bytes consumed). Blocks of an extracted .str (mods, kernel_io.cpp) are already raw: copy instead of decompressing
bool RawStreamTake(uint32_t src, uint32_t& size, std::string& path, uint64_t& offset);
bool RawStreamActive();
void ModelCaptureStreamBlock(uint32_t guest, uint32_t size, const std::string& stream, uint64_t offset);   // gpu/model_capture.cpp
extern "C" PPC_FUNC(__imp__sub_822702F8);
PPC_FUNC(sub_822702F8)
{
    uint32_t size = 0;
    std::string path;
    uint64_t offset = 0;
    if (RawStreamActive() && RawStreamTake(ctx.r4.u32, size, path, offset))
    {
        const uint32_t n = std::min(size, ctx.r5.u32);
        memcpy(base + ctx.r3.u32, base + ctx.r4.u32, n);
        // the block now lives at r3: model capture (F11) links VB/IB/texture addresses to stream positions
        ModelCaptureStreamBlock(ctx.r3.u32, n, path, offset);
        ctx.r3.u64 = n;
        return;
    }
    if (RawStreamActive())
    {
        const uint8_t* p = base + ctx.r4.u32;
        static std::atomic<int> warned{ 0 };
        if (!(p[0] == 0x78 && ((p[0] << 8) | p[1]) % 31 == 0) && warned++ < 5)
            fprintf(stderr, "[mods] WARNING: uncompress of data without a zlib header at %08X (%X bytes): untracked raw stream block?\n",
                    ctx.r4.u32, ctx.r5.u32);
    }
    __imp__sub_822702F8(ctx, base);
}

// sub_820F8758(this, event): system events (0 = guide opened -> automatic pause, 3 = guide closed...)
extern "C" PPC_FUNC(__imp__sub_820F8758);
PPC_FUNC(sub_820F8758)
{
    static const bool log = getenv("CARS_LOG_PAUSE") != nullptr;
    static std::atomic<int> n{ 0 };
    if (log && n++ < 200)
    {
        fprintf(stderr, "[pause] system event %d", ctx.r4.s32);
        LogGuestBacktrace(ctx);
    }
    __imp__sub_820F8758(ctx, base);
}
