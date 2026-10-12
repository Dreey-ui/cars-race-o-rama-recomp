// Audio: driver de render do XAudio (como o AudioSystem do Xenia). O motor de audio do jogo (XAudio2 estatico) registra
// um callback que o sistema chama a cada 5,333 ms (256 amostras a 48 kHz); o callback mistura e entrega um frame via
// XAudioSubmitRenderDriverFrame: 6 canais (5.1: FL FR FC LFE BL BR) x 256 amostras, float big-endian, um canal apos o outro.
// Os frames vao para uma voz 5.1 do XAudio2 do Windows (que faz o downmix para os alto-falantes do PC). O ritmo do callback
// segue a fila do dispositivo: o relogio da placa de som manda, sem acumular atraso nem faltar amostra.
#include "kernel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

#include <windows.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

struct AudioClient
{
    bool inUse = false;
    uint32_t callback = 0;
    uint32_t argPtr = 0;   // ponteiro guest para uma palavra com o argumento (o callback recebe o ponteiro)
};

static std::mutex g_audioMutex;
static AudioClient g_clients[8];
static std::atomic<bool> g_audioThreadStarted{ false };
static std::atomic<uint64_t> g_framesSubmitted{ 0 };

// ---- saida no host (XAudio2 2.9 do Windows 10) ----
namespace
{
constexpr uint32_t kFrameSamples = 256, kChannels = 6, kRing = 64;
constexpr uint32_t kTargetQueued = 6;   // ~32 ms de latencia
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
std::mutex g_outMutex;
alignas(16) float g_ring[kRing][kFrameSamples * kChannels];
uint32_t g_ringPos = 0;
std::atomic<float> g_peak{ 0.0f };
std::atomic<uint32_t> g_dropped{ 0 };
// Ultimos 10 s do mix (estereo 16 bits): F12 do log de sessao salva em marca_NN\som_ultimos_10s.wav
constexpr uint32_t kRecentSamples = 48000 * 10;   // multiplo de kFrameSamples
int16_t g_recent[kRecentSamples * 2];
uint32_t g_recentPos = 0;
bool g_recentFull = false;

bool InitHostAudio()
{
    // O XAudio2 usa o MMDevice (COM) para achar a placa de som
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        fprintf(stderr, "[audio] XAudio2Create falhou (%08X): sem som\n", unsigned(hr));
        return false;
    }
    hr = g_xa->CreateMasteringVoice(&g_master);
    if (FAILED(hr))
    {
        fprintf(stderr, "[audio] sem dispositivo de saida de audio (CreateMasteringVoice %08X)\n", unsigned(hr));
        return false;
    }
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = kChannels;
    fmt.Format.nSamplesPerSec = 48000;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = kChannels * 4;
    fmt.Format.nAvgBytesPerSec = 48000 * kChannels * 4;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                        SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    fmt.SubFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &fmt.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO)))
    {
        fprintf(stderr, "[audio] CreateSourceVoice (5.1 float 48 kHz) falhou\n");
        return false;
    }
    g_voice->Start();
    XAUDIO2_VOICE_DETAILS details{};
    g_master->GetVoiceDetails(&details);
    fprintf(stderr, "[audio] saida XAudio2: %u canais a %u Hz\n", details.InputChannels, details.InputSampleRate);
    return true;
}

uint32_t QueuedFrames()
{
    if (!g_voice)
        return 0;
    XAUDIO2_VOICE_STATE st{};
    g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return st.BuffersQueued;
}

void SubmitHostFrame(uint32_t guestSamples)
{
    std::lock_guard<std::mutex> lock(g_outMutex);
    if (!g_voice)
        return;
    if (QueuedFrames() >= kRing - 2)
    {
        g_dropped++;
        return;
    }
    float* out = g_ring[g_ringPos];
    const uint32_t* in = reinterpret_cast<const uint32_t*>(g_mem.base + guestSamples);
    float peak = 0.0f;
    for (uint32_t s = 0; s < kFrameSamples; s++)
        for (uint32_t c = 0; c < kChannels; c++)
        {
            const uint32_t bits = __builtin_bswap32(in[c * kFrameSamples + s]);
            float v;
            memcpy(&v, &bits, 4);
            if (!(v == v))   // NaN
                v = 0.0f;
            out[s * kChannels + c] = v;
            peak = std::max(peak, std::fabs(v));
        }
    if (peak > g_peak.load())
        g_peak = peak;
    // Downmix estereo 16 bits: anel dos ultimos 10 s e CARS_AUDIO_DUMP=1 (grava tudo em audio_dump.raw para conferir o som)
    int16_t* st = g_recent + size_t(g_recentPos) * 2;
    for (uint32_t s = 0; s < kFrameSamples; s++)
    {
        const float* f = out + s * kChannels;
        const float l = f[0] + 0.707f * (f[2] + f[4]), r = f[1] + 0.707f * (f[2] + f[5]);
        st[s * 2] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
        st[s * 2 + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
    }
    g_recentPos += kFrameSamples;
    if (g_recentPos >= kRecentSamples)
    {
        g_recentPos = 0;
        g_recentFull = true;
    }
    static FILE* dump = getenv("CARS_AUDIO_DUMP") ? fopen("audio_dump.raw", "wb") : nullptr;
    if (dump)
    {
        fwrite(st, sizeof(int16_t) * 2, kFrameSamples, dump);
        fflush(dump);
    }
    XAUDIO2_BUFFER b{};
    b.AudioBytes = sizeof(g_ring[0]);
    b.pAudioData = reinterpret_cast<const BYTE*>(out);
    g_voice->SubmitSourceBuffer(&b);
    g_ringPos = (g_ringPos + 1) % kRing;
}
}  // namespace

