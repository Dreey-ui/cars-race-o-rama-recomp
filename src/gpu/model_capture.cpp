// Captura de modelos das cenas pela GPU emulada (veja model_capture.h).
// Cada bloco do stream de uma cena extraida que o jogo copia para a memoria (hook do uncompress) fica registrado com a
// sua posicao no stream; num draw, o endereco do vertex buffer da POSITION mostra se a malha vem de uma cena. A malha e
// lida da memoria guest com o formato dos vfetch do vertex shader (POSITION, NORMAL, TEXCOORD0, COLOR0), os indices sao
// convertidos para lista de triangulos e a textura do sampler 0 do pixel shader e achada pela posicao no stream
// (nome do .dds da pasta). Cada malha e gravada uma vez (VB + IB + contagem); instancias repetidas sao ignoradas.
// Coordenadas: as do vertex buffer (o .soi traz matriz identidade para a geometria da cena), com Z invertido para o
// sistema de mao direita do .obj (a ordem dos triangulos tambem e invertida).
#include "model_capture.h"
#include "renderer.h"
#include "shaders.h"
#include "xenos_regs.h"
#include "runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct Block
{
    uint32_t size;
    std::string stream;
    uint64_t offset;
};
std::mutex g_mapMutex;
std::map<uint32_t, Block> g_blocks;                                // endereco guest -> bloco do stream
std::map<std::pair<std::string, uint64_t>, std::string> g_texNames;   // (stream, posicao) -> .dds

struct Mesh
{
    std::string stream;
    uint64_t vbOffset = 0, ibOffset = 0;
    std::string texture;
    std::vector<float> pos, nrm, uv, col;
    std::vector<uint32_t> tris;
    std::string info;   // layout do draw (comentarios no .obj: formatos dos vfetch, contagens, primitiva)
};
std::mutex g_meshMutex;
std::vector<Mesh> g_meshes;
std::unordered_set<uint64_t> g_seen;
std::atomic<bool> g_active{ false };

bool Resolve(uint32_t guest, std::string& stream, uint64_t& offset)
{
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto it = g_blocks.upper_bound(guest);
    if (it == g_blocks.begin())
        return false;
    --it;
    if (guest - it->first >= it->second.size)
        return false;
    stream = it->second.stream;
    offset = it->second.offset + (guest - it->first);
    return true;
}

uint32_t SwapIndex(uint32_t v, uint32_t endian, bool is32)
{
    switch (endian)
    {
    case 1: return is32 ? (((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF)) : uint16_t((v << 8) | ((v >> 8) & 0xFF));
    case 2: return is32 ? __builtin_bswap32(v) : uint16_t((v << 8) | ((v >> 8) & 0xFF));
    case 3: return is32 ? ((v << 16) | (v >> 16)) : v;
    default: return v;
    }
}

void SwapBytes(uint8_t* p, uint32_t n, uint32_t endian)
{
    for (uint32_t i = 0; i + 1 < n; i += (endian == 1 ? 2 : 4))
    {
        if (endian == 1)
            std::swap(p[i], p[i + 1]);
        else if (endian == 2 && i + 3 < n)
        {
            std::swap(p[i], p[i + 3]);
            std::swap(p[i + 1], p[i + 2]);
        }
        else if (endian == 3 && i + 3 < n)
        {
            std::swap(p[i], p[i + 2]);
            std::swap(p[i + 1], p[i + 3]);
        }
    }
}

float Half(uint16_t h)
{
    const int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 1024), e - 25);
    return s ? -v : v;
}

uint32_t ElementSize(uint32_t format)
{
    switch (format)
    {
    case 26: case 32: case 34: case 37: return 8;
    case 35: case 38: return 16;
    case 57: return 12;
    default: return 4;
    }
}

