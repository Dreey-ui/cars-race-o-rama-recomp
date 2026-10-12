// Builds the scene packages from the extracted folders (see scene_assets.h and tools/scenes_extract.py).
//
// .res (version 3 without compression, which the game's reader sub_821C1AA0 accepts with flags 0), little-endian:
//   +0 version 3, +4 header size after +8 (8), +8 flags 0, +12 0, directory {u32 count, u32 size,
//   entries {u32 name length, name, u32 offset, u32 size}}, data (each file aligned to 0x800).
// Stream (raw .str, in the layout the .toc of the .res folder already describes): zero it, copy _data\*.bin and write the
// blocks of each .dds on top, with the Xbox 360 tiling/endian/channel order (same rules as tools/scenes_textures.py:
// Xenia's GetGuestTextureLayout, mipmaps starting at mip_address, small levels packed into one tile).
#include "scene_assets.h"
#include "gpu/model_capture.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchTexels, uint32_t bppLog2);   // gpu/textures.cpp

namespace
{
std::string Lower(std::string s)
{
    for (auto& c : s)
        c = char(tolower((unsigned char)c));
    return s;
}

bool ReadWhole(const fs::path& p, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f)
        return false;
    _fseeki64(f, 0, SEEK_END);
    out.resize(size_t(_ftelli64(f)));
    _fseeki64(f, 0, SEEK_SET);
    const size_t n = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(n);
    return true;
}

std::vector<std::string> ReadLines(const fs::path& p)
{
    std::vector<std::string> lines;
    std::vector<uint8_t> d;
    if (!ReadWhole(p, d))
        return lines;
    std::string cur;
    for (uint8_t c : d)
    {
        if (c == '\n')
        {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        }
        else
            cur += char(c);
    }
    if (!cur.empty())
        lines.push_back(cur);
    return lines;
}

// Index of an extracted package folder (tools/scenes_extract.py): _index.txt, or _indice.txt with the Portuguese
// keywords of older extractions (tamanho/dados/textura -> size/data/texture)
std::vector<std::string> ReadIndex(const fs::path& folder)
{
    std::error_code ec;
    std::vector<std::string> lines = ReadLines(fs::exists(folder / L"_index.txt", ec) ? folder / L"_index.txt" : folder / L"_indice.txt");
    static const std::pair<const char*, const char*> kOld[] = { { "tamanho ", "size " }, { "dados ", "data " }, { "textura ", "texture " } };
    for (auto& l : lines)
        for (const auto& [from, to] : kOld)
            if (l.compare(0, strlen(from), from) == 0)
            {
                l = to + l.substr(strlen(from));
                break;
            }
    return lines;
}

bool IsIndexFile(const std::string& lowerName) { return lowerName == "_index.txt" || lowerName == "_indice.txt"; }

// ---------------------------------------------------------------------------------------------
// .res

struct ResEntry
{
    std::string name;
    fs::path file;
    uint32_t size = 0, offset = 0;
};
struct ResLayout
{
    std::vector<ResEntry> entries;
    uint32_t dirSize = 0, dataStart = 0;
    uint64_t total = 0;
};

bool ResFolderLayout(const fs::path& folder, ResLayout& out)
{
    std::error_code ec;
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (std::string n : ReadIndex(folder))
    {
        if (n.empty() || n[0] == '#')
            continue;
        if (n.size() > 7 && n.compare(n.size() - 7, 7, " zlibme") == 0)
            n.resize(n.size() - 7);
        if (seen.insert(Lower(n)).second)
            names.push_back(n);
    }
    // files the user put in the folder that are not in the index: at the end
    std::vector<std::string> extra;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::string rel = it->path().lexically_relative(folder).make_preferred().string();
        if (!IsIndexFile(Lower(rel)) && !seen.count(Lower(rel)))
            extra.push_back(rel);
    }
    std::sort(extra.begin(), extra.end());
    names.insert(names.end(), extra.begin(), extra.end());
    uint32_t dirSize = 0;
    for (const auto& n : names)
    {
        ResEntry e;
        e.name = n;
        e.file = folder / fs::path(n);
        const uint64_t size = fs::file_size(e.file, ec);
        if (ec)
            continue;
        e.size = uint32_t(size);
        dirSize += 12 + uint32_t(n.size());
        out.entries.push_back(std::move(e));
    }
    if (out.entries.empty())
        return false;
    out.dataStart = (16 + 8 + dirSize + 0x7FF) & ~0x7FFu;
    out.dirSize = out.dataStart - 24;
    uint32_t pos = 0;
    for (auto& e : out.entries)
    {
        e.offset = pos;
        pos = (pos + e.size + 0x7FF) & ~0x7FFu;
    }
    out.total = uint64_t(out.dataStart) + pos;
    return true;
}

bool BuildRes(const fs::path& folder, std::vector<uint8_t>& out)
{
    ResLayout layout;
    if (!ResFolderLayout(folder, layout))
        return false;
    out.assign(size_t(layout.total), 0);
    uint8_t* p = out.data();
    auto put32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
    put32(3);
    put32(8);
    put32(0);
    put32(0);
    put32(uint32_t(layout.entries.size()));
    put32(layout.dirSize);
    for (const auto& e : layout.entries)
    {
        put32(uint32_t(e.name.size()));
        memcpy(p, e.name.data(), e.name.size());
        p += e.name.size();
        put32(e.offset);
        put32(e.size);
    }
    std::vector<uint8_t> buf;
    for (const auto& e : layout.entries)
        if (ReadWhole(e.file, buf))
            memcpy(out.data() + layout.dataStart + e.offset, buf.data(), std::min<size_t>(buf.size(), e.size));
    fprintf(stderr, "[mods] package built from folder %s: %zu files, %.1f MB uncompressed\n", folder.string().c_str(),
            layout.entries.size(), layout.total / 1048576.0);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Textures: Xenos fetch constant -> level layout (same as tools/scenes_textures.py)

struct Fetch
{
    uint32_t tiled, pitch, format, endian, width, height, swizzle, maxLevel, dimension, packed, mipAddress;
};

Fetch DecodeFetch(const uint32_t d[6])
{
    Fetch f;
    f.tiled = d[0] >> 31;
    f.pitch = ((d[0] >> 22) & 0x1FF) << 5;
    f.format = d[1] & 0x3F;
    f.endian = (d[1] >> 6) & 3;
    f.width = (d[2] & 0x1FFF) + 1;
    f.height = ((d[2] >> 13) & 0x1FFF) + 1;
    f.swizzle = (d[3] >> 1) & 0xFFF;
    f.maxLevel = (d[4] >> 6) & 0xF;
    f.dimension = (d[5] >> 9) & 3;
    f.packed = (d[5] >> 11) & 1;
    f.mipAddress = (d[5] >> 12) << 12;
    return f;
}

enum class Kind { Bc, Argb, Plain };
struct Format
{
    uint32_t bw, bpb;
    Kind kind;
    uint32_t fourcc;   // Bc
    uint32_t bits, r, g, b, a;   // Plain: DDS masks
};

constexpr uint32_t FourCC(const char* s) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; }

const Format* FindFormat(uint32_t f)
{
    static const Format dxt1{ 4, 8, Kind::Bc, FourCC("DXT1") }, dxt3{ 4, 16, Kind::Bc, FourCC("DXT3") },
        dxt5{ 4, 16, Kind::Bc, FourCC("DXT5") }, ati2{ 4, 16, Kind::Bc, FourCC("ATI2") }, ati1{ 4, 8, Kind::Bc, FourCC("ATI1") },
        argb{ 1, 4, Kind::Argb }, r565{ 1, 2, Kind::Plain, 0, 16, 0xF800, 0x7E0, 0x1F, 0 },
        r1555{ 1, 2, Kind::Plain, 0, 16, 0x7C00, 0x3E0, 0x1F, 0x8000 }, r4444{ 1, 2, Kind::Plain, 0, 16, 0xF00, 0xF0, 0xF, 0xF000 },
        l8{ 1, 1, Kind::Plain, 0, 8, 0xFF, 0, 0, 0 }, a8l8{ 1, 2, Kind::Plain, 0, 16, 0xFF, 0, 0, 0xFF00 };
    switch (f)
    {
    case 18: case 51: return &dxt1;
    case 19: case 52: return &dxt3;
    case 20: case 53: return &dxt5;
    case 49: return &ati2;
    case 59: return &ati1;
    case 6: case 14: case 50: return &argb;
    case 4: return &r565;
    case 3: return &r1555;
    case 15: return &r4444;
    case 2: return &l8;
    case 10: return &a8l8;
    }
    return nullptr;
}

