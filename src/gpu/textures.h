#pragma once
#include <cstdint>
#include <vector>

#include <dxgiformat.h>

// Decoded texture fetch constant (xe_gpu_texture_fetch_t)
struct TextureFetch
{
    uint32_t raw[6];
    uint32_t type;          // 2 = texture
    uint32_t format;        // xenos::TextureFormat
    uint32_t endian;
    bool tiled;
    uint32_t pitchTexels;
    uint32_t baseAddress;   // physical
    uint32_t width, height, depth;
    uint32_t dimension;     // 0 1D, 1 2D, 2 3D, 3 cube
    uint32_t swizzle;       // 12 bits
    uint32_t clampX, clampY, clampZ;
    uint32_t magFilter, minFilter, mipFilter, anisoFilter;
    uint32_t borderColor;
    uint32_t signX;
};
TextureFetch DecodeTextureFetch(const uint32_t* dwords);

struct HostTextureData
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    uint32_t rowPitch = 0;      // bytes per row (of blocks, for BC)
    uint32_t rows = 0;          // rows (of blocks)
    uint32_t components = 4;    // components of the guest format (for swizzle with replication)
    std::vector<uint8_t> data;
};

// Computes the guest memory range used by the base level (for hashing/invalidation)
bool GuestTextureExtent(const TextureFetch& f, uint32_t& guestAddr, uint32_t& sizeBytes);
// Converts the base level to a host format. false if the format is not supported.
bool LoadGuestTexture2D(const TextureFetch& f, HostTextureData& out);
// In two steps (no intermediate copy): Prepare fills format/size/rowPitch/rows (no data);
// Decode writes rowPitch*rows bytes into dst (may be the upload buffer)
bool PrepareGuestTexture2D(const TextureFetch& f, HostTextureData& out);
void DecodeGuestTexture2D(const TextureFetch& f, const HostTextureData& meta, uint8_t* dst);
// Host format and component count for a Xenos format (without converting data)
bool HostFormatForTexture(uint32_t xenosFormat, DXGI_FORMAT& fmt, uint32_t& components);
uint64_t HashGuestMemory(uint32_t guestAddr, uint32_t size);
// Byte offset of texel (x, y) in a 2D Xenos-tiled surface (pitch in texels, aligned to 32)
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchTexels, uint32_t bppLog2);
