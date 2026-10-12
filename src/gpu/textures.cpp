// Reading guest textures: Xenos 2D untiling (Xenia's texture_address.h), endian and format conversion.
#include "textures.h"
#include "xenos_regs.h"
#include "runtime.h"

#include <windows.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <immintrin.h>

TextureFetch DecodeTextureFetch(const uint32_t* d)
{
    TextureFetch f{};
    memcpy(f.raw, d, sizeof(f.raw));
    f.type = Bits(d[0], 0, 2);
    f.signX = Bits(d[0], 2, 2);
    f.clampX = Bits(d[0], 10, 3);
    f.clampY = Bits(d[0], 13, 3);
    f.clampZ = Bits(d[0], 16, 3);
    f.pitchTexels = Bits(d[0], 22, 9) * 32;
    f.tiled = Bits(d[0], 31, 1) != 0;
    f.format = Bits(d[1], 0, 6);
    f.endian = Bits(d[1], 6, 2);
    f.baseAddress = Bits(d[1], 12, 20) << 12;
    f.dimension = Bits(d[5], 9, 2);
    if (f.dimension == 2)
    {
        f.width = Bits(d[2], 0, 11) + 1;
        f.height = Bits(d[2], 11, 11) + 1;
        f.depth = Bits(d[2], 22, 10) + 1;
    }
    else if (f.dimension == 0)
    {
        f.width = Bits(d[2], 0, 24) + 1;
        f.height = 1;
        f.depth = 1;
    }
    else
    {
        f.width = Bits(d[2], 0, 13) + 1;
        f.height = Bits(d[2], 13, 13) + 1;
        f.depth = Bits(d[2], 26, 6) + 1;
    }
    f.swizzle = Bits(d[3], 1, 12);
    f.magFilter = Bits(d[3], 19, 2);
    f.minFilter = Bits(d[3], 21, 2);
    f.mipFilter = Bits(d[3], 23, 2);
    f.anisoFilter = Bits(d[3], 25, 3);
    f.borderColor = Bits(d[5], 0, 2);
    return f;
}

enum class Conv { Copy, R5G6B5, R6G5B5, A1R5G5B5, A4R4G4B4, Depth24, Dxt3A, Yuv422Gbgr, Yuv422Bgrg };

struct FormatInfo
{
    uint32_t xenos;
    uint32_t blockSize;      // 1 or 4 (BC)
    uint32_t bytesPerBlock;
    DXGI_FORMAT host;
    uint32_t hostBytesPerBlock;
    Conv conv;
    uint32_t components;
};