uint32_t Log2Ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
uint32_t Log2Floor(uint32_t v) { uint32_t r = 0; while (v >> (r + 1)) r++; return r; }
uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v); }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

void PackedMipOffset(uint32_t w, uint32_t h, uint32_t mip, uint32_t bw, uint32_t& x, uint32_t& y)
{
    const uint32_t lw = Log2Ceil(w), lh = Log2Ceil(h), ls = std::min(lw, lh);
    x = y = 0;
    if (ls > 4 + mip)
        return;
    const uint32_t base = ls > 4 ? ls - 4 : 0, pm = mip - base;
    if (pm < 3)
    {
        if (lw > lh)
            y = 16 >> pm;
        else
            x = 16 >> pm;
    }
    else if (lw > lh)
        x = (1u << (lw - base)) >> (pm - 2);
    else
        y = (1u << (lh - base)) >> (pm - 2);
    const uint32_t shift = bw == 4 ? 2 : 0;
    x >>= shift;
    y >>= shift;
}

struct Level
{
    uint32_t w, h, base, pitch, ox, oy, stride;
};

std::vector<Level> Levels(const Fetch& f, uint32_t bw, uint32_t bpb)
{
    const uint32_t w = f.width, h = f.height, faces = f.dimension == 3 ? 6 : 1;
    const uint32_t maxLevel = std::min(f.maxLevel, Log2Floor(std::max(w, h)));
    const uint32_t smallLog2 = std::min(Log2Ceil(w), Log2Ceil(h));
    const uint32_t packedLevel = f.packed ? (smallLog2 > 4 ? smallLog2 - 4 : 0) : 99;
    const uint32_t linAlign = f.tiled ? 32 : std::max(256 / bpb, 32u);
    auto stride = [&](uint32_t pitch, uint32_t height) { return AlignUp(pitch * bpb * AlignUp(AlignUp(height, bw) / bw, 32), 4096); };
    std::vector<Level> out;
    const uint32_t pitch0 = AlignUp(AlignUp(f.pitch ? f.pitch : w, bw) / bw, 32);
    uint32_t ox = 0, oy = 0;
    if (packedLevel == 0)
        PackedMipOffset(w, h, 0, bw, ox, oy);
    out.push_back({ w, h, 0, pitch0, ox, oy, stride(pitch0, packedLevel == 0 ? NextPow2(h) : h) });
    if (maxLevel == 0 || !f.mipAddress)
        return out;
    struct Storage { uint32_t off, pitch, stride; };
    std::map<uint32_t, Storage> storage;
    if (packedLevel == 0)
    {
        const uint32_t pitch = AlignUp(AlignUp(NextPow2(w), bw) / bw, linAlign);
        storage[0] = { 0, pitch, stride(pitch, NextPow2(h)) };
    }
    else
    {
        uint32_t mipOff = 0;
        for (uint32_t lv = 1; lv <= std::min(maxLevel, packedLevel); lv++)
        {
            const uint32_t pitch = AlignUp(AlignUp(std::max(NextPow2(w) >> lv, 1u), bw) / bw, linAlign);
            const uint32_t st = stride(pitch, std::max(NextPow2(h) >> lv, 1u));
            storage[lv] = { mipOff, pitch, st };
            mipOff += st * faces;
        }
    }
    for (uint32_t lv = 1; lv <= maxLevel; lv++)
    {
        Storage s;
        uint32_t x = 0, y = 0;
        if (lv >= packedLevel)
        {
            s = storage[packedLevel ? packedLevel : 0];
            PackedMipOffset(w, h, lv, bw, x, y);
        }
        else
            s = storage[lv];
        out.push_back({ std::max(w >> lv, 1u), std::max(h >> lv, 1u), f.mipAddress + s.off, s.pitch, x, y, s.stride });
    }
    return out;
}

void Swap(uint8_t* p, size_t n, uint32_t endian)
{
    switch (endian)
    {
    case 1:
        for (size_t i = 0; i + 1 < n; i += 2)
            std::swap(p[i], p[i + 1]);
        break;
    case 2:
        for (size_t i = 0; i + 3 < n; i += 4)
        {
            std::swap(p[i], p[i + 3]);
            std::swap(p[i + 1], p[i + 2]);
        }
        break;
    case 3:
        for (size_t i = 0; i + 3 < n; i += 4)
        {
            std::swap(p[i], p[i + 2]);
            std::swap(p[i + 1], p[i + 3]);
        }
        break;
    }
}

// Checks the .dds (plain DDS or with a DX10 header) and returns where the pixels start; rgbaOrder = pixels R G B A (DX10
// R8G8B8A8) instead of B G R A
bool CheckDds(const std::vector<uint8_t>& d, const Fetch& f, const Format& fmt, uint32_t levels, size_t blocksBytes, size_t& dataStart,
              bool& rgbaOrder, std::string& why)
{
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; };
    rgbaOrder = false;
    if (d.size() < 128 || u32(0) != FourCC("DDS "))
    {
        why = "not a DDS file";
        return false;
    }
    const uint32_t h = u32(12), w = u32(16), mips = std::max(u32(28), 1u), pfFlags = u32(80), fourcc = u32(84), bits = u32(88);
    const uint32_t caps2 = u32(112);
    dataStart = 128;
    char expect[96];
    snprintf(expect, sizeof(expect), "%ux%u with %u mipmap(s)%s", f.width, f.height, levels, f.dimension == 3 ? ", cube" : "");
    bool cube = (caps2 & 0x200) != 0;
    bool formatOk = false;
    if ((pfFlags & 4) && fourcc == FourCC("DX10"))
    {
        dataStart = 148;
        const uint32_t dxgi = u32(128), misc = u32(136);
        cube = (misc & 4) != 0;
        switch (fmt.kind)
        {
        case Kind::Bc:
            formatOk = (fmt.fourcc == FourCC("DXT1") && (dxgi == 71 || dxgi == 72)) || (fmt.fourcc == FourCC("DXT3") && (dxgi == 74 || dxgi == 75)) ||
                       (fmt.fourcc == FourCC("DXT5") && (dxgi == 77 || dxgi == 78)) || (fmt.fourcc == FourCC("ATI1") && dxgi == 80) ||
                       (fmt.fourcc == FourCC("ATI2") && dxgi == 83);
            break;
        case Kind::Argb:
            formatOk = dxgi == 87 || dxgi == 91 || dxgi == 28 || dxgi == 29;
            rgbaOrder = dxgi == 28 || dxgi == 29;
            break;
        case Kind::Plain:
            formatOk = (fmt.r == 0xF800 && dxgi == 85) || (fmt.r == 0x7C00 && dxgi == 86) || (fmt.r == 0xF00 && dxgi == 115) ||
                       (fmt.bits == 8 && dxgi == 61);
            break;
        }
    }
    else if (fmt.kind == Kind::Bc)
        formatOk = (pfFlags & 4) && fourcc == fmt.fourcc;
    else if (fmt.kind == Kind::Argb)
    {
        formatOk = bits == 32 && ((u32(92) == 0xFF0000 && u32(96) == 0xFF00 && u32(100) == 0xFF) ||
                                  (u32(92) == 0xFF && u32(96) == 0xFF00 && u32(100) == 0xFF0000));
        rgbaOrder = u32(92) == 0xFF;
    }
    else
        formatOk = bits == fmt.bits && u32(92) == fmt.r && u32(96) == fmt.g && u32(100) == fmt.b;
    if (!formatOk || w != f.width || h != f.height || mips != levels || cube != (f.dimension == 3))
    {
        why = std::string("format/size differs from the original (the original is ") + expect + ")";
        return false;
    }
    if (d.size() < dataStart + blocksBytes)
    {
        why = std::string("file smaller than expected (") + expect + ")";
        return false;
    }
    return true;
}