// Elemento de vertice do Xenos (bytes ja em little-endian) -> ate 4 floats; devolve o numero de componentes
int Decode(const ShaderVertexInput& in, const uint8_t* p, float out[4])
{
    auto u32 = [&](int i) { uint32_t v; memcpy(&v, p + 4 * i, 4); return v; };
    auto norm = [&](int64_t v, int bits, bool sgn) {
        if (in.isInteger)
            return float(v);
        const double maxv = double((1ull << (sgn ? bits - 1 : bits)) - 1);
        return float(std::max(-1.0, double(v) / maxv));
    };
    auto field = [&](uint32_t w, int shift, int bits) -> int64_t {
        const uint32_t v = (w >> shift) & ((1u << bits) - 1);
        if (in.isSigned && (v >> (bits - 1)))
            return int64_t(v) - (int64_t(1) << bits);
        return v;
    };
    int n = 0;
    switch (in.format)
    {
    case 6:   // 8_8_8_8
        for (int i = 0; i < 4; i++)
            out[i] = norm(field(u32(0), 8 * i, 8), 8, in.isSigned);
        n = 4;
        break;
    case 7:   // 2_10_10_10
        out[0] = norm(field(u32(0), 0, 10), 10, in.isSigned);
        out[1] = norm(field(u32(0), 10, 10), 10, in.isSigned);
        out[2] = norm(field(u32(0), 20, 10), 10, in.isSigned);
        out[3] = norm(field(u32(0), 30, 2), 2, in.isSigned);
        n = 4;
        break;
    case 16:  // 10_11_11
        out[0] = norm(field(u32(0), 0, 11), 11, in.isSigned);
        out[1] = norm(field(u32(0), 11, 11), 11, in.isSigned);
        out[2] = norm(field(u32(0), 22, 10), 10, in.isSigned);
        n = 3;
        break;
    case 17:  // 11_11_10
        out[0] = norm(field(u32(0), 0, 10), 10, in.isSigned);
        out[1] = norm(field(u32(0), 10, 11), 11, in.isSigned);
        out[2] = norm(field(u32(0), 21, 11), 11, in.isSigned);
        n = 3;
        break;
    case 25:  // 16_16
        out[0] = norm(field(u32(0), 0, 16), 16, in.isSigned);
        out[1] = norm(field(u32(0), 16, 16), 16, in.isSigned);
        n = 2;
        break;
    case 26:  // 16_16_16_16
        for (int i = 0; i < 4; i++)
            out[i] = norm(field(u32(i / 2), 16 * (i & 1), 16), 16, in.isSigned);
        n = 4;
        break;
    case 31:  // 16_16_FLOAT
        out[0] = Half(uint16_t(u32(0)));
        out[1] = Half(uint16_t(u32(0) >> 16));
        n = 2;
        break;
    case 32:  // 16_16_16_16_FLOAT
        for (int i = 0; i < 4; i++)
            out[i] = Half(uint16_t(u32(i / 2) >> (16 * (i & 1))));
        n = 4;
        break;
    case 36: case 37: case 57: case 38:   // 32_FLOAT .. 32_32_32_32_FLOAT
        n = in.format == 36 ? 1 : in.format == 37 ? 2 : in.format == 57 ? 3 : 4;
        for (int i = 0; i < n; i++)
            memcpy(&out[i], p + 4 * i, 4);
        break;
    case 33: case 34: case 35:   // 32 / 32_32 / 32_32_32_32 inteiros
        n = in.format == 33 ? 1 : in.format == 34 ? 2 : 4;
        for (int i = 0; i < n; i++)
            out[i] = in.isSigned ? norm(int32_t(u32(i)), 32, true) : norm(int64_t(u32(i)), 32, false);
        break;
    default:
        return 0;
    }
    if (in.expAdjust)
        for (int i = 0; i < n; i++)
            out[i] = std::ldexp(out[i], in.expAdjust);
    return n;
}

const ShaderVertexInput* FindInput(const GuestShader* vs, const char* semantic, uint32_t index)
{
    for (const auto& in : vs->inputs)
        if (!strcmp(in.semantic, semantic) && in.semanticIndex == index)
            return &in;
    return nullptr;
}

std::string Sanitize(std::string s)
{
    for (auto& c : s)
        if (c == '\\' || c == '/' || c == ' ' || c == '.')
            c = '_';
    return s;
}

