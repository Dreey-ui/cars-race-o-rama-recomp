// Xbox 360 XMA decoder (the hardware the game's XAudio2 uses for music and effects), using FFmpeg's xma2 decoder
// (official libavcodec). Based on Xenia's XmaDecoder/XmaContext, but decoding whole 2 KB packets.
//
// This game's protocol (static XAudio2, 0x822D5120..0x822D5710):
// - reads the physical address of the context table at 0x7FEA1800 (lwbrx: little-endian) and the "context being processed"
//   at 0x7FEA1818 (kept at 0: never matches an allocated context, so the game's lock always succeeds);
// - XMACreateContext/XMAReleaseContext allocate 64-byte contexts (XMA_CONTEXT_DATA, big-endian);
// - the game copies the context into a working copy (lock), changes it and returns it + writes the "kick" bit
//   (sub_822D55E8). On the kick we decode right away, like Xenia does (the game expects the result on return).
// Output: 16-bit big-endian PCM, interleaved stereo, in a ring of 256-byte blocks.
#include "kernel.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")

namespace
{
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kMmioBase = 0x7FEA0000;
constexpr uint32_t kBytesPerPacket = 2048;
constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
constexpr uint32_t kBitsPerPacketHeader = 32;
constexpr uint32_t kOutputBlock = 256;
const int kSampleRates[4] = { 24000, 32000, 44100, 48000 };

// XMA_CONTEXT_DATA (Xenia), loaded with a 32-bit swap to use the bitfields
struct ContextData
{
    // DWORD 0
    uint32_t input_buffer_0_packet_count : 12;
    uint32_t loop_count : 8;
    uint32_t input_buffer_0_valid : 1;
    uint32_t input_buffer_1_valid : 1;
    uint32_t output_buffer_block_count : 5;
    uint32_t output_buffer_write_offset : 5;
    // DWORD 1
    uint32_t input_buffer_1_packet_count : 12;
    uint32_t loop_subframe_start : 2;
    uint32_t loop_subframe_end : 3;
    uint32_t loop_subframe_skip : 3;
    uint32_t subframe_decode_count : 4;
    uint32_t output_buffer_padding : 3;
    uint32_t sample_rate : 2;
    uint32_t is_stereo : 1;
    uint32_t unk_dword_1_c : 1;
    uint32_t output_buffer_valid : 1;
    // DWORD 2
    uint32_t input_buffer_read_offset : 26;
    uint32_t error_status : 5;
    uint32_t error_set : 1;
    // DWORD 3
    uint32_t loop_start : 26;
    uint32_t parser_error_status : 5;
    uint32_t parser_error_set : 1;
    // DWORD 4
    uint32_t loop_end : 26;
    uint32_t packet_metadata : 5;
    uint32_t current_buffer : 1;
    // DWORD 5..8
    uint32_t input_buffer_0_ptr;
    uint32_t input_buffer_1_ptr;
    uint32_t output_buffer_ptr;
    uint32_t work_buffer_ptr;
    // DWORD 9
    uint32_t output_buffer_read_offset : 5;
    uint32_t : 25;
    uint32_t stop_when_done : 1;
    uint32_t interrupt_when_done : 1;
    // DWORD 10..15
    uint32_t unk_dwords_10_15[6];