// Writes the .dds pixels into tmp (the whole texture, in the Xenos layout the fetch constant describes); whatever is not
// a pixel (tile padding) stays as it was in tmp
bool EncodeTexture(std::vector<uint8_t>& tmp, const uint32_t fetch[6], const std::vector<uint8_t>& dds, std::string& why)
{
    const Fetch f = DecodeFetch(fetch);
    const Format* fmt = FindFormat(f.format);
    if (!fmt)
    {
        why = "unsupported format";
        return false;
    }
    const uint32_t bw = fmt->bw, bpb = fmt->bpb, bppLog2 = Log2Floor(bpb), faces = f.dimension == 3 ? 6 : 1;
    const std::vector<Level> lv = Levels(f, bw, bpb);
    size_t blocks = 0;
    for (const auto& l : lv)
        blocks += size_t((l.w + bw - 1) / bw) * ((l.h + bw - 1) / bw);
    blocks *= faces;
    size_t p = 0;
    bool rgba = false;
    if (!CheckDds(dds, f, *fmt, uint32_t(lv.size()), blocks * bpb, p, rgba, why))
        return false;
    const size_t size = tmp.size();
    Swap(tmp.data(), tmp.size(), f.endian);
    uint32_t order[4];
    for (int i = 0; i < 4; i++)
        order[i] = (f.swizzle >> (3 * i)) & 7;
    for (uint32_t face = 0; face < faces; face++)
        for (const auto& l : lv)
        {
            const uint32_t bx = (l.w + bw - 1) / bw, by = (l.h + bw - 1) / bw;
            const uint32_t base = l.base + face * l.stride;
            for (uint32_t y = 0; y < by; y++)
                for (uint32_t x = 0; x < bx; x++, p += bpb)
                {
                    const uint32_t o = f.tiled ? base + TiledOffset2D(x + l.ox, y + l.oy, l.pitch, bppLog2)
                                               : base + (y + l.oy) * l.pitch * bpb + (l.ox + x) * bpb;
                    if (uint64_t(o) + bpb > size)
                        continue;
                    const uint8_t* src = dds.data() + p;
                    if (fmt->kind == Kind::Argb)
                    {
                        // .dds in B G R A (or R G B A in DX10): channel c goes to byte order[c] (fetch constant swizzle)
                        const uint8_t c[4] = { rgba ? src[0] : src[2], src[1], rgba ? src[2] : src[0], src[3] };
                        for (int ch = 0; ch < 4; ch++)
                            if (order[ch] < 4)
                                tmp[o + order[ch]] = c[ch];
                    }
                    else
                        memcpy(tmp.data() + o, src, bpb);
                }
        }
    Swap(tmp.data(), tmp.size(), f.endian);
    return true;
}

bool WriteTexture(std::vector<uint8_t>& stream, const uint32_t fetch[6], uint32_t size, const std::vector<uint32_t>& places,
                  const fs::path& ddsPath, std::string& why)
{
    std::vector<uint8_t> dds;
    if (!ReadWhole(ddsPath, dds))
    {
        why = "could not read the file";
        return false;
    }
    if (places.empty() || places[0] + uint64_t(size) > stream.size())
    {
        why = "position outside the stream";
        return false;
    }
    std::vector<uint8_t> tmp(stream.begin() + places[0], stream.begin() + places[0] + size);
    if (!EncodeTexture(tmp, fetch, dds, why))
        return false;
    for (uint32_t q : places)
        if (q + uint64_t(size) <= stream.size())
            memcpy(stream.data() + q, tmp.data(), size);
    return true;
}

bool StreamSize(const fs::path& folder, uint64_t& size)
{
    for (const auto& line : ReadIndex(folder))
        if (line.compare(0, 5, "size ") == 0)
        {
            size = strtoull(line.c_str() + 5, nullptr, 16);
            return size != 0;
        }
    return false;
}

