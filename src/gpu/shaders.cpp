// Shader cache: the game's Xenos containers -> HLSL (XenosRecomp) -> DXIL (dxcompiler.dll), on demand.
#include "xenos_recomp_pch.h"
#include "shader_recompiler.h"   // XenosRecomp
#include "shaders.h"
#include "shader_common_hlsl.h"

#include <windows.h>
#include <dxgiformat.h>
#include <dxcapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <immintrin.h>
#include <regex>
#include <thread>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

static std::mutex g_shaderMutex;
static std::unordered_map<uint64_t, GuestShader*> g_shaders[2];   // [isPixel] microcode hash -> shader
static DxcCreateInstanceProc g_dxcCreate = nullptr;
static IDxcCompiler3* GetDxc();
static fs::path g_cacheDir;

static uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

static uint64_t HashMicrocode(const uint8_t* code, uint32_t size) { return XXH3_64bits(code, size); }

// Validates a container (same criteria as XenosRecomp) and returns its total size
static size_t ContainerSize(const uint8_t* p, size_t avail)
{
    if (avail < sizeof(ShaderContainer))
        return 0;
    auto c = reinterpret_cast<const ShaderContainer*>(p);
    const size_t size = size_t(c->virtualSize) + size_t(c->physicalSize);
    // (XenosRecomp requires field1C == field20 == 0, but in this game's vertex shaders they are table offsets)
    if ((c->flags & 0xFFFFFF00) != 0x102A1100 || size > avail || size < sizeof(ShaderContainer) ||
        c->constantTableOffset == 0 || c->constantTableOffset >= c->virtualSize || c->shaderOffset == 0 ||
        c->shaderOffset >= c->virtualSize || c->field1C >= c->virtualSize || c->field20 >= c->virtualSize)
        return 0;
    // Consistent constant table (the memory scan finds garbage with a similar-looking header)
    const size_t ctOff = c->constantTableOffset;
    if (ctOff + 4 + sizeof(ConstantTable) > c->virtualSize)
        return 0;
    auto ctc = reinterpret_cast<const ConstantTableContainer*>(p + ctOff);
    const uint8_t* ctData = reinterpret_cast<const uint8_t*>(&ctc->constantTable);
    const size_t ctLimit = c->virtualSize - (ctOff + 4);
    const uint32_t nConst = ctc->constantTable.constants, infoOff = ctc->constantTable.constantInfo;
    if (nConst > 512 || uint64_t(infoOff) + uint64_t(nConst) * sizeof(ConstantInfo) > ctLimit)
        return 0;
    for (uint32_t i = 0; i < nConst; i++)
    {
        auto ci = reinterpret_cast<const ConstantInfo*>(ctData + infoOff + i * sizeof(ConstantInfo));
        const uint32_t name = ci->name;
        if (name >= ctLimit || uint32_t(ci->registerSet.get()) > 3 || ci->registerCount > 512)
            return 0;
        if (!memchr(ctData + name, 0, ctLimit - name))
            return 0;
    }
    // Shader inside the container
    auto sh = reinterpret_cast<const Shader*>(p + c->shaderOffset);
    if (c->shaderOffset + sizeof(Shader) > c->virtualSize || sh->size == 0 || sh->size > c->physicalSize ||
        uint64_t(sh->physicalOffset) + sh->size > c->physicalSize)
        return 0;
    return size;
}

