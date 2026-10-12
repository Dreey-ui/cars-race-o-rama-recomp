// Backend D3D12 para a GPU Xenos.
// Estado: registradores do processador de comandos PM4 (como o hardware ve). Shaders: XenosRecomp -> DXIL.
// Render targets: uma textura do host por (base na EDRAM, pitch, formato). Resolve: copia o RT para uma textura
// do host associada ao endereco de destino; texturas amostradas nesse endereco usam a copia do host.
#include "renderer.h"
#include "shaders.h"
#include "textures.h"
#include "memwatch.h"
#include "model_capture.h"
#include "settings.h"
#include "session_log.h"
#include "xenos_regs.h"
#include "runtime.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

static constexpr uint32_t kFrameCount = 2;
static constexpr uint64_t kUploadSize = 192ull << 20;
static constexpr uint32_t kSrvHeapSize = 65536;
static constexpr uint32_t kSamplerHeapSize = 2048;
static constexpr uint32_t kRtvHeapSize = 1024;
static constexpr uint32_t kDsvHeapSize = 256;
static constexpr uint32_t kRtHeight = 1024;   // a EDRAM nao tem altura; RTs do host tem altura fixa
static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// ---------------------------------------------------------------------------------------------
// Estado global

static HWND g_hwnd = nullptr;
static std::atomic<bool> g_resizePending{ false };
static bool g_vsync = true;
static bool g_enabled = false;
static bool g_logDraws = false;
static uint32_t* g_regs = nullptr;
static std::vector<uint64_t> g_dumpFrames;   // CARS_DUMP_FRAMES
static uint64_t g_traceFrame = 0;             // CARS_TRACE_FRAME: registra todos os draws/resolves deste frame
static FILE* g_traceOut = nullptr;            // F12: o trace vai para marca_NN\frame.txt (senao para o log)
static std::filesystem::path g_captureDir;    // F12: pasta dos dumps do frame registrado (vazio = pasta atual)
static FILE* TraceOut() { return g_traceOut ? g_traceOut : stderr; }
static std::filesystem::path CapturePath(const char* name) { return g_captureDir.empty() ? std::filesystem::path(name) : g_captureDir / name; }
static std::atomic<uint64_t> g_lastSwapTick{ 0 };   // GetTickCount64 do ultimo swap (detector de travada em main.cpp)

static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<IDXGISwapChain3> g_swapChain;
static ComPtr<ID3D12Resource> g_backBuffers[kFrameCount];
static ComPtr<ID3D12DescriptorHeap> g_bbRtvHeap;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static HANDLE g_fenceEvent = nullptr;
static uint64_t g_fenceValue = 0;

struct Frame
{
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12Resource> upload;
    uint8_t* uploadPtr = nullptr;
    uint64_t uploadOffset = 0;
    uint64_t fence = 0;
};
static Frame g_frames[kFrameCount];
static uint32_t g_frameIndex = 0;
static uint64_t g_frameNumber = 1;

static ComPtr<ID3D12DescriptorHeap> g_srvHeap, g_samplerHeap, g_rtvHeap, g_dsvHeap;
static uint32_t g_srvSize = 0, g_samplerSize = 0, g_rtvSize = 0, g_dsvSize = 0;
static uint32_t g_srvNext = 3, g_samplerNext = 1, g_rtvNext = 1, g_dsvNext = 0;   // srv 0..2 = texturas padrao; rtv 0 = nulo

static ComPtr<ID3D12RootSignature> g_rootSig, g_blitRootSig;
static ComPtr<ID3D12PipelineState> g_blitPso;
static std::vector<uint8_t> g_rectGs;
static ComPtr<ID3D12Resource> g_defaultTextures[3];

static uint32_t g_resolvesThisFrame = 0;
static GuestShader* g_vs = nullptr;
static GuestShader* g_ps = nullptr;

struct Stats
{
    uint32_t draws = 0, skippedShader = 0, pendingShader = 0, skippedOther = 0, resolves = 0, psos = 0, texUploads = 0, swaps = 0;
    double drawMs = 0, texMs = 0, waitMs = 0, presentMs = 0, bindMs = 0, vbMs = 0, psoMs = 0, hashMs = 0;
    double worstFrameMs = 0;
} g_stats;

struct ScopedMs
{
    double& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ScopedMs(double& a) : acc(a) {}
    ~ScopedMs() { acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
};

// ---------------------------------------------------------------------------------------------
// Janela

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:
        if (getenv("CARS_HIDDEN_WINDOW"))
            return 0;   // janela de teste automatico: encerrada pelo script
        fprintf(stderr, "[janela] fechada pelo usuario\n");
        ModelCaptureFinish();   // captura de modelos ligada: salva o que ja foi visto
        SessionLogEnd("janela fechada pelo usuario");
        ExitProcess(0);
    case WM_ACTIVATEAPP:
        fprintf(stderr, "[janela] %s\n", wp ? "jogo em foco" : "jogo perdeu o foco (outra janela na frente)");
        break;
    case WM_SIZE:
    {
        g_resizePending = true;
        static WPARAM lastType = SIZE_RESTORED;
        if (wp != lastType && (wp == SIZE_MINIMIZED || wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED))
            fprintf(stderr, "[janela] %s (%ux%u)\n", wp == SIZE_MINIMIZED ? "minimizada" : wp == SIZE_MAXIMIZED ? "maximizada" : "restaurada",
                    unsigned(LOWORD(lp)), unsigned(HIWORD(lp)));
        lastType = wp;
        return 0;
    }
    case WM_EXITSIZEMOVE:
    {
        static RECT last{};
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right != last.right || rc.bottom != last.bottom)
            fprintf(stderr, "[janela] tamanho %ldx%ld\n", rc.right, rc.bottom);
        last = rc;
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void WindowThread(std::promise<HWND>* ready)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"CarsRecompWindow";
    RegisterClassExW(&wc);
    RECT rc{ 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Cars Race-O-Rama (recomp)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd)
        ShowWindow(hwnd, getenv("CARS_HIDDEN_WINDOW") ? SW_SHOWMINNOACTIVE : SW_SHOW);   // testes automaticos: minimizada
    ready->set_value(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---------------------------------------------------------------------------------------------
// Utilitarios D3D12

static D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(ID3D12DescriptorHeap* heap, uint32_t index, uint32_t size)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(index) * size;
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(ID3D12DescriptorHeap* heap, uint32_t index, uint32_t size)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += uint64_t(index) * size;
    return h;
}

static void WaitForFence(uint64_t value)
{
    if (g_fence->GetCompletedValue() < value)
    {
        g_fence->SetEventOnCompletion(value, g_fenceEvent);
        while (WaitForSingleObject(g_fenceEvent, 2000) == WAIT_TIMEOUT)
        {
            const HRESULT reason = g_device->GetDeviceRemovedReason();
            fprintf(stderr, "[gpu] esperando a GPU ha 2 s (fence %llu, completada %llu, device removed reason %08X)\n",
                    (unsigned long long)value, (unsigned long long)g_fence->GetCompletedValue(), unsigned(reason));
            if (FAILED(reason))
            {
                fprintf(stderr, "[gpu] device D3D12 removido: desligando o renderer\n");
                g_enabled = false;
                return;
            }
        }
    }
}

// Estado ja aplicado na lista de comandos atual (para nao repetir chamadas iguais a cada draw)
struct BoundState
{
    bool valid = false;
    uint32_t numRts = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4]{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    float blendFactor[4]{};
    uint32_t stencilRef = 0;
    ID3D12PipelineState* pso = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS cb[3]{};
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};
static BoundState g_bound;

// Bancos de constantes ALU escritos desde o ultimo draw (bit 0 VS, bit 1 PS): kernel_gpu.cpp
extern uint32_t g_aluDirty;
// Copias das constantes no buffer de upload do frame atual (reusadas enquanto o jogo nao escreve nelas)
static D3D12_GPU_VIRTUAL_ADDRESS g_vsCbCache = 0, g_psCbCache = 0;

static void BeginCommandList()
{
    Frame& f = g_frames[g_frameIndex];
    f.alloc->Reset();
    g_cmd->Reset(f.alloc.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
    g_cmd->SetDescriptorHeaps(2, heaps);
    g_bound = BoundState{};
    // o buffer de upload deste frame recomeca do zero: as copias de constantes antigas nao valem mais
    g_vsCbCache = g_psCbCache = 0;
}

static void SubmitAndWait()
{
    g_cmd->Close();
    ID3D12CommandList* lists[] = { g_cmd.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence.Get(), ++g_fenceValue);
    WaitForFence(g_fenceValue);
    g_frames[g_frameIndex].uploadOffset = 0;
    BeginCommandList();
}

static uint8_t* UploadAlloc(uint64_t size, uint64_t align, D3D12_GPU_VIRTUAL_ADDRESS* gpu, ID3D12Resource** res = nullptr,
                            uint64_t* offsetOut = nullptr)
{
    if (size > kUploadSize)
        return nullptr;
    Frame* f = &g_frames[g_frameIndex];
    uint64_t off = (f->uploadOffset + align - 1) & ~(align - 1);
    if (off + size > kUploadSize)
    {
        SubmitAndWait();
        f = &g_frames[g_frameIndex];
        off = 0;
    }
    f->uploadOffset = off + size;
    if (gpu)
        *gpu = f->upload->GetGPUVirtualAddress() + off;
    if (res)
        *res = f->upload.Get();
    if (offsetOut)
        *offsetOut = off;
    return f->uploadPtr + off;
}

struct HostTexture
{
    ComPtr<ID3D12Resource> res;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    uint32_t components = 4;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    std::unordered_map<uint32_t, uint32_t> srvBySwizzle;
    uint32_t rtv = UINT32_MAX, dsv = UINT32_MAX;
    // Textura de resolve com RB_COPY_DEST_INFO.copy_dest_swap: no Xenos o resolve grava com vermelho e azul trocados (e o
    // swizzle do fetch destroca). Aqui a copia e direta, entao a troca e aplicada no SRV (veja GetSrv)
    bool rbSwap = false;
};

static void Transition(HostTexture& t, D3D12_RESOURCE_STATES s)
{
    if (t.state == s)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = s;
    g_cmd->ResourceBarrier(1, &b);
    t.state = s;
}

static void TransitionRaw(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    g_cmd->ResourceBarrier(1, &b);
}

static bool IsBlockCompressed(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

static bool CreateTexture2D(HostTexture& t, DXGI_FORMAT fmt, uint32_t w, uint32_t h, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE* clear = nullptr)
{
    if (IsBlockCompressed(fmt))
    {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
    }
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, clear, IID_PPV_ARGS(&t.res))))
        return false;
    t.format = fmt;
    t.width = w;
    t.height = h;
    t.state = initial;
    t.srvBySwizzle.clear();
    return true;
}

static uint32_t AllocSrv()
{
    if (g_srvNext >= kSrvHeapSize)
        g_srvNext = 3;   // em ultimo caso reaproveita (nao deveria acontecer)
    return g_srvNext++;
}

// SRV com o swizzle do fetch constant (3 bits por componente: 0..3 = X..W, 4 = 0, 5 = 1)
// applyRbSwap = false: mapeamento direto (apresentacao do frontbuffer, que o hardware de video le sem o swizzle do fetch)
static uint32_t GetSrv(HostTexture& t, uint32_t swizzle, bool applyRbSwap = true)
{
    const bool rb = applyRbSwap && t.rbSwap;
    const uint32_t key = swizzle | (rb ? 1u << 12 : 0u);
    auto it = t.srvBySwizzle.find(key);
    if (it != t.srvBySwizzle.end())
        return it->second;
    uint32_t map[4];
    for (int c = 0; c < 4; c++)
    {
        uint32_t sel = (swizzle >> (3 * c)) & 7;
        if (rb && (sel == 0 || sel == 2))
            sel ^= 2;   // o componente X da memoria (com dest_swap) e o Z do render target, e vice-versa
        if (sel < 4)
            map[c] = std::min(sel, t.components - 1);   // componentes ausentes repetem o ultimo
        else if (sel == 5)
            map[c] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
        else
            map[c] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = t.format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map[0], map[1], map[2], map[3]);
    sd.Texture2D.MipLevels = 1;
    const uint32_t index = AllocSrv();
    g_device->CreateShaderResourceView(t.res.Get(), &sd, CpuHandle(g_srvHeap.Get(), index, g_srvSize));
    t.srvBySwizzle.emplace(key, index);
    return index;
}