bool BuildStream(const fs::path& folder, std::vector<uint8_t>& out)
{
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t size = 0;
    if (!StreamSize(folder, size))
    {
        fprintf(stderr, "[mods] ERROR %s: _index.txt without a size\n", folder.string().c_str());
        return false;
    }
    out.assign(size_t(size), 0);
    const auto lines = ReadIndex(folder);
    std::vector<uint8_t> buf;
    uint32_t datas = 0, textures = 0, errors = 0;
    for (const auto& line : lines)
        if (line.compare(0, 5, "data ") == 0)
        {
            char* end = nullptr;
            const uint64_t pos = strtoull(line.c_str() + 5, &end, 16);
            const fs::path file = folder / fs::path(std::string(end && *end == ' ' ? end + 1 : ""));
            if (ReadWhole(file, buf) && pos + buf.size() <= out.size())
            {
                memcpy(out.data() + pos, buf.data(), buf.size());
                datas++;
            }
            else
            {
                fprintf(stderr, "[mods] ERROR %s: could not read %s\n", folder.string().c_str(), file.string().c_str());
                errors++;
            }
        }
    for (const auto& line : lines)
    {
        if (line.compare(0, 8, "texture ") != 0)
            continue;
        uint32_t fetch[6], tsize;
        char places[4096], rest[1024];
        const std::string head = line.substr(0, line.find("  |"));
        if (sscanf(head.c_str(), "texture %x %x %x %x %x %x %x %4095s %1023[^\n]", &fetch[0], &fetch[1], &fetch[2], &fetch[3], &fetch[4],
                   &fetch[5], &tsize, places, rest) != 9)
            continue;
        std::vector<uint32_t> pos;
        for (char* p = places; *p;)
        {
            char* e = nullptr;
            pos.push_back(uint32_t(strtoul(p, &e, 16)));
            p = (*e == ',') ? e + 1 : e;
            if (e == p && *p)
                break;
        }
        std::string why;
        if (WriteTexture(out, fetch, tsize, pos, folder / fs::path(std::string(rest)), why))
        {
            textures++;
            for (uint32_t q : pos)
                ModelCaptureTexture(folder.string(), q, rest);   // texture name for model capture (F11)
        }
        else
        {
            fprintf(stderr, "[mods] ERROR texture %s: %s\n", rest, why.c_str());
            errors++;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[mods] stream built from folder %s: %u .dds textures, %u data blocks, %.1f MB in %.0f ms%s\n",
            folder.string().c_str(), textures, datas, size / 1048576.0, ms, errors ? " (WITH ERRORS, see above)" : "");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Scene as loose files (layout of "<game>\extracted_scenes", tools/scenes_decompile.py, copied to mods\C\Scenes):
//   <scene folder>\_package\X.x360.soi/.toc   skeleton of the original package (records, sectors, placement)
//   each .toc resource at its game path: "C\Scenes\<Scene>\geo\Y" -> mods\C\Scenes\<Scene>\geo\Y.xng (tex\Y.dds,
//   col\Y.gol); names outside C\Scenes ("C\ShadowMap\Tex\Y") -> <package folder>\Tex\Y.dds
// The .soi (xgs/texture/collision headers), the .toc (position/size of each resource, blocks of each sector) and the raw
// stream are regenerated from the files; materials do not go into the .res: without the .xnm.bpb the game reads the text .xnm.
//
// .soi (big-endian): +0 version (0x102; 0x101 = uncompressed stream, MiniMap), +8 sectors, +0xC collisions, +0x10 models,
//   +0x18 textures, +0x1C .soi-only textures (PC DDS, after the others), +0x20 A slots, +0x24 B slots, +0x28/+0x30/+0x34 position (relative to 0x3C) of the 1st record of models/
//   collisions/textures; +0x50 size of each stream memory slot (A = GPU, then B = CPU). Record: 0x3C of
//   placement before it, u32 index, name[0x100], u32 0, u32 0/FFFFFFFF, u32 n, n properties x 0x208, content:
//   texture 0x38 (D3D header, fetch constant at +0x20; or u32 size + PC DDS), xgs (= .xng without the submesh
//   data) or collision 0x7C (.gol header with flag 9 + 0 + root node).
// .toc: sectors {name[0x100], 21 u32 [0, resources, in part A, in part B, -1, slot A, slot B, neighbors x8, box x6],
//   record {position in the .str, total, A, B, A in the .str, B in the .str, A blocks, B blocks} (version 0x101: only the first 4) +
//   size of each block in the .str, resources x 0x118 {name[0x100], 0, index/-1, index, position in its part, size,
//   type 0 model / 1 texture / 2 collision}}. Part A = GPU (textures and models aligned to 0x1000), B = CPU (collisions,
//   aligned to 0x80); each block is 128 KB uncompressed (in the raw stream, the whole block).

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t BE16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
float BEF32(const uint8_t* p) { const uint32_t u = BE32(p); float f; memcpy(&f, &u, 4); return f; }
void PutBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void AppendBE32(std::vector<uint8_t>& out, uint32_t v) { uint8_t b[4]; PutBE32(b, v); out.insert(out.end(), b, b + 4); }
void AppendF32(std::vector<uint8_t>& out, float f) { uint32_t u; memcpy(&u, &f, 4); AppendBE32(out, u); }
void Append(std::vector<uint8_t>& out, const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); }

constexpr uint32_t kBlock = 0x20000;

uint32_t VertexStride(uint32_t flags)
{
    // position, normal, color (ARGB), UV0, UV1, UV2, tangent: the order of the streams in the submesh data
    static const uint32_t bits[][2] = { { 0x01, 12 }, { 0x02, 12 }, { 0x08, 4 }, { 0x04, 8 }, { 0x10, 8 }, { 0x4000, 8 }, { 0x20, 12 } };
    uint32_t s = 0;
    for (const auto& b : bits)
        if (flags & b[0])
            s += b[1];
    return s;
}

// Walks a model: xgs (header in the .soi, no data) or .xng (each submesh's data right after its record).
// len = size; with hdr/blob, builds the xgs (record marked "data in the stream") and the submesh data in sequence.
bool WalkModel(const uint8_t* d, size_t n, bool hasData, size_t& len, std::vector<uint8_t>* hdr, std::vector<uint8_t>* blob)
{
    if (n < 12 || memcmp(d, hasData ? "xng" : "xgs", 4) != 0)
        return false;
    if (hdr)
    {
        hdr->assign(d, d + 12);
        memcpy(hdr->data(), "xgs", 4);
    }
    auto copy = [&](size_t a, size_t b) { if (hdr) hdr->insert(hdr->end(), d + a, d + b); };
    const uint32_t nobj = BE32(d + 8);
    if (nobj > 100000)
        return false;
    size_t p = 12;
    for (uint32_t o = 0; o < nobj; o++)
    {
        if (p + 0xE4 > n)
            return false;
        const uint32_t nmat = BE32(d + p + 0xE0);
        const size_t head = 0xE4 + size_t(nmat) * 0x40;
        if (nmat > 4096 || p + head + 4 > n)
            return false;
        const uint32_t nlod = d[p + head];
        copy(p, p + head + 4);
        p += head + 4;
        for (uint32_t l = 0; l < nlod; l++)
        {
            if (p + 8 > n)
                return false;
            const uint32_t nsub = BE32(d + p + 4);
            if (nsub > 100000)
                return false;
            copy(p, p + 8);
            p += 8;
            for (uint32_t s = 0; s < nsub; s++)
            {
                if (p + 18 > n)
                    return false;
                const uint32_t flags = BE32(d + p + 4), nf = d[p + 8];
                const size_t rl = 18 + 4 * size_t(nf);
                if (p + rl > n)
                    return false;
                const uint32_t nv = BE16(d + p + 14 + 4 * nf), ni = BE16(d + p + 16 + 4 * nf);
                if (hdr)
                {
                    const size_t at = hdr->size();
                    copy(p, p + rl);
                    (*hdr)[at + 10] = 1;   // data in the stream
                }
                p += rl;
                if (hasData)
                {
                    const size_t size = ((size_t(ni) * 2 + 3) & ~size_t(3)) + size_t(nv) * VertexStride(flags);
                    if (p + size > n)
                        return false;
                    if (blob)
                        Append(*blob, d + p, size);
                    p += size;
                }
            }
        }
    }
    len = p;
    return true;
}

// .gol (loose collision) -> content of the .soi record (0x7C) + stream data (float4 vertices, 64-byte nodes aligned
// to 32, 32-byte triangles). The .gol does not store the w values (1.0), the zeros or the node radius (box diagonal).
#pragma clang fp contract(off)
bool GolToCol(const std::vector<uint8_t>& g, std::vector<uint8_t>& payload, std::vector<uint8_t>& blob, std::string& why)
{
    if (g.size() < 0x5C || BE32(g.data()) != 0x4D2 || memcmp(g.data() + 4, "col", 4) != 0)
    {
        why = "not a .gol";
        return false;
    }
    const uint32_t nv = BE32(&g[0x48]), nn = BE32(&g[0x4C]), nt = BE32(&g[0x50]);
    const uint64_t need = 0x58 + uint64_t(nv) * 12 + 4 + uint64_t(nn) * 32 + uint64_t(nt) * 22;
    if (need != g.size())
    {
        why = "file size does not match the header counts (vertices/nodes/triangles)";
        return false;
    }
    payload.assign(g.begin(), g.begin() + 0x58);
    PutBE32(&payload[0x0C], 9);   // 9 = data in the stream
    payload.resize(0x5C, 0);
    size_t p = 0x58 + size_t(nv) * 12 + 4;
    if (nn)
        Append(payload, &g[p], 32);   // root node
    payload.resize(0x7C, 0);
    blob.clear();
    blob.reserve(size_t(nv) * 16 + 32 + size_t(nn) * 64 + size_t(nt) * 32);
    for (uint32_t i = 0; i < nv; i++)
    {
        Append(blob, &g[0x58 + size_t(i) * 12], 12);
        AppendF32(blob, 1.0f);
    }
    blob.resize(AlignUp(uint32_t(blob.size()), 32), 0);
    for (uint32_t i = 0; i < nn; i++, p += 32)
    {
        const uint8_t* q = &g[p];   // {A, minimum xyz, size xyz, children}
        const float sx = BEF32(q + 16), sy = BEF32(q + 20), sz = BEF32(q + 24);
        float s = sx * sx + sy * sy;   // same summation order as the game (matches the original streams)
        s = s + sz * sz;
        Append(blob, q + 4, 12);
        AppendF32(blob, 1.0f);
        Append(blob, q + 16, 12);
        AppendF32(blob, 1.0f);
        Append(blob, q + 28, 4);
        Append(blob, q, 4);
        AppendF32(blob, sqrtf(s));
        blob.resize(blob.size() + 20, 0);
    }
    for (uint32_t i = 0; i < nt; i++, p += 22)
    {
        const uint8_t* q = &g[p];   // {d, normal xyz, 3 indices u16}
        Append(blob, q + 4, 12);
        AppendF32(blob, 1.0f);
        Append(blob, q, 4);
        Append(blob, q + 16, 6);
        blob.resize(blob.size() + 6, 0);
    }
    return true;
}
#pragma clang fp contract(on)

// Texture size in the stream from the fetch constant (end of the last level, aligned to 4 KB)
uint32_t TextureBytes(const uint32_t fetch[6])
{
    const Fetch f = DecodeFetch(fetch);
    const Format* fmt = FindFormat(f.format);
    if (!fmt)
        return 0;
    const uint32_t faces = f.dimension == 3 ? 6 : 1;
    uint32_t end = 0;
    for (const auto& l : Levels(f, fmt->bw, fmt->bpb))
        end = std::max(end, l.base + l.stride * faces);
    return AlignUp(end, 0x1000);
}

struct DdsInfo
{
    uint32_t w = 0, h = 0, mips = 1, xenos = 0;   // xenos: equivalent Xbox 360 format (0 = unsupported)
    bool cube = false;
};

bool ReadDdsInfo(const std::vector<uint8_t>& d, uint32_t origFormat, DdsInfo& out)
{
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; };
    if (d.size() < 128 || u32(0) != FourCC("DDS "))
        return false;
    out.h = u32(12);
    out.w = u32(16);
    out.mips = std::max(u32(28), 1u);
    const uint32_t pf = u32(80), fourcc = u32(84), bits = u32(88), r = u32(92), g = u32(96), b = u32(100), a = u32(104);
    out.cube = (u32(112) & 0x200) != 0;
    const bool as16 = origFormat >= 51 && origFormat <= 53;   // k_DXTn_AS_16_16_16_16
    const uint32_t dxt1 = as16 ? 51 : 18, dxt3 = as16 ? 52 : 19, dxt5 = as16 ? 53 : 20;
    const uint32_t argb = (origFormat == 14 || origFormat == 50) ? origFormat : 6;
    if ((pf & 4) && fourcc == FourCC("DX10"))
    {
        out.cube = (u32(136) & 4) != 0;
        switch (u32(128))
        {
        case 71: case 72: out.xenos = dxt1; break;
        case 74: case 75: out.xenos = dxt3; break;
        case 77: case 78: out.xenos = dxt5; break;
        case 80: out.xenos = 59; break;
        case 83: out.xenos = 49; break;
        case 87: case 91: case 28: case 29: out.xenos = argb; break;
        case 85: out.xenos = 4; break;
        case 86: out.xenos = 3; break;
        case 115: out.xenos = 15; break;
        case 61: out.xenos = 2; break;
        }
    }
    else if (pf & 4)
    {
        if (fourcc == FourCC("DXT1"))
            out.xenos = dxt1;
        else if (fourcc == FourCC("DXT2") || fourcc == FourCC("DXT3"))
            out.xenos = dxt3;
        else if (fourcc == FourCC("DXT4") || fourcc == FourCC("DXT5"))
            out.xenos = dxt5;
        else if (fourcc == FourCC("ATI1") || fourcc == FourCC("BC4U"))
            out.xenos = 59;
        else if (fourcc == FourCC("ATI2") || fourcc == FourCC("BC5U"))
            out.xenos = 49;
    }
    else if (bits == 32 && g == 0xFF00 && ((r == 0xFF0000 && b == 0xFF) || (r == 0xFF && b == 0xFF0000)))
        out.xenos = argb;
    else if (bits == 16 && r == 0xF800 && g == 0x7E0 && b == 0x1F)
        out.xenos = 4;
    else if (bits == 16 && r == 0x7C00 && g == 0x3E0 && b == 0x1F)
        out.xenos = 3;
    else if (bits == 16 && r == 0xF00 && g == 0xF0 && b == 0xF)
        out.xenos = 15;
    else if (bits == 16 && r == 0xFF && a == 0xFF00)
        out.xenos = 10;
    else if (bits == 8 && r == 0xFF)
        out.xenos = 2;
    return true;
}