static const char* const kSemantics[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
                                          "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };

static const char* const kVariables[] = { "Position", "BlendWeight", "BlendIndices", "Normal", "PointSize", "TexCoord",
                                          "Tangent", "Binormal", "TessFactor", "PositionT", "Color", "Fog", "Depth", "Sample" };

static bool IsFloatVertexFormat(uint32_t f) { return f == 31 || f == 32 || f == 36 || f == 37 || f == 38 || f == 57; }
static bool IsFloatBlendIndices(const ShaderVertexInput& in)
{
    return in.hlslUint && strcmp(in.semantic, "BLENDINDICES") == 0 && IsFloatVertexFormat(in.format);
}

uint32_t VertexInputDxgiFormat(const ShaderVertexInput& in, int& intType)
{
    // intType: type the HLSL must declare. hlslUint -> 1; non-normalized integer -> 1/2; otherwise 0
    const bool integer = in.isInteger;
    const bool s = in.isSigned;
    // BLENDINDICES: XenosRecomp declares uint4, but here the indices come as float (32_32_32_32_FLOAT etc.):
    // the input becomes float4 (RewriteIntegerInputs changes the declaration) and the shader gets the value, not the bits
    if (IsFloatBlendIndices(in))
    {
        intType = 0;
        switch (in.format)
        {
        case 36: return DXGI_FORMAT_R32_FLOAT;
        case 37: return DXGI_FORMAT_R32G32_FLOAT;
        case 57: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 31: return DXGI_FORMAT_R16G16_FLOAT;
        case 32: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
    }
    auto pick = [&](DXGI_FORMAT unorm, DXGI_FORMAT snorm, DXGI_FORMAT uint_, DXGI_FORMAT sint_) -> uint32_t {
        if (in.hlslUint) { intType = 1; return s ? sint_ : uint_; }
        if (integer) { intType = s ? 2 : 1; return s ? sint_ : uint_; }
        intType = 0;
        return s ? snorm : unorm;
    };
    switch (in.format)
    {
    case 6:  return pick(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT);
    case 7:
        if (in.hlslUint) { intType = 1; return DXGI_FORMAT_R32_UINT; }
        intType = 0;
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 16: case 17:
        intType = in.hlslUint ? 1 : 0;
        return in.hlslUint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R11G11B10_FLOAT;
    case 25: return pick(DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT);
    case 26: return pick(DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT);
    case 31: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16G16_FLOAT;
    case 32: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 33: intType = s ? 2 : 1; return s ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
    case 34: intType = s ? 2 : 1; return s ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32G32_UINT;
    case 35: intType = s ? 2 : 1; return s ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_UINT;
    case 36: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
    case 37: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT;
    case 57: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT;
    case 38: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: intType = in.hlslUint ? 1 : 0; return in.hlslUint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
    }
}

// float4 HLSL inputs fed by an integer format: declare int4/uint4 and convert at the start of main
static void RewriteIntegerInputs(const GuestShader& s, std::string& hlsl)
{
    std::string conversions;
    for (const auto& in : s.inputs)
    {
        if (IsFloatBlendIndices(in))
        {
            // uint4 -> float4 (the indices come as float)
            const std::string from = fmt::format("in uint4 iBlendIndices{} : BLENDINDICES{},", in.semanticIndex, in.semanticIndex);
            const size_t pos = hlsl.find(from);
            if (pos != std::string::npos)
                hlsl.replace(pos, from.size(), fmt::format("in float4 iBlendIndices{} : BLENDINDICES{},", in.semanticIndex, in.semanticIndex));
            continue;
        }
        int intType = 0;
        VertexInputDxgiFormat(in, intType);
        if (in.hlslUint || intType == 0)
            continue;
        uint32_t usage = 0;
        while (usage < 14 && strcmp(kSemantics[usage], in.semantic) != 0)
            usage++;
        if (usage >= 14)
            continue;
        const std::string var = fmt::format("i{}{}", kVariables[usage], in.semanticIndex);
        const std::string from = fmt::format("in float4 {} : {}{},", var, in.semantic, in.semanticIndex);
        const size_t pos = hlsl.find(from);
        if (pos == std::string::npos)
            continue;
        hlsl.replace(pos, from.size(), fmt::format("in {} {}_raw : {}{},", intType == 2 ? "int4" : "uint4", var, in.semantic, in.semanticIndex));
        conversions += fmt::format("\tfloat4 {0} = float4({0}_raw);\n", var);
    }
    if (conversions.empty())
        return;
    const size_t mainPos = hlsl.find("void main(");
    const size_t body = hlsl.find(")\n{\n", mainPos);
    if (body != std::string::npos)
        hlsl.insert(body + 4, conversions);
}

static bool IsUintUsage(uint32_t usage)
{
    // same criteria as XenosRecomp's USAGE_TYPES
    return usage == 2 || usage == 3 || usage == 6 || usage == 7;
}

static bool Is16BitVertexFormat(uint32_t f) { return f == 25 || f == 26 || f == 31 || f == 32; }

static void ParseVertexInputs(GuestShader& s, const uint8_t* code, uint32_t codeSize);
static std::unordered_map<uint32_t, std::vector<GuestShader*>> g_vsBySize;   // microcode size -> VS

// Reads the container metadata (vertex inputs, samplers, bools)
static void ParseContainer(GuestShader& s)
{
    const uint8_t* data = s.container.data();
    auto container = reinterpret_cast<const ShaderContainer*>(data);
    s.isPixel = (container->flags & 1) == 0;

    auto ctc = reinterpret_cast<const ConstantTableContainer*>(data + container->constantTableOffset);
    const uint8_t* ctData = reinterpret_cast<const uint8_t*>(&ctc->constantTable);
    for (uint32_t i = 0; i < ctc->constantTable.constants; i++)
    {
        auto ci = reinterpret_cast<const ConstantInfo*>(ctData + ctc->constantTable.constantInfo + i * sizeof(ConstantInfo));
        if (ci->registerSet == RegisterSet::Sampler)
        {
            for (uint32_t j = 0; j < ci->registerCount && ci->registerIndex + j < 32; j++)
                s.samplerMask |= 1u << (ci->registerIndex + j);
        }
        else if (ci->registerSet == RegisterSet::Bool)
        {
            for (uint32_t j = 0; j < std::max<uint32_t>(1, ci->registerCount); j++)
                s.boolRegs.push_back(ci->registerIndex + j);
        }
    }

    if (s.isPixel)
    {
        s.psOutputs = uint32_t(reinterpret_cast<const PixelShader*>(data + container->shaderOffset)->outputs.get());
        return;
    }

    auto shader = reinterpret_cast<const VertexShader*>(data + container->shaderOffset);
    ParseVertexInputs(s, data + container->virtualSize + shader->physicalOffset, shader->size);
}

// Addresses (in instructions) of the vfetches, in program order
static std::vector<uint32_t> VertexFetchSlots(const GuestShader& s)
{
    auto container = reinterpret_cast<const ShaderContainer*>(s.container.data());
    auto shader = reinterpret_cast<const VertexShader*>(s.container.data() + container->shaderOffset);
    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < shader->vertexElementCount; i++)
        slots.push_back(shader->vertexElementsAndInterpolators[shader->field18 + i] & 0xFFF);
    return slots;
}

// Vertex inputs from the microcode (the 360's D3D rewrites the vfetches with the vertex declaration,
// so the microcode in guest memory is the source of truth for format/offset/stride)
static void ParseVertexInputs(GuestShader& s, const uint8_t* code, uint32_t codeSize)
{
    s.inputs.clear();
    s.swappedTexcoordMask = 0;
    s.usesPackedNormals = false;
    auto container = reinterpret_cast<const ShaderContainer*>(s.container.data());
    auto shader = reinterpret_cast<const VertexShader*>(s.container.data() + container->shaderOffset);

    struct Elem { uint32_t address, usage, usageIndex; };
    std::vector<Elem> elems;
    for (uint32_t i = 0; i < shader->vertexElementCount; i++)
    {
        const uint32_t v = shader->vertexElementsAndInterpolators[shader->field18 + i];
        elems.push_back({ v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF });
    }
    std::sort(elems.begin(), elems.end(), [](const Elem& a, const Elem& b) { return a.address < b.address; });

    // A "mini" vfetch reuses the fetch constant and stride of the last full vfetch
    uint32_t lastSlot = 0, lastStride = 0;
    for (const auto& e : elems)
    {
        if ((e.address + 1) * 12 > codeSize)
            continue;
        const uint8_t* ins = code + e.address * 12;
        const uint32_t d0 = BE32(ins), d1 = BE32(ins + 4), d2 = BE32(ins + 8);

        ShaderVertexInput in;
        in.semantic = e.usage < 14 ? kSemantics[e.usage] : "TEXCOORD";
        in.semanticIndex = e.usageIndex;
        in.hlslUint = IsUintUsage(e.usage);
        const bool mini = (d1 >> 30) & 1;
        if (!mini)
        {
            lastSlot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
            lastStride = (d2 & 0xFF) * 4;
        }
        in.fetchSlot = lastSlot;
        in.strideBytes = lastStride;
        in.offsetBytes = uint32_t((int32_t(d2 << 1) >> 9)) * 4;   // offset: signed bits 8..30
        in.format = (d1 >> 16) & 0x3F;
        in.isSigned = (d1 >> 12) & 1;
        in.isInteger = (d1 >> 13) & 1;
        in.signedRf = (d1 >> 14) & 1;
        in.expAdjust = int32_t(d1 << 2) >> 26;   // signed bits 24..29
        in.srcRegister = (d0 >> 5) & 0x3F;
        in.srcComponent = (d0 >> 30) & 3;
        if (e.usage == 5 && Is16BitVertexFormat(in.format))
            s.swappedTexcoordMask |= 1u << e.usageIndex;
        if ((e.usage == 3 || e.usage == 6 || e.usage == 7) && (in.format == 16 || in.format == 17))
            s.usesPackedNormals = true;
        s.inputs.push_back(in);
    }
}

static void AddContainer(const uint8_t* p, size_t size, const std::string& name)
{
    auto* s = new GuestShader();
    s->container.assign(p, p + size);
    s->name = name;
    auto container = reinterpret_cast<const ShaderContainer*>(s->container.data());
    auto shader = reinterpret_cast<const Shader*>(s->container.data() + container->shaderOffset);
    const size_t codeOffset = size_t(container->virtualSize) + shader->physicalOffset;
    if (codeOffset + shader->size > size || shader->size == 0)
    {
        delete s;
        return;
    }
    s->hash = HashMicrocode(s->container.data() + codeOffset, shader->size);
    ParseContainer(*s);
    auto& map = g_shaders[s->isPixel ? 1 : 0];
    if (!map.emplace(s->hash, s).second)
    {
        delete s;   // duplicate
        return;
    }
    if (!s->isPixel)
        g_vsBySize[shader->size].push_back(s);
}

// Is the VS microcode the same as the container's, except in the vfetch slots (rewritten by D3D)?
static bool MatchesIgnoringFetches(const GuestShader& s, const uint8_t* code, uint32_t size)
{
    auto container = reinterpret_cast<const ShaderContainer*>(s.container.data());
    auto shader = reinterpret_cast<const Shader*>(s.container.data() + container->shaderOffset);
    const uint8_t* ref = s.container.data() + container->virtualSize + shader->physicalOffset;
    std::vector<bool> skip(size / 12 + 1, false);
    for (uint32_t slot : VertexFetchSlots(s))
        if (slot < skip.size())
            skip[slot] = true;
    // The first dwords are control flow instructions (not fetches): compare everything except the vfetch slots
    for (uint32_t i = 0; i < size; i += 4)
    {
        if (skip[i / 12])
            continue;
        if (memcmp(ref + i, code + i, std::min<uint32_t>(4, size - i)) != 0)
            return false;
    }
    return true;
}

static void ScanBuffer(const uint8_t* data, size_t size, const std::string& name)
{
    for (size_t i = 0; i + sizeof(ShaderContainer) < size;)
    {
        const size_t cs = ContainerSize(data + i, size - i);
        if (cs)
        {
            AddContainer(data + i, cs, name);
            i += cs;
        }
        else
        {
            i += 4;
        }
    }
}

static bool InitDxc()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const fs::path dir = fs::path(exePath).parent_path();
    HMODULE dll = LoadLibraryW((dir / L"dxcompiler.dll").c_str());
    if (!dll)
        dll = LoadLibraryW(L"dxcompiler.dll");
    if (!dll)
    {
        fprintf(stderr, "[shaders] dxcompiler.dll not found (copy it from XenosRecomp/thirdparty/dxc-bin/bin/x64)\n");
        return false;
    }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
    IDxcCompiler3* probe = nullptr;
    if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&probe))))
    {
        fprintf(stderr, "[shaders] failed to create the DXC compiler\n");
        return false;
    }
    probe->Release();
    g_dxcCreate = create;
    g_cacheDir = dir / L"shader_cache";
    fs::create_directories(g_cacheDir);
    return true;
}

