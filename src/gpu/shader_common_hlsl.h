// Cabecalho HLSL incluido no inicio de todo shader gerado pelo XenosRecomp (equivalente ao shader_common.h dele,
// adaptado: spec constants vindas do constant buffer e transformacao de posicao Xenos -> host por draw).
#pragma once

static const char kShaderCommonHlsl[] = R"HLSL(
#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

// O XenosRecomp so declara TEXCOORD0..15 e COLOR0..1 como interpoladores: FOG usa o TEXCOORD15
#define iFog0 iTexCoord15
#define oFog0 oTexCoord15

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_SpecConstantsValue : packoffset(c17.y); \
    uint g_AlphaFunc : packoffset(c17.z); \
    uint g_PosFlags : packoffset(c17.w); \
    float4 g_PosScaleOffset : packoffset(c18);

#define g_SpecConstants() g_SpecConstantsValue

// Posicao do VS (espaco de clip do Xenos) -> espaco de clip do host, com o viewport do host cobrindo o render target inteiro
// g_PosFlags bit 0: PA_CL_VTE_CNTL.vtx_xy_fmt (xy ja em coordenadas de tela, sem divisao por w): w passa a ser 1
#define XeTransformPos(p) (((g_PosFlags & 1) != 0) ? float4((p).xy * g_PosScaleOffset.xy + g_PosScaleOffset.zw, (p).z, 1.0) : float4((p).xy * g_PosScaleOffset.xy + g_PosScaleOffset.zw * (p).w, (p).zw))

// Alpha test do Xenos (RB_COLORCONTROL.alpha_func: 0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always)
#define XeAlphaPass(a) ((g_AlphaFunc == 7) || (g_AlphaFunc == 1 && (a) < g_AlphaThreshold) || \
    (g_AlphaFunc == 2 && (a) == g_AlphaThreshold) || (g_AlphaFunc == 3 && (a) <= g_AlphaThreshold) || \
    (g_AlphaFunc == 4 && (a) > g_AlphaThreshold) || (g_AlphaFunc == 5 && (a) != g_AlphaThreshold) || \
    (g_AlphaFunc == 6 && (a) >= g_AlphaThreshold))

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[NonUniformResourceIndex(resourceDescriptorIndex)];
    return texture.Sample(g_SamplerDescriptorHeap[NonUniformResourceIndex(samplerDescriptorIndex)], texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[NonUniformResourceIndex(resourceDescriptorIndex)];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[NonUniformResourceIndex(resourceDescriptorIndex)].Sample(
        g_SamplerDescriptorHeap[NonUniformResourceIndex(samplerDescriptorIndex)], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[NonUniformResourceIndex(resourceDescriptorIndex)].Sample(
        g_SamplerDescriptorHeap[NonUniformResourceIndex(samplerDescriptorIndex)], cubeMapData.cubeMapDirections[texCoord.z]);
}

// (macro: as spec constants ficam no cbuffer, declarado depois deste cabecalho)
#define tfetchR11G11B10(value) XeFetchR11G11B10((value), g_SpecConstants())
float4 XeFetchR11G11B10(uint4 value, uint specConstants)
{
    if (specConstants & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[NonUniformResourceIndex(resourceDescriptorIndex)]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}
)HLSL";