// Fetch constant for a .dds of a different size / mipmap count / format: changes only width, height, pitch, format,
// last mip and mip_address (right after level 0); like = fetch of another texture of the package in the new format when
// the kind changes (DXT <-> ARGB): endian and swizzle come from it
void MakeFetch(const uint32_t orig[6], uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const uint32_t* like, uint32_t out[6])
{
    memcpy(out, orig, 24);
    if (like)
    {
        out[1] = (out[1] & ~0xC0u) | (like[1] & 0xC0u);
        out[3] = like[3];
    }
    out[0] = (out[0] & ~(0x1FFu << 22)) | (((AlignUp(w, 32) / 32) & 0x1FF) << 22);
    out[1] = (out[1] & ~0x3Fu) | format;
    out[2] = (out[2] & ~0x3FFFFFFu) | ((w - 1) & 0x1FFF) | (((h - 1) & 0x1FFF) << 13);
    out[4] = (out[4] & ~(0xFu << 6) & ~(0xFu << 2)) | (((levels - 1) & 0xF) << 6);
    out[5] &= 0xFFFu;
    if (levels > 1)
    {
        const Fetch f = DecodeFetch(out);
        const Format* fmt = FindFormat(format);
        if (fmt)
            out[5] |= Levels(f, fmt->bw, fmt->bpb)[0].stride * (f.dimension == 3 ? 6 : 1);
    }
}

struct SoiRec
{
    uint32_t type;           // 0 model, 1 texture, 2 collision (as in the .toc)
    size_t lead, payload, payloadLen;
    std::string name;
};

bool XgsLen(const uint8_t* d, size_t n, size_t& len) { return WalkModel(d, n, false, len, nullptr, nullptr); }

bool ParseSoi(const std::vector<uint8_t>& s, std::vector<SoiRec>& recs, std::string& why)
{
    if (s.size() < 0x58)
    {
        why = ".soi too short";
        return false;
    }
    uint32_t h[15];
    for (int i = 0; i < 15; i++)
        h[i] = BE32(&s[4 * i]);
    // textures: the h[6] stream ones and, right after them, h[7] with the PC DDS inside the .soi (index -1; 86 in HUB)
    const uint32_t secs[3][3] = { { 2, h[3], h[12] }, { 0, h[4], h[10] }, { 1, h[6] + h[7], h[13] } };
    for (const auto& sec : secs)
    {
        size_t p = 0x3C + size_t(sec[2]);
        for (uint32_t k = 0; k < sec[1]; k++)
        {
            if (p + 0x110 > s.size())
            {
                why = "record outside the .soi";
                return false;
            }
            SoiRec r;
            r.type = sec[0];
            r.lead = p;
            r.name.assign((const char*)&s[p + 4], strnlen((const char*)&s[p + 4], 0x100));
            const uint32_t nprops = BE32(&s[p + 0x10C]);
            const size_t q = p + 0x110 + size_t(nprops) * 0x208;
            if (nprops > 4096 || q > s.size())
            {
                why = "properties of " + r.name + " outside the .soi";
                return false;
            }
            size_t len = 0;
            if (r.type == 1)
            {
                if (q + 0x38 <= s.size() && BE32(&s[q + 0x18]) == 0xFFFF0000 && BE32(&s[q + 0x1C]) == 0xFFFF0000)
                    len = 0x38;
                else if (q + 8 <= s.size() && memcmp(&s[q + 4], "DDS ", 4) == 0)
                    len = 4 + size_t(BE32(&s[q]));
            }
            else if (r.type == 2)
                len = (q + 0x7C <= s.size() && BE32(&s[q]) == 0x4D2 && memcmp(&s[q + 4], "col", 4) == 0) ? 0x7C : 0;
            else if (!XgsLen(&s[q], s.size() - q, len))
                len = 0;
            if (!len || q + len > s.size())
            {
                why = "content of " + r.name + " not recognized";
                return false;
            }
            r.payload = q;
            r.payloadLen = len;
            recs.push_back(std::move(r));
            p = q + len + 0x3C;
        }
    }
    std::sort(recs.begin(), recs.end(), [](const SoiRec& a, const SoiRec& b) { return a.lead < b.lead; });
    for (size_t i = 0; i + 1 < recs.size(); i++)
        if (recs[i].payload + recs[i].payloadLen + 0x3C != recs[i + 1].lead)
        {
            why = ".soi records are not contiguous (" + recs[i].name + ")";
            return false;
        }
    if (!recs.empty() && recs.back().payload + recs.back().payloadLen != s.size())
    {
        why = "leftover bytes at the end of the .soi";
        return false;
    }
    return true;
}

struct TocRes
{
    size_t pos;
    std::string name;
    uint32_t f[6];
};
struct TocSector
{
    size_t pos;
    uint32_t hdr[21];
    uint32_t rec[8] = {};
    std::vector<uint32_t> table;
    std::vector<TocRes> res;
};

bool ParseToc(const std::vector<uint8_t>& t, bool blocked, std::vector<TocSector>& out, std::string& why)
{
    const size_t recSize = blocked ? 0x20 : 0x10;
    size_t p = 0;
    while (p < t.size())
    {
        if (p + 0x154 + recSize > t.size())
        {
            why = "incomplete sector in the .toc";
            return false;
        }
        TocSector s;
        s.pos = p;
        for (int i = 0; i < 21; i++)
            s.hdr[i] = BE32(&t[p + 0x100 + 4 * i]);
        size_t q = p + 0x154;
        for (size_t i = 0; i < recSize / 4; i++)
            s.rec[i] = BE32(&t[q + 4 * i]);
        q += recSize;
        const uint64_t nb = blocked ? uint64_t(s.rec[6]) + s.rec[7] : 0;
        const uint64_t nres = s.hdr[1];
        if (nb > 1000000 || nres > 1000000 || q + nb * 4 + nres * 0x118 > t.size())
        {
            why = "sector with invalid counts in the .toc";
            return false;
        }
        for (uint64_t k = 0; k < nb; k++, q += 4)
            s.table.push_back(BE32(&t[q]));
        for (uint64_t k = 0; k < nres; k++, q += 0x118)
        {
            TocRes r;
            r.pos = q;
            r.name.assign((const char*)&t[q], strnlen((const char*)&t[q], 0x100));
            for (int i = 0; i < 6; i++)
                r.f[i] = BE32(&t[q + 0x100 + 4 * i]);
            s.res.push_back(std::move(r));
        }
        out.push_back(std::move(s));
        p = q;
    }
    return true;
}