void ShadersInit(const std::string& gameRoot, const uint8_t* image, size_t imageSize)
{
    InitDxc();
    size_t files = 0;
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(fs::path(gameRoot) / "C" / "Shaders", ec))
    {
        if (!entry.is_regular_file())
            continue;
        FILE* f = _wfopen(entry.path().c_str(), L"rb");
        if (!f)
            continue;
        std::vector<uint8_t> buf(size_t(entry.file_size()));
        fread(buf.data(), 1, buf.size(), f);
        fclose(f);
        ScanBuffer(buf.data(), buf.size(), entry.path().filename().string());
        files++;
    }
    if (image)
        ScanBuffer(image, imageSize, "<xex>");
    fprintf(stderr, "[shaders] %zu files read; %zu vertex shaders and %zu pixel shaders indexed\n",
            files, g_shaders[0].size(), g_shaders[1].size());
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}

static bool LoadFile(const fs::path& p, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    out.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

static void SaveFile(const fs::path& p, const void* data, size_t size)
{
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f)
        return;
    fwrite(data, 1, size, f);
    fclose(f);
}

// Generator version: change it when the translation changes, to invalidate the disk cache
static constexpr uint32_t kShaderGenVersion = 9;

static std::string CacheBaseName(const GuestShader& s)
{
    char base[64];
    snprintf(base, sizeof(base), "%c_%016llX_v%u", s.isPixel ? 'p' : 'v', (unsigned long long)s.hash, kShaderGenVersion);
    return base;
}