static const FormatInfo kFormats[] = {
    { 2,  1, 1,  DXGI_FORMAT_R8_UNORM,             1,  Conv::Copy, 1 },      // k_8
    { 3,  1, 2,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::A1R5G5B5, 4 },  // k_1_5_5_5
    { 4,  1, 2,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::R5G6B5, 3 },    // k_5_6_5
    { 5,  1, 2,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::R6G5B5, 3 },    // k_6_5_5
    { 6,  1, 4,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::Copy, 4 },      // k_8_8_8_8
    { 7,  1, 4,  DXGI_FORMAT_R10G10B10A2_UNORM,    4,  Conv::Copy, 4 },      // k_2_10_10_10
    { 8,  1, 1,  DXGI_FORMAT_R8_UNORM,             1,  Conv::Copy, 1 },      // k_8_A
    { 9,  1, 1,  DXGI_FORMAT_R8_UNORM,             1,  Conv::Copy, 1 },      // k_8_B
    { 10, 1, 2,  DXGI_FORMAT_R8G8_UNORM,           2,  Conv::Copy, 2 },      // k_8_8
    { 11, 1, 4,  DXGI_FORMAT_R8G8B8A8_UNORM,       8,  Conv::Yuv422Bgrg, 3 },// k_Cr_Y1_Cb_Y0_REP (2 pixels / 32 bits)
    { 12, 1, 4,  DXGI_FORMAT_R8G8B8A8_UNORM,       8,  Conv::Yuv422Gbgr, 3 },// k_Y1_Cr_Y0_Cb_REP
    { 14, 1, 4,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::Copy, 4 },      // k_8_8_8_8_A
    { 15, 1, 2,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::A4R4G4B4, 4 },  // k_4_4_4_4
    { 18, 4, 8,  DXGI_FORMAT_BC1_UNORM,            8,  Conv::Copy, 4 },      // k_DXT1
    { 19, 4, 16, DXGI_FORMAT_BC2_UNORM,            16, Conv::Copy, 4 },      // k_DXT2_3
    { 20, 4, 16, DXGI_FORMAT_BC3_UNORM,            16, Conv::Copy, 4 },      // k_DXT4_5
    { 22, 1, 4,  DXGI_FORMAT_R32_FLOAT,            4,  Conv::Depth24, 1 },   // k_24_8
    { 23, 1, 4,  DXGI_FORMAT_R32_FLOAT,            4,  Conv::Depth24, 1 },   // k_24_8_FLOAT (approximate)
    { 24, 1, 2,  DXGI_FORMAT_R16_UNORM,            2,  Conv::Copy, 1 },      // k_16
    { 25, 1, 4,  DXGI_FORMAT_R16G16_UNORM,         4,  Conv::Copy, 2 },      // k_16_16
    { 26, 1, 8,  DXGI_FORMAT_R16G16B16A16_UNORM,   8,  Conv::Copy, 4 },      // k_16_16_16_16
    { 30, 1, 2,  DXGI_FORMAT_R16_FLOAT,            2,  Conv::Copy, 1 },      // k_16_FLOAT
    { 31, 1, 4,  DXGI_FORMAT_R16G16_FLOAT,         4,  Conv::Copy, 2 },      // k_16_16_FLOAT
    { 32, 1, 8,  DXGI_FORMAT_R16G16B16A16_FLOAT,   8,  Conv::Copy, 4 },      // k_16_16_16_16_FLOAT
    { 33, 1, 4,  DXGI_FORMAT_R32_FLOAT,            4,  Conv::Copy, 1 },      // k_32 (treated as float)
    { 36, 1, 4,  DXGI_FORMAT_R32_FLOAT,            4,  Conv::Copy, 1 },      // k_32_FLOAT
    { 37, 1, 8,  DXGI_FORMAT_R32G32_FLOAT,         8,  Conv::Copy, 2 },      // k_32_32_FLOAT
    { 38, 1, 16, DXGI_FORMAT_R32G32B32A32_FLOAT,   16, Conv::Copy, 4 },      // k_32_32_32_32_FLOAT
    { 49, 4, 16, DXGI_FORMAT_BC5_UNORM,            16, Conv::Copy, 2 },      // k_DXN
    { 50, 1, 4,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::Copy, 4 },      // k_8_8_8_8_AS_16_16_16_16
    { 51, 4, 8,  DXGI_FORMAT_BC1_UNORM,            8,  Conv::Copy, 4 },      // k_DXT1_AS_16_16_16_16
    { 52, 4, 16, DXGI_FORMAT_BC2_UNORM,            16, Conv::Copy, 4 },
    { 53, 4, 16, DXGI_FORMAT_BC3_UNORM,            16, Conv::Copy, 4 },
    { 54, 1, 4,  DXGI_FORMAT_R10G10B10A2_UNORM,    4,  Conv::Copy, 4 },
    { 58, 4, 8,  DXGI_FORMAT_R8_UNORM,             16, Conv::Dxt3A, 1 },     // k_DXT3A -> R8 (decompressed)
    { 59, 4, 8,  DXGI_FORMAT_BC4_UNORM,            8,  Conv::Copy, 1 },      // k_DXT5A
    { 62, 1, 4,  DXGI_FORMAT_R8G8B8A8_UNORM,       4,  Conv::Copy, 4 },      // k_8_8_8_8_GAMMA_EDRAM
};

static const FormatInfo* FindFormat(uint32_t xenos)
{
    for (const auto& f : kFormats)
        if (f.xenos == xenos)
            return &f;
    return nullptr;
}

bool HostFormatForTexture(uint32_t xenosFormat, DXGI_FORMAT& fmt, uint32_t& components)
{
    const FormatInfo* fi = FindFormat(xenosFormat);
    if (!fi)
        return false;
    fmt = fi->host;
    components = fi->components;
    return true;
}