fs::path LoosePath(const fs::path& root, const fs::path& packDir, const std::string& name, uint32_t type)
{
    static const char* const ext[] = { ".xng", ".dds", ".gol" };
    std::vector<std::string> parts;
    std::string cur;
    for (char c : name)
    {
        if (c == '\\' || c == '/')
        {
            if (!cur.empty())
                parts.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty())
        parts.push_back(cur);
    fs::path p;
    if (parts.size() > 3 && Lower(parts[0]) == "c" && Lower(parts[1]) == "scenes")
    {
        p = root;
        for (const auto& s : parts)
            p /= s;
    }
    else
    {
        p = packDir;
        for (size_t i = parts.size() >= 2 ? parts.size() - 2 : 0; i < parts.size(); i++)
            p /= parts[i];
    }
    p += ext[type < 3 ? type : 0];
    return p;
}

// New content of a resource: data in the stream + content of the .soi record
struct LooseItem
{
    std::vector<uint8_t> data, payload;
    bool resized = false, changed = false;
};

struct LooseStats
{
    uint32_t models = 0, textures = 0, collisions = 0, changed = 0, resized = 0;
};

// CRC32 (same as zlib's), 8 bytes per step
uint32_t Crc32(const uint8_t* p, size_t n)
{
    static uint32_t t[8][256];
    static const bool init = [] {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int k = 1; k < 8; k++)
                t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
        return true;
    }();
    (void)init;
    uint32_t c = 0xFFFFFFFFu;
    for (; n >= 8; p += 8, n -= 8)
    {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= c;
        c = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^ t[3][b & 0xFF] ^ t[2][(b >> 8) & 0xFF] ^
            t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
    }
    while (n--)
        c = (c >> 8) ^ t[0][(c ^ *p++) & 0xFF];
    return ~c;
}

// _package\X.files.txt (tools/scenes_decompile.py): "CRC32 path" of each loose file as it was extracted
std::map<std::string, uint32_t> ReadManifest(const fs::path& file)
{
    std::map<std::string, uint32_t> out;
    for (const auto& line : ReadLines(file))
        if (line.size() > 9 && line[0] != '#' && line[8] == ' ')
            out[Lower(line.substr(9))] = uint32_t(strtoul(line.substr(0, 8).c_str(), nullptr, 16));
    return out;
}

bool LoadLooseItem(const fs::path& file, uint32_t type, const std::vector<uint8_t>& soi, const SoiRec* rec, uint32_t origSize,
                   const std::vector<SoiRec>& allRecs, const uint32_t* origCrc, LooseItem& it, std::string& why)
{
    std::vector<uint8_t> d;
    if (!ReadWhole(file, d))
    {
        why = "file not found";
        return false;
    }
    if (origCrc && Crc32(d.data(), d.size()) != *origCrc)
        it.changed = true;
    const std::vector<uint8_t> orig = rec ? std::vector<uint8_t>(soi.begin() + rec->payload, soi.begin() + rec->payload + rec->payloadLen)
                                          : std::vector<uint8_t>();
    if (type == 0)
    {
        size_t len = 0;
        if (!WalkModel(d.data(), d.size(), true, len, &it.payload, &it.data))
        {
            why = "invalid .xng";
            return false;
        }
        if (len != d.size())
            fprintf(stderr, "[mods] warning: %s has %zu leftover bytes at the end\n", file.string().c_str(), d.size() - len);
    }
    else if (type == 2)
    {
        if (!GolToCol(d, it.payload, it.data, why))
            return false;
    }
    else
    {
        if (!rec)
        {
            why = "texture without a record in the .soi";
            return false;
        }
        if (orig.size() != 0x38)   // PC DDS inside the package (CR_01 MiniMap): the whole .dds goes in
        {
            it.payload.clear();
            AppendBE32(it.payload, uint32_t(d.size()));
            Append(it.payload, d.data(), d.size());
            it.data = d;
        }
        else
        {
            uint32_t fetch[6];
            for (int i = 0; i < 6; i++)
                fetch[i] = BE32(&orig[0x20 + 4 * i]);
            const Fetch f = DecodeFetch(fetch);
            const Format* fmt = FindFormat(f.format);
            DdsInfo di;
            if (!fmt || !ReadDdsInfo(d, f.format, di))
            {
                why = fmt ? "not a DDS file" : "format of the original not supported";
                return false;
            }
            const uint32_t levels = uint32_t(Levels(f, fmt->bw, fmt->bpb).size());
            uint32_t nf[6];
            memcpy(nf, fetch, sizeof(nf));
            uint32_t size = origSize;
            if (di.w != f.width || di.h != f.height || di.mips != levels || di.xenos != f.format || di.cube != (f.dimension == 3))
            {
                const Format* nfmt = di.xenos ? FindFormat(di.xenos) : nullptr;
                if (!nfmt || !di.w || !di.h || di.w > 8192 || di.h > 8192 || di.cube != (f.dimension == 3))
                {
                    why = "unsupported .dds format (use DXT1/DXT3/DXT5, or ARGB 8888 like the original" +
                          std::string(f.dimension == 3 ? ", cube" : "") + ")";
                    return false;
                }
                const uint32_t* like = nullptr;
                uint32_t likeBuf[6];
                if (nfmt->kind != fmt->kind)   // DXT <-> ARGB: endian/swizzle from another texture of the package in that format
                {
                    for (const auto& r : allRecs)
                        if (r.type == 1 && r.payloadLen == 0x38)
                        {
                            for (int i = 0; i < 6; i++)
                                likeBuf[i] = BE32(&soi[r.payload + 0x20 + 4 * i]);
                            const Format* lf = FindFormat(DecodeFetch(likeBuf).format);
                            if (lf && lf->kind == nfmt->kind)
                            {
                                like = likeBuf;
                                break;
                            }
                        }
                    if (!like)
                    {
                        why = "the package has no texture in that format; save it in the same kind as the original";
                        return false;
                    }
                }
                const uint32_t mips = std::min(di.mips, Log2Floor(std::max(di.w, di.h)) + 1);
                MakeFetch(fetch, di.xenos, di.w, di.h, mips, like, nf);
                size = TextureBytes(nf);
                it.changed = true;
            }
            it.data.assign(size, 0);
            if (!EncodeTexture(it.data, nf, d, why))
                return false;
            it.payload = orig;
            for (int i = 0; i < 6; i++)
                PutBE32(&it.payload[0x20 + 4 * i], nf[i]);
        }
    }
    it.resized = it.data.size() != origSize;
    if (rec && it.payload != orig)
        it.changed = true;
    it.changed |= it.resized;
    return true;
}

struct LooseOut
{
    std::vector<uint8_t> res, str;
    bool blocked = true;
};