static bool LoadCachedDxil(GuestShader& s)
{
    return LoadFile(g_cacheDir / (CacheBaseName(s) + ".dxil"), s.dxil) && !s.dxil.empty();
}

static void Compile(GuestShader& s)
{
    const std::string baseName = CacheBaseName(s);
    const char* base = baseName.c_str();
    const fs::path dxilPath = g_cacheDir / (baseName + ".dxil");
    if (LoadFile(dxilPath, s.dxil) && !s.dxil.empty())
        return;

    ShaderRecompiler recompiler;
    recompiler.recompile(s.container.data(), std::string_view(kShaderCommonHlsl));
    std::string hlsl = std::move(recompiler.out);
    s.specMask = recompiler.specConstantsMask;

    // Adjustments for the backend: per-draw position transform and alpha test with a function
    ReplaceAll(hlsl, "oPos.xy += g_HalfPixelOffset * oPos.w;", "oPos = XeTransformPos(oPos);");
    ReplaceAll(hlsl, "clip(oC0.w - g_AlphaThreshold);", "if (!XeAlphaPass(oC0.w)) discard;");
    // vfetch/tfetch with no component written (prefetch): XenosRecomp generates "rN. = x.;" -- remove it
    {
        static const std::regex emptyAssign(R"(\n\s*r\d+\.\s*=\s*[^;\n]*\.\s*\)*\s*;)");
        hlsl = std::regex_replace(hlsl, emptyAssign, "");
    }
    if (!s.isPixel)
    {
        RewriteIntegerInputs(s, hlsl);
        // without UNLEASHED_RECOMP, XenosRecomp does not zero oPos (shaders that write the position in parts fail validation)
        const size_t mainPos = hlsl.find("void main(");
        const size_t body = hlsl.find(")\n{\n", mainPos);
        if (body != std::string::npos)
            hlsl.insert(body + 4, "\toPos = 0.0;\n");
    }
    {
        const std::string withName = "// source: " + s.name + "\n" + hlsl;
        SaveFile(g_cacheDir / (std::string(base) + ".hlsl"), withName.data(), withName.size());
    }

    IDxcCompiler3* dxc = GetDxc();
    if (!dxc)
    {
        s.failed = true;
        return;
    }
    DxcBuffer src{ hlsl.data(), hlsl.size(), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-T", s.isPixel ? L"ps_6_0" : L"vs_6_0", L"-HV", L"2021", L"-all-resources-bound",
                              L"-Wno-ignored-attributes", L"-Qstrip_debug", L"-Qstrip_reflect", L"-O3" };
    IDxcResult* result = nullptr;
    HRESULT hr = dxc->Compile(&src, args, UINT(std::size(args)), nullptr, IID_PPV_ARGS(&result));
    HRESULT status = E_FAIL;
    if (SUCCEEDED(hr) && result)
        result->GetStatus(&status);
    if (FAILED(hr) || FAILED(status))
    {
        s.failed = true;
        if (result && result->HasOutput(DXC_OUT_ERRORS))
        {
            IDxcBlobUtf8* errors = nullptr;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            if (errors)
            {
                fprintf(stderr, "[shaders] error compiling %s (%s):\n%.2000s\n", base, s.name.c_str(), errors->GetStringPointer());
                errors->Release();
            }
        }
        if (result)
            result->Release();
        return;
    }
    IDxcBlob* object = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (object)
    {
        auto p = static_cast<const uint8_t*>(object->GetBufferPointer());
        s.dxil.assign(p, p + object->GetBufferSize());
        object->Release();
        SaveFile(dxilPath, s.dxil.data(), s.dxil.size());
    }
    else
    {
        s.failed = true;
    }
    result->Release();
}

