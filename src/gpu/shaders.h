#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Vertex input of a vertex shader: semantic + where the vfetch reads from (fetch constant, offset, stride, format)
struct ShaderVertexInput
{
    const char* semantic = "";
    uint32_t semanticIndex = 0;
    bool hlslUint = false;       // the generated HLSL declares uint4 (NORMAL/TANGENT/BINORMAL/BLENDINDICES)
    uint32_t fetchSlot = 0;      // vertex fetch constant (0..95)
    uint32_t offsetBytes = 0;
    uint32_t strideBytes = 0;
    uint32_t format = 0;         // xenos::VertexFormat
    bool isSigned = false;
    bool isInteger = false;      // num_format_all = 1 (not normalized)
    int32_t expAdjust = 0;       // result * 2^expAdjust (XenosRecomp ignores it)
    bool signedRf = false;       // signed_rf_mode_all
    uint32_t srcRegister = 0;    // register holding the vertex index (r0.x = vertex index at the start)
    uint32_t srcComponent = 0;
};

struct GuestShader
{
    uint64_t hash = 0;           // microcode hash
    bool isPixel = false;
    std::string name;            // source file (diagnostics)
    std::vector<uint8_t> container;
    std::vector<ShaderVertexInput> inputs;
    uint32_t samplerMask = 0;    // bit N = uses texture sampler/fetch constant N
    std::vector<uint32_t> boolRegs;  // referenced bool registers (index in the constant table)
    uint32_t specMask = 0;
    uint32_t swappedTexcoordMask = 0;   // TEXCOORDn with a 16-bit format (components swapped by the 32-bit swap)
    bool usesPackedNormals = false;
    uint32_t psOutputs = 0;      // bits: oC0..oC3, 0x10 = oDepth

    // Backend data (input layout etc.), created the first time the shader is used
    void* backendData = nullptr;

    // Raw-microcode VS (no container in the game): the container is synthesized and the shader is compiled
    // per pixel shader (outputs are linked by index to the PS interpolators)
    bool isRaw = false;
    std::vector<uint8_t> rawMicrocode;
    std::vector<std::pair<GuestShader*, GuestShader*>> linked;   // PS -> compiled variant

    // Compilation result (on demand, on worker threads: the draw is skipped until it is ready)
    std::atomic<uint8_t> state{ 0 };   // 0 = not compiled, 1 = queued, 2 = ready (dxil or failed)
    bool failed = false;
    std::vector<uint8_t> dxil;

    bool IsReady() const { return state.load(std::memory_order_acquire) == 2; }
};

// Indexes the game's shader containers (.vsh/.psh files and containers embedded in the XEX image)
void ShadersInit(const std::string& gameRoot, const uint8_t* image, size_t imageSize);

// Finds the shader whose microcode (big-endian, as in guest memory) is this one; queues it for compilation the
// first time (see GuestShader::IsReady). Returns nullptr if the microcode matches no known container
// (*pending = true while the background guest memory scan may still find it).
GuestShader* ShadersFind(bool pixel, const uint8_t* microcode, uint32_t sizeBytes, bool* pending = nullptr);

// For a raw VS: returns the variant with outputs linked to this PS's interpolators (compiled in the background)
GuestShader* ShadersLinkRawVertexShader(GuestShader* rawVs, GuestShader* ps);

// Shaders compiled in the background since the last call (statistics) and time spent (ms)
void ShadersTakeStats(uint32_t& compiled, double& compileMs, uint32_t& scans, double& scanMs);

// DXGI format for a vertex input (data with 32-bit swap). intType: 0 = float, 1 = uint, 2 = sint
// (for float4 HLSL inputs with a non-normalized integer format, the shader is rewritten to convert).
uint32_t VertexInputDxgiFormat(const ShaderVertexInput& in, int& intType);

// Compiles the backend's own HLSL (blit, rect list geometry shader) with DXC
bool CompileHlsl(const char* source, const wchar_t* target, std::vector<uint8_t>& out);