bool BuildLoosePack(const fs::path& root, const fs::path& packDir, const std::string& stem, LooseOut& out, std::string& why)
{
    const auto t0 = std::chrono::steady_clock::now();
    const fs::path skel = packDir / L"_package";
    fs::path soiPath, tocPath;
    std::error_code ec;
    for (fs::directory_iterator it(skel, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string n = Lower(it->path().filename().string());
        if (n == Lower(stem) + ".soi")
            soiPath = it->path();
        else if (n == Lower(stem) + ".toc")
            tocPath = it->path();
    }
    std::vector<uint8_t> soi, toc;
    if (soiPath.empty() || tocPath.empty() || !ReadWhole(soiPath, soi) || !ReadWhole(tocPath, toc))
    {
        why = "skeleton " + (skel / (stem + ".soi")).string() + " / .toc not found";
        return false;
    }
    std::vector<SoiRec> recs;
    if (!ParseSoi(soi, recs, why))
        return false;
    out.blocked = BE32(&soi[0]) >= 0x102;
    std::vector<TocSector> sectors;
    if (!ParseToc(toc, out.blocked, sectors, why))
        return false;
    if (sectors.size() != BE32(&soi[8]))
        fprintf(stderr, "[mods] warning: %s: %zu sectors in the .toc, %u in the .soi\n", stem.c_str(), sectors.size(), BE32(&soi[8]));
    std::map<std::string, std::vector<size_t>> soiByName;   // "name|type" -> records
    for (size_t i = 0; i < recs.size(); i++)
        soiByName[Lower(recs[i].name) + "|" + char('0' + recs[i].type)].push_back(i);

    // Resources: each loose file converted once (the same resource can be in several HUB sectors)
    const auto manifest = ReadManifest(skel / (stem + ".files.txt"));
    std::map<std::string, LooseItem> items;
    LooseStats st;
    for (const auto& s : sectors)
        for (const auto& r : s.res)
        {
            const uint32_t type = r.f[5];
            if (type > 2)
                continue;
            const std::string key = Lower(r.name) + "|" + char('0' + type);
            if (items.count(key))
                continue;
            auto byName = soiByName.find(key);
            const SoiRec* rec = byName != soiByName.end() ? &recs[byName->second[0]] : nullptr;
            const fs::path file = LoosePath(root, packDir, r.name, type);
            LooseItem it;
            std::string w;
            const std::string rel = Lower(file.lexically_relative(root / L"C" / L"Scenes").string());
            auto m = manifest.find(rel);
            if (!LoadLooseItem(file, type, soi, rec, r.f[4], recs, m != manifest.end() ? &m->second : nullptr, it, w))
            {
                why = file.string() + ": " + w;
                return false;
            }
            (type == 0 ? st.models : type == 1 ? st.textures : st.collisions)++;
            st.changed += it.changed;
            st.resized += it.resized;
            if (it.changed)
                fprintf(stderr, "[mods] %s: %s%s\n", stem.c_str(), file.lexically_relative(root).string().c_str(),
                        it.resized ? " (new size)" : " (modified)");
            items.emplace(key, std::move(it));
        }
    // Textures that only exist in the .soi (PC DDS inside the package, outside the stream): the loose .dds, if any
    for (const auto& r : recs)
    {
        const std::string key = Lower(r.name) + "|d";
        auto inToc = items.find(Lower(r.name) + "|1");
        if (r.type != 1 || r.payloadLen == 0x38 || items.count(key) || (inToc != items.end() && inToc->second.payload.size() != 0x38))
            continue;
        std::vector<uint8_t> d;
        const fs::path file = LoosePath(root, packDir, r.name, 1);
        if (!ReadWhole(file, d) || d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0)
            continue;
        LooseItem it;
        AppendBE32(it.payload, uint32_t(d.size()));
        Append(it.payload, d.data(), d.size());
        it.changed = it.payload.size() != r.payloadLen || memcmp(it.payload.data(), &soi[r.payload], r.payloadLen) != 0;
        auto m = manifest.find(Lower(file.lexically_relative(root / L"C" / L"Scenes").string()));
        if (m != manifest.end() && Crc32(d.data(), d.size()) != m->second)
            it.changed = true;
        st.textures++;
        st.changed += it.changed;
        if (it.changed)
            fprintf(stderr, "[mods] %s: %s (modified)\n", stem.c_str(), file.lexically_relative(root).string().c_str());
        items.emplace(key, std::move(it));
    }

    // Sectors: position of each resource in its part (keeps the original ones if nothing changed size)
    struct SectorData
    {
        std::vector<uint8_t> a, b;
    };
    std::vector<SectorData> data(sectors.size());
    std::vector<std::vector<uint32_t>> newOff(sectors.size()), newSize(sectors.size());
    for (size_t si = 0; si < sectors.size(); si++)
    {
        auto& s = sectors[si];
        newOff[si].resize(s.res.size());
        newSize[si].resize(s.res.size());
        uint32_t ends[2] = { 0, 0 };
        for (int part = 0; part < 2; part++)
        {
            std::vector<size_t> idx;
            bool resized = false;
            for (size_t k = 0; k < s.res.size(); k++)
            {
                const uint32_t type = s.res[k].f[5];
                if (type > 2 || (type == 2) != (part == 1))
                    continue;
                idx.push_back(k);
                const auto& it = items[Lower(s.res[k].name) + "|" + char('0' + type)];
                newSize[si][k] = uint32_t(it.data.size());
                resized |= newSize[si][k] != s.res[k].f[4];
            }
            std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return s.res[x].f[3] < s.res[y].f[3]; });
            const uint32_t align = part == 0 ? 0x1000 : 0x80;
            uint32_t pos = 0;
            std::map<uint32_t, uint32_t> moved;   // original position -> new (repeated entries at the same place)
            for (size_t k : idx)
            {
                uint32_t o = s.res[k].f[3];
                if (resized)
                {
                    auto m = moved.find(o);
                    if (m != moved.end())
                        o = m->second;
                    else
                    {
                        const uint32_t n = AlignUp(pos, align);
                        moved[o] = n;
                        o = n;
                    }
                }
                newOff[si][k] = o;
                pos = std::max(pos, o + newSize[si][k]);
            }
            // part total: the original if everything fit; otherwise the aligned end (4 KB; 2 KB in an unblocked stream)
            ends[part] = AlignUp(pos, out.blocked ? 0x1000 : 0x800);
            const uint32_t orig = s.rec[2 + part];
            const uint32_t total = (!resized && orig >= pos) ? orig : ends[part];
            auto& buf = part == 0 ? data[si].a : data[si].b;
            buf.assign(total, 0);
            for (size_t k : idx)
            {
                const auto& it = items[Lower(s.res[k].name) + "|" + char('0' + s.res[k].f[5])];
                memcpy(buf.data() + newOff[si][k], it.data.data(), it.data.size());
            }
        }
    }

    // Raw stream: sectors in the original .str order; in 0x102, 128 KB blocks (the last one of each part aligned to 4 KB)
    std::vector<size_t> order;
    for (size_t si = 0; si < sectors.size(); si++)
        if (sectors[si].rec[1] || !data[si].a.empty() || !data[si].b.empty())
            order.push_back(si);
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return sectors[x].rec[0] < sectors[y].rec[0]; });
    std::vector<bool> hasData(sectors.size(), false);
    for (size_t si : order)
        hasData[si] = true;
    std::vector<std::array<uint32_t, 8>> recOut(sectors.size());
    std::vector<std::vector<uint32_t>> tableOut(sectors.size());
    size_t total = 0;
    for (size_t si : order)
        total += data[si].a.size() + data[si].b.size();
    out.str.clear();
    out.str.reserve(total);
    for (size_t si = 0; si < sectors.size(); si++)
        for (int i = 0; i < 8; i++)
            recOut[si][i] = sectors[si].rec[i];
    for (size_t si : order)
    {
        const uint32_t da = uint32_t(data[si].a.size()), db = uint32_t(data[si].b.size());
        auto& r = recOut[si];
        r[0] = uint32_t(out.str.size());
        r[1] = da + db;
        r[2] = da;
        r[3] = db;
        if (out.blocked)
        {
            r[4] = da;
            r[5] = db;
            r[6] = (da + kBlock - 1) / kBlock;
            r[7] = (db + kBlock - 1) / kBlock;
            for (uint32_t left = da; left; left -= std::min(left, kBlock))
                tableOut[si].push_back(std::min(left, kBlock));
            for (uint32_t left = db; left; left -= std::min(left, kBlock))
                tableOut[si].push_back(std::min(left, kBlock));
        }
        Append(out.str, data[si].a.data(), da);
        Append(out.str, data[si].b.data(), db);
    }

    // new .toc
    std::vector<uint8_t> ntoc;
    ntoc.reserve(toc.size() + 4096);
    for (size_t si = 0; si < sectors.size(); si++)
    {
        const auto& s = sectors[si];
        Append(ntoc, &toc[s.pos], 0x154);
        for (int i = 0; i < (out.blocked ? 8 : 4); i++)
            AppendBE32(ntoc, recOut[si][i]);
        for (uint32_t v : (out.blocked && hasData[si] ? tableOut[si] : s.table))
            AppendBE32(ntoc, v);
        for (size_t k = 0; k < s.res.size(); k++)
        {
            const auto& r = s.res[k];
            Append(ntoc, &toc[r.pos], 0x100);
            uint32_t f[6];
            memcpy(f, r.f, sizeof(f));
            if (f[5] <= 2)
            {
                f[3] = newOff[si][k];
                f[4] = newSize[si][k];
            }
            for (uint32_t v : f)
                AppendBE32(ntoc, v);
        }
    }

    // new .soi: content of each record replaced; section positions and stream memory slots adjusted
    std::vector<uint8_t> nsoi;
    nsoi.reserve(soi.size() + 4096);
    std::vector<std::pair<size_t, int64_t>> growth;   // (end of the original content, accumulated growth up to there)
    int64_t grown = 0;
    size_t cursor = 0;
    auto pick = [&](const SoiRec& r) -> const LooseItem* {
        auto it = items.find(Lower(r.name) + "|" + char('0' + r.type));
        if (r.type == 1)   // stream texture (0x38 header) or PC DDS in the .soi: the new content must be of the same kind
        {
            const bool dds = r.payloadLen != 0x38;
            if (it != items.end() && (it->second.payload.size() != 0x38) == dds)
                return &it->second;
            it = items.find(Lower(r.name) + "|d");
            return dds && it != items.end() ? &it->second : nullptr;
        }
        return it != items.end() ? &it->second : nullptr;
    };
    for (const auto& r : recs)
    {
        Append(nsoi, &soi[cursor], r.payload - cursor);
        const LooseItem* it = pick(r);
        if (it && !it->payload.empty())
        {
            Append(nsoi, it->payload.data(), it->payload.size());
            grown += int64_t(it->payload.size()) - int64_t(r.payloadLen);
        }
        else
            Append(nsoi, &soi[r.payload], r.payloadLen);
        cursor = r.payload + r.payloadLen;
        growth.push_back({ cursor, grown });
    }
    Append(nsoi, soi.data() + cursor, soi.size() - cursor);
    auto remap = [&](size_t oldPos) {
        int64_t g = 0;
        for (const auto& e : growth)
            if (e.first <= oldPos)
                g = e.second;
        return size_t(int64_t(oldPos) + g);
    };
    for (int i = 10; i <= 13; i++)
    {
        const uint32_t v = BE32(&soi[4 * i]);
        PutBE32(&nsoi[4 * i], uint32_t(remap(0x3C + size_t(v)) - 0x3C));
    }
    const uint32_t slotsA = BE32(&soi[0x20]), slotsB = BE32(&soi[0x24]);
    if (0x50 + 4 * size_t(slotsA + slotsB) <= nsoi.size())
        for (size_t si = 0; si < sectors.size(); si++)
            for (int part = 0; part < 2; part++)
            {
                const uint32_t slot = sectors[si].hdr[5 + part] == 0xFFFFFFFF ? 0 : sectors[si].hdr[5 + part];
                const uint32_t count = part == 0 ? slotsA : slotsB;
                if (slot >= count)
                    continue;
                uint8_t* p = &nsoi[0x50 + 4 * size_t((part == 0 ? 0 : slotsA) + slot)];
                const uint32_t need = recOut[si][2 + part];
                if (need > BE32(p))
                {
                    fprintf(stderr, "[mods] %s: stream slot %c%u grown from %X to %X\n", stem.c_str(), part ? 'B' : 'A', slot, BE32(p), need);
                    PutBE32(p, need);
                }
            }

    // .res with the new .soi and .toc (the .xnm materials stay loose)
    const std::string soiName = soiPath.filename().string(), tocName = tocPath.filename().string();
    const uint32_t dirSize0 = 24 + 12 * 2 + uint32_t(soiName.size() + tocName.size());
    const uint32_t dataStart = AlignUp(dirSize0, 0x800);
    const uint32_t tocOff = AlignUp(uint32_t(nsoi.size()), 0x800);
    out.res.assign(size_t(dataStart) + tocOff + AlignUp(uint32_t(ntoc.size()), 0x800), 0);
    uint8_t* p = out.res.data();
    auto put32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
    put32(3);
    put32(8);
    put32(0);
    put32(0);
    put32(2);
    put32(dataStart - 24);
    for (const auto& e : { std::make_pair(&soiName, std::make_pair(0u, uint32_t(nsoi.size()))),
                           std::make_pair(&tocName, std::make_pair(tocOff, uint32_t(ntoc.size()))) })
    {
        put32(uint32_t(e.first->size()));
        memcpy(p, e.first->data(), e.first->size());
        p += e.first->size();
        put32(e.second.first);
        put32(e.second.second);
    }
    memcpy(out.res.data() + dataStart, nsoi.data(), nsoi.size());
    memcpy(out.res.data() + dataStart + tocOff, ntoc.data(), ntoc.size());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[mods] loose scene built: %s (%u .xng models, %u .dds textures, %u .gol collisions; %u modified, %u with a new size), "
                    "stream %.1f MB in %.0f ms\n",
            (packDir / stem).lexically_relative(root).string().c_str(), st.models, st.textures, st.collisions, st.changed, st.resized,
            out.str.size() / 1048576.0, ms);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Temporary file per folder, built once per run (deleted when the game closes)