// ---------------------------------------------------------------------------------------------
// Unknown shader: look for the container in guest memory (shaders loaded from game packages or built
// at runtime). The scan runs on its own thread, only over the allocated ranges of the guest heaps + image, and the draw
// that uses the shader is skipped until it finishes (it used to scan ~1.5 GB on the GPU thread for every new shader).
extern uint8_t* GuestMemoryBase();
extern void GuestAllocatedRanges(std::vector<std::pair<uint32_t, uint32_t>>& out);   // memory.cpp
static std::unordered_map<uint64_t, uint32_t> g_missNeedScan;   // hash -> scan that must finish (g_shaderMutex)
static std::mutex g_scanMutex;
static std::condition_variable g_scanCv;
static bool g_scanRequested = false;
static std::atomic<uint32_t> g_scanStarted{ 0 }, g_scanDone{ 0 };
static std::atomic<uint32_t> g_statScans{ 0 }, g_statCompiled{ 0 };
static std::atomic<uint64_t> g_statScanUs{ 0 }, g_statCompileUs{ 0 };

// Positions (multiples of 4) with the magic 0x102A11xx (big-endian) in [p, p+size)
static void FindMagic(const uint8_t* p, size_t size, std::vector<size_t>& hits)
{
    const __m128i mask = _mm_set1_epi32(0x00FFFFFF), magic = _mm_set1_epi32(0x00112A10);
    size_t i = 0;
    for (; i + 64 <= size; i += 64)
    {
        const __m128i a = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i)), mask), magic);
        const __m128i b = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 16)), mask), magic);
        const __m128i c = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 32)), mask), magic);
        const __m128i d = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 48)), mask), magic);
        if (!_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d))))
            continue;
        for (size_t k = 0; k < 64; k += 4)
            if ((p[i + k] == 0x10) & (p[i + k + 1] == 0x2A) & (p[i + k + 2] == 0x11))
                hits.push_back(i + k);
    }
    for (; i + 4 <= size; i += 4)
        if (p[i] == 0x10 && p[i + 1] == 0x2A && p[i + 2] == 0x11)
            hits.push_back(i);
}

static void ScanGuestMemoryForContainers()
{
    uint8_t* base = GuestMemoryBase();
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    ranges.emplace_back(0x82000000u, 0x7D0000u);   // XEX image (.data/.bss)
    GuestAllocatedRanges(ranges);
    // Copy the containers found (without locking the shaders: the GPU keeps running during the scan)
    std::vector<std::vector<uint8_t>> found;
    std::vector<size_t> hits;
    uint64_t bytes = 0;
    for (const auto& [addr, size] : ranges)
    {
        const uint8_t* p = base + addr;
        hits.clear();
        FindMagic(p, size, hits);
        bytes += size;
        for (size_t off : hits)
        {
            const size_t cs = ContainerSize(p + off, size - off);
            if (cs)
                found.emplace_back(p + off, p + off + cs);
        }
    }
    std::lock_guard<std::mutex> lock(g_shaderMutex);
    const size_t before = g_shaders[0].size() + g_shaders[1].size();
    for (const auto& c : found)
        if (ContainerSize(c.data(), c.size()) == c.size())   // revalidate (memory may have changed during the copy)
            AddContainer(c.data(), c.size(), "<guest memory>");
    fprintf(stderr, "[shaders] guest memory scan (%.0f MB in %zu ranges): %zu new containers\n", bytes / 1048576.0,
            ranges.size(), g_shaders[0].size() + g_shaders[1].size() - before);
}

