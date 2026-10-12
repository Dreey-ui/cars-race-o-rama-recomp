#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Entrada de vertice de um vertex shader: semantica + de onde o vfetch le (fetch constant, offset, stride, formato)
struct ShaderVertexInput
{
    const char* semantic = "";
    uint32_t semanticIndex = 0;
    bool hlslUint = false;       // o HLSL gerado declara uint4 (NORMAL/TANGENT/BINORMAL/BLENDINDICES)
    uint32_t fetchSlot = 0;      // fetch constant de vertice (0..95)
    uint32_t offsetBytes = 0;
    uint32_t strideBytes = 0;
    uint32_t format = 0;         // xenos::VertexFormat
    bool isSigned = false;
    bool isInteger = false;      // num_format_all = 1 (nao normalizado)
    int32_t expAdjust = 0;       // resultado * 2^expAdjust (o XenosRecomp ignora)
    bool signedRf = false;       // signed_rf_mode_all
    uint32_t srcRegister = 0;    // registrador com o indice do vertice (r0.x = indice do vertice no inicio)
    uint32_t srcComponent = 0;
};

struct GuestShader
{
    uint64_t hash = 0;           // hash do microcodigo
    bool isPixel = false;
    std::string name;            // arquivo de origem (diagnostico)
    std::vector<uint8_t> container;
    std::vector<ShaderVertexInput> inputs;
    uint32_t samplerMask = 0;    // bit N = usa o sampler/fetch constant de textura N
    std::vector<uint32_t> boolRegs;  // registradores bool referenciados (indice no constant table)
    uint32_t specMask = 0;
    uint32_t swappedTexcoordMask = 0;   // TEXCOORDn com formato de 16 bits (componentes trocados pelo swap de 32 bits)
    bool usesPackedNormals = false;
    uint32_t psOutputs = 0;      // bits: oC0..oC3, 0x10 = oDepth

    // Dados do backend (input layout etc.), criados na primeira vez que o shader e usado
    void* backendData = nullptr;

    // VS de microcodigo cru (sem container no jogo): o container e sintetizado e o shader e compilado
    // por pixel shader (as saidas sao ligadas por indice aos interpoladores do PS)
    bool isRaw = false;
    std::vector<uint8_t> rawMicrocode;
    std::vector<std::pair<GuestShader*, GuestShader*>> linked;   // PS -> variante compilada

    // Resultado da compilacao (sob demanda, em threads de trabalho: o draw e pulado ate ficar pronto)
    std::atomic<uint8_t> state{ 0 };   // 0 = nao compilado, 1 = na fila, 2 = pronto (dxil ou failed)
    bool failed = false;
    std::vector<uint8_t> dxil;

    bool IsReady() const { return state.load(std::memory_order_acquire) == 2; }
};

// Indexa containers de shader do jogo (arquivos .vsh/.psh e containers embutidos na imagem do XEX)
void ShadersInit(const std::string& gameRoot, const uint8_t* image, size_t imageSize);

// Acha o shader cujo microcodigo (big-endian, como na memoria guest) e este; poe na fila de compilacao na
// primeira vez (veja GuestShader::IsReady). Retorna nullptr se o microcodigo nao corresponde a nenhum container
// conhecido (*pending = true enquanto a varredura da memoria guest em segundo plano ainda pode acha-lo).
GuestShader* ShadersFind(bool pixel, const uint8_t* microcode, uint32_t sizeBytes, bool* pending = nullptr);

// Para um VS cru: devolve a variante com as saidas ligadas aos interpoladores deste PS (compilada em segundo plano)
GuestShader* ShadersLinkRawVertexShader(GuestShader* rawVs, GuestShader* ps);

// Shaders compilados em segundo plano desde a ultima chamada (estatistica) e tempo gasto (ms)
void ShadersTakeStats(uint32_t& compiled, double& compileMs, uint32_t& scans, double& scanMs);

// Formato DXGI para uma entrada de vertice (dados com swap de 32 bits). intType: 0 = float, 1 = uint, 2 = sint
// (para entradas float4 no HLSL com formato inteiro nao normalizado, o shader e reescrito para converter).
uint32_t VertexInputDxgiFormat(const ShaderVertexInput& in, int& intType);

// Compila HLSL proprio do backend (blit, geometry shader de rect list) com o DXC
bool CompileHlsl(const char* source, const wchar_t* target, std::vector<uint8_t>& out);