void WriteMesh(const Mesh& m, const fs::path& file)
{
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f)
        return;
    const std::string mtl = m.texture.empty() ? "sem_textura" : Sanitize(m.texture.substr(0, m.texture.size() - 4));
    fprintf(f, "# malha do stream em %llX (indices em %llX), capturada pela GPU (F11)\n", (unsigned long long)m.vbOffset,
            (unsigned long long)m.ibOffset);
    fputs(m.info.c_str(), f);
    fprintf(f, "mtllib ../materiais.mtl\no malha_%llX\n", (unsigned long long)m.vbOffset);
    const size_t nv = m.pos.size() / 3;
    for (size_t i = 0; i < nv; i++)
    {
        if (!m.col.empty())
            fprintf(f, "v %.6g %.6g %.6g %.4g %.4g %.4g\n", m.pos[i * 3], m.pos[i * 3 + 1], -m.pos[i * 3 + 2], m.col[i * 4], m.col[i * 4 + 1],
                    m.col[i * 4 + 2]);
        else
            fprintf(f, "v %.6g %.6g %.6g\n", m.pos[i * 3], m.pos[i * 3 + 1], -m.pos[i * 3 + 2]);
    }
    for (size_t i = 0; i < nv; i++)
        fprintf(f, "vt %.6g %.6g\n", m.uv.empty() ? 0.0f : m.uv[i * 2], m.uv.empty() ? 0.0f : 1.0f - m.uv[i * 2 + 1]);
    for (size_t i = 0; i < nv; i++)
        fprintf(f, "vn %.4g %.4g %.4g\n", m.nrm.empty() ? 0.0f : m.nrm[i * 3], m.nrm.empty() ? 1.0f : m.nrm[i * 3 + 1],
                m.nrm.empty() ? 0.0f : -m.nrm[i * 3 + 2]);
    fprintf(f, "usemtl %s\n", mtl.c_str());
    for (size_t t = 0; t + 2 < m.tris.size(); t += 3)
    {
        const uint32_t a = m.tris[t] + 1, b = m.tris[t + 2] + 1, c = m.tris[t + 1] + 1;   // ordem invertida (Z invertido)
        fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
    }
    fclose(f);
}

// Junta todas as malhas de _modelos\malhas (desta e de capturas anteriores) num .obj so
void WriteCombined(const fs::path& dir, const std::string& scene)
{
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir / L"malhas", ec))
        if (e.path().extension() == L".obj")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    FILE* out = _wfopen((dir / (scene + ".obj")).c_str(), L"wb");
    if (!out)
        return;
    fprintf(out, "# %s: %zu malhas capturadas (F11). Cada malha tambem esta em malhas\\\nmtllib materiais.mtl\n", scene.c_str(), files.size());
    uint64_t base = 0;
    char line[512];
    for (const auto& p : files)
    {
        FILE* in = _wfopen(p.c_str(), L"rb");
        if (!in)
            continue;
        uint64_t count = 0;
        while (fgets(line, sizeof(line), in))
        {
            if (line[0] == 'v' && line[1] == ' ')
                count++;
            if (line[0] == 'f' && line[1] == ' ')
            {
                unsigned a, b, c;
                if (sscanf(line, "f %u/%*u/%*u %u/%*u/%*u %u", &a, &b, &c) == 3)
                    fprintf(out, "f %llu/%llu/%llu %llu/%llu/%llu %llu/%llu/%llu\n", (unsigned long long)(a + base), (unsigned long long)(a + base),
                            (unsigned long long)(a + base), (unsigned long long)(b + base), (unsigned long long)(b + base),
                            (unsigned long long)(b + base), (unsigned long long)(c + base), (unsigned long long)(c + base),
                            (unsigned long long)(c + base));
            }
            else if (line[0] != '#' && strncmp(line, "mtllib", 6) != 0)
                fputs(line, out);
        }
        fclose(in);
        base += count;
    }
    fclose(out);
}

