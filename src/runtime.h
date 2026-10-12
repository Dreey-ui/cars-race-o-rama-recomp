// Cars Race-O-Rama (Xbox 360) - runtime da recompilação estática
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ppc_context.h>

// Memória guest: um bloco de 4 GB reservado; o endereço guest N fica em base + N
struct GuestMemory
{
    uint8_t* base = nullptr;
    bool Init();
    bool Commit(uint32_t addr, uint32_t size);
};
extern GuestMemory g_mem;

inline uint8_t  R8 (uint32_t a) { return *(uint8_t*)(g_mem.base + a); }
inline uint16_t R16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(g_mem.base + a)); }
inline uint32_t R32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(g_mem.base + a)); }
inline uint64_t R64(uint32_t a) { return __builtin_bswap64(*(uint64_t*)(g_mem.base + a)); }
inline void W8 (uint32_t a, uint8_t v)  { *(uint8_t*)(g_mem.base + a) = v; }
inline void W16(uint32_t a, uint16_t v) { *(uint16_t*)(g_mem.base + a) = __builtin_bswap16(v); }
inline void W32(uint32_t a, uint32_t v) { *(uint32_t*)(g_mem.base + a) = __builtin_bswap32(v); }
inline void W64(uint32_t a, uint64_t v) { *(uint64_t*)(g_mem.base + a) = __builtin_bswap64(v); }

// Alocador first-fit sobre uma faixa do espaço guest
class GuestHeap
{
public:
    void Init(uint32_t lo, uint32_t hi);
    uint32_t Alloc(uint32_t size, uint32_t alignment = 0x1000, bool zero = true);
    bool AllocFixed(uint32_t addr, uint32_t size);
    bool Free(uint32_t addr, uint32_t* sizeOut = nullptr);
    bool Contains(uint32_t addr) const { return addr >= m_lo && addr < m_hi; }
    // Faixas alocadas agora (inicio, tamanho), em ordem (a memoria nunca e descommitada: seguro ler depois)
    void Snapshot(std::vector<std::pair<uint32_t, uint32_t>>& out) const;
private:
    struct Impl;
    uint32_t m_lo = 0, m_hi = 0;
    Impl* m_impl = nullptr;
};
extern GuestHeap g_virtualHeap;   // NtAllocateVirtualMemory, pilhas
extern GuestHeap g_physicalHeap;  // MmAllocatePhysicalMemoryEx
extern GuestHeap g_poolHeap;      // objetos do kernel, ExAllocatePool

struct GuestThread
{
    uint32_t pcr = 0;
    uint32_t kthread = 0;
    uint32_t stackBase = 0;   // endereço mais alto
    uint32_t stackLimit = 0;  // endereço mais baixo
    uint32_t threadId = 0;
    uint32_t tls[64] = {};
    PPCContext* ctx = nullptr;     // contexto em execucao (diagnostico: amostragem de pilha)
    uint32_t entry = 0;            // rotina inicial
    struct Apc { uint32_t routine, context, arg1, arg2; };
    std::vector<Apc> apcs;         // APCs de usuario pendentes (entregues em esperas alertaveis)
};
GuestThread* CurrentThread();
// Registro de todas as threads guest (diagnostico)
void KRegisterThreadForSampling(GuestThread* gt);
void KDumpGuestStacks();

// Roda uma função guest na thread host atual, com contexto próprio.
void RunGuestMain(PPCFunc* entry, uint32_t stackSize);

inline void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t guestAddress)
{
    (PPC_LOOKUP_FUNC(base, guestAddress))(ctx, base);
}

extern bool g_traceImports;
extern std::string g_gameRoot;   // pasta da ISO extraída

#define KERNEL_IMPORT(name) PPC_FUNC(__imp__##name)

constexpr uint32_t STATUS_SUCCESS               = 0x00000000;
constexpr uint32_t STATUS_TIMEOUT               = 0x00000102;
constexpr uint32_t STATUS_NO_MORE_FILES         = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL          = 0xC0000001;
constexpr uint32_t STATUS_INVALID_HANDLE        = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER     = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE          = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE           = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY             = 0xC0000017;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