// data.data vazio: decodifica a textura guest (fetch) direto no buffer de upload
static void UploadTexture2D(HostTexture& t, const HostTextureData& data, const TextureFetch* fetch = nullptr)
{
    ID3D12Resource* upload = nullptr;
    uint64_t offset = 0;
    const size_t bytes = size_t(data.rowPitch) * data.rows;
    uint8_t* dst = UploadAlloc(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, nullptr, &upload, &offset);
    if (!dst)
        return;
    if (fetch)
        DecodeGuestTexture2D(*fetch, data, dst);
    else
        memcpy(dst, data.data.data(), bytes);
    Transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = offset;
    srcLoc.PlacedFootprint.Footprint.Format = data.format;
    srcLoc.PlacedFootprint.Footprint.Width = t.width;
    srcLoc.PlacedFootprint.Footprint.Height = t.height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = data.rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = t.res.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    Transition(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_stats.texUploads++;
}

// ---------------------------------------------------------------------------------------------
// Caches: texturas guest, texturas de resolve, render targets, samplers, PSOs

struct GuestTexEntry
{
    HostTexture tex;
    uint64_t contentHash = 0;
    uint64_t quickHash = 0;     // hash por amostragem (checado todo frame)
    uint64_t lastFullCheck = 0; // hash completo a cada 120 frames
    uint64_t lastCheck = 0;
    bool valid = false;
};
static std::unordered_map<uint64_t, GuestTexEntry*> g_guestTextures;
static std::unordered_map<uint32_t, HostTexture*> g_resolved;   // endereco fisico -> copia do resolve

struct RenderTarget
{
    HostTexture tex;
    bool depth = false;
    uint32_t xenosFormat = 0;
};
static std::unordered_map<uint64_t, RenderTarget*> g_rts;
static std::unordered_map<uint64_t, uint32_t> g_samplers;
static std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> g_psos;

static DXGI_FORMAT ColorRtFormat(uint32_t f)
{
    switch (f)
    {
    case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3: case 12: case 5: case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static DXGI_FORMAT DepthRtFormat(uint32_t f) { return f ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT; }

static RenderTarget* GetRenderTarget(uint32_t base, uint32_t pitch, uint32_t format, bool depth)
{
    // Chave pelo formato do HOST: formatos Xenos com os mesmos bits (2 e 10, 0 e 1...) sao a mesma memoria na EDRAM
    const uint64_t hostFmt = uint64_t(depth ? DepthRtFormat(format) : ColorRtFormat(format));
    const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (hostFmt << 32) | (uint64_t(depth) << 48);
    auto it = g_rts.find(key);
    if (it != g_rts.end())
        return it->second;
    auto* rt = new RenderTarget();
    rt->depth = depth;
    rt->xenosFormat = format;
    D3D12_CLEAR_VALUE clear{};
    if (depth)
    {
        clear.Format = DepthRtFormat(format);
        clear.DepthStencil.Depth = 1.0f;
        if (!CreateTexture2D(rt->tex, clear.Format, pitch, kRtHeight, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear))
            return nullptr;
        rt->tex.dsv = g_dsvNext++ % kDsvHeapSize;
        g_device->CreateDepthStencilView(rt->tex.res.Get(), nullptr, CpuHandle(g_dsvHeap.Get(), rt->tex.dsv, g_dsvSize));
        g_cmd->ClearDepthStencilView(CpuHandle(g_dsvHeap.Get(), rt->tex.dsv, g_dsvSize),
                                     D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }
    else
    {
        clear.Format = ColorRtFormat(format);
        if (!CreateTexture2D(rt->tex, clear.Format, pitch, kRtHeight, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear))
            return nullptr;
        rt->tex.rtv = g_rtvNext++ % kRtvHeapSize;
        g_device->CreateRenderTargetView(rt->tex.res.Get(), nullptr, CpuHandle(g_rtvHeap.Get(), rt->tex.rtv, g_rtvSize));
        const float black[4] = {};
        g_cmd->ClearRenderTargetView(CpuHandle(g_rtvHeap.Get(), rt->tex.rtv, g_rtvSize), black, 0, nullptr);
    }
    if (g_logDraws)
        fprintf(stderr, "[gpu] novo RT %s base=%u pitch=%u formato=%u\n", depth ? "depth" : "cor", base, pitch, format);
    g_rts.emplace(key, rt);
    return rt;
}

// Hash por amostragem: 64 blocos de 64 bytes espalhados (texturas pequenas: hash completo)
static uint64_t QuickHashGuestMemory(uint32_t addr, uint32_t size)
{
    if (size <= 16384)
        return HashGuestMemory(addr, size);
    uint8_t buf[64 * 64];
    const uint8_t* p = g_mem.base + addr;
    for (uint32_t i = 0; i < 64; i++)
    {
        const uint64_t off = uint64_t(size - 64) * i / 63;
        memcpy(buf + i * 64, p + off, 64);
    }
    return XXH3_64bits(buf, sizeof(buf)) ^ size;
}

static HostTexture* GetGuestTexture(const TextureFetch& f)
{
    struct Key { uint32_t base, format, w, h, pitch, tiled, endian, dim; } k{ f.baseAddress, f.format, f.width, f.height,
                                                                             f.pitchTexels, f.tiled, f.endian, f.dimension };
    const uint64_t key = XXH3_64bits(&k, sizeof(k));
    GuestTexEntry*& e = g_guestTextures[key];
    if (!e)
        e = new GuestTexEntry();
    if (e->lastCheck == g_frameNumber && e->valid)
        return &e->tex;
    e->lastCheck = g_frameNumber;
    uint32_t addr = 0, size = 0;
    if (!GuestTextureExtent(f, addr, size))
        return e->valid ? &e->tex : nullptr;
    // Mudancas sao detectadas por um hash rapido de 64 amostras espalhadas a cada frame; o hash completo roda
    // a cada 120 frames (ou quando a amostra muda), em vez de hashear a textura inteira todo frame
    uint64_t h = 0, q = 0;
    {
        ScopedMs th(g_stats.hashMs);
        q = QuickHashGuestMemory(addr, size);
        if (e->valid && q == e->quickHash && g_frameNumber - e->lastFullCheck < 120)
            return &e->tex;
        h = HashGuestMemory(addr, size);
        e->lastFullCheck = g_frameNumber;
    }
    if (!e->valid && g_logDraws)
        fprintf(stderr, "[gpu] textura nova: fmt %u %ux%u pitch %u tiled %d endian %u em %08X (%u bytes)\n", f.format, f.width,
                f.height, f.pitchTexels, f.tiled, f.endian, addr, size);
    if (e->valid && h == e->contentHash)
    {
        e->quickHash = q;
        return &e->tex;
    }
    ScopedMs timer(g_stats.texMs);
    HostTextureData data;
    if (!PrepareGuestTexture2D(f, data))
        return nullptr;
    if (!e->valid || e->tex.width < data.width || e->tex.format != data.format || e->tex.height < data.height ||
        e->tex.width != ((IsBlockCompressed(data.format) ? (data.width + 3) & ~3u : data.width)))
    {
        e->tex = HostTexture();
        if (!CreateTexture2D(e->tex, data.format, data.width, data.height, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST))
            return nullptr;
    }
    e->tex.components = data.components;
    UploadTexture2D(e->tex, data, &f);
    e->contentHash = h;
    e->quickHash = q;
    e->valid = true;
    return &e->tex;
}

static D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t c)
{
    switch (c)
    {
    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 2: case 4: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
}

static uint32_t GetSampler(const TextureFetch& f)
{
    const uint64_t key = uint64_t(f.clampX) | (uint64_t(f.clampY) << 3) | (uint64_t(f.clampZ) << 6) | (uint64_t(f.magFilter) << 9) |
                         (uint64_t(f.minFilter) << 11) | (uint64_t(f.mipFilter) << 13) | (uint64_t(f.anisoFilter) << 15) |
                         (uint64_t(f.borderColor) << 18);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end())
        return it->second;
    D3D12_SAMPLER_DESC sd{};
    const auto lin = [](uint32_t v) { return v == 1 || v == 3 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT; };
    if (f.anisoFilter != 0 && f.anisoFilter != 7)
    {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min(16u, 1u << (f.anisoFilter - 1));
    }
    else
    {
        sd.Filter = D3D12_ENCODE_BASIC_FILTER(lin(f.minFilter), lin(f.magFilter), lin(f.mipFilter), D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        sd.MaxAnisotropy = 1;
    }
    sd.AddressU = AddressMode(f.clampX);
    sd.AddressV = AddressMode(f.clampY);
    sd.AddressW = AddressMode(f.clampZ);
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    if (f.borderColor == 1)
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    const uint32_t index = g_samplerNext++ % kSamplerHeapSize;
    g_device->CreateSampler(&sd, CpuHandle(g_samplerHeap.Get(), index, g_samplerSize));
    g_samplers.emplace(key, index);
    return index;
}

// ---------------------------------------------------------------------------------------------
// Inicializacao

static const char kBlitHlsl[] = R"HLSL(
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
cbuffer Blit : register(b0) { float2 uvScale; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
VsOut vsmain(uint id : SV_VertexID)
{
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p * uvScale;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 psmain(VsOut i) : SV_Target { return float4(tex.Sample(samp, i.uv).rgb, 1); }
)HLSL";

// Rect list do Xenos: 3 vertices de um retangulo; o 4o e deduzido (oposto ao canto do angulo reto)
static const char kRectGsHlsl[] = R"HLSL(
struct V
{
    float4 pos : SV_Position;
    float4 t[16] : TEXCOORD0;
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
};
[maxvertexcount(4)]
void main(triangle V v[3], inout TriangleStream<V> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d02 = dot(p0 - p2, p0 - p2), d12 = dot(p1 - p2, p1 - p2);
    uint c, a, b;
    if (d12 >= d01 && d12 >= d02) { c = 0; a = 1; b = 2; }
    else if (d02 >= d01) { c = 1; a = 0; b = 2; }
    else { c = 2; a = 0; b = 1; }
    V d;
    d.pos = v[a].pos + v[b].pos - v[c].pos;
    [unroll] for (int i = 0; i < 16; i++)
        d.t[i] = v[a].t[i] + v[b].t[i] - v[c].t[i];
    d.c0 = v[a].c0 + v[b].c0 - v[c].c0;
    d.c1 = v[a].c1 + v[b].c1 - v[c].c1;
    s.Append(v[c]);
    s.Append(v[a]);
    s.Append(v[b]);
    s.Append(d);
}
)HLSL";

static bool CreateRootSignatures()
{
    // Igual ao que o HLSL do XenosRecomp espera: b0/b1/b2 em space4 + heaps bindless em space0..3
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    for (int i = 0; i < 4; i++)
    {
        ranges[i].RangeType = i == 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = UINT_MAX;
        ranges[i].BaseShaderRegister = 0;
        ranges[i].RegisterSpace = i;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[7]{};
    for (int i = 0; i < 3; i++)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[i].Descriptor.ShaderRegister = i;
        params[i].Descriptor.RegisterSpace = 4;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (int i = 0; i < 4; i++)
    {
        params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[3 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[3 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 7;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    {
        fprintf(stderr, "[gpu] root signature: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig))))
        return false;

    // Blit do frontbuffer para o backbuffer
    D3D12_DESCRIPTOR_RANGE srvRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER bp[2]{};
    bp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    bp[0].Constants.Num32BitValues = 2;
    bp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    bp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    bp[1].DescriptorTable.NumDescriptorRanges = 1;
    bp[1].DescriptorTable.pDescriptorRanges = &srvRange;
    bp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC brs{ 2, bp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    if (FAILED(D3D12SerializeRootSignature(&brs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_blitRootSig))))
        return false;

    // Os shaders internos (blit e GS) usam entry points proprios
    std::string vsSrc = std::string(kBlitHlsl) + "\n";
    std::vector<uint8_t> vs, ps;
    std::string vsMain = vsSrc;
    vsMain += "VsOut main(uint id : SV_VertexID) { return vsmain(id); }\n";
    std::string psMain = vsSrc;
    psMain += "float4 main(VsOut i) : SV_Target { return psmain(i); }\n";
    if (!CompileHlsl(vsMain.c_str(), L"vs_6_0", vs) || !CompileHlsl(psMain.c_str(), L"ps_6_0", ps))
        return false;
    if (!CompileHlsl(kRectGsHlsl, L"gs_6_0", g_rectGs))
        fprintf(stderr, "[gpu] aviso: geometry shader de rect list nao compilou\n");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_blitRootSig.Get();
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    return SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_blitPso)));
}

static void CreateBackBufferViews()
{
    for (uint32_t i = 0; i < kFrameCount; i++)
    {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
        g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, CpuHandle(g_bbRtvHeap.Get(), i, g_rtvSize));
    }
}

static void CreateDefaultTextures()
{
    // 0 = 2D branco, 1 = 3D branco, 2 = cubo branco (texturas nao suportadas/ausentes)
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    for (int i = 0; i < 3; i++)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = i == 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1;
        d.Height = 1;
        d.DepthOrArraySize = i == 2 ? 6 : 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&g_defaultTextures[i]));
        const uint32_t subresources = i == 2 ? 6 : 1;
        for (uint32_t s = 0; s < subresources; s++)
        {
            ID3D12Resource* upload = nullptr;
            uint64_t off = 0;
            uint8_t* p = UploadAlloc(256, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, nullptr, &upload, &off);
            memset(p, 0xFF, 4);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = off;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256 };
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_defaultTextures[i].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = s;
            g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        TransitionRaw(g_defaultTextures[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (i == 0) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; }
        if (i == 1) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1; }
        if (i == 2) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1; }
        g_device->CreateShaderResourceView(g_defaultTextures[i].Get(), &sd, CpuHandle(g_srvHeap.Get(), i, g_srvSize));
    }
    // Sampler 0: linear com wrap
    D3D12_SAMPLER_DESC sd{};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    sd.MaxAnisotropy = 1;
    g_device->CreateSampler(&sd, CpuHandle(g_samplerHeap.Get(), 0, g_samplerSize));
    // RTV nulo (slot 0) para render targets nao usados no meio da lista
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(nullptr, &rd, CpuHandle(g_rtvHeap.Get(), 0, g_rtvSize));
}

bool RendererInit(bool vsync, uint32_t* registers, const uint8_t* image, size_t imageSize)
{
    g_vsync = vsync;
    g_regs = registers;
    g_logDraws = getenv("CARS_GPU_LOG") != nullptr;
    if (const char* tf = getenv("CARS_TRACE_FRAME"))
        g_traceFrame = strtoull(tf, nullptr, 10);
    if (const char* dump = getenv("CARS_DUMP_FRAMES"))
        for (const char* p = dump; *p;)
        {
            g_dumpFrames.push_back(strtoull(p, const_cast<char**>(&p), 10));
            while (*p == ',' || *p == ' ')
                p++;
            if (*p && (*p < '0' || *p > '9'))
                break;
        }

    ShadersInit(g_gameRoot, image, imageSize);

    std::promise<HWND> ready;
    auto fut = ready.get_future();
    std::thread(WindowThread, &ready).detach();
    g_hwnd = fut.get();
    if (!g_hwnd)
        return false;

    if (getenv("CARS_D3D12_DEBUG"))
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
    {
        fprintf(stderr, "[gpu] falha ao criar o device D3D12\n");
        return false;
    }
    {
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 ad{};
        if (SUCCEEDED(factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc1(&ad)))
        {
            LARGE_INTEGER umd{};
            adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
            char name[128];
            WideCharToMultiByte(CP_ACP, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
            fprintf(stderr, "[sessao] GPU: %s (%.1f GB de VRAM, driver %u.%u.%u.%u)\n", name, ad.DedicatedVideoMemory / 1073741824.0,
                    unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)), unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
        }
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
            fprintf(stderr, "[sessao] monitor: %lux%lu a %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    }
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Format = kBackBufferFormat;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = kFrameCount;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &sc, nullptr, nullptr, &sc1)))
        return false;
    sc1.As(&g_swapChain);
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    auto makeHeap = [](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{ type, n, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        return SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    };
    makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrameCount, false, g_bbRtvHeap);
    makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, true, g_srvHeap);
    makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true, g_samplerHeap);
    makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize, false, g_rtvHeap);
    makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize, false, g_dsvHeap);
    g_srvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_samplerSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    CreateBackBufferViews();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (auto& f : g_frames)
    {
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kUploadSize;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&f.upload))))
            return false;
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.uploadPtr));
    }
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd));
    g_cmd->Close();
    BeginCommandList();

    if (!CreateRootSignatures())
    {
        fprintf(stderr, "[gpu] falha ao criar root signatures / shaders internos\n");
        return false;
    }
    CreateDefaultTextures();
    SubmitAndWait();
    g_enabled = true;
    fprintf(stderr, "[gpu] D3D12 pronto (vsync %s)\n", vsync ? "ligado" : "desligado");
    return true;
}

bool RendererEnabled() { return g_enabled; }

// ---------------------------------------------------------------------------------------------
// Shaders

struct VsBackendData
{
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<uint32_t> fetchSlots;   // slot do IA -> fetch constant
    std::vector<uint32_t> strides;
};

static std::unordered_set<uint64_t> g_missingShaders;

