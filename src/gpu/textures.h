#pragma once
#include <cstdint>
#include <vector>

#include <dxgiformat.h>

// Fetch constant de textura decodificado (xe_gpu_texture_fetch_t)
struct TextureFetch
{
    uint32_t raw[6];
    uint32_t type;          // 2 = textura
    uint32_t format;        // xenos::TextureFormat
    uint32_t endian;
    bool tiled;
    uint32_t pitchTexels;
    uint32_t baseAddress;   // fisico
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
    uint32_t rowPitch = 0;      // bytes por linha (de blocos, para BC)
    uint32_t rows = 0;          // linhas (de blocos)
    uint32_t components = 4;    // componentes do formato guest (para o swizzle com replicacao)
    std::vector<uint8_t> data;
};

// Calcula a faixa de memoria guest ocupada pelo nivel base (para hash/invalidacao)
bool GuestTextureExtent(const TextureFetch& f, uint32_t& guestAddr, uint32_t& sizeBytes);
// Converte o nivel base para um formato do host. false se o formato nao e suportado.
bool LoadGuestTexture2D(const TextureFetch& f, HostTextureData& out);
// Em duas etapas (sem copia intermediaria): Prepare preenche formato/tamanho/rowPitch/rows (sem data);
// Decode escreve rowPitch*rows bytes em dst (pode ser o buffer de upload)
bool PrepareGuestTexture2D(const TextureFetch& f, HostTextureData& out);
void DecodeGuestTexture2D(const TextureFetch& f, const HostTextureData& meta, uint8_t* dst);
// Formato do host e numero de componentes para um formato Xenos (sem converter dados)
bool HostFormatForTexture(uint32_t xenosFormat, DXGI_FORMAT& fmt, uint32_t& components);
uint64_t HashGuestMemory(uint32_t guestAddr, uint32_t size);
// Offset em bytes do texel (x, y) numa superficie 2D com tiling do Xenos (pitch em texels, alinhado a 32)
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchTexels, uint32_t bppLog2);