static void ScanThread()
{
    SetThreadDescription(GetCurrentThread(), L"shader scan");
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(g_scanMutex);
            g_scanCv.wait(lock, [] { return g_scanRequested; });
            g_scanRequested = false;
        }
        const uint32_t gen = g_scanStarted.fetch_add(1) + 1;
        const auto t0 = std::chrono::steady_clock::now();
        ScanGuestMemoryForContainers();
        g_statScanUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        g_statScans++;
        g_scanDone.store(gen, std::memory_order_release);
    }
}

static void RequestScan()
{
    static std::once_flag started;
    std::call_once(started, [] { std::thread(ScanThread).detach(); });
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_scanRequested = true;
    }
    g_scanCv.notify_one();
}

// ---------------------------------------------------------------------------------------------
// Compilation on worker threads (one IDxcCompiler3 per thread: DXC is not thread-safe per instance)

static IDxcCompiler3* GetDxc()
{
    thread_local IDxcCompiler3* compiler = nullptr;
    if (!compiler && g_dxcCreate)
        g_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    return compiler;
}

static std::mutex g_jobMutex;
static std::condition_variable g_jobCv;
static std::deque<GuestShader*> g_jobs;

static void CompileWorker()
{
    SetThreadDescription(GetCurrentThread(), L"shader compiler");
    for (;;)
    {
        GuestShader* s = nullptr;
        {
            std::unique_lock<std::mutex> lock(g_jobMutex);
            g_jobCv.wait(lock, [] { return !g_jobs.empty(); });
            s = g_jobs.front();
            g_jobs.pop_front();
        }
        const auto t0 = std::chrono::steady_clock::now();
        Compile(*s);
        g_statCompileUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        g_statCompiled++;
        s->state.store(2, std::memory_order_release);
    }
}

// Queues the shader (once). If the DXIL is already in the disk cache, loads it right away (fast, no skipped draw).
static void QueueCompile(GuestShader* s)
{
    uint8_t expected = 0;
    if (!s->state.compare_exchange_strong(expected, 1))
        return;
    if (LoadCachedDxil(*s))
    {
        s->state.store(2, std::memory_order_release);
        return;
    }
    static std::once_flag started;
    std::call_once(started, [] {
        const unsigned n = std::clamp(std::thread::hardware_concurrency() / 3, 2u, 4u);
        for (unsigned i = 0; i < n; i++)
            std::thread(CompileWorker).detach();
    });
    {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        g_jobs.push_back(s);
    }
    g_jobCv.notify_one();
}

void ShadersTakeStats(uint32_t& compiled, double& compileMs, uint32_t& scans, double& scanMs)
{
    compiled = g_statCompiled.exchange(0);
    compileMs = g_statCompileUs.exchange(0) / 1000.0;
    scans = g_statScans.exchange(0);
    scanMs = g_statScanUs.exchange(0) / 1000.0;
}