void RendererSetShader(bool pixel, const uint8_t* microcode, uint32_t sizeDwords)
{
    if (!g_enabled)
        return;
    bool pending = false;
    GuestShader* s = ShadersFind(pixel, microcode, sizeDwords * 4, &pending);
    if (!s && !pending)
    {
        const uint64_t h = XXH3_64bits(microcode, sizeDwords * 4);
        if (g_missingShaders.insert(h).second && g_missingShaders.size() <= 30)
        {
            fprintf(stderr, "[gpu] %s shader desconhecido (%u dwords, hash %016llX)\n", pixel ? "pixel" : "vertex",
                    sizeDwords, (unsigned long long)h);
            char path[96];
            snprintf(path, sizeof(path), "shader_cache/missing_%c_%016llX.bin", pixel ? 'p' : 'v', (unsigned long long)h);
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(microcode, 1, sizeDwords * 4, f);
                fclose(f);
            }
        }
    }
    else if (s && !s->failed && g_logDraws && !s->backendData && !pixel)
    {
        fprintf(stderr, "[gpu] VS %s (%016llX): %zu entradas\n", s->name.c_str(), (unsigned long long)s->hash, s->inputs.size());
    }
    (pixel ? g_ps : g_vs) = s;
}

static VsBackendData* GetVsData(GuestShader* vs)
{
    if (vs->backendData)
        return static_cast<VsBackendData*>(vs->backendData);
    auto* d = new VsBackendData();
    for (const auto& in : vs->inputs)
    {
        uint32_t slot = 0;
        while (slot < d->fetchSlots.size() && d->fetchSlots[slot] != in.fetchSlot)
            slot++;
        if (slot == d->fetchSlots.size())
        {
            d->fetchSlots.push_back(in.fetchSlot);
            d->strides.push_back(in.strideBytes);
        }
        int intType = 0;
        D3D12_INPUT_ELEMENT_DESC e{};
        e.SemanticName = in.semantic;
        e.SemanticIndex = in.semanticIndex;
        e.Format = DXGI_FORMAT(VertexInputDxgiFormat(in, intType));
        e.InputSlot = slot;
        e.AlignedByteOffset = in.offsetBytes;
        e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d->elements.push_back(e);
    }
    vs->backendData = d;
    return d;
}

// ---------------------------------------------------------------------------------------------
// Estado de pipeline

static D3D12_BLEND MapBlend(uint32_t f, bool alpha)
{
    switch (f)
    {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP MapBlendOp(uint32_t op)
{
    switch (op)
    {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
    }
}

struct PsoKey
{
    GuestShader* vs;
    GuestShader* ps;
    uint32_t topologyType;
    uint32_t useGs;
    uint32_t numRts;
    DXGI_FORMAT rtFormats[4];
    DXGI_FORMAT dsFormat;
    uint32_t blend[4];
    uint32_t colorMask;
    uint32_t depthControl;
    uint32_t stencilMasks;
    uint32_t cull;          // 0 nenhum, 1 front, 2 back
    uint32_t frontCcw;
    uint32_t wireframe;
    uint32_t stripCut;      // 0 desligado, 1 = 0xFFFF, 2 = 0xFFFFFFFF (primitive restart)
};

static ID3D12PipelineState* GetPso(const PsoKey& key)
{
    const uint64_t h = XXH3_64bits(&key, sizeof(key));
    auto it = g_psos.find(h);
    if (it != g_psos.end())
        return it->second.Get();

    VsBackendData* vsd = GetVsData(key.vs);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rootSig.Get();
    pd.VS = { key.vs->dxil.data(), key.vs->dxil.size() };
    pd.PS = { key.ps->dxil.data(), key.ps->dxil.size() };
    if (key.useGs)
        pd.GS = { g_rectGs.data(), g_rectGs.size() };
    pd.BlendState.IndependentBlendEnable = TRUE;
    for (uint32_t i = 0; i < 4; i++)
    {
        auto& rt = pd.BlendState.RenderTarget[i];
        const uint32_t bc = key.blend[i];
        const uint32_t srcC = bc & 31, opC = (bc >> 5) & 7, dstC = (bc >> 8) & 31;
        const uint32_t srcA = (bc >> 16) & 31, opA = (bc >> 21) & 7, dstA = (bc >> 24) & 31;
        rt.BlendEnable = !(srcC == 1 && dstC == 0 && opC == 0 && srcA == 1 && dstA == 0 && opA == 0);
        rt.SrcBlend = MapBlend(srcC, false);
        rt.DestBlend = MapBlend(dstC, false);
        rt.BlendOp = MapBlendOp(opC);
        rt.SrcBlendAlpha = MapBlend(srcA, true);
        rt.DestBlendAlpha = MapBlend(dstA, true);
        rt.BlendOpAlpha = MapBlendOp(opA);
        rt.RenderTargetWriteMask = UINT8((key.colorMask >> (4 * i)) & 0xF);
    }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = key.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = key.frontCcw;
    pd.RasterizerState.DepthClipEnable = TRUE;
    const uint32_t dc = key.depthControl;
    if (key.dsFormat != DXGI_FORMAT_UNKNOWN)
    {
        auto& ds = pd.DepthStencilState;
        ds.DepthEnable = (dc >> 1) & 1;
        ds.DepthWriteMask = ((dc >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D12_COMPARISON_FUNC(((dc >> 4) & 7) + 1);
        ds.StencilEnable = dc & 1;
        ds.StencilReadMask = UINT8((key.stencilMasks >> 8) & 0xFF);
        ds.StencilWriteMask = UINT8((key.stencilMasks >> 16) & 0xFF);
        ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC(((dc >> 8) & 7) + 1);
        ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP(((dc >> 11) & 7) + 1);
        ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP(((dc >> 14) & 7) + 1);
        ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP(((dc >> 17) & 7) + 1);
        if ((dc >> 7) & 1)
        {
            ds.BackFace.StencilFunc = D3D12_COMPARISON_FUNC(((dc >> 20) & 7) + 1);
            ds.BackFace.StencilFailOp = D3D12_STENCIL_OP(((dc >> 23) & 7) + 1);
            ds.BackFace.StencilPassOp = D3D12_STENCIL_OP(((dc >> 26) & 7) + 1);
            ds.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP(((dc >> 29) & 7) + 1);
        }
        else
        {
            ds.BackFace = ds.FrontFace;
        }
    }
    pd.InputLayout = { vsd->elements.data(), UINT(vsd->elements.size()) };
    pd.IBStripCutValue = key.stripCut == 1   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                         : key.stripCut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                             : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topologyType);
    pd.NumRenderTargets = key.numRts;
    for (uint32_t i = 0; i < key.numRts; i++)
        pd.RTVFormats[i] = key.rtFormats[i];
    pd.DSVFormat = key.dsFormat;
    pd.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        fprintf(stderr, "[gpu] falha ao criar PSO (VS %s, PS %s)\n", key.vs->name.c_str(), key.ps->name.c_str());
        g_psos.emplace(h, nullptr);
        return nullptr;
    }
    g_stats.psos++;
    g_psos.emplace(h, pso);
    return pso.Get();
}

// ---------------------------------------------------------------------------------------------
// Draw

static const uint32_t kColorInfoRegs[4] = { xreg::RB_COLOR_INFO, xreg::RB_COLOR1_INFO, xreg::RB_COLOR2_INFO, xreg::RB_COLOR3_INFO };
static const uint32_t kBlendRegs[4] = { xreg::RB_BLENDCONTROL0, xreg::RB_BLENDCONTROL1, xreg::RB_BLENDCONTROL2, xreg::RB_BLENDCONTROL3 };

static uint32_t SwapIndex(uint32_t v, uint32_t endian, bool is32)
{
    switch (endian)
    {
    case 1: return is32 ? (((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF)) : uint16_t((v << 8) | ((v >> 8) & 0xFF));
    case 2: return is32 ? __builtin_bswap32(v) : uint16_t((v << 8) | ((v >> 8) & 0xFF));
    case 3: return is32 ? ((v << 16) | (v >> 16)) : v;
    default: return v;
    }
}

// Copia com swap de endian para o buffer de upload (dst alinhado a 16, memoria write-combined):
// pshufb de 16 bytes + store nao temporal
static void CopySwapped(uint8_t* dst, const uint8_t* src, uint32_t size, uint32_t endian)
{
    const uint32_t words = size / 4;
    if (endian == 0)
    {
        memcpy(dst, src, words * 4);
        return;
    }
    // 1/2: swap de 32 bits (os dados ficam no layout do hardware: X nos 16 bits baixos); 3: troca as metades de 16 bits
    const __m128i mask = endian == 3 ? _mm_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13)
                                     : _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    uint32_t i = 0;
    for (; i + 16 <= words; i += 16)
    {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4 + 16));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4 + 32));
        const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4 + 48));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i * 4), _mm_shuffle_epi8(a, mask));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i * 4 + 16), _mm_shuffle_epi8(b, mask));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i * 4 + 32), _mm_shuffle_epi8(c, mask));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i * 4 + 48), _mm_shuffle_epi8(d, mask));
    }
    for (; i + 4 <= words; i += 4)
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i * 4),
                         _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4)), mask));
    auto* d = reinterpret_cast<uint32_t*>(dst);
    auto* s = reinterpret_cast<const uint32_t*>(src);
    for (; i < words; i++)
        d[i] = endian == 3 ? ((s[i] << 16) | (s[i] >> 16)) : __builtin_bswap32(s[i]);
    _mm_sfence();
}

// ---------------------------------------------------------------------------------------------
// Cache de vertex/index buffers entre frames: a copia convertida fica num pool persistente (heap de upload) e as paginas
// guest ficam vigiadas (memwatch); so recopia quando a CPU do jogo escreve nelas. Paginas escritas com frequencia
// (dados dinamicos) usam o caminho por draw (anel de upload).

struct PoolChunk
{
    ComPtr<ID3D12Resource> res;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint64_t size = 0, used = 0;
};
static std::vector<PoolChunk> g_pool;
static uint64_t g_poolUsed = 0;
static constexpr uint64_t kPoolChunk = 64ull << 20, kPoolBudget = 1024ull << 20;

static uint8_t* PoolAlloc(uint64_t size, D3D12_GPU_VIRTUAL_ADDRESS& gpu)
{
    size = (size + 255) & ~255ull;
    for (auto& c : g_pool)
        if (c.size - c.used >= size)
        {
            uint8_t* p = c.cpu + c.used;
            gpu = c.gpu + c.used;
            c.used += size;
            g_poolUsed += size;
            return p;
        }
    PoolChunk c;
    c.size = std::max(kPoolChunk, size);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = c.size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&c.res))))
        return nullptr;
    const D3D12_RANGE none{ 0, 0 };
    c.res->Map(0, &none, reinterpret_cast<void**>(&c.cpu));
    c.gpu = c.res->GetGPUVirtualAddress();
    g_pool.push_back(std::move(c));
    return PoolAlloc(size, gpu);
}

struct VbEntry
{
    uint32_t epoch = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
};
static std::unordered_map<uint64_t, VbEntry> g_vbCache;

struct IbEntry
{
    uint32_t guest = 0, count = 0, flags = 0, reset = 0, epoch = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint32_t minIndex = 0, maxIndex = 0;
};
static std::unordered_map<uint64_t, IbEntry> g_ibCache;
static uint32_t g_cacheUploads = 0;   // estatistica: buffers (re)copiados para o cache
// CARS_NO_BUFFER_CACHE=1 desliga o cache (comparacao)
static const bool g_bufferCache = getenv("CARS_NO_BUFFER_CACHE") == nullptr;

static void CopySwapped(uint8_t* dst, const uint8_t* src, uint32_t size, uint32_t endian);

// VB inteiro (fetch constant: endereco + tamanho) no cache; 0 = usar o caminho sem cache
static D3D12_GPU_VIRTUAL_ADDRESS CachedVertexBuffer(uint32_t guest, uint32_t size, uint32_t endian)
{
    if (size == 0 || size > (32u << 20) || guest < 0xA0000000u)
        return 0;
    const uint32_t phys = guest & 0x1FFFFFFF;
    VbEntry& e = g_vbCache[(uint64_t(phys) << 32) | (uint64_t(size) << 2) | endian];
    const uint32_t epoch = MemWatchEpoch();
    if (e.gpu && (e.epoch == epoch || MemWatchIsClean(phys, size)))
    {
        e.epoch = epoch;
        return e.gpu;
    }
    // (Re)carrega. Vigia ANTES de copiar: uma escrita durante a copia suja a pagina e a proxima checagem recarrega
    if (!MemWatchProtect(phys, size))
    {
        e.gpu = 0;   // pagina dinamica
        return 0;
    }
    const uint32_t ep = MemWatchEpoch();
    uint8_t* dst = PoolAlloc(size, e.gpu);
    if (!dst)
    {
        e.gpu = 0;
        return 0;
    }
    CopySwapped(dst, g_mem.base + guest, size & ~3u, endian);
    e.epoch = ep;
    g_cacheUploads++;
    return e.gpu;
}

// Indices: swap + restart (vira o cut value do host) + faixa min/max numa passada
static void ConvertIndices(uint8_t* dst, const uint8_t* src, uint32_t n, bool index32, uint32_t endian, bool restart,
                           uint32_t resetIndex, uint32_t& minIndex, uint32_t& maxIndex)
{
    if (index32)
    {
        auto* d = reinterpret_cast<uint32_t*>(dst);
        auto* s = reinterpret_cast<const uint32_t*>(src);
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t v = SwapIndex(s[i], endian, true);
            if (restart && v == resetIndex)
            {
                d[i] = 0xFFFFFFFF;
                continue;
            }
            d[i] = v;
            minIndex = std::min(minIndex, v);
            maxIndex = std::max(maxIndex, v);
        }
    }
    else
    {
        auto* d = reinterpret_cast<uint16_t*>(dst);
        auto* s = reinterpret_cast<const uint16_t*>(src);
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t v = SwapIndex(s[i], endian, false);
            if (restart && v == resetIndex)
            {
                d[i] = 0xFFFF;
                continue;
            }
            d[i] = uint16_t(v);
            minIndex = std::min(minIndex, v);
            maxIndex = std::max(maxIndex, v);
        }
    }
}