    void Load(uint32_t guest)
    {
        auto* d = reinterpret_cast<uint32_t*>(this);
        for (int i = 0; i < 16; i++)
            d[i] = R32(guest + i * 4);
    }
    void Store(uint32_t guest) const
    {
        auto* d = reinterpret_cast<const uint32_t*>(this);
        for (int i = 0; i < 10; i++)   // dwords 10..15 belong to the game
            W32(guest + i * 4, d[i]);
    }
    bool BufferValid(uint32_t b) const { return b ? input_buffer_1_valid : input_buffer_0_valid; }
    uint32_t BufferPackets(uint32_t b) const { return b ? input_buffer_1_packet_count : input_buffer_0_packet_count; }
    uint32_t BufferPtr(uint32_t b) const { return b ? input_buffer_1_ptr : input_buffer_0_ptr; }
    void Invalidate(uint32_t b)
    {
        if (b)
            input_buffer_1_valid = 0;
        else
            input_buffer_0_valid = 0;
    }
};
static_assert(sizeof(ContextData) == 64, "XMA_CONTEXT_DATA is 64 bytes");

struct HostContext
{
    std::mutex mutex;
    bool allocated = false;
    AVCodecContext* av = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int rate = 0, channels = 0;
    std::vector<uint8_t> pending;   // already decoded PCM (16-bit BE interleaved) that did not fit in the output yet
    size_t pendingPos = 0;
    // last input state we wrote: if the game changes it (new sound), the decoder restarts
    uint32_t lastReadOffset = ~0u, lastBuffer = ~0u, lastPtr0 = 0, lastPtr1 = 0;
};

uint32_t g_contextArray = 0;   // guest address (physical 0xA0000000+) of the context table
HostContext g_hosts[kContextCount];
std::mutex g_allocMutex;
bool g_ready = false;
std::atomic<uint64_t> g_statPackets{ 0 }, g_statBlocks{ 0 };

void ResetDecoder(HostContext& h)
{
    if (h.av)
        avcodec_flush_buffers(h.av);
    h.pending.clear();
    h.pendingPos = 0;
}

bool PrepareDecoder(HostContext& h, int rate, int channels)
{
    if (h.av && h.rate == rate && h.channels == channels)
        return true;
    if (h.av)
        avcodec_free_context(&h.av);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec)
        return false;
    h.av = avcodec_alloc_context3(codec);
    h.av->sample_rate = rate;
    av_channel_layout_default(&h.av->ch_layout, channels);
    h.av->block_align = kBytesPerPacket;
    // XMA2WAVEFORMATEX (extra part, 34 bytes): 1 stream, channels = those of the context
    h.av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    h.av->extradata_size = 34;
    h.av->extradata[0] = 1;   // NumStreams (LE16)
    const uint32_t mask = channels == 2 ? 3u : 4u;
    memcpy(h.av->extradata + 2, &mask, 4);   // ChannelMask
    h.av->extradata[32] = 4;                  // EncoderVersion
    if (avcodec_open2(h.av, codec, nullptr) < 0)
    {
        avcodec_free_context(&h.av);
        return false;
    }
    if (!h.packet)
        h.packet = av_packet_alloc();
    if (!h.frame)
        h.frame = av_frame_alloc();
    h.rate = rate;
    h.channels = channels;
    h.pending.clear();
    h.pendingPos = 0;
    return true;
}