// ---- 2D tiling (Xenia's texture_address.h) ----
static int32_t TiledCombine(int32_t outerInnerBytes, uint32_t bank, uint32_t pipe, uint32_t yLsb)
{
    return int32_t((yLsb << 4) | (pipe << 6) | (bank << 11)) | (outerInnerBytes & 0b1111) |
           (((outerInnerBytes >> 4) & 0b1) << 5) | (((outerInnerBytes >> 5) & 0b111) << 8) | (outerInnerBytes >> 8 << 12);
}

static int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitchAligned, uint32_t bppLog2)
{
    const int32_t outerBlocks = ((y >> 5) * int32_t(pitchAligned >> 5) + (x >> 5)) << 6;
    const int32_t innerBlocks = (((y >> 1) & 0b111) << 3) | (x & 0b111);
    const int32_t outerInnerBytes = (outerBlocks | innerBlocks) << bppLog2;
    const uint32_t bank = (y >> 4) & 0b1;
    const uint32_t pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1);
    return TiledCombine(outerInnerBytes, bank, pipe, y & 1);
}

uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchTexels, uint32_t bppLog2)
{
    return uint32_t(Tiled2D(int32_t(x), int32_t(y), (pitchTexels + 31) & ~31u, bppLog2));
}

static uint32_t Log2(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

struct Layout
{
    uint32_t blocksX, blocksY;
    uint32_t pitchBlocks;     // aligned to 32 blocks
    uint32_t rowPitchBytes;   // linear
    uint32_t extent;          // guest bytes
};

static Layout ComputeLayout(const TextureFetch& f, const FormatInfo& fi)
{
    Layout l{};
    l.blocksX = (f.width + fi.blockSize - 1) / fi.blockSize;
    l.blocksY = (f.height + fi.blockSize - 1) / fi.blockSize;
    uint32_t pitch = std::max(f.pitchTexels, f.width);
    l.pitchBlocks = AlignUp((pitch + fi.blockSize - 1) / fi.blockSize, 32);
    if (f.tiled)
    {
        l.extent = l.pitchBlocks * AlignUp(l.blocksY, 32) * fi.bytesPerBlock;
    }
    else
    {
        l.rowPitchBytes = AlignUp(l.pitchBlocks * fi.bytesPerBlock, 256);
        l.extent = l.rowPitchBytes * (l.blocksY - 1) + l.blocksX * fi.bytesPerBlock;
    }
    return l;
}

// How many bytes from addr are committed (avoids reading past what the game allocated)
static uint32_t ReadableBytes(uint32_t addr, uint32_t len)
{
    // Physical memory (0xA0000000+): always committed (section). VirtualQuery on these 512 MB views is expensive.
    if (addr >= 0xA0000000u)
    {
        const uint64_t windowEnd = (uint64_t(addr) & ~0x1FFFFFFFull) + 0x20000000ull;
        return uint32_t(std::min<uint64_t>(len, windowEnd - addr));
    }
    uint32_t ok = 0;
    while (ok < len)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(g_mem.base + addr + ok, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            break;
        const uint64_t regionEnd = uint64_t((uint8_t*)mbi.BaseAddress - g_mem.base) + mbi.RegionSize;
        const uint64_t cur = uint64_t(addr) + ok;
        ok += uint32_t(std::min<uint64_t>(regionEnd - cur, len - ok));
    }
    return ok;
}

bool GuestTextureExtent(const TextureFetch& f, uint32_t& guestAddr, uint32_t& sizeBytes)
{
    const FormatInfo* fi = FindFormat(f.format);
    if (!fi)
        return false;
    const Layout l = ComputeLayout(f, *fi);
    guestAddr = GpuPhysToGuest(f.baseAddress);
    sizeBytes = ReadableBytes(guestAddr, l.extent);
    return sizeBytes != 0;
}

uint64_t HashGuestMemory(uint32_t guestAddr, uint32_t size)
{
    return XXH3_64bits(g_mem.base + guestAddr, size);
}

static void SwapBlock(uint8_t* p, uint32_t bytes, uint32_t endian)
{
    switch (endian)
    {
    case 1:   // 8in16
        for (uint32_t i = 0; i + 1 < bytes; i += 2)
            std::swap(p[i], p[i + 1]);
        break;
    case 2:   // 8in32
        for (uint32_t i = 0; i + 3 < bytes; i += 4)
        {
            std::swap(p[i], p[i + 3]);
            std::swap(p[i + 1], p[i + 2]);
        }
        break;
    case 3:   // 16in32
        for (uint32_t i = 0; i + 3 < bytes; i += 4)
        {
            std::swap(p[i], p[i + 2]);
            std::swap(p[i + 1], p[i + 3]);
        }
        break;
    }
}

static uint8_t Expand(uint32_t v, uint32_t bits) { return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

static void YuvToRgb(int y, int u, int v, uint8_t* out)
{
    const int c = y - 16, d = u - 128, e = v - 128;
    out[0] = uint8_t(std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255));
    out[1] = uint8_t(std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255));
    out[2] = uint8_t(std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255));
    out[3] = 255;
}

