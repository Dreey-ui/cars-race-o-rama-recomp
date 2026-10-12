// Hooks em funcoes do proprio jogo. CARS_LOG_PAUSE=1 loga quem pausa o jogo (diagnostico do menu de pausa que abria sozinho:
// era o Start automatico de teste, ja removido)
#include "runtime.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <string>

// Pilha guest da thread atual: LR salvo pelo prologo de cada frame (r12 em -8 do frame do chamador)
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

// sub_82162350(gerenciador, pausar: 1 / -1 despausar, motivo): pausa nativa do jogo
extern "C" PPC_FUNC(__imp__sub_82162350);
PPC_FUNC(sub_82162350)
{
    static const bool log = getenv("CARS_LOG_PAUSE") != nullptr;
    static std::atomic<int> n{ 0 };
    if (log && n++ < 200)
    {
        fprintf(stderr, "[pausa] sub_82162350(%d, %d)", ctx.r4.s32, ctx.r5.s32);
        LogGuestBacktrace(ctx);
    }
    __imp__sub_82162350(ctx, base);
}

// sub_822C4708(valor) do D3D: enfileira o flip do frame. valor = endereco do frontbuffer (bits 12-31) | intervalo de
// apresentacao em vblanks (bits 8-11; o jogo usa 2 = 30 fps) | limite de tearing (bits 0-7). O alvo do flip e
// "ultimo alvo + intervalo" (sub_822C45F0 apresenta no vblank). FPS=60 no .ini troca o intervalo 2 por 1.
extern "C" PPC_FUNC(__imp__sub_822C4708);
PPC_FUNC(sub_822C4708)
{
    static const bool unlock60 = SettingsGetInt(L"Video", L"FPS", 60) >= 60;
    if (unlock60 && ((ctx.r3.u32 >> 8) & 0xF) > 1)
        ctx.r3.u64 = (ctx.r3.u32 & ~0xF00u) | 0x100u;
    __imp__sub_822C4708(ctx, base);
    // Sem esperar o proximo vblank de 60 Hz: o flip acontece agora (a GPU nao fica parada esperando) e o ritmo de 60 fps
    // fica com o vsync/limitador do host (RendererSwap). Antes, um frame que passasse de 16,7 ms perdia um vblank inteiro.
    if (unlock60)
    {
        extern void GpuImmediateVblank(const PPCContext& caller);
        GpuImmediateVblank(ctx);
    }
}

// sub_82284D80 = luaX_lexerror(LexState*, msg, token) do Lua 5.1 do jogo: loga erros de sintaxe dos scripts (mods em texto)
// LexState: +4 linha atual, +44 TString* do nome do chunk (texto em TString+16)
extern "C" PPC_FUNC(__imp__sub_82284D80);
PPC_FUNC(sub_82284D80)
{
    const uint32_t ls = ctx.r3.u32, msg = ctx.r4.u32;
    const uint32_t source = R32(ls + 44);
    fprintf(stderr, "[lua] erro de sintaxe em %s, linha %d: %s (token %d)\n", source ? (const char*)(base + source + 16) : "?",
            int32_t(R32(ls + 4)), msg ? (const char*)(base + msg) : "?", ctx.r5.s32);
    fflush(stderr);
    __imp__sub_82284D80(ctx, base);
}

// zlib 1.0 do jogo: inflate(z_stream*, flush) em duas copias. CARS_LOG_INFLATE=1 loga cada chamada (z_stream: +0 next_in,
// +4 avail_in, +8 total_in, +12 next_out, +16 avail_out, +20 total_out) com a pilha guest
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

// sub_822702F8(destino, origem, tamanho, opaco) = uncompress do jogo (z_stream com avail_in = avail_out = tamanho; devolve
// os bytes consumidos). Blocos de um .str extraido (mods, kernel_io.cpp) ja estao crus: copia em vez de descomprimir
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
        // o bloco agora vive em r3: a captura de modelos (F11) liga enderecos de VB/IB/textura a posicoes no stream
        ModelCaptureStreamBlock(ctx.r3.u32, n, path, offset);
        ctx.r3.u64 = n;
        return;
    }
    if (RawStreamActive())
    {
        const uint8_t* p = base + ctx.r4.u32;
        static std::atomic<int> warned{ 0 };
        if (!(p[0] == 0x78 && ((p[0] << 8) | p[1]) % 31 == 0) && warned++ < 5)
            fprintf(stderr, "[mods] AVISO: uncompress de dados sem cabecalho zlib em %08X (%X bytes): bloco de stream cru nao rastreado?\n",
                    ctx.r4.u32, ctx.r5.u32);
    }
    __imp__sub_822702F8(ctx, base);
}

// sub_820F8758(this, evento): eventos do sistema (0 = guia aberto -> pausa automatica, 3 = guia fechado...)
extern "C" PPC_FUNC(__imp__sub_820F8758);
PPC_FUNC(sub_820F8758)
{
    static const bool log = getenv("CARS_LOG_PAUSE") != nullptr;
    static std::atomic<int> n{ 0 };
    if (log && n++ < 200)
    {
        fprintf(stderr, "[pausa] evento de sistema %d", ctx.r4.s32);
        LogGuestBacktrace(ctx);
    }
    __imp__sub_820F8758(ctx, base);
}