// Planar float -> interleaved 16-bit big-endian
void AppendFrame(HostContext& h, const AVFrame* f)
{
    const int ch = std::min(h.channels, f->ch_layout.nb_channels);
    const size_t start = h.pending.size();
    h.pending.resize(start + size_t(f->nb_samples) * h.channels * 2);
    uint8_t* out = h.pending.data() + start;
    for (int s = 0; s < f->nb_samples; s++)
        for (int c = 0; c < h.channels; c++)
        {
            const float v = reinterpret_cast<const float*>(f->data[std::min(c, ch - 1)])[s];
            const int x = int(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
            *out++ = uint8_t(x >> 8);
            *out++ = uint8_t(x);
        }
}

// Decodes the next packet of the current input buffer. false = no input.
bool DecodeNextPacket(HostContext& h, ContextData& d)
{
    for (int tries = 0; tries < 2; tries++)
    {
        const uint32_t b = d.current_buffer;
        if (!d.BufferValid(b))
        {
            if (!d.BufferValid(b ^ 1))
                return false;
            d.current_buffer = b ^ 1;   // the other buffer is ready
            d.input_buffer_read_offset = kBitsPerPacketHeader;
            continue;
        }
        const uint32_t packets = d.BufferPackets(b);
        const uint32_t index = d.input_buffer_read_offset / kBitsPerPacket;
        if (index >= packets || !d.BufferPtr(b))
        {
            d.Invalidate(b);
            d.current_buffer = b ^ 1;
            d.input_buffer_read_offset = kBitsPerPacketHeader;
            continue;
        }
        const uint8_t* src = g_mem.base + (0xA0000000u | (d.BufferPtr(b) & 0x1FFFFFFF)) + size_t(index) * kBytesPerPacket;
        av_packet_unref(h.packet);
        if (av_new_packet(h.packet, kBytesPerPacket) == 0)
        {
            memcpy(h.packet->data, src, kBytesPerPacket);
            if (avcodec_send_packet(h.av, h.packet) == 0)
                while (avcodec_receive_frame(h.av, h.frame) == 0)
                    AppendFrame(h, h.frame);
        }
        g_statPackets++;
        // next packet; end of buffer -> release it to the game and switch to the other one
        uint32_t next = (index + 1) * kBitsPerPacket + kBitsPerPacketHeader;
        // Loop (approximated per packet): when passing the end of the loop region, go back to its first packet
        if (d.loop_count && d.loop_end > d.loop_start && d.loop_end <= (index + 1) * kBitsPerPacket &&
            d.loop_end > index * kBitsPerPacket)
        {
            next = d.loop_start;
            avcodec_flush_buffers(h.av);   // discontinuity: the next packet starts from scratch
            if (d.loop_count != 255)
                d.loop_count--;
        }
        if (next / kBitsPerPacket >= packets)
        {
            d.Invalidate(b);
            d.current_buffer = b ^ 1;
            next = kBitsPerPacketHeader;
        }
        d.input_buffer_read_offset = next;
        return true;
    }
    return false;
}

void Work(uint32_t guest)
{
    if (!g_ready || guest < g_contextArray || guest >= g_contextArray + kContextCount * 64)
        return;
    const uint32_t id = (guest - g_contextArray) / 64;
    HostContext& h = g_hosts[id];
    std::lock_guard<std::mutex> lock(h.mutex);
    if (!h.allocated)
        return;
    ContextData d;
    d.Load(guest);
    if (!d.output_buffer_valid || !d.output_buffer_block_count || !d.output_buffer_ptr)
        return;
    if (!PrepareDecoder(h, kSampleRates[d.sample_rate], d.is_stereo ? 2 : 1))
        return;
    // New sound (the game moved the read position or the buffers): restart the decoder
    if (d.input_buffer_read_offset != h.lastReadOffset || d.current_buffer != h.lastBuffer ||
        (d.input_buffer_0_ptr != h.lastPtr0 && d.input_buffer_0_valid && d.current_buffer == 0) ||
        (d.input_buffer_1_ptr != h.lastPtr1 && d.input_buffer_1_valid && d.current_buffer == 1))
        ResetDecoder(h);

    const uint32_t capacity = d.output_buffer_block_count * kOutputBlock;
    uint32_t write = d.output_buffer_write_offset * kOutputBlock;
    const uint32_t read = d.output_buffer_read_offset * kOutputBlock;
    uint32_t space = read == write ? capacity : (write < read ? read - write : capacity - write + read);
    uint8_t* out = g_mem.base + (0xA0000000u | (d.output_buffer_ptr & 0x1FFFFFFF));
    while (space >= kOutputBlock)
    {
        if (h.pending.size() - h.pendingPos < kOutputBlock)
        {
            // compact what is left and decode one more packet
            h.pending.erase(h.pending.begin(), h.pending.begin() + ptrdiff_t(h.pendingPos));
            h.pendingPos = 0;
            if (!DecodeNextPacket(h, d))
                break;
            continue;
        }
        memcpy(out + write, h.pending.data() + h.pendingPos, kOutputBlock);
        h.pendingPos += kOutputBlock;
        write = (write + kOutputBlock) % capacity;
        space -= kOutputBlock;
        g_statBlocks++;
    }
    d.output_buffer_write_offset = write / kOutputBlock;
    if (space < kOutputBlock)
        d.output_buffer_valid = 0;   // output full: the game frees space and enables it again
    d.error_status = 0;
    d.error_set = 0;
    d.Store(guest);
    h.lastReadOffset = d.input_buffer_read_offset;
    h.lastBuffer = d.current_buffer;
    h.lastPtr0 = d.input_buffer_0_ptr;
    h.lastPtr1 = d.input_buffer_1_ptr;
}
}  // namespace