// IB de um draw no cache (com min/max ja calculados); nullptr = usar o caminho sem cache
static const IbEntry* CachedIndexBuffer(uint32_t guest, uint32_t n, bool index32, uint32_t endian, bool restart, uint32_t resetIndex)
{
    const uint32_t bytes = (n * (index32 ? 4 : 2) + 3) & ~3u;
    if (guest < 0xA0000000u || bytes == 0)
        return nullptr;
    const uint32_t phys = guest & 0x1FFFFFFF;
    const uint32_t flags = (index32 ? 1u : 0u) | (endian << 1) | (restart ? 8u : 0u);
    const uint64_t key = (uint64_t(phys) << 32) ^ (uint64_t(n) << 4) ^ flags ^ (uint64_t(resetIndex) * 0x9E3779B97F4A7C15ull);
    IbEntry& e = g_ibCache[key];
    const uint32_t epoch = MemWatchEpoch();
    if (e.gpu && e.guest == guest && e.count == n && e.flags == flags && e.reset == resetIndex &&
        (e.epoch == epoch || MemWatchIsClean(phys, bytes)))
    {
        e.epoch = epoch;
        return &e;
    }
    if (!MemWatchProtect(phys, bytes))
    {
        e.gpu = 0;
        return nullptr;
    }
    const uint32_t ep = MemWatchEpoch();
    uint8_t* dst = PoolAlloc(bytes, e.gpu);
    if (!dst)
    {
        e.gpu = 0;
        return nullptr;
    }
    e.minIndex = UINT32_MAX;
    e.maxIndex = 0;
    ConvertIndices(dst, g_mem.base + guest, n, index32, endian, restart, resetIndex, e.minIndex, e.maxIndex);
    e.guest = guest;
    e.count = n;
    e.flags = flags;
    e.reset = resetIndex;
    e.epoch = ep;
    g_cacheUploads++;
    return &e;
}

struct ScreenRect { int32_t x0, y0, x1, y1; };

static ScreenRect GetScissor(uint32_t rtWidth, uint32_t rtHeight)
{
    const uint32_t* r = g_regs;
    const uint32_t tl = r[xreg::PA_SC_WINDOW_SCISSOR_TL], br = r[xreg::PA_SC_WINDOW_SCISSOR_BR];
    ScreenRect s{ int32_t(Bits(tl, 0, 14)), int32_t(Bits(tl, 16, 14)), int32_t(Bits(br, 0, 14)), int32_t(Bits(br, 16, 14)) };
    if (!(tl >> 31))
    {
        const uint32_t wo = r[xreg::PA_SC_WINDOW_OFFSET];
        const int32_t ox = SBits(wo, 0, 15), oy = SBits(wo, 16, 15);
        s.x0 += ox; s.x1 += ox; s.y0 += oy; s.y1 += oy;
    }
    const uint32_t stl = r[xreg::PA_SC_SCREEN_SCISSOR_TL], sbr = r[xreg::PA_SC_SCREEN_SCISSOR_BR];
    s.x0 = std::max(s.x0, SBits(stl, 0, 15));
    s.y0 = std::max(s.y0, SBits(stl, 16, 15));
    s.x1 = std::min(s.x1, SBits(sbr, 0, 15));
    s.y1 = std::min(s.y1, SBits(sbr, 16, 15));
    s.x0 = std::clamp(s.x0, 0, int32_t(rtWidth));
    s.x1 = std::clamp(s.x1, 0, int32_t(rtWidth));
    s.y0 = std::clamp(s.y0, 0, int32_t(rtHeight));
    s.y1 = std::clamp(s.y1, 0, int32_t(rtHeight));
    return s;
}

struct SharedConstants
{
    uint32_t tex2D[16];
    uint32_t tex3D[16];
    uint32_t texCube[16];
    uint32_t sampler[16];
    uint32_t booleans;
    uint32_t swappedTexcoords;
    float halfPixelOffset[2];
    float alphaThreshold;
    uint32_t specConstants;
    uint32_t alphaFunc;
    uint32_t posFlags;
    float posScaleOffset[4];
};
static_assert(offsetof(SharedConstants, booleans) == 256, "layout do cbuffer SharedConstants");
static_assert(offsetof(SharedConstants, posScaleOffset) == 288, "layout do cbuffer SharedConstants");

// F12 / CARS_TRACE_FRAME: texturas usadas no frame registrado, salvas no swap
struct TraceTextureDump { HostTexture* tex; uint32_t w, h, addr; };
static std::vector<TraceTextureDump> g_traceTextureDumps;