struct Built
{
    std::wstring path;
    HANDLE keep = nullptr;
    uint64_t size = 0;
};
std::mutex g_mutex;
std::map<std::wstring, Built> g_built;

std::wstring Key(const fs::path& folder, bool stream)
{
    std::wstring k = folder.wstring();
    for (auto& c : k)
        c = wchar_t(towlower(c));
    return k + (stream ? L"|s" : L"|r");
}

bool WriteTemp(const std::vector<uint8_t>& data, Built& out)
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    static int counter = 0;
    const std::wstring path = std::wstring(dir) + L"CarsRecomp_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(++counter) + L".tmp";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "[mods] ERROR: could not create the temporary file %ls\n", path.c_str());
        return false;
    }
    for (size_t done = 0; done < data.size();)
    {
        DWORD n = 0;
        const DWORD chunk = DWORD(std::min<size_t>(data.size() - done, 64u << 20));
        if (!WriteFile(h, data.data() + done, chunk, &n, nullptr) || !n)
            break;
        done += n;
    }
    out = Built{ path, h, data.size() };
    return true;
}

FILE* OpenTemp(const Built& b)
{
    HANDLE r = CreateFileW(b.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (r == INVALID_HANDLE_VALUE)
        return nullptr;
    const int fd = _open_osfhandle(intptr_t(r), _O_RDONLY | _O_BINARY);
    return fd >= 0 ? _fdopen(fd, "rb") : nullptr;
}

// Loose scenes: .res and .str built together (the .toc depends on the stream layout), once per run
struct LooseBuilt
{
    bool tried = false, ok = false, blocked = true;
    Built res, str;
};
std::map<std::wstring, LooseBuilt> g_loose;

LooseBuilt* GetLoose(const fs::path& root, const fs::path& packDir, const std::string& stem)
{
    LooseBuilt& b = g_loose[Key(packDir / stem, false)];
    if (!b.tried)
    {
        b.tried = true;
        LooseOut out;
        std::string why;
        if (!BuildLoosePack(root, packDir, stem, out, why))
            fprintf(stderr, "[mods] ERROR loose scene %s: %s (using the game's original package)\n", (packDir / stem).string().c_str(), why.c_str());
        else
        {
            b.blocked = out.blocked;
            b.ok = WriteTemp(out.res, b.res) && WriteTemp(out.str, b.str);
        }
    }
    return b.ok ? &b : nullptr;
}
}  // namespace

bool LooseSceneExists(const fs::path& packDir, const std::string& stem)
{
    std::error_code ec;
    return fs::is_regular_file(packDir / L"_package" / (stem + ".soi"), ec) && fs::is_regular_file(packDir / L"_package" / (stem + ".toc"), ec);
}

FILE* LooseSceneOpen(const fs::path& root, const fs::path& packDir, const std::string& stem, bool stream, uint64_t& size, bool& rawBlocks)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LooseBuilt* b = GetLoose(root, packDir, stem);
    if (!b)
        return nullptr;
    const Built& f = stream ? b->str : b->res;
    size = f.size;
    rawBlocks = stream && b->blocked;
    return OpenTemp(f);
}

bool LooseSceneSize(const fs::path& root, const fs::path& packDir, const std::string& stem, bool stream, uint64_t& size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LooseBuilt* b = GetLoose(root, packDir, stem);
    if (!b)
        return false;
    size = (stream ? b->str : b->res).size;
    return true;
}

bool LooseSceneBuildFiles(const fs::path& root, const fs::path& packDir, const std::string& stem, const fs::path& outRes, const fs::path& outStr)
{
    LooseOut out;
    std::string why;
    if (!BuildLoosePack(root, packDir, stem, out, why))
    {
        fprintf(stderr, "ERROR: %s\n", why.c_str());
        return false;
    }
    for (const auto& [path, data] : { std::make_pair(&outRes, &out.res), std::make_pair(&outStr, &out.str) })
    {
        FILE* f = _wfopen(path->c_str(), L"wb");
        if (!f)
            return false;
        fwrite(data->data(), 1, data->size(), f);
        fclose(f);
    }
    return true;
}

bool SceneFolderSize(const fs::path& folder, bool stream, uint64_t& size)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_built.find(Key(folder, stream));
        if (it != g_built.end())
        {
            size = it->second.size;
            return true;
        }
    }
    if (stream)
        return StreamSize(folder, size);
    ResLayout layout;
    if (!ResFolderLayout(folder, layout))
        return false;
    size = layout.total;
    return true;
}

FILE* SceneFolderOpen(const fs::path& folder, bool stream, uint64_t& size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring key = Key(folder, stream);
    auto it = g_built.find(key);
    if (it == g_built.end())
    {
        std::vector<uint8_t> data;
        if (!(stream ? BuildStream(folder, data) : BuildRes(folder, data)))
            return nullptr;
        Built b;
        if (!WriteTemp(data, b))
            return nullptr;
        it = g_built.emplace(key, b).first;
    }
    size = it->second.size;
    return OpenTemp(it->second);
}