void XmaInit()
{
    g_contextArray = g_physicalHeap.Alloc(kContextCount * 64, 0x1000);
    g_mem.Commit(kMmioBase, 0x10000);
    // ContextArrayAddress (0x7FEA1800) is read with lwbrx: physical value in little-endian
    *reinterpret_cast<uint32_t*>(g_mem.base + kMmioBase + 0x1800) = g_contextArray & 0x1FFFFFFF;
    *reinterpret_cast<uint32_t*>(g_mem.base + kMmioBase + 0x1818) = 0;   // CurrentContextIndex
    g_ready = avcodec_find_decoder(AV_CODEC_ID_XMA2) != nullptr;
    av_log_set_level(AV_LOG_ERROR);   // xma2 warnings ("Could not update timestamps for skipped samples") only flooded the log
    fprintf(stderr, "[xma] %u contexts at %08X, FFmpeg xma2 decoder %s\n", kContextCount, g_contextArray,
            g_ready ? "ok" : "MISSING (no XMA audio)");
}

void XmaTakeStats(uint64_t& packets, uint64_t& blocks)
{
    packets = g_statPackets.exchange(0);
    blocks = g_statBlocks.exchange(0);
}

// (output pointer for the context)
KERNEL_IMPORT(XMACreateContext)
{
    std::lock_guard<std::mutex> lock(g_allocMutex);
    for (uint32_t i = 1; i < kContextCount; i++)   // 0 stays free (CurrentContextIndex)
    {
        HostContext& h = g_hosts[i];
        std::lock_guard<std::mutex> hl(h.mutex);
        if (h.allocated)
            continue;
        h.allocated = true;
        ResetDecoder(h);
        h.lastReadOffset = ~0u;
        const uint32_t guest = g_contextArray + i * 64;
        memset(g_mem.base + guest, 0, 64);
        if (ctx.r3.u32)
            W32(ctx.r3.u32, guest);
        RetStatus(ctx, STATUS_SUCCESS);
        return;
    }
    RetStatus(ctx, 0xC0000017);   // STATUS_NO_MEMORY
}

KERNEL_IMPORT(XMAReleaseContext)
{
    const uint32_t guest = ctx.r3.u32;
    if (g_contextArray && guest >= g_contextArray && guest < g_contextArray + kContextCount * 64)
    {
        HostContext& h = g_hosts[(guest - g_contextArray) / 64];
        std::lock_guard<std::mutex> hl(h.mutex);
        h.allocated = false;
        ResetDecoder(h);
        memset(g_mem.base + guest, 0, 64);
    }
    ctx.r3.u64 = 0;
}

// Kick of a voice's contexts (structure: +0 number of contexts, +8 records of 96 bytes with the context pointer
// at +64): the original returns the working copy and writes the kick bits; then we decode each context
extern "C" PPC_FUNC(__imp__sub_822D55E8);
PPC_FUNC(sub_822D55E8)
{
    const uint32_t voice = ctx.r3.u32;
    __imp__sub_822D55E8(ctx, base);
    const uint32_t n = R32(voice), records = R32(voice + 8);
    for (uint32_t i = 0; i < n && i < 8; i++)
        Work(R32(records + i * 96 + 64));
}