static void BindTextures(GuestShader* s, SharedConstants& sc)
{
    for (uint32_t slot = 0; slot < 16; slot++)
    {
        if (!(s->samplerMask & (1u << slot)))
            continue;
        const TextureFetch f = DecodeTextureFetch(&g_regs[xreg::FETCH_CONSTANTS + slot * 6]);
        sc.sampler[slot] = GetSampler(f);
        if (f.type != 2)
            continue;
        if (f.dimension == 2 || f.dimension == 3)
            continue;   // 3D / cubo: textura padrao (TODO)
        HostTexture* t = nullptr;
        auto it = g_resolved.find(f.baseAddress & 0x1FFFFFFF);
        const bool fromResolve = it != g_resolved.end();
        if (fromResolve)
            t = it->second;
        else
            t = GetGuestTexture(f);
        // CARS_LOG_FULLSCREEN=1: cada uso de textura de tela cheia (diagnostico das telas de carregamento)
        static const bool logFullscreen = getenv("CARS_LOG_FULLSCREEN") != nullptr;
        if (logFullscreen && f.width == 1280 && f.height == 720)
            fprintf(stderr, "[tela] frame %llu: s%u fmt %u endian %u em %08X %s VS=%s PS=%s\n", (unsigned long long)g_frameNumber, slot,
                    f.format, f.endian, f.baseAddress, t ? (fromResolve ? "resolve" : "ok") : "FALHOU", g_vs ? g_vs->name.c_str() : "-",
                    s->name.c_str());
        if (g_frameNumber == g_traceFrame)
        {
            const uint8_t* m = g_mem.base + GpuPhysToGuest(f.baseAddress);
            fprintf(TraceOut(), "[frame]   textura s%u (do draw seguinte): fmt %u %ux%u dim %u tiled %d endian %u swizzle %03X em %08X -> %s | "
                            "bytes %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    slot, f.format, f.width, f.height, f.dimension, f.tiled, f.endian, f.swizzle, f.baseAddress,
                    t ? (fromResolve ? "resolve" : "ok") : "FALHOU", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
            // texturas grandes nao comprimidas: salvas no fim do frame (o que foi enviado para a GPU, para comparar com a tela)
            // (uma vez cada: no HUB a mesma textura aparece em centenas de draws e cada copia da GPU custa uma espera)
            if (t && f.width >= 256 && f.height >= 256 && g_traceTextureDumps.size() < 32 &&
                (t->format == DXGI_FORMAT_R8G8B8A8_UNORM || t->format == DXGI_FORMAT_B8G8R8A8_UNORM || t->format == DXGI_FORMAT_R10G10B10A2_UNORM) &&
                std::none_of(g_traceTextureDumps.begin(), g_traceTextureDumps.end(), [&](const TraceTextureDump& d) { return d.tex == t; }))
                g_traceTextureDumps.push_back({ t, std::min(t->width, f.width), std::min(t->height, f.height), f.baseAddress });
        }
        if (!t)
            continue;
        Transition(*t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sc.tex2D[slot] = GetSrv(*t, f.swizzle);
    }
}

static bool ReadBool(uint32_t index) { return (g_regs[xreg::BOOL_CONSTANTS + index / 32] >> (index % 32)) & 1; }

static void DoResolve();
static void DumpTexture(HostTexture& t, uint32_t w, uint32_t h, const std::filesystem::path& path);

static void TraceSkip(int line)
{
    if (g_frameNumber == g_traceFrame)
        fprintf(TraceOut(), "[frame] draw PULADO (renderer.cpp:%d) VS=%s PS=%s\n", line, g_vs ? g_vs->name.c_str() : "-", g_ps ? g_ps->name.c_str() : "-");
}

void RendererDraw(const DrawCall& draw)
{
    if (!g_enabled)
        return;
    ScopedMs timer(g_stats.drawMs);
    const uint32_t* r = g_regs;
    const uint32_t mode = r[xreg::RB_MODECONTROL] & 7;
    if (mode == 6)
    {
        DoResolve();
        return;
    }
    if (mode != 4 && mode != 5)
        return;
    GuestShader* vs = g_vs;
    if (vs && vs->isRaw && g_ps && !g_ps->failed)
        vs = ShadersLinkRawVertexShader(vs, g_ps);
    if (vs && g_ps && (!vs->IsReady() || !g_ps->IsReady()))
    {
        g_stats.pendingShader++; TraceSkip(__LINE__);   // compilando em segundo plano
        return;
    }
    if (!vs || !g_ps || vs->failed || g_ps->failed || vs->dxil.empty() || g_ps->dxil.empty())
    {
        g_stats.skippedShader++; TraceSkip(__LINE__);
        return;
    }
    if (draw.numIndices == 0)
        return;

    // ---- render targets ----
    const uint32_t pitch = r[xreg::RB_SURFACE_INFO] & 0x3FFF;
    if (pitch == 0 || pitch > 8192)
    {
        g_stats.skippedOther++; TraceSkip(__LINE__);
        return;
    }
    PsoKey key{};
    key.vs = vs;
    key.ps = g_ps;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4]{};
    RenderTarget* colorRts[4]{};
    const uint32_t colorMaskReg = r[xreg::RB_COLOR_MASK];
    for (uint32_t i = 0; i < 4; i++)
    {
        rtvs[i] = CpuHandle(g_rtvHeap.Get(), 0, g_rtvSize);
        if (mode != 4 || !(g_ps->psOutputs & (1u << i)) || !((colorMaskReg >> (4 * i)) & 0xF))
            continue;
        const uint32_t info = r[kColorInfoRegs[i]];
        RenderTarget* rt = GetRenderTarget(info & 0xFFF, pitch, Bits(info, 16, 4), false);
        if (!rt)
            continue;
        colorRts[i] = rt;
        rtvs[i] = CpuHandle(g_rtvHeap.Get(), rt->tex.rtv, g_rtvSize);
        key.numRts = i + 1;
        key.rtFormats[i] = rt->tex.format;
        key.colorMask |= ((colorMaskReg >> (4 * i)) & 0xF) << (4 * i);
        key.blend[i] = r[kBlendRegs[i]];
    }
    const uint32_t dc = r[xreg::RB_DEPTHCONTROL];
    RenderTarget* depthRt = nullptr;
    if (dc & 3)
    {
        const uint32_t di = r[xreg::RB_DEPTH_INFO];
        depthRt = GetRenderTarget(di & 0xFFF, pitch, Bits(di, 16, 1), true);
        if (depthRt)
        {
            key.dsFormat = depthRt->tex.format;
            key.depthControl = dc;
            const uint32_t srm = r[xreg::RB_STENCILREFMASK];
            key.stencilMasks = srm & 0xFFFF00;
        }
    }
    if (!key.numRts && !depthRt)
    {
        if (g_frameNumber == g_traceFrame)
            fprintf(TraceOut(), "[frame]   sem RT: modo %u, psOutputs %X, mascara %08X, color info %08X, dc %08X\n", mode, g_ps->psOutputs,
                    colorMaskReg, r[xreg::RB_COLOR_INFO], dc);
        g_stats.skippedOther++; TraceSkip(__LINE__);
        return;
    }
    for (uint32_t i = 0; i < key.numRts; i++)
        if (!colorRts[i])
            key.rtFormats[i] = DXGI_FORMAT_UNKNOWN;

    // ---- primitiva ----
    std::vector<uint32_t> converted;
    bool useConverted = false;
    D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    auto indexAt = [&](uint32_t i) -> uint32_t {
        if (!draw.indexed)
            return i;
        const uint32_t base = GpuPhysToGuest(draw.indexAddress);
        const uint32_t v = draw.index32 ? *reinterpret_cast<const uint32_t*>(g_mem.base + base + i * 4)
                                        : *reinterpret_cast<const uint16_t*>(g_mem.base + base + i * 2);
        return SwapIndex(v, draw.indexEndian, draw.index32);
    };
    const uint32_t n = draw.numIndices;
    switch (draw.primType)
    {
    case 1: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
    case 2: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 3: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 4: break;
    case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 5:   // fan
        for (uint32_t i = 1; i + 1 < n; i++)
            converted.insert(converted.end(), { indexAt(0), indexAt(i), indexAt(i + 1) });
        useConverted = true;
        break;
    case 13:  // quad list
        for (uint32_t q = 0; q + 3 < n; q += 4)
            converted.insert(converted.end(), { indexAt(q), indexAt(q + 1), indexAt(q + 2), indexAt(q), indexAt(q + 2), indexAt(q + 3) });
        useConverted = true;
        break;
    case 8:   // rect list -> geometry shader
        if (g_rectGs.empty())
        {
            g_stats.skippedOther++; TraceSkip(__LINE__);
            return;
        }
        key.useGs = 1;
        break;
    case 12:  // line loop
        for (uint32_t i = 0; i < n; i++)
            converted.push_back(indexAt(i));
        converted.push_back(indexAt(0));
        topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        key.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        useConverted = true;
        break;
    default:
        g_stats.skippedOther++; TraceSkip(__LINE__);
        return;
    }

    // ---- rasterizacao ----
    const uint32_t sc = r[xreg::PA_SU_SC_MODE_CNTL];
    // Primitive restart (PA_SU_SC_MODE_CNTL.multi_prim_ib_ena): em strips, o indice VGT_MULTI_PRIM_IB_RESET_INDX separa
    // tiras. Vira o "cut value" do D3D12 (sem isso a vegetacao, desenhada como uma tira so, liga moitas distantes).
    const bool restart = draw.indexed && !useConverted && (draw.primType == 3 || draw.primType == 6) && ((sc >> 21) & 1);
    key.stripCut = restart ? (draw.index32 ? 2u : 1u) : 0u;
    const bool triangles = key.topologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !key.useGs;
    if (triangles)
    {
        if ((sc & 3) == 3)
            return;   // tudo descartado
        key.cull = (sc & 1) ? 1 : (sc & 2) ? 2 : 0;
        key.frontCcw = ((sc >> 2) & 1) == 0;
        key.wireframe = Bits(sc, 3, 2) != 0 && Bits(sc, 5, 3) == 1;
    }

    ID3D12PipelineState* pso = nullptr;
    { ScopedMs tp(g_stats.psoMs); pso = GetPso(key); }
    if (!pso)
    {
        g_stats.skippedOther++; TraceSkip(__LINE__);
        return;
    }

    // ---- indices (uma passada: swap + restart + faixa min/max) ----
    const auto vbStart = std::chrono::steady_clock::now();
    const int32_t baseVertex = int32_t(r[xreg::VGT_INDX_OFFSET]);
    uint32_t minIndex = UINT32_MAX, maxIndex = 0;
    D3D12_INDEX_BUFFER_VIEW ibv{};
    uint32_t drawCount = n;
    if (useConverted)
    {
        if (converted.empty())
            return;
        for (uint32_t v : converted)
        {
            minIndex = std::min(minIndex, v);
            maxIndex = std::max(maxIndex, v);
        }
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
        const uint32_t bytes = uint32_t(converted.size() * 4);
        memcpy(UploadAlloc(bytes, 16, &gpu), converted.data(), bytes);
        ibv = { gpu, bytes, DXGI_FORMAT_R32_UINT };
        drawCount = uint32_t(converted.size());
    }
    else if (draw.indexed)
    {
        const uint32_t isz = draw.index32 ? 4 : 2;
        const uint32_t bytes = (n * isz + 3) & ~3u;
        const uint32_t guestIb = GpuPhysToGuest(draw.indexAddress);
        const uint32_t resetIndex = draw.index32 ? r[xreg::VGT_MULTI_PRIM_IB_RESET_INDX] : (r[xreg::VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFF);
        if (const IbEntry* ib = g_bufferCache ? CachedIndexBuffer(guestIb, n, draw.index32, draw.indexEndian, restart, resetIndex) : nullptr)
        {
            minIndex = ib->minIndex;
            maxIndex = ib->maxIndex;
            ibv = { ib->gpu, bytes, draw.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
        }
        else
        {
            D3D12_GPU_VIRTUAL_ADDRESS gpu;
            uint8_t* dst = UploadAlloc(bytes, 16, &gpu);
            ConvertIndices(dst, g_mem.base + guestIb, n, draw.index32, draw.indexEndian, restart, resetIndex, minIndex, maxIndex);
            ibv = { gpu, bytes, draw.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
        }
    }
    else
    {
        minIndex = 0;
        maxIndex = n - 1;
    }
    if (minIndex > maxIndex)
        return;   // so indices de restart
    if (ModelCaptureActive())
        ModelCaptureDraw(draw, vs, g_ps, r);   // F11: guarda a malha (model_capture.cpp)

    // ---- vertex buffers: so os vertices [min, max] do draw ----
    // O VB do host comeca no vertice vbStartVertex; o deslocamento volta no BaseVertexLocation / StartVertexLocation
    const int64_t firstVertex = int64_t(minIndex) + baseVertex;
    const uint32_t vbStartVertex = uint32_t(std::max<int64_t>(0, firstVertex));
    const uint32_t vertexCount = uint32_t(int64_t(maxIndex) + baseVertex - int64_t(vbStartVertex) + 1);
    VsBackendData* vsd = GetVsData(vs);
    D3D12_VERTEX_BUFFER_VIEW vbvs[32]{};
    const uint32_t numVbs = std::min<uint32_t>(uint32_t(vsd->fetchSlots.size()), 32);
    for (uint32_t i = 0; i < numVbs; i++)
    {
        const uint32_t slot = vsd->fetchSlots[i];
        const uint32_t d0 = r[xreg::FETCH_CONSTANTS + slot * 2], d1 = r[xreg::FETCH_CONSTANTS + slot * 2 + 1];
        if ((d0 & 3) != 3)
            continue;
        const uint32_t addr = GpuPhysToGuest(d0 & ~3u);
        const uint32_t fetchSize = Bits(d1, 2, 24) * 4;
        const uint32_t stride = vsd->strides[i];
        const uint64_t srcOff = uint64_t(vbStartVertex) * stride;
        if (srcOff >= fetchSize)
            continue;
        uint64_t size = stride ? uint64_t(vertexCount) * stride : 16;
        size = std::min<uint64_t>(size, fetchSize - srcOff);
        size = std::min<uint64_t>((size + 3) & ~3ull, 32u << 20);
        if (!size)
            continue;
        // VB inteiro no cache entre frames: a view aponta para os mesmos vertices [vbStartVertex, ...] do caminho por draw
        if (const D3D12_GPU_VIRTUAL_ADDRESS cached = g_bufferCache ? CachedVertexBuffer(addr, fetchSize, d1 & 3) : 0)
        {
            vbvs[i] = { cached + srcOff, uint32_t(std::min<uint64_t>(size, fetchSize - srcOff)), stride };
            continue;
        }
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
        uint8_t* dst = UploadAlloc(size, 16, &gpu);
        if (!dst)
            continue;
        CopySwapped(dst, g_mem.base + addr + srcOff, uint32_t(size), d1 & 3);
        vbvs[i] = { gpu, uint32_t(size), stride };
    }

    g_stats.vbMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - vbStart).count();
    // ---- constantes ----
    // Os bancos de constantes do VS (4 KB) e do PS (3,5 KB) so sao copiados de novo quando o jogo escreveu neles
    // desde o ultimo draw (g_aluDirty, marcado pelo processador de comandos); senao o draw reusa a copia anterior
    D3D12_GPU_VIRTUAL_ADDRESS vsCb, psCb, sharedCb;
    if ((g_aluDirty & 1) || !g_vsCbCache)
    {
        memcpy(UploadAlloc(4096, 256, &vsCb), &r[xreg::ALU_CONSTANTS], 4096);
        g_vsCbCache = vsCb;
    }
    vsCb = g_vsCbCache;
    if ((g_aluDirty & 2) || !g_psCbCache)
    {
        memcpy(UploadAlloc(3584, 256, &psCb), &r[xreg::ALU_CONSTANTS + 256 * 4], 3584);
        g_psCbCache = psCb;
    }
    psCb = g_psCbCache;
    g_aluDirty = 0;
    SharedConstants shared{};
    for (uint32_t i = 0; i < 16; i++)
    {
        shared.tex3D[i] = 1;
        shared.texCube[i] = 2;
    }
    { ScopedMs tb(g_stats.bindMs); BindTextures(g_ps, shared); }
    if (vs->samplerMask)
        BindTextures(vs, shared);
    for (uint32_t b : vs->boolRegs)
        if (b < 16 && ReadBool(b))
            shared.booleans |= 1u << b;
    for (uint32_t b : g_ps->boolRegs)
        if (b < 16 && ReadBool(b))
            shared.booleans |= 1u << (16 + b);
    // Os dados de vertice ja ficam no layout do hardware (X nos 16 bits baixos depois do swap de 32 bits, como no Xenia):
    // a troca yx do XenosRecomp (feita para o Sonic) inverteria TEXCOORDs de 16 bits que ja estao certos
    shared.swappedTexcoords = 0;
    const uint32_t cc = r[xreg::RB_COLORCONTROL];
    shared.alphaThreshold = AsFloat(r[xreg::RB_ALPHA_REF]);
    shared.alphaFunc = (cc & 8) ? (cc & 7) : 7;
    shared.specConstants = ((cc & 8) ? 2u : 0u) | (vs->usesPackedNormals ? 1u : 0u);

    // Viewport do Xenos -> espaco de clip do host (viewport do host = RT inteiro)
    const uint32_t rtW = pitch, rtH = kRtHeight;
    const uint32_t vte = r[xreg::PA_CL_VTE_CNTL];
    const float sx = (vte & 1) ? AsFloat(r[xreg::PA_CL_VPORT_XSCALE]) : 1.0f;
    float ox = (vte & 2) ? AsFloat(r[xreg::PA_CL_VPORT_XOFFSET]) : 0.0f;
    const float sy = (vte & 4) ? AsFloat(r[xreg::PA_CL_VPORT_YSCALE]) : 1.0f;
    float oy = (vte & 8) ? AsFloat(r[xreg::PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float zs = (vte & 16) ? AsFloat(r[xreg::PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float zo = (vte & 32) ? AsFloat(r[xreg::PA_CL_VPORT_ZOFFSET]) : 0.0f;
    if (sc & (1u << 16))
    {
        const uint32_t wo = r[xreg::PA_SC_WINDOW_OFFSET];
        ox += float(SBits(wo, 0, 15));
        oy += float(SBits(wo, 16, 15));
    }
    if ((r[xreg::PA_SU_VTX_CNTL] & 1) == 0)
    {
        ox += 0.5f;
        oy += 0.5f;
    }
    shared.posFlags = (vte >> 8) & 1;
    shared.posScaleOffset[0] = 2.0f * sx / float(rtW);
    shared.posScaleOffset[1] = -2.0f * sy / float(rtH);
    shared.posScaleOffset[2] = 2.0f * ox / float(rtW) - 1.0f;
    shared.posScaleOffset[3] = 1.0f - 2.0f * oy / float(rtH);
    memcpy(UploadAlloc(512, 256, &sharedCb), &shared, sizeof(shared));

    // ---- comandos (so o estado que mudou desde o ultimo draw: cada chamada custa no driver) ----
    const ScreenRect s = GetScissor(rtW, rtH);
    if (s.x1 <= s.x0 || s.y1 <= s.y0)
    {
        TraceSkip(__LINE__);
        return;
    }
    for (uint32_t i = 0; i < 4; i++)
        if (colorRts[i])
            Transition(colorRts[i]->tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (depthRt)
        Transition(depthRt->tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    BoundState& b = g_bound;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = depthRt ? CpuHandle(g_dsvHeap.Get(), depthRt->tex.dsv, g_dsvSize) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (!b.valid || b.numRts != key.numRts || b.dsv.ptr != dsv.ptr ||
        memcmp(b.rtvs, rtvs, sizeof(rtvs[0]) * key.numRts) != 0)
    {
        g_cmd->OMSetRenderTargets(key.numRts, rtvs, FALSE, depthRt ? &dsv : nullptr);
        b.numRts = key.numRts;
        b.dsv = dsv;
        memcpy(b.rtvs, rtvs, sizeof(rtvs));
    }
    const D3D12_VIEWPORT vp{ 0, 0, float(rtW), float(rtH), std::clamp(std::min(zo, zo + zs), 0.0f, 1.0f),
                             std::clamp(std::max(zo, zo + zs), 0.0f, 1.0f) };
    if (!b.valid || memcmp(&b.viewport, &vp, sizeof(vp)) != 0)
    {
        g_cmd->RSSetViewports(1, &vp);
        b.viewport = vp;
    }
    const D3D12_RECT rect{ s.x0, s.y0, s.x1, s.y1 };
    if (!b.valid || memcmp(&b.scissor, &rect, sizeof(rect)) != 0)
    {
        g_cmd->RSSetScissorRects(1, &rect);
        b.scissor = rect;
    }
    const float blendFactor[4] = { AsFloat(r[xreg::RB_BLEND_RED]), AsFloat(r[xreg::RB_BLEND_RED + 1]),
                                   AsFloat(r[xreg::RB_BLEND_RED + 2]), AsFloat(r[xreg::RB_BLEND_RED + 3]) };
    if (!b.valid || memcmp(b.blendFactor, blendFactor, sizeof(blendFactor)) != 0)
    {
        g_cmd->OMSetBlendFactor(blendFactor);
        memcpy(b.blendFactor, blendFactor, sizeof(blendFactor));
    }
    const uint32_t stencilRef = r[xreg::RB_STENCILREFMASK] & 0xFF;
    if (!b.valid || b.stencilRef != stencilRef)
    {
        g_cmd->OMSetStencilRef(stencilRef);
        b.stencilRef = stencilRef;
    }
    if (!b.valid)
    {
        // root signature e tabelas bindless: iguais para todos os draws da lista de comandos
        g_cmd->SetGraphicsRootSignature(g_rootSig.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE srvBase = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        g_cmd->SetGraphicsRootDescriptorTable(3, srvBase);
        g_cmd->SetGraphicsRootDescriptorTable(4, srvBase);
        g_cmd->SetGraphicsRootDescriptorTable(5, srvBase);
        g_cmd->SetGraphicsRootDescriptorTable(6, g_samplerHeap->GetGPUDescriptorHandleForHeapStart());
        b.pso = nullptr;
        b.cb[0] = b.cb[1] = b.cb[2] = 0;
        b.topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        b.valid = true;
    }
    if (b.pso != pso)
    {
        g_cmd->SetPipelineState(pso);
        b.pso = pso;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS cbs[3] = { vsCb, psCb, sharedCb };
    for (uint32_t i = 0; i < 3; i++)
        if (b.cb[i] != cbs[i])
        {
            g_cmd->SetGraphicsRootConstantBufferView(i, cbs[i]);
            b.cb[i] = cbs[i];
        }
    if (b.topology != topo)
    {
        g_cmd->IASetPrimitiveTopology(topo);
        b.topology = topo;
    }
    g_cmd->IASetVertexBuffers(0, numVbs, vbvs);

    if (useConverted || draw.indexed)
    {
        g_cmd->IASetIndexBuffer(&ibv);
        g_cmd->DrawIndexedInstanced(drawCount, 1, 0, int32_t(int64_t(baseVertex) - vbStartVertex), 0);
    }
    else
    {
        g_cmd->DrawInstanced(n, 1, uint32_t(std::max<int64_t>(0, int64_t(baseVertex) - vbStartVertex)), 0);
    }
    g_stats.draws++;
    if (g_logDraws && g_frameNumber < 400 && g_stats.draws <= 3)
        fprintf(stderr, "[gpu] draw prim=%u n=%u idx=%d VS=%s PS=%s rt0=%s pitch=%u\n", draw.primType, n, draw.indexed,
                vs->name.c_str(), g_ps->name.c_str(), colorRts[0] ? "sim" : "nao", pitch);
    if (g_frameNumber == g_traceFrame)
    {
        static std::unordered_set<GuestShader*> listed;
        if (listed.insert(vs).second)
            for (const auto& in : vs->inputs)
            {
                const uint32_t d1 = r[xreg::FETCH_CONSTANTS + in.fetchSlot * 2 + 1];
                fprintf(TraceOut(), "[frame]   VS %s entrada %s%u: fmt %u off %u stride %u slot %u int %d sinal %d rf %d exp %d endian %u indice r%u.%c\n",
                        vs->name.c_str(), in.semantic, in.semanticIndex, in.format, in.offsetBytes, in.strideBytes, in.fetchSlot,
                        in.isInteger, in.isSigned, in.signedRf, in.expAdjust, d1 & 3, in.srcRegister, "xyzw"[in.srcComponent]);
            }
        const uint32_t ci = r[xreg::RB_COLOR_INFO], di = r[xreg::RB_DEPTH_INFO];
        fprintf(TraceOut(), "[frame] DRAW prim=%u n=%u idx=%d VS=%s PS=%s | rt0 base=%u fmt=%u pitch=%u msaa=%u nrt=%u | depth %s base=%u dc=%08X"
                        " | vp s=(%.1f,%.1f,%.3f) o=(%.1f,%.1f,%.3f) vte=%03X | scissor %d,%d-%d,%d | blend0=%08X mask=%04X cull=%u"
                        " | wofs=%08X sc=%08X\n",
                draw.primType, n, draw.indexed, vs->name.c_str(), g_ps->name.c_str(), ci & 0xFFF, Bits(ci, 16, 4), pitch,
                Bits(r[xreg::RB_SURFACE_INFO], 16, 2), key.numRts, depthRt ? "sim" : "nao", di & 0xFFF, dc, sx, sy, zs, ox, oy, zo,
                vte, s.x0, s.y0, s.x1, s.y1, key.blend[0], key.colorMask, key.cull, r[xreg::PA_SC_WINDOW_OFFSET], sc);
    }
}

// ---------------------------------------------------------------------------------------------
// Resolve (copia EDRAM -> textura) e clears

static float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    uint32_t f;
    if (e == 0)
        f = m ? 0 : s << 31;   // subnormais -> 0
    else if (e == 31)
        f = (s << 31) | 0x7F800000 | (m << 13);
    else
        f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    if (e == 0 && m)
        return (s ? -1.0f : 1.0f) * float(m) / 16777216.0f;
    return AsFloat(f);
}


static void DecodeClearColor(uint32_t xenosFormat, uint32_t hi, uint32_t lo, float out[4])
{
    switch (xenosFormat)
    {
    case 0: case 1:
        for (int i = 0; i < 4; i++)
            out[i] = float((hi >> (8 * i)) & 0xFF) / 255.0f;
        break;
    case 2: case 10:
        for (int i = 0; i < 3; i++)
            out[i] = float((hi >> (10 * i)) & 0x3FF) / 1023.0f;
        out[3] = float(hi >> 30) / 3.0f;
        break;
    case 3: case 5: case 7: case 12:
        out[0] = HalfToFloat(uint16_t(lo)); out[1] = HalfToFloat(uint16_t(lo >> 16));
        out[2] = HalfToFloat(uint16_t(hi)); out[3] = HalfToFloat(uint16_t(hi >> 16));
        break;
    case 4: case 6:
        out[0] = HalfToFloat(uint16_t(hi)); out[1] = HalfToFloat(uint16_t(hi >> 16)); out[2] = 0; out[3] = 0;
        break;
    case 14:
        out[0] = AsFloat(hi); out[1] = out[2] = out[3] = 0;
        break;
    default:
        out[0] = out[1] = out[2] = out[3] = 0;
        break;
    }
}

// Texturas de destino de resolve, com a faixa de memoria guest que ocupam (textura tiled: macro tiles de 32x32).
// O D3D faz resolves parciais (predicated tiling, sub-retangulos) avancando RB_COPY_DEST_BASE em macro tiles:
// um destino dentro da faixa de uma textura existente vira um deslocamento (dx, dy) nessa textura.
struct ResolveEntry
{
    HostTexture* tex;
    uint32_t size;          // bytes na memoria guest
    uint32_t pitchAligned;  // pixels, multiplo de 32
    uint32_t bpp;
};
static std::map<uint32_t, ResolveEntry> g_resolveEntries;

static uint32_t DestFormatBpp(uint32_t f)
{
    switch (f)
    {
    case 2: case 8: case 9: return 1;
    case 3: case 4: case 5: case 10: case 15: case 24: case 30: return 2;
    case 26: case 32: case 34: case 37: return 8;
    case 35: case 38: return 16;
    default: return 4;
    }
}

static HostTexture* GetResolveTexture(uint32_t phys, DXGI_FORMAT fmt, uint32_t destPitch, uint32_t destHeight, uint32_t bpp,
                                      uint32_t needW, uint32_t needH, uint32_t& dx, uint32_t& dy)
{
    dx = dy = 0;
    // Dentro de uma textura de resolve ja existente (base menor ou igual)?
    auto it = g_resolveEntries.upper_bound(phys);
    if (it != g_resolveEntries.begin())
    {
        --it;
        const ResolveEntry& e = it->second;
        const uint32_t offset = phys - it->first;
        if (offset < e.size && e.bpp == bpp && e.tex->format == fmt)
        {
            const uint32_t macroBytes = 32 * 32 * bpp;
            const uint32_t pitchMacro = std::max(1u, e.pitchAligned / 32);
            const uint32_t idx = offset / macroBytes;
            dx = (idx % pitchMacro) * 32;
            dy = (idx / pitchMacro) * 32;
            if (dx + needW <= e.tex->width && dy + needH <= e.tex->height)
                return e.tex;
            dx = dy = 0;
        }
    }
    HostTexture*& t = g_resolved[phys];
    const uint32_t w = std::max(destPitch, needW), h = std::max(destHeight, needH);
    if (!(t && t->format == fmt && t->width >= w && t->height >= h))
    {
        auto* nt = new HostTexture();
        if (!CreateTexture2D(*nt, fmt, std::max(w, t ? t->width : 0u), std::max(h, t ? t->height : 0u), D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST))
        {
            delete nt;
            return t;
        }
        t = nt;   // a antiga e vazada de proposito (pode estar em uso na GPU)
    }
    ResolveEntry e{ t, ((std::max(destPitch, 1u) + 31) & ~31u) * ((std::max(destHeight, 1u) + 31) & ~31u) * bpp,
                    (std::max(destPitch, 1u) + 31) & ~31u, bpp };
    // Entradas "filhas" dentro desta faixa (criadas antes da textura inteira aparecer) deixam de valer
    for (auto c = g_resolveEntries.upper_bound(phys); c != g_resolveEntries.end() && c->first < phys + e.size;)
    {
        g_resolved.erase(c->first);
        c = g_resolveEntries.erase(c);
    }
    g_resolveEntries[phys] = e;
    return t;
}

static void DoResolve()
{
    const uint32_t* r = g_regs;
    const uint32_t cc = r[xreg::RB_COPY_CONTROL];
    const uint32_t src = cc & 7;
    const bool colorClear = (cc >> 8) & 1, depthClear = (cc >> 9) & 1;
    const uint32_t copyCmd = (cc >> 20) & 3;
    const uint32_t pitch = r[xreg::RB_SURFACE_INFO] & 0x3FFF;
    if (!pitch)
        return;

    // Retangulo: D3D9 sempre poe os 3 vertices no vertex fetch 0
    const uint32_t d0 = r[xreg::FETCH_CONSTANTS], d1 = r[xreg::FETCH_CONSTANTS + 1];
    if ((d0 & 3) != 3)
        return;
    const uint8_t* vtx = g_mem.base + GpuPhysToGuest(d0 & ~3u);
    const float hp = (r[xreg::PA_SU_VTX_CNTL] & 1) == 0 ? 0.5f : 0.0f;
    float v[6];
    for (int i = 0; i < 6; i++)
    {
        uint32_t raw;
        memcpy(&raw, vtx + i * 4, 4);
        raw = SwapIndex(raw, d1 & 3, true);
        v[i] = AsFloat(raw) + hp;
    }
    auto px = [](float f) { return int32_t(std::floor(f + 0.496f)); };
    int32_t x0 = px(std::min({ v[0], v[2], v[4] })), y0 = px(std::min({ v[1], v[3], v[5] }));
    int32_t x1 = px(std::max({ v[0], v[2], v[4] })), y1 = px(std::max({ v[1], v[3], v[5] }));
    if (r[xreg::PA_SU_SC_MODE_CNTL] & (1u << 16))
    {
        const uint32_t wo = r[xreg::PA_SC_WINDOW_OFFSET];
        x0 += SBits(wo, 0, 15); x1 += SBits(wo, 0, 15);
        y0 += SBits(wo, 16, 15); y1 += SBits(wo, 16, 15);
    }
    const ScreenRect s = GetScissor(pitch, kRtHeight);
    x0 = std::clamp(x0, s.x0, s.x1); x1 = std::clamp(x1, s.x0, s.x1);
    y0 = std::clamp(y0, s.y0, s.y1); y1 = std::clamp(y1, s.y0, s.y1);
    x0 &= ~7; y0 &= ~7;
    x1 = std::min((x1 + 7) & ~7, int32_t(pitch));
    y1 = std::min((y1 + 7) & ~7, int32_t(kRtHeight));
    if (x1 <= x0 || y1 <= y0)
        return;
    g_stats.resolves++;
    g_resolvesThisFrame++;
    const D3D12_RECT rect{ x0, y0, x1, y1 };
    if (g_frameNumber == g_traceFrame)
        fprintf(TraceOut(), "[frame] RESOLVE src=%u (color info %08X, depth info %08X) pitch=%u msaa=%u rect %d,%d-%d,%d -> dest %08X pitch/h %08X info %08X | clear cor=%d depth=%d cmd=%u | wofs=%08X\n",
                src, src < 4 ? r[kColorInfoRegs[src]] : 0, r[xreg::RB_DEPTH_INFO], pitch, Bits(r[xreg::RB_SURFACE_INFO], 16, 2), x0, y0, x1, y1,
                r[xreg::RB_COPY_DEST_BASE], r[xreg::RB_COPY_DEST_PITCH], r[xreg::RB_COPY_DEST_INFO], colorClear, depthClear, copyCmd,
                r[xreg::PA_SC_WINDOW_OFFSET]);

    if (src < 4)
    {
        const uint32_t info = r[kColorInfoRegs[src]];
        RenderTarget* rt = GetRenderTarget(info & 0xFFF, pitch, Bits(info, 16, 4), false);
        if (rt && copyCmd != 3)
        {
            const uint32_t destBase = r[xreg::RB_COPY_DEST_BASE] & 0x1FFFFFFF;
            const uint32_t dp = r[xreg::RB_COPY_DEST_PITCH];
            const uint32_t bpp = DestFormatBpp(Bits(r[xreg::RB_COPY_DEST_INFO], 7, 6));
            uint32_t dx = 0, dy = 0;
            HostTexture* dst = GetResolveTexture(destBase, rt->tex.format, Bits(dp, 0, 14), Bits(dp, 16, 14), bpp, uint32_t(x1),
                                                 uint32_t(y1), dx, dy);
            if (dst)
            {
                Transition(rt->tex, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(*dst, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION sl{}, dl{};
                sl.pResource = rt->tex.res.Get();
                sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dl.pResource = dst->res.Get();
                dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                // Destino = retangulo da EDRAM + deslocamento do macro tile (dx, dy)
                const uint32_t cx1 = std::min<uint32_t>(uint32_t(x1), dst->width - std::min(dst->width, dx));
                const uint32_t cy1 = std::min<uint32_t>(uint32_t(y1), dst->height - std::min(dst->height, dy));
                D3D12_BOX box{ uint32_t(x0), uint32_t(y0), 0, std::min(cx1, rt->tex.width), std::min(cy1, rt->tex.height), 1 };
                if (box.right > box.left && box.bottom > box.top)
                    g_cmd->CopyTextureRegion(&dl, uint32_t(x0) + dx, uint32_t(y0) + dy, 0, &sl, &box);
                if (g_frameNumber == g_traceFrame)
                {
                    fprintf(TraceOut(), "[frame]   -> copia para (%u,%u) na textura de %ux%u\n", x0 + dx, y0 + dy, dst->width, dst->height);
                    // Conteudo do RT (EDRAM) no momento do resolve
                    static int resolveIndex = 0;
                    char path[80];
                    snprintf(path, sizeof(path), "frame_%llu_rt%02d.png", (unsigned long long)g_frameNumber, resolveIndex++);
                    Transition(rt->tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    DumpTexture(rt->tex, rt->tex.width, std::min<uint32_t>(rt->tex.height, 720), CapturePath(path));
                }
                // copy_dest_swap (bit 24): o Xenos grava com R e B trocados; aplicado no SRV (HostTexture::rbSwap)
                dst->rbSwap = (r[xreg::RB_COPY_DEST_INFO] >> 24) & 1;
                Transition(*dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(rt->tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
                if (g_logDraws && g_frameNumber < 400)
                    fprintf(stderr, "[gpu] resolve RT%u (base %u) -> %08X rect %d,%d-%d,%d\n", src, info & 0xFFF, destBase, x0, y0, x1, y1);
            }
        }
        if (rt && colorClear)
        {
            float color[4];
            DecodeClearColor(Bits(info, 16, 4), r[xreg::RB_COLOR_CLEAR], r[xreg::RB_COLOR_CLEAR_LO], color);
            Transition(rt->tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_cmd->ClearRenderTargetView(CpuHandle(g_rtvHeap.Get(), rt->tex.rtv, g_rtvSize), color, 1, &rect);
        }
    }
    if (depthClear)
    {
        const uint32_t di = r[xreg::RB_DEPTH_INFO];
        RenderTarget* drt = GetRenderTarget(di & 0xFFF, pitch, Bits(di, 16, 1), true);
        if (drt)
        {
            const uint32_t dv = r[xreg::RB_DEPTH_CLEAR];
            Transition(drt->tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            g_cmd->ClearDepthStencilView(CpuHandle(g_dsvHeap.Get(), drt->tex.dsv, g_dsvSize), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                         float(dv >> 8) / 16777215.0f, UINT8(dv & 0xFF), 1, &rect);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Swap / apresentacao

// Uma linha de textura (formatos de frontbuffer/RT) -> BGR 8 bits como aparece na tela (o blit usa o SRV sem troca R/B)
static uint8_t ToByte(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
static void RowToBgr8(DXGI_FORMAT format, const uint8_t* s, uint8_t* d, uint32_t w)
{
    for (uint32_t x = 0; x < w; x++, d += 3)
    {
        switch (format)
        {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        {
            uint32_t v;
            memcpy(&v, s + x * 4, 4);
            d[2] = uint8_t((v & 0x3FF) >> 2); d[1] = uint8_t(((v >> 10) & 0x3FF) >> 2); d[0] = uint8_t(((v >> 20) & 0x3FF) >> 2);
            break;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        {
            uint16_t c[4];
            memcpy(c, s + x * 8, 8);
            d[2] = ToByte(HalfToFloat(c[0])); d[1] = ToByte(HalfToFloat(c[1])); d[0] = ToByte(HalfToFloat(c[2]));
            break;
        }
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 2];
            break;
        default:   // R8G8B8A8
            d[2] = s[x * 4]; d[1] = s[x * 4 + 1]; d[0] = s[x * 4 + 2];
            break;
        }
    }
}

namespace capture { void QueueImageWrite(std::filesystem::path path, std::vector<uint8_t> bgr, uint32_t w, uint32_t h); }

// ---- Depuracao: CARS_DUMP_FRAMES=100,300 salva o frontbuffer desses frames em frame_N.png; F12 salva o frame seguinte ----
// (o PNG e gravado pela thread de capturas: a thread de render so espera a copia da GPU)
static void DumpTexture(HostTexture& t, uint32_t w, uint32_t h, const std::filesystem::path& path)
{
    w = std::min(w, t.width);
    h = std::min(h, t.height);
    const uint32_t bpp = (t.format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    const uint32_t rowPitch = (t.width * bpp + 255) & ~255u;
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = uint64_t(rowPitch) * t.height;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
        return;
    const D3D12_RESOURCE_STATES old = t.state;
    Transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = t.res.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { t.format, t.width, t.height, 1, rowPitch };
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(t, old);
    SubmitAndWait();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    if (p && w && h)
    {
        std::vector<uint8_t> bgr(size_t(w) * 3 * h);
        for (uint32_t y = 0; y < h; y++)
            RowToBgr8(t.format, p + size_t(y) * rowPitch, bgr.data() + size_t(y) * w * 3, w);
        capture::QueueImageWrite(path, std::move(bgr), w, h);
    }
    rb->Unmap(0, nullptr);
}

static void HandleResize()
{
    if (!g_resizePending.exchange(false))
        return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0)
        return;
    g_queue->Signal(g_fence.Get(), ++g_fenceValue);
    WaitForFence(g_fenceValue);
    for (auto& b : g_backBuffers)
        b.Reset();
    g_swapChain->ResizeBuffers(kFrameCount, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    CreateBackBufferViews();
}

// ---------------------------------------------------------------------------------------------
// Frontbuffer na memoria guest, sob demanda (veja RendererHandleFrontbufferAccess em renderer.h)

static std::mutex g_fbMutex;
static HostTexture* g_fbTex = nullptr;
static uint32_t g_fbPhys = 0, g_fbBytes = 0, g_fbW = 0, g_fbH = 0;
static std::atomic<bool> g_fbProtected{ false };
static std::atomic<uint32_t> g_fbReadbacks{ 0 };   // estatistica: copias GPU -> memoria guest por segundo
static ComPtr<ID3D12CommandAllocator> g_fbAlloc;
static ComPtr<ID3D12GraphicsCommandList> g_fbList;
static ComPtr<ID3D12Fence> g_fbFence;
static ComPtr<ID3D12Resource> g_fbReadback;
static uint64_t g_fbReadbackSize = 0, g_fbFenceValue = 0;
static HANDLE g_fbEvent = nullptr;

// g_fbPhys = inicio exato do frontbuffer; a protecao cobre as paginas inteiras [g_fbPhys & ~0xFFF, +g_fbBytes)
static void ProtectFrontbuffer(bool on)
{
    for (uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
    {
        DWORD old;
        VirtualProtect(g_mem.base + view + (g_fbPhys & ~0xFFFu), g_fbBytes, on ? PAGE_NOACCESS : PAGE_READWRITE, &old);
    }
}

// Le a textura do frontbuffer na GPU (lista de comandos propria: a do processador de comandos e de outra thread) e escreve
// BGRA8 linear na memoria guest. Chamado com g_fbMutex travado e as paginas ja liberadas.
static void ReadbackFrontbufferLocked()
{
    HostTexture& t = *g_fbTex;
    const uint32_t w = std::min(g_fbW, t.width), h = std::min(g_fbH, t.height);
    const uint32_t bpp = t.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    const uint32_t rowPitch = (t.width * bpp + 255) & ~255u;
    if (!g_fbList)
    {
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_fbAlloc));
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_fbAlloc.Get(), nullptr, IID_PPV_ARGS(&g_fbList));
        g_fbList->Close();
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fbFence));
        g_fbEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    const uint64_t need = uint64_t(rowPitch) * h;
    if (!g_fbReadback || g_fbReadbackSize < need)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = need;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_fbReadback.Reset();
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g_fbReadback))))
            return;
        g_fbReadbackSize = need;
    }
    g_fbAlloc->Reset();
    g_fbList->Reset(g_fbAlloc.Get(), nullptr);
    // No fim de cada frame (RendererSwap) a textura do frontbuffer fica como shader resource
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = srv;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_fbList->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = t.res.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g_fbReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { t.format, t.width, h, 1, rowPitch };
    const D3D12_BOX box{ 0, 0, 0, t.width, h, 1 };
    g_fbList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_fbList->ResourceBarrier(1, &b);
    g_fbList->Close();
    ID3D12CommandList* lists[] = { g_fbList.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fbFence.Get(), ++g_fbFenceValue);
    if (g_fbFence->GetCompletedValue() < g_fbFenceValue)
    {
        g_fbFence->SetEventOnCompletion(g_fbFenceValue, g_fbEvent);
        WaitForSingleObject(g_fbEvent, 2000);
    }
    const uint8_t* p = nullptr;
    const D3D12_RANGE range{ 0, size_t(need) };
    if (FAILED(g_fbReadback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&p)))) || !p)
        return;
    // Layout com tiling 2D do Xenos (como o resolve grava; o jogo le o frontbuffer como superficie tiled)
    uint8_t* out = g_mem.base + 0xA0000000u + g_fbPhys;
    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t* s = p + size_t(y) * rowPitch;
        for (uint32_t x = 0; x < w; x++)
        {
            uint8_t* d = out + TiledOffset2D(x, y, g_fbW, 2);
            uint8_t r, g, bl, a;
            switch (t.format)
            {
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            {
                uint32_t v;
                memcpy(&v, s + x * 4, 4);
                r = uint8_t((v & 0x3FF) >> 2); g = uint8_t(((v >> 10) & 0x3FF) >> 2); bl = uint8_t(((v >> 20) & 0x3FF) >> 2);
                a = uint8_t((v >> 30) * 85);
                break;
            }
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                bl = s[x * 4]; g = s[x * 4 + 1]; r = s[x * 4 + 2]; a = s[x * 4 + 3];
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            {
                uint16_t c[4];
                memcpy(c, s + x * 8, 8);
                r = ToByte(HalfToFloat(c[0])); g = ToByte(HalfToFloat(c[1])); bl = ToByte(HalfToFloat(c[2])); a = ToByte(HalfToFloat(c[3]));
                break;
            }
            default:   // R8G8B8A8
                r = s[x * 4]; g = s[x * 4 + 1]; bl = s[x * 4 + 2]; a = s[x * 4 + 3];
                break;
            }
            // Como o resolve do Xenos (k_8_8_8_8, endian 0): com dest_swap bytes B G R A, sem dest_swap R G B A
            if (t.rbSwap) { d[0] = bl; d[1] = g; d[2] = r; } else { d[0] = r; d[1] = g; d[2] = bl; }
            d[3] = a;
        }
    }
    const D3D12_RANGE none{ 0, 0 };
    g_fbReadback->Unmap(0, &none);
}

bool RendererHandleFrontbufferAccess(uint32_t guestAddr)
{
    if (!g_fbProtected.load() || guestAddr < 0xA0000000u || ((guestAddr & 0x1FFFFFFF) - (g_fbPhys & ~0xFFFu)) >= g_fbBytes)
        return false;
    std::lock_guard<std::mutex> lock(g_fbMutex);
    if (!g_fbProtected.load())
        return true;   // outra thread ja tratou
    ProtectFrontbuffer(false);
    g_fbProtected = false;
    if (g_fbTex)
        ReadbackFrontbufferLocked();
    g_fbReadbacks++;
    static int logged = 0;
    if (logged++ < 10)
        fprintf(stderr, "[frontbuffer] a CPU leu o frontbuffer (%08X): imagem copiada da GPU para a memoria do jogo\n", guestAddr);
    return true;
}

// Fim do swap: o frontbuffer atual fica protegido ate a CPU tocar nele
static void ArmFrontbuffer(HostTexture* tex, uint32_t phys, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lock(g_fbMutex);
    const uint32_t exact = phys & 0x1FFFFFFF;
    // superficie tiled: linhas e colunas alinhadas a 32 texels
    const uint32_t bytes = ((exact & 0xFFF) + ((w + 31) & ~31u) * ((h + 31) & ~31u) * 4 + 0xFFF) & ~0xFFFu;
    if (g_fbProtected && (exact != g_fbPhys || bytes != g_fbBytes))
    {
        ProtectFrontbuffer(false);
        g_fbProtected = false;
    }
    g_fbTex = tex;
    g_fbPhys = exact;
    g_fbBytes = bytes;
    g_fbW = w;
    g_fbH = h;
    if (!g_fbProtected && tex)
    {
        ProtectFrontbuffer(true);
        g_fbProtected = true;
    }
}

// ---------------------------------------------------------------------------------------------
// Log de sessao: 4 capturas por segundo do frontbuffer (copia na GPU para um buffer de readback, convertida na thread
// "capturas") num anel dos ultimos 5 s. F12 = marca: salva o anel em marca_NN\ (JPEG + a mais recente em PNG), os ultimos
// 10 s de som e o trace completo do frame seguinte (frame.txt + imagens do frame, das texturas e dos resolves)

bool AudioSaveRecent(const std::filesystem::path& path);   // kernel_audio.cpp

namespace capture
{
constexpr uint32_t kSlots = 4;      // copias em voo na GPU
constexpr uint32_t kRing = 20;      // 5 s
constexpr double kInterval = 0.25;

struct Slot
{
    ComPtr<ID3D12Resource> rb;
    uint64_t size = 0;
    std::atomic<bool> busy{ false };
};
struct Job
{
    int slot = -1;                      // -1: salvar o anel em saveDir; -2: gravar bgr em file (PNG)
    uint64_t fence = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0, rowPitch = 0;
    uint64_t frame = 0;
    double time = 0;
    int mark = 0;
    std::filesystem::path saveDir;
    std::filesystem::path file;
    std::vector<uint8_t> bgr;
};
struct Image
{
    std::vector<uint8_t> bgr;
    uint32_t w = 0, h = 0;
    uint64_t frame = 0;
    double time = 0;
};

Slot g_slots[kSlots];
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::vector<Job> g_pending;             // copias gravadas na lista deste frame (o fence sai no Signal do swap)
Image g_ring[kRing];                    // so a thread de capturas mexe
uint32_t g_ringNext = 0;
HANDLE g_event = nullptr;
bool g_force = false;                   // F12: captura este frame mesmo fora do intervalo
int g_markIndex = 0;
std::filesystem::path g_markDir;        // F12: salvar o anel no fim deste swap
uint64_t g_noticeUntil = 0;             // aviso no titulo da janela
wchar_t g_notice[96];

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void Save(const Job& job)
{
    uint32_t saved = 0;
    const Image* newest = nullptr;
    for (uint32_t i = 0; i < kRing; i++)
    {
        const Image& img = g_ring[(g_ringNext + i) % kRing];   // da mais antiga para a mais nova
        if (img.bgr.empty() || job.time - img.time > 6.0)
            continue;
        char name[64];
        snprintf(name, sizeof(name), "tela_%02u_%+.2fs.jpg", ++saved, img.time - job.time);
        WriteImageFile(job.saveDir / name, img.bgr.data(), img.w, img.h, img.w * 3, true);
        newest = &img;
    }
    if (newest)
        WriteImageFile(job.saveDir / "tela_agora.png", newest->bgr.data(), newest->w, newest->h, newest->w * 3, false);
    const bool audio = AudioSaveRecent(job.saveDir / "som_ultimos_10s.wav");
    fprintf(stderr, "[MARCA %02d] salvas %u capturas dos ultimos segundos%s em %s\n", job.mark, saved,
            audio ? " e o som dos ultimos 10 s" : "", job.saveDir.string().c_str());
    SessionLogFlush();
}

void Thread()
{
    SetThreadDescription(GetCurrentThread(), L"capturas");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (job.slot == -2)
        {
            WriteImageFile(job.file, job.bgr.data(), job.w, job.h, job.w * 3, false);
            continue;
        }
        if (job.slot < 0)
        {
            Save(job);
            continue;
        }
        if (g_fence->GetCompletedValue() < job.fence)
        {
            g_fence->SetEventOnCompletion(job.fence, g_event);
            WaitForSingleObject(g_event, 5000);
        }
        Slot& slot = g_slots[job.slot];
        uint8_t* p = nullptr;
        const D3D12_RANGE range{ 0, size_t(job.rowPitch) * job.h };
        if (g_fence->GetCompletedValue() >= job.fence && SUCCEEDED(slot.rb->Map(0, &range, reinterpret_cast<void**>(&p))) && p)
        {
            Image& img = g_ring[g_ringNext];
            img.bgr.resize(size_t(job.w) * 3 * job.h);
            for (uint32_t y = 0; y < job.h; y++)
                RowToBgr8(job.format, p + size_t(y) * job.rowPitch, img.bgr.data() + size_t(y) * job.w * 3, job.w);
            img.w = job.w;
            img.h = job.h;
            img.frame = job.frame;
            img.time = job.time;
            g_ringNext = (g_ringNext + 1) % kRing;
            const D3D12_RANGE none{ 0, 0 };
            slot.rb->Unmap(0, &none);
        }
        slot.busy = false;
    }
}

// Na lista de comandos do swap, antes do blit: copia o frontbuffer para um slot livre
void Record(HostTexture& fb, uint32_t width, uint32_t height)
{
    static double last = 0;
    const double now = Now();
    if (!g_force && now - last < kInterval)
        return;
    Slot* slot = nullptr;
    int index = 0;
    for (; index < int(kSlots); index++)
        if (!g_slots[index].busy)
        {
            slot = &g_slots[index];
            break;
        }
    if (!slot)
        return;
    const uint32_t w = std::min(width, fb.width), h = std::min(height, fb.height);
    const uint32_t bpp = fb.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    const uint32_t rowPitch = (fb.width * bpp + 255) & ~255u;
    const uint64_t need = uint64_t(rowPitch) * h;
    if (!slot->rb || slot->size < need)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = need;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        slot->rb.Reset();
        slot->size = 0;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&slot->rb))))
            return;
        slot->size = need;
    }
    last = now;
    g_force = false;
    slot->busy = true;
    Transition(fb, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = fb.res.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = slot->rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { fb.format, fb.width, h, 1, rowPitch };
    const D3D12_BOX box{ 0, 0, 0, fb.width, h, 1 };
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Job job;
    job.slot = index;
    job.format = fb.format;
    job.w = w;
    job.h = h;
    job.rowPitch = rowPitch;
    job.frame = g_frameNumber;
    job.time = now;
    g_pending.push_back(std::move(job));
}

void EnsureThread()
{
    static const bool started = [] {
        g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(Thread).detach();
        return true;
    }();
    (void)started;
}

void QueueImageWrite(std::filesystem::path path, std::vector<uint8_t> bgr, uint32_t w, uint32_t h)
{
    EnsureThread();
    Job job;
    job.slot = -2;
    job.file = std::move(path);
    job.bgr = std::move(bgr);
    job.w = w;
    job.h = h;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_jobs.push_back(std::move(job));
    }
    g_cv.notify_one();
}

// Depois do Signal do swap: as copias deste frame (e o pedido de salvar da marca) vao para a thread de capturas
void Submit(uint64_t fence)
{
    if (g_pending.empty() && g_markDir.empty())
        return;
    EnsureThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Job& job : g_pending)
        {
            job.fence = fence;
            g_jobs.push_back(std::move(job));
        }
        if (!g_markDir.empty())
        {
            Job save;
            save.saveDir = g_markDir;
            save.mark = g_markIndex;
            save.time = Now();
            g_jobs.push_back(std::move(save));
        }
    }
    g_pending.clear();
    g_markDir.clear();
    g_cv.notify_one();
}
}  // namespace capture

// F12: marca no log + capturas dos ultimos segundos + som + trace do frame seguinte, tudo em marca_NN
static void StartMark()
{
    using namespace capture;
    g_markDir = SessionNewMark(g_markIndex);
    fprintf(stderr, "[MARCA %02d] ===== F12: algo estranho aqui (frame %llu) =====\n", g_markIndex, (unsigned long long)g_frameNumber);
    g_force = true;
    if (!g_traceOut && g_traceFrame <= g_frameNumber)
    {
        g_traceFrame = g_frameNumber + 1;
        g_dumpFrames.push_back(g_frameNumber + 1);
        g_captureDir = g_markDir;
        g_traceOut = _wfopen((g_markDir / L"frame.txt").c_str(), L"w");
        if (g_traceOut)
            fprintf(g_traceOut, "[frame] trace do frame %llu (marca %02d)\n", (unsigned long long)g_traceFrame, g_markIndex);
    }
    swprintf(g_notice, 96, L"Cars Race-O-Rama (recomp) - MARCA %02d salva na pasta logs", g_markIndex);
    g_noticeUntil = GetTickCount64() + 3000;
    SetWindowTextW(g_hwnd, g_notice);
}

uint64_t RendererLastSwapTick() { return g_lastSwapTick.load(); }

// Limite de FPS do .ini ([Video] FPS). Com o vblank imediato (FPS=60) o jogo nao se limita sozinho: o vsync do host segura
// num monitor de 60 Hz; com vsync desligado ou monitor mais rapido, este limitador (timer de alta resolucao) segura
static bool g_presentOccluded = false;

static void LimitFrameRate()
{
    static const int cap = SettingsGetInt(L"Video", L"FPS", 60);
    static const int monitorHz = [] {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? int(dm.dmDisplayFrequency) : 60;
    }();
    if (cap <= 0 || cap > 1000 || (g_vsync && monitorHz <= cap + 2 && !g_presentOccluded))
        return;
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return int64_t(f.QuadPart); }();
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static int64_t next = 0;
    const int64_t period = freq / cap;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next == 0 || now.QuadPart - next > period * 4)
        next = now.QuadPart;   // recomeca depois de uma pausa longa
    next += period;
    for (;;)
    {
        QueryPerformanceCounter(&now);
        const int64_t remaining = next - now.QuadPart;
        if (remaining <= 0)
            break;
        if (timer && remaining > freq / 1000)
        {
            LARGE_INTEGER due;
            due.QuadPart = -int64_t((remaining - freq / 2000) * 10000000 / freq);   // acorda 0,5 ms antes
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        else
        {
            YieldProcessor();
        }
    }
}

void RendererSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height)
{
    if (!g_enabled)
        return;
    g_stats.swaps++;
    g_lastSwapTick = GetTickCount64();
    // CARS_WATCH=endereco,tamanho (hex): a cada 10 frames loga quantos bytes nao-zero ha nessa regiao da memoria guest
    static const auto watch = [] {
        std::pair<uint32_t, uint32_t> w{ 0, 0 };
        if (const char* s = getenv("CARS_WATCH"))
        {
            char* end = nullptr;
            w.first = uint32_t(strtoul(s, &end, 16));
            if (end && *end == ',')
                w.second = uint32_t(strtoul(end + 1, nullptr, 16));
        }
        return w;
    }();
    if (watch.second && g_frameNumber % 10 == 0)
    {
        const uint8_t* p = g_mem.base + watch.first;
        uint32_t nz = 0;
        uint64_t sum = 0;
        for (uint32_t i = 0; i < watch.second; i += 4)
        {
            uint32_t v;
            memcpy(&v, p + i, 4);
            nz += v != 0;
            sum += v;
        }
        fprintf(stderr, "[watch] frame %llu: %08X: %u de %u palavras nao-zero (soma %016llX)\n", (unsigned long long)g_frameNumber,
                watch.first, nz, watch.second / 4, (unsigned long long)sum);
    }
    {
        // F12 (com a janela do jogo em foco): marca "algo estranho" no log de sessao (StartMark)
        static bool f12Down = false;
        const bool down = GetForegroundWindow() == g_hwnd && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (down && !f12Down)
            StartMark();
        f12Down = down;
        // F11: captura de modelos das cenas (liga / salva)
        static bool f11Down = false;
        const bool down11 = GetForegroundWindow() == g_hwnd && (GetAsyncKeyState(VK_F11) & 0x8000);
        static std::vector<double> modelsAt = [] {   // CARS_MODELOS_AT=segundos[,...]: o mesmo sem tecla (testes)
            std::vector<double> v;
            for (const char* p = getenv("CARS_MODELOS_AT"); p && *p;)
            {
                char* end = nullptr;
                v.push_back(strtod(p, &end));
                if (end == p)
                    break;
                p = *end ? end + 1 : end;
            }
            return v;
        }();
        static const auto modelsStart = std::chrono::steady_clock::now();
        bool toggleModels = down11 && !f11Down;
        if (!modelsAt.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - modelsStart).count() >= modelsAt.front())
        {
            modelsAt.erase(modelsAt.begin());
            toggleModels = true;
        }
        if (toggleModels)
        {
            const std::wstring text = ModelCaptureToggle();
            swprintf(capture::g_notice, 96, L"Cars Race-O-Rama (recomp) - %ls", text.c_str());
            capture::g_noticeUntil = GetTickCount64() + 4000;
            SetWindowTextW(g_hwnd, capture::g_notice);
        }
        f11Down = down11;
        // CARS_MARK_AT=segundos[,segundos...]: marca sem tecla (teste do log de sessao; nao mexe na entrada do jogo)
        static std::vector<double> markAt = [] {
            std::vector<double> v;
            for (const char* p = getenv("CARS_MARK_AT"); p && *p;)
            {
                char* end = nullptr;
                v.push_back(strtod(p, &end));
                if (end == p)
                    break;
                p = *end ? end + 1 : end;
            }
            return v;
        }();
        static const auto start = std::chrono::steady_clock::now();
        if (!markAt.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= markAt.front())
        {
            markAt.erase(markAt.begin());
            StartMark();
        }
    }
    if (g_frameNumber <= 3)
        fprintf(stderr, "[gpu] swap %llu (frontbuffer %08X %ux%u)\n", (unsigned long long)g_frameNumber, frontbufferPhys, width, height);
    const uint32_t bbIndex = g_swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = g_backBuffers[bbIndex].Get();
    TransitionRaw(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = CpuHandle(g_bbRtvHeap.Get(), bbIndex, g_rtvSize);
    const float clear[4] = { 0.02f, 0.02f, 0.03f, 1.0f };
    g_cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);

    auto it = g_resolved.find(frontbufferPhys & 0x1FFFFFFF);
    {
        // diagnostico: troca de frontbuffer / frontbuffer sem conteudo
        static uint32_t lastFb = 0;
        static bool lastOk = true;
        const bool ok = it != g_resolved.end();
        if (frontbufferPhys != lastFb || ok != lastOk)
            fprintf(stderr, "[swap] frame %llu: frontbuffer %08X %ux%u %s (resolves neste frame: %u)\n",
                    (unsigned long long)g_frameNumber, frontbufferPhys, width, height, ok ? "com conteudo" : "SEM CONTEUDO",
                    g_resolvesThisFrame);
        lastFb = frontbufferPhys;
        lastOk = ok;
        g_resolvesThisFrame = 0;
    }
    if (it != g_resolved.end() && width && height)
    {
        HostTexture& fb = *it->second;
        for (const auto& d : g_traceTextureDumps)
        {
            char path[96];
            snprintf(path, sizeof(path), "frame_%llu_tex%08X.png", (unsigned long long)g_frameNumber, d.addr);
            DumpTexture(*d.tex, d.w, d.h, CapturePath(path));
        }
        g_traceTextureDumps.clear();
        if (std::find(g_dumpFrames.begin(), g_dumpFrames.end(), g_frameNumber) != g_dumpFrames.end())
        {
            char path[64];
            snprintf(path, sizeof(path), "frame_%llu.png", (unsigned long long)g_frameNumber);
            DumpTexture(fb, width, height, CapturePath(path));
            // CARS_DUMP_RESOLVE=007A9000,045CD000: salva tambem essas texturas de resolve
            if (const char* list = getenv("CARS_DUMP_RESOLVE"))
                for (const char* p = list; *p;)
                {
                    char* end = nullptr;
                    const uint32_t a = uint32_t(strtoul(p, &end, 16));
                    if (end == p)
                        break;
                    auto rit = g_resolved.find(a);
                    if (rit != g_resolved.end())
                    {
                        snprintf(path, sizeof(path), "frame_%llu_r%08X.png", (unsigned long long)g_frameNumber, a);
                        DumpTexture(*rit->second, rit->second->width, rit->second->height, CapturePath(path));
                    }
                    p = *end ? end + 1 : end;
                }
        }
        capture::Record(fb, width, height);
        Transition(fb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const uint32_t srv = GetSrv(fb, 0x688, false);   // identidade, sem a troca R/B do resolve (o video le o frontbuffer direto)
        const D3D12_RESOURCE_DESC bd = bb->GetDesc();
        // Mantem a proporcao do frontbuffer (letterbox)
        const float bw = float(bd.Width), bh = float(bd.Height);
        const float scale = std::min(bw / float(width), bh / float(height));
        const float vw = float(width) * scale, vh = float(height) * scale;
        D3D12_VIEWPORT vp{ (bw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh, 0, 1 };
        D3D12_RECT sr{ 0, 0, LONG(bd.Width), LONG(bd.Height) };
        g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_cmd->RSSetViewports(1, &vp);
        g_cmd->RSSetScissorRects(1, &sr);
        g_cmd->SetGraphicsRootSignature(g_blitRootSig.Get());
        g_cmd->SetPipelineState(g_blitPso.Get());
        const float uvScale[2] = { float(width) / float(fb.width), float(height) / float(fb.height) };
        g_cmd->SetGraphicsRoot32BitConstants(0, 2, uvScale, 0);
        g_cmd->SetGraphicsRootDescriptorTable(1, GpuHandle(g_srvHeap.Get(), srv, g_srvSize));
        g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_cmd->DrawInstanced(3, 1, 0, 0);
    }
    TransitionRaw(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    g_cmd->Close();
    ID3D12CommandList* lists[] = { g_cmd.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    {
        ScopedMs timer(g_stats.presentMs);
        LimitFrameRate();
        // janela minimizada/oculta: o present nao espera o vsync -> o limitador assume
        g_presentOccluded = g_swapChain->Present(g_vsync ? 1 : 0, 0) == DXGI_STATUS_OCCLUDED;
    }
    g_queue->Signal(g_fence.Get(), ++g_fenceValue);
    g_frames[g_frameIndex].fence = g_fenceValue;
    capture::Submit(g_fenceValue);
    // F12: fim do frame registrado
    if (g_traceOut && g_frameNumber >= g_traceFrame)
    {
        fclose(g_traceOut);
        g_traceOut = nullptr;
        g_captureDir.clear();
    }
    // Frontbuffer: paginas protegidas na memoria guest ate a CPU ler (ai a imagem e copiada da GPU, sob demanda)
    if (width && height)
    {
        auto fbIt = g_resolved.find(frontbufferPhys & 0x1FFFFFFF);
        ArmFrontbuffer(fbIt != g_resolved.end() ? fbIt->second : nullptr, frontbufferPhys, width, height);
    }

    HandleResize();
    g_frameIndex = (g_frameIndex + 1) % kFrameCount;
    {
        ScopedMs timer(g_stats.waitMs);
        WaitForFence(g_frames[g_frameIndex].fence);
    }
    g_frames[g_frameIndex].uploadOffset = 0;
    g_frameNumber++;
    BeginCommandList();
    MemWatchSetFrame(g_frameNumber);
    // Pool do cache de VB/IB cheio: espera a GPU terminar tudo e recomeca (as copias sao refeitas sob demanda)
    if (g_poolUsed > kPoolBudget)
    {
        WaitForFence(g_fenceValue);
        for (auto& chunk : g_pool)
            chunk.used = 0;
        g_poolUsed = 0;
        g_vbCache.clear();
        g_ibCache.clear();
        fprintf(stderr, "[gpu] cache de VB/IB cheio: recomecando\n");
    }

    // Titulo e estatisticas
    static auto last = std::chrono::steady_clock::now();
    static auto lastSwap = last;
    static uint64_t lastFrame = 0;
    const auto now = std::chrono::steady_clock::now();
    g_stats.worstFrameMs = std::max(g_stats.worstFrameMs, std::chrono::duration<double, std::milli>(now - lastSwap).count());
    lastSwap = now;
    const double dt = std::chrono::duration<double>(now - last).count();
    if (dt >= 1.0)
    {
        const double fps = double(g_frameNumber - lastFrame) / dt;
        wchar_t title[160];
        swprintf(title, 160, L"Cars Race-O-Rama (recomp) - %.1f fps - %u draws/s", fps, g_stats.draws);
        SetWindowTextW(g_hwnd, GetTickCount64() < capture::g_noticeUntil ? capture::g_notice : title);
        uint32_t compiled = 0, scans = 0;
        double compileMs = 0, scanMs = 0;
        ShadersTakeStats(compiled, compileMs, scans, scanMs);
        extern uint64_t GpuTakeCpBusyUs();
        extern uint64_t GpuTakeCpWaitUs();
        const double cpWaitMs = GpuTakeCpWaitUs() / 1000.0;
        // trabalho real do CP = tempo dentro dos comandos - espera pela CPU (WAIT_REG_MEM) - present - espera da GPU do host
        const double cpMs = GpuTakeCpBusyUs() / 1000.0 - cpWaitMs - g_stats.presentMs - g_stats.waitMs;
        fprintf(stderr, "[gpu] %.1f fps (pior frame %.0f ms) | draws %u, sem shader %u, compilando %u, outros pulados %u, resolves %u, PSOs novos %u, uploads de textura %u%s"
                        " | ms/s: CP trabalho %.0f (esperando CPU %.0f), draw %.0f (bind %.0f, hash %.0f, tex %.0f, vb %.0f, pso %.0f), present %.0f, espera GPU %.0f"
                        " | shaders compilados %u (%.0f ms), varreduras %u (%.0f ms) | frontbuffer lido pela CPU %u | cache VB/IB: %u copias, %u escritas da CPU, %.0f MB\n",
                fps, g_stats.worstFrameMs, g_stats.draws, g_stats.skippedShader, g_stats.pendingShader, g_stats.skippedOther, g_stats.resolves,
                g_stats.psos, g_stats.texUploads, it != g_resolved.end() ? "" : " (frontbuffer ainda nao resolvido)", cpMs, cpWaitMs, g_stats.drawMs,
                g_stats.bindMs, g_stats.hashMs, g_stats.texMs, g_stats.vbMs, g_stats.psoMs, g_stats.presentMs, g_stats.waitMs, compiled,
                compileMs, scans, scanMs, g_fbReadbacks.exchange(0), g_cacheUploads, MemWatchTakeFaults(), g_poolUsed / 1048576.0);
        g_cacheUploads = 0;
        g_stats = Stats{};
        last = now;
        lastFrame = g_frameNumber;
    }
}