bool CompileHlsl(const char* source, const wchar_t* target, std::vector<uint8_t>& out)
{
    IDxcCompiler3* dxc = GetDxc();
    if (!dxc)
        return false;
    DxcBuffer src{ source, strlen(source), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-T", target, L"-HV", L"2021", L"-O3", L"-Qstrip_debug" };
    IDxcResult* result = nullptr;
    HRESULT status = E_FAIL;
    if (SUCCEEDED(dxc->Compile(&src, args, UINT(std::size(args)), nullptr, IID_PPV_ARGS(&result))) && result)
        result->GetStatus(&status);
    bool ok = false;
    if (result && SUCCEEDED(status))
    {
        IDxcBlob* object = nullptr;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (object)
        {
            auto p = static_cast<const uint8_t*>(object->GetBufferPointer());
            out.assign(p, p + object->GetBufferSize());
            object->Release();
            ok = true;
        }
    }
    else if (result && result->HasOutput(DXC_OUT_ERRORS))
    {
        IDxcBlobUtf8* errors = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors)
        {
            fprintf(stderr, "[shaders] error in internal shader:\n%s\n", errors->GetStringPointer());
            errors->Release();
        }
    }
    if (result)
        result->Release();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Containers synthesized for raw microcode (internal shaders without a container in the game)

// Walks the control flow: vfetch slots and texture fetch constants used
static void CollectFetches(const uint8_t* code, uint32_t size, std::vector<uint32_t>& vfetchSlots, uint32_t& samplerMask)
{
    uint32_t cfEnd = size;
    for (uint32_t off = 0; off + 12 <= cfEnd && off + 12 <= size; off += 12)
    {
        const uint32_t w0 = BE32(code + off), w1 = BE32(code + off + 4), w2 = BE32(code + off + 8);
        const uint32_t cf[2][2] = { { w0, w1 & 0xFFFF }, { (w1 >> 16) | (w2 << 16), w2 >> 16 } };
        for (const auto& c : cf)
        {
            const uint32_t op = (c[1] >> 12) & 0xF;
            const bool isExec = op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 13 || op == 14;
            if (!isExec)
                continue;
            const uint32_t addr = c[0] & 0xFFF, count = (c[0] >> 12) & 7, seq = (c[0] >> 16) & 0xFFF;
            if (addr)
                cfEnd = std::min(cfEnd, addr * 12);
            for (uint32_t i = 0; i < count; i++)
            {
                if (!((seq >> (2 * i)) & 1))
                    continue;
                const uint32_t slot = addr + i;
                if ((slot + 1) * 12 > size)
                    continue;
                const uint32_t d0 = BE32(code + slot * 12);
                if ((d0 & 0x1F) == 0)
                    vfetchSlots.push_back(slot);
                else if ((d0 & 0x1F) == 1)
                    samplerMask |= 1u << ((d0 >> 20) & 0x1F);
            }
        }
    }
    std::sort(vfetchSlots.begin(), vfetchSlots.end());
    vfetchSlots.erase(std::unique(vfetchSlots.begin(), vfetchSlots.end()), vfetchSlots.end());
}

static void PutBE32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v[off] = uint8_t(x >> 24); v[off + 1] = uint8_t(x >> 16); v[off + 2] = uint8_t(x >> 8); v[off + 3] = uint8_t(x);
}
static void PutBE16(std::vector<uint8_t>& v, size_t off, uint16_t x)
{
    v[off] = uint8_t(x >> 8); v[off + 1] = uint8_t(x);
}

// interp[i] = Interpolator word (usageIndex | usage << 4 | reg << 8) of export i
static std::vector<uint8_t> BuildVertexContainer(const uint8_t* code, uint32_t size, const uint32_t interp[16])
{
    std::vector<uint32_t> vfetch;
    uint32_t samplerMask = 0;
    CollectFetches(code, size, vfetch, samplerMask);

    struct Const { const char* name; uint16_t set, index, count; };
    std::vector<Const> consts = { { "C", 2 /*Float4*/, 0, 256 } };
    static const char* const kSamplerNames[16] = { "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15" };
    for (uint32_t i = 0; i < 16; i++)
        if (samplerMask & (1u << i))
            consts.push_back({ kSamplerNames[i], 3 /*Sampler*/, uint16_t(i), 1 });

    std::vector<uint8_t> out(0x1000, 0);
    // Constant table
    const size_t ctc = 36;                 // ConstantTableContainer
    const size_t ct = ctc + 4;             // ConstantTable (base of the offsets)
    const size_t infos = ct + 28;
    size_t strings = infos + consts.size() * 20;
    PutBE32(out, ct + 12, uint32_t(consts.size()));
    PutBE32(out, ct + 16, uint32_t(infos - ct));
    for (size_t i = 0; i < consts.size(); i++)
    {
        const size_t ci = infos + i * 20;
        PutBE32(out, ci, uint32_t(strings - ct));
        PutBE16(out, ci + 4, consts[i].set);
        PutBE16(out, ci + 6, consts[i].index);
        PutBE16(out, ci + 8, consts[i].count);
        const size_t len = strlen(consts[i].name) + 1;
        memcpy(&out[strings], consts[i].name, len);
        strings += len;
    }
    PutBE32(out, ctc, uint32_t(strings - ct));
    // Shader + VertexShader
    const size_t sh = (strings + 15) & ~size_t(15);
    PutBE32(out, sh + 0, 0);              // physicalOffset
    PutBE32(out, sh + 4, size);
    PutBE32(out, sh + 20, 16u << 5);      // 16 interpolators
    PutBE32(out, sh + 24, 0);             // field18
    PutBE32(out, sh + 28, uint32_t(vfetch.size()));
    size_t w = sh + 36;
    for (size_t i = 0; i < vfetch.size(); i++, w += 4)
        PutBE32(out, w, vfetch[i] | (5u << 12) | (uint32_t(i & 15) << 16));   // TEXCOORDi
    for (int i = 0; i < 16; i++, w += 4)
        PutBE32(out, w, interp[i]);
    const size_t virtualSize = (w + 15) & ~size_t(15);
    out.resize(virtualSize);
    out.insert(out.end(), code, code + size);
    // Header
    PutBE32(out, 0, 0x102A1101);          // vertex shader
    PutBE32(out, 4, uint32_t(virtualSize));
    PutBE32(out, 8, size);
    PutBE32(out, 16, uint32_t(ctc));
    PutBE32(out, 24, uint32_t(sh));
    return out;
}