void WriteMaterials(const fs::path& dir)
{
    // materiais de todas as malhas da pasta (lidos dos usemtl): map_Kd aponta para o .dds da pasta do stream
    std::error_code ec;
    std::set<std::string> mats;
    char line[512];
    for (const auto& e : fs::directory_iterator(dir / L"malhas", ec))
        if (FILE* in = _wfopen(e.path().c_str(), L"rb"))
        {
            while (fgets(line, sizeof(line), in))
                if (!strncmp(line, "usemtl ", 7))
                {
                    std::string m = line + 7;
                    while (!m.empty() && (m.back() == '\n' || m.back() == '\r'))
                        m.pop_back();
                    mats.insert(m);
                }
            fclose(in);
        }
    // nome sanitizado -> caminho do .dds (o mesmo usado pelos .dds da pasta)
    std::map<std::string, std::string> paths;
    for (fs::recursive_directory_iterator it(dir.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == L".dds")
        {
            std::string rel = it->path().lexically_relative(dir.parent_path()).string();
            paths[Sanitize(rel.substr(0, rel.size() - 4))] = rel;
        }
    FILE* f = _wfopen((dir / L"materiais.mtl").c_str(), L"wb");
    if (!f)
        return;
    for (const auto& m : mats)
    {
        fprintf(f, "newmtl %s\nKd 1 1 1\n", m.c_str());
        auto it = paths.find(m);
        if (it != paths.end())
        {
            std::string rel = it->second;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            fprintf(f, "map_Kd ../%s\n", rel.c_str());
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

std::vector<Mesh> TakeMeshes()
{
    std::vector<Mesh> meshes;
    std::lock_guard<std::mutex> lock(g_meshMutex);
    meshes.swap(g_meshes);
    return meshes;
}

size_t Save(const std::vector<Mesh>& meshes)
{
    std::set<std::string> streams;
    for (const auto& m : meshes)
    {
        const fs::path dir = fs::path(m.stream) / L"_modelos";
        std::error_code ec;
        fs::create_directories(dir / L"malhas", ec);
        char name[64];
        snprintf(name, sizeof(name), "%08llX_%08llX.obj", (unsigned long long)m.vbOffset, (unsigned long long)m.ibOffset);
        WriteMesh(m, dir / L"malhas" / name);
        streams.insert(m.stream);
    }
    for (const auto& s : streams)
    {
        const fs::path dir = fs::path(s) / L"_modelos";
        std::string scene = fs::path(s).filename().string();
        if (scene.size() > 9)
            scene.resize(scene.size() - 9);   // tira ".x360.str"
        WriteMaterials(dir);
        WriteCombined(dir, scene);
        fprintf(stderr, "[modelos] %s: malhas salvas em %s\n", scene.c_str(), dir.string().c_str());
    }
    return meshes.size();
}
}  // namespace

void ModelCaptureStreamBlock(uint32_t guest, uint32_t size, const std::string& stream, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto it = g_blocks.lower_bound(guest);
    while (it != g_blocks.end() && it->first < guest + size)
        it = g_blocks.erase(it);
    if (it != g_blocks.begin())
    {
        auto prev = std::prev(it);
        if (prev->first + prev->second.size > guest)
            prev->second.size = guest - prev->first;
    }
    g_blocks[guest] = { size, stream, offset };
}

void ModelCaptureTexture(const std::string& stream, uint64_t offset, const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_texNames[{ stream, offset }] = name;
}

bool ModelCaptureActive() { return g_active.load(std::memory_order_relaxed); }

void ModelCaptureDraw(const DrawCall& draw, const GuestShader* vs, const GuestShader* ps, const uint32_t* r)
{
    const ShaderVertexInput* pos = FindInput(vs, "POSITION", 0);
    if (!pos || draw.numIndices < 3 || (draw.primType != 4 && draw.primType != 5 && draw.primType != 6 && draw.primType != 13))
        return;
    auto slotAddr = [&](uint32_t slot, uint32_t& endian, uint32_t& size) {
        const uint32_t d0 = r[xreg::FETCH_CONSTANTS + slot * 2], d1 = r[xreg::FETCH_CONSTANTS + slot * 2 + 1];
        endian = d1 & 3;
        size = ((d1 >> 2) & 0xFFFFFF) * 4;
        return (d0 & 3) == 3 ? GpuPhysToGuest(d0 & ~3u) : 0u;
    };
    uint32_t endian = 0, vbSize = 0;
    const uint32_t vb = slotAddr(pos->fetchSlot, endian, vbSize);
    const int32_t baseVertex = int32_t(r[xreg::VGT_INDX_OFFSET]);
    const uint32_t ib = draw.indexed ? GpuPhysToGuest(draw.indexAddress) : 0;
    const uint64_t key = (uint64_t(vb) << 32) ^ (uint64_t(ib) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(draw.numIndices) << 7) ^ uint32_t(baseVertex);
    {
        std::lock_guard<std::mutex> lock(g_meshMutex);
        if (!g_seen.insert(key).second)
            return;
    }
    Mesh m;
    if (!vb || !Resolve(vb, m.stream, m.vbOffset))
        return;   // geometria que nao vem de uma cena extraida (carros, interface...)
    std::string ibStream;
    if (ib)
        Resolve(ib, ibStream, m.ibOffset);
    // indices -> lista de triangulos
    const uint32_t n = draw.numIndices;
    const bool restart = draw.indexed && (draw.primType == 6) && ((r[xreg::PA_SU_SC_MODE_CNTL] >> 21) & 1);
    const uint32_t reset = draw.index32 ? r[xreg::VGT_MULTI_PRIM_IB_RESET_INDX] : (r[xreg::VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFF);
    std::vector<int64_t> idx(n);
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t v = i;
        if (draw.indexed)
            v = SwapIndex(draw.index32 ? *reinterpret_cast<const uint32_t*>(g_mem.base + ib + i * 4)
                                       : *reinterpret_cast<const uint16_t*>(g_mem.base + ib + i * 2),
                          draw.indexEndian, draw.index32);
        idx[i] = (restart && v == reset) ? -1 : int64_t(v) + baseVertex;
    }
    std::vector<int64_t> tri;
    switch (draw.primType)
    {
    case 4:
        tri.assign(idx.begin(), idx.begin() + (n / 3) * 3);
        break;
    case 5:
        for (uint32_t i = 1; i + 1 < n; i++)
            tri.insert(tri.end(), { idx[0], idx[i], idx[i + 1] });
        break;
    case 13:
        for (uint32_t q = 0; q + 3 < n; q += 4)
            tri.insert(tri.end(), { idx[q], idx[q + 1], idx[q + 2], idx[q], idx[q + 2], idx[q + 3] });
        break;
    case 6:
        for (uint32_t i = 0, k = 0; i + 2 < n; i++)
        {
            if (idx[i] < 0 || idx[i + 1] < 0 || idx[i + 2] < 0)
            {
                k = 0;   // reinicia a tira (a paridade conta a partir do novo comeco)
                continue;
            }
            if (k++ & 1)
                tri.insert(tri.end(), { idx[i + 1], idx[i], idx[i + 2] });
            else
                tri.insert(tri.end(), { idx[i], idx[i + 1], idx[i + 2] });
        }
        break;
    }
    int64_t lo = INT64_MAX, hi = -1;
    for (int64_t v : tri)
        if (v >= 0)
        {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    if (hi < 0 || hi - lo > 2000000)
        return;
    const uint32_t count = uint32_t(hi - lo + 1);
    for (int64_t v : tri)
        m.tris.push_back(uint32_t(std::max<int64_t>(v, lo) - lo));
    // atributos
    struct Attr { const ShaderVertexInput* in; std::vector<float>* out; int comps; };
    const Attr attrs[] = { { pos, &m.pos, 3 }, { FindInput(vs, "NORMAL", 0), &m.nrm, 3 }, { FindInput(vs, "TEXCOORD", 0), &m.uv, 2 },
                           { FindInput(vs, "COLOR", 0), &m.col, 4 } };
    for (const Attr& a : attrs)
    {
        if (!a.in)
            continue;
        uint32_t e = 0, size = 0;
        const uint32_t base = slotAddr(a.in->fetchSlot, e, size);
        const uint32_t esz = ElementSize(a.in->format);
        if (!base || !a.in->strideBytes)
            continue;
        a.out->resize(size_t(count) * a.comps);
        bool ok = true;
        for (uint32_t v = 0; v < count && ok; v++)
        {
            const uint64_t off = uint64_t(lo + v) * a.in->strideBytes + a.in->offsetBytes;
            uint8_t bytes[16] = {};
            if (off + esz > size)
            {
                ok = false;
                break;
            }
            memcpy(bytes, g_mem.base + base + off, esz);
            SwapBytes(bytes, esz, e);
            float f[4] = { 0, 0, 0, 1 };
            const int nc = Decode(*a.in, bytes, f);
            if (!nc)
                ok = false;
            for (int c = 0; c < a.comps; c++)
                (*a.out)[size_t(v) * a.comps + c] = c < nc ? f[c] : (c == 3 ? 1.0f : 0.0f);
        }
        if (!ok)
            a.out->clear();
    }
    if (m.pos.empty())
        return;
    {
        char line[256];
        snprintf(line, sizeof(line), "# draw: primitiva %u, %u indices (%s, %u bits), base %d, vertices %lld..%lld (%u)\n", draw.primType,
                 draw.numIndices, draw.indexed ? "indexado" : "sem indices", draw.index32 ? 32 : 16, baseVertex, (long long)lo,
                 (long long)hi, count);
        m.info += line;
        for (const auto& in : vs->inputs)
        {
            uint32_t e = 0, size = 0;
            const uint32_t a = slotAddr(in.fetchSlot, e, size);
            std::string st;
            uint64_t off = 0;
            Resolve(a, st, off);
            snprintf(line, sizeof(line), "# vfetch %s%u: formato %u, offset %u, stride %u, slot %u (stream %llX, %u bytes), sinal %d, inteiro %d, exp %d\n",
                     in.semantic, in.semanticIndex, in.format, in.offsetBytes, in.strideBytes, in.fetchSlot, (unsigned long long)off, size,
                     in.isSigned, in.isInteger, in.expAdjust);
            m.info += line;
        }
    }
    // textura do sampler 0 do pixel shader (ou o primeiro usado)
    for (uint32_t slot = 0; ps && slot < 16; slot++)
        if (ps->samplerMask & (1u << slot))
        {
            const uint32_t d1 = r[xreg::FETCH_CONSTANTS + slot * 6 + 1];
            std::string stream;
            uint64_t off = 0;
            if (Resolve(GpuPhysToGuest(d1 & 0xFFFFF000u), stream, off))
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                auto it = g_texNames.find({ stream, off });
                if (it != g_texNames.end())
                    m.texture = it->second;
            }
            break;
        }
    std::lock_guard<std::mutex> lock(g_meshMutex);
    g_meshes.push_back(std::move(m));
}

void ModelCaptureFinish()
{
    if (g_active.exchange(false))
        fprintf(stderr, "[modelos] jogo fechando: %zu malhas salvas\n", Save(TakeMeshes()));
}

std::wstring ModelCaptureToggle()
{
    if (!g_active.exchange(true))
    {
        {
            std::lock_guard<std::mutex> lock(g_meshMutex);
            g_seen.clear();
        }
        fprintf(stderr, "[modelos] captura ligada: ande pela cena; F11 de novo salva\n");
        return L"CAPTURA DE MODELOS ligada (F11 de novo salva)";
    }
    g_active = false;
    // grava numa thread propria (milhares de .obj em texto: o jogo nao para)
    auto meshes = std::make_shared<std::vector<Mesh>>(TakeMeshes());
    const size_t n = meshes->size();
    std::thread([meshes] {
        SetThreadDescription(GetCurrentThread(), L"salvar modelos");
        Save(*meshes);
        fprintf(stderr, "[modelos] captura salva: %zu malhas novas\n", meshes->size());
    }).detach();
    wchar_t t[128];
    swprintf(t, 128, L"MODELOS: salvando %zu malhas (pasta _modelos da cena)", n);
    return t;
}