// Converts a guest block (endian already fixed) to the host format
static void ConvertBlock(const FormatInfo& fi, const uint8_t* src, uint8_t* dst)
{
    switch (fi.conv)
    {
    case Conv::Copy:
        memcpy(dst, src, fi.bytesPerBlock);
        break;
    case Conv::R5G6B5:
    {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        dst[0] = Expand(v & 31, 5); dst[1] = Expand((v >> 5) & 63, 6); dst[2] = Expand((v >> 11) & 31, 5); dst[3] = 255;
        break;
    }
    case Conv::R6G5B5:
    {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        dst[0] = Expand(v & 63, 6); dst[1] = Expand((v >> 6) & 31, 5); dst[2] = Expand((v >> 11) & 31, 5); dst[3] = 255;
        break;
    }
    case Conv::A1R5G5B5:
    {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        dst[0] = Expand(v & 31, 5); dst[1] = Expand((v >> 5) & 31, 5); dst[2] = Expand((v >> 10) & 31, 5); dst[3] = (v >> 15) ? 255 : 0;
        break;
    }
    case Conv::A4R4G4B4:
    {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        dst[0] = Expand(v & 15, 4); dst[1] = Expand((v >> 4) & 15, 4); dst[2] = Expand((v >> 8) & 15, 4); dst[3] = Expand((v >> 12) & 15, 4);
        break;
    }
    case Conv::Depth24:
    {
        uint32_t v;
        memcpy(&v, src, 4);
        const float d = float(v >> 8) / 16777215.0f;
        memcpy(dst, &d, 4);
        break;
    }
    case Conv::Yuv422Gbgr:   // Y1 Cr Y0 Cb (bytes from the LSB: Cb Y0 Cr Y1)
        YuvToRgb(src[1], src[0], src[2], dst);
        YuvToRgb(src[3], src[0], src[2], dst + 4);
        break;
    case Conv::Yuv422Bgrg:   // Cr Y1 Cb Y0 (bytes from the LSB: Y0 Cb Y1 Cr)
        YuvToRgb(src[0], src[1], src[3], dst);
        YuvToRgb(src[2], src[1], src[3], dst + 4);
        break;
    case Conv::Dxt3A:
        // 4x4 blocks of 4-bit alpha -> 16 bytes (block rows written by the caller)
        for (int i = 0; i < 16; i++)
        {
            const uint8_t nib = (src[i / 2] >> ((i & 1) * 4)) & 15;
            dst[i] = Expand(nib, 4);
        }
        break;
    }
}

bool PrepareGuestTexture2D(const TextureFetch& f, HostTextureData& out)
{
    const FormatInfo* fi = FindFormat(f.format);
    if (!fi)
        return false;
    const Layout l = ComputeLayout(f, *fi);
    // YUV 4:2:2: each 32-bit guest block becomes 2 pixels
    const bool yuv = fi->conv == Conv::Yuv422Gbgr || fi->conv == Conv::Yuv422Bgrg;
    const bool dxt3a = fi->conv == Conv::Dxt3A;

    out.format = fi->host;
    out.components = fi->components;
    out.width = yuv ? l.blocksX * 2 : f.width;
    out.height = f.height;
    if (dxt3a)
    {
        out.rowPitch = AlignUp(l.blocksX * 4, 256);
        out.rows = l.blocksY * 4;
    }
    else
    {
        out.rowPitch = AlignUp(l.blocksX * fi->hostBytesPerBlock, 256);   // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
        out.rows = l.blocksY;
    }
    return true;
}