GuestShader* ShadersLinkRawVertexShader(GuestShader* raw, GuestShader* ps)
{
    std::lock_guard<std::mutex> lock(g_shaderMutex);
    for (auto& [p, v] : raw->linked)
        if (p == ps)
            return v;
    // PS interpolators: register -> semantic (on Xenos, VS export i arrives in PS register i)
    uint32_t interp[16];
    bool usedTex[16] = {};
    bool assigned[16] = {};
    {
        auto c = reinterpret_cast<const ShaderContainer*>(ps->container.data());
        auto p = reinterpret_cast<const PixelShader*>(ps->container.data() + c->shaderOffset);
        const uint32_t n = (p->interpolatorInfo >> 5) & 0x1F;
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t v = p->interpolators[i];
            const uint32_t reg = (v >> 8) & 0xF;
            interp[reg] = (v & 0xFF) | (reg << 8);
            assigned[reg] = true;
            if (((v >> 4) & 0xF) == 5)
                usedTex[v & 0xF] = true;
        }
    }
    for (uint32_t i = 0, free = 15; i < 16; i++)
    {
        if (assigned[i])
            continue;
        while (free > 0 && usedTex[free])
            free--;
        interp[i] = free | (5u << 4) | (i << 8);   // free TEXCOORD (output the PS does not read)
        usedTex[free] = true;
    }
    auto* v = new GuestShader();
    v->container = BuildVertexContainer(raw->rawMicrocode.data(), uint32_t(raw->rawMicrocode.size()), interp);
    v->hash = raw->hash ^ (ps->hash * 0x9E3779B97F4A7C15ull);
    v->name = "raw:" + ps->name;
    ParseContainer(*v);
    raw->linked.push_back({ ps, v });
    QueueCompile(v);
    return v;
}

// VS variant with vfetches rewritten by the vertex declaration: creates its own shader for it
static GuestShader* FindVertexVariant(const uint8_t* microcode, uint32_t sizeBytes, uint64_t h)
{
    auto bySize = g_vsBySize.find(sizeBytes);
    if (bySize == g_vsBySize.end())
        return nullptr;
    GuestShader* base = nullptr;
    for (GuestShader* c : bySize->second)
        if (MatchesIgnoringFetches(*c, microcode, sizeBytes))
        {
            base = c;
            break;
        }
    if (!base)
        return nullptr;
    auto* v = new GuestShader();
    v->hash = h;
    v->isPixel = false;
    v->name = base->name + "*";
    v->container = base->container;
    {
        // The variant's microcode replaces the container's (XenosRecomp translates the rewritten vfetches)
        auto c = reinterpret_cast<const ShaderContainer*>(v->container.data());
        auto sh = reinterpret_cast<const Shader*>(v->container.data() + c->shaderOffset);
        memcpy(v->container.data() + c->virtualSize + sh->physicalOffset, microcode, sizeBytes);
    }
    v->samplerMask = base->samplerMask;
    v->boolRegs = base->boolRegs;
    ParseVertexInputs(*v, microcode, sizeBytes);
    g_shaders[0].emplace(h, v);
    return v;
}

GuestShader* ShadersFind(bool pixel, const uint8_t* microcode, uint32_t sizeBytes, bool* pending)
{
    if (pending)
        *pending = false;
    const uint64_t h = HashMicrocode(microcode, sizeBytes);
    std::lock_guard<std::mutex> lock(g_shaderMutex);
    auto& map = g_shaders[pixel ? 1 : 0];
    auto lookup = [&]() -> GuestShader* {
        auto it = map.find(h);
        if (it != map.end())
            return it->second;
        return pixel ? nullptr : FindVertexVariant(microcode, sizeBytes, h);
    };
    GuestShader* s = lookup();
    if (!s)
    {
        // Unknown: request a guest memory scan (in the background) and wait for it to finish
        auto it = g_missNeedScan.find(h);
        if (it == g_missNeedScan.end())
        {
            g_missNeedScan.emplace(h, g_scanStarted.load() + 1);
            RequestScan();
            if (pending)
                *pending = true;
            return nullptr;
        }
        if (g_scanDone.load(std::memory_order_acquire) < it->second)
        {
            if (pending)
                *pending = true;
            return nullptr;
        }
    }
    if (!s && !pixel)
    {
        // No container anywhere: raw-microcode VS (container synthesized when linking with the PS)
        s = new GuestShader();
        s->hash = h;
        s->isRaw = true;
        s->state = 2;
        s->name = "raw";
        s->rawMicrocode.assign(microcode, microcode + sizeBytes);
        map.emplace(h, s);
        fprintf(stderr, "[shaders] VS without container (%u bytes, %016llX): using a synthesized container\n", sizeBytes,
                (unsigned long long)h);
        return s;
    }
    if (!s)
        return nullptr;
    QueueCompile(s);
    return s;
}