// F12 (log de sessao): ultimos 10 s do som em WAV estereo 16 bits 48 kHz
bool AudioSaveRecent(const std::filesystem::path& path)
{
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lock(g_outMutex);
        if (g_recentFull)
            pcm.assign(g_recent + size_t(g_recentPos) * 2, g_recent + size_t(kRecentSamples) * 2);
        pcm.insert(pcm.end(), g_recent, g_recent + size_t(g_recentPos) * 2);
    }
    if (pcm.empty())
        return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f)
        return false;
    const uint32_t dataBytes = uint32_t(pcm.size() * 2);
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
    auto put32 = [&](int o, uint32_t v) { memcpy(h + o, &v, 4); };
    put32(4, 36 + dataBytes);
    put32(24, 48000);
    put32(28, 48000 * 4);
    h[32] = 4;    // block align
    h[34] = 16;   // bits
    memcpy(h + 36, "data", 4);
    put32(40, dataBytes);
    fwrite(h, 1, sizeof(h), f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return true;
}

static void AudioThread()
{
    SetThreadDescription(GetCurrentThread(), L"audio (driver)");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    GuestThread* gt = KSetupGuestThread(0x40000);
    auto ctx = std::make_unique<PPCContext>();
    KInitContext(*ctx, gt);
    KSetCurrentThread(gt);
    gt->ctx = ctx.get();
    gt->entry = 0xA0D10000;
    KRegisterThreadForSampling(gt);
    const bool hostOk = InitHostAudio();

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    auto lastReport = clock::now();
    uint64_t lastFrames = 0;
    for (;;)
    {
        // Ritmo base de 5,333 ms, corrigido pela fila do dispositivo: fila baixa -> adianta, fila cheia -> espera
        const uint32_t queued = hostOk ? QueuedFrames() : kTargetQueued;
        int64_t us = 5333;
        if (queued < kTargetQueued - 2)
            us = 4300;
        else if (queued > kTargetQueued + 2)
            us = 6500;
        next += std::chrono::microseconds(us);
        std::this_thread::sleep_until(next);
        if (clock::now() - next > std::chrono::milliseconds(100))
            next = clock::now();   // ficou muito para tras (depurador, etc.): nao tenta compensar
        if (hostOk && QueuedFrames() >= kRing - 4)
            continue;   // dispositivo parado/cheio: nao gera mais
        for (int i = 0; i < 8; i++)
        {
            uint32_t cb, arg;
            {
                std::lock_guard<std::mutex> lock(g_audioMutex);
                if (!g_clients[i].inUse)
                    continue;
                cb = g_clients[i].callback;
                arg = g_clients[i].argPtr;
            }
            ctx->r3.u64 = arg;
            try { CallGuest(*ctx, g_mem.base, cb); } catch (const GuestThreadExit&) {}
        }
        if (clock::now() - lastReport > std::chrono::seconds(10))
        {
            const uint64_t frames = g_framesSubmitted.load();
            extern void XmaTakeStats(uint64_t& packets, uint64_t& blocks);
            uint64_t xmaPackets = 0, xmaBlocks = 0;
            XmaTakeStats(xmaPackets, xmaBlocks);
            fprintf(stderr, "[audio] %.0f frames/s, pico %.3f, fila %u, descartados %u | XMA: %llu pacotes, %llu blocos de saida\n",
                    (frames - lastFrames) / std::chrono::duration<double>(clock::now() - lastReport).count(), g_peak.exchange(0.0f),
                    QueuedFrames(), g_dropped.exchange(0), (unsigned long long)xmaPackets, (unsigned long long)xmaBlocks);
            lastFrames = frames;
            lastReport = clock::now();
        }
    }
}

KERNEL_IMPORT(XAudioRegisterRenderDriverClient)
{
    // (callback_ptr -> { callback, callback_arg }, driver_ptr)
    const uint32_t cbPtr = ctx.r3.u32, driverPtr = ctx.r4.u32;
    if (!cbPtr || !R32(cbPtr))
    {
        ctx.r3.u64 = 0x80070057;   // E_INVALIDARG
        return;
    }
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        for (int i = 0; i < 8; i++)
            if (!g_clients[i].inUse)
            {
                index = i;
                break;
            }
        if (index < 0)
        {
            ctx.r3.u64 = 0x8007000E;
            return;
        }
        auto& c = g_clients[index];
        c.callback = R32(cbPtr);
        c.argPtr = GuestAllocSmall(4);
        W32(c.argPtr, R32(cbPtr + 4));
        c.inUse = true;
    }
    if (driverPtr)
        W32(driverPtr, 0x41550000u | uint32_t(index));
    if (g_traceImports)
        fprintf(stderr, "[audio] cliente %d registrado (callback %08X, arg %08X)\n", index, R32(cbPtr), R32(cbPtr + 4));
    if (!g_audioThreadStarted.exchange(true))
        std::thread(AudioThread).detach();
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XAudioUnregisterRenderDriverClient)
{
    const uint32_t index = ctx.r3.u32 & 0xFFFF;
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (index < 8)
        g_clients[index].inUse = false;
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XAudioSubmitRenderDriverFrame)
{
    // (driver, amostras: 6 canais x 256 em float big-endian)
    g_framesSubmitted++;
    if (ctx.r4.u32)
        SubmitHostFrame(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XAudioGetSpeakerConfig)
{
    if (ctx.r3.u32)
        W32(ctx.r3.u32, 0x00010001);   // estereo
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    if (ctx.r4.u32)
        W32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XAudioGetVoiceCategoryVolume)
{
    if (ctx.r4.u32)
    {
        float one = 1.0f;
        uint32_t v;
        memcpy(&v, &one, 4);
        W32(ctx.r4.u32, v);
    }
    ctx.r3.u64 = 0;
}