void DecodeGuestTexture2D(const TextureFetch& f, const HostTextureData& meta, uint8_t* dst)
{
    const FormatInfo* fi = FindFormat(f.format);
    if (!fi)
        return;
    const Layout l = ComputeLayout(f, *fi);
    const uint32_t base = GpuPhysToGuest(f.baseAddress);
    const uint32_t readable = ReadableBytes(base, l.extent);
    const uint8_t* src = g_mem.base + base;
    const uint32_t bppLog2 = Log2(fi->bytesPerBlock);
    const bool dxt3a = fi->conv == Conv::Dxt3A;

    // The GPU endian swap works on memory words: it is equivalent to an XOR on each byte's address
    // (within groups of 4 bytes -> one pshufb per 16-byte block)
    static const uint32_t kByteXor[4] = { 0, 1, 3, 2 };
    const uint32_t bx0 = kByteXor[f.endian & 3];
    alignas(16) uint8_t maskBytes[16];
    for (uint32_t i = 0; i < 16; i++)
        maskBytes[i] = uint8_t(i ^ bx0);
    const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(maskBytes));

    // In 2D tiling, 16 consecutive bytes (8 for 1-byte formats) of consecutive blocks in x are contiguous and aligned
    const uint32_t bpb = fi->bytesPerBlock;
    const uint32_t runBytes = std::min(16u, 8u << bppLog2);
    const uint32_t perRun = runBytes / bpb;
    alignas(16) uint8_t run[16];
    uint8_t conv[16];
    for (uint32_t by = 0; by < l.blocksY; by++)
    {
        uint8_t* dstRow = dst + size_t(by) * meta.rowPitch * (dxt3a ? 4 : 1);
        for (uint32_t bx = 0; bx < l.blocksX; bx += perRun)
        {
            const uint32_t offset = f.tiled ? uint32_t(Tiled2D(int32_t(bx), int32_t(by), l.pitchBlocks, bppLog2))
                                            : by * l.rowPitchBytes + bx * bpb;
            const uint32_t n = std::min(perRun, l.blocksX - bx);
            if (offset + runBytes <= readable)
            {
                if (runBytes == 16)
                    _mm_store_si128(reinterpret_cast<__m128i*>(run),
                                    _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + offset)), mask));
                else
                    _mm_storel_epi64(reinterpret_cast<__m128i*>(run),
                                     _mm_shuffle_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + offset)), mask));
            }
            else
            {
                for (uint32_t k = 0; k < runBytes; k++)
                    run[k] = offset + k < readable ? src[(offset + k) ^ bx0] : 0;
            }
            if (fi->conv == Conv::Copy)
            {
                // the destination row is aligned to 256 bytes: the whole 16-byte block fits
                memcpy(dstRow + bx * bpb, run, runBytes);
                continue;
            }
            for (uint32_t i = 0; i < n; i++)
            {
                const uint8_t* block = run + i * bpb;
                if (dxt3a)
                {
                    ConvertBlock(*fi, block, conv);
                    for (int r = 0; r < 4; r++)
                        memcpy(dstRow + size_t(r) * meta.rowPitch + (bx + i) * 4, conv + r * 4, 4);
                }
                else
                {
                    ConvertBlock(*fi, block, dstRow + (bx + i) * fi->hostBytesPerBlock);
                }
            }
        }
    }
}

bool LoadGuestTexture2D(const TextureFetch& f, HostTextureData& out)
{
    if (!PrepareGuestTexture2D(f, out))
        return false;
    out.data.assign(size_t(out.rowPitch) * out.rows, 0);
    DecodeGuestTexture2D(f, out, out.data.data());
    return true;
}
