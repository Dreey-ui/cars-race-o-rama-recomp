#pragma once
#include <filesystem>
#include "runtime.h"

#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <vector>

enum class KType { Event, Semaphore, Mutant, Thread, File };

struct KObject
{
    KType type = KType::Event;
    bool manualReset = false;   // Event
    bool signaled = false;
    int32_t count = 0;          // Semaphore
    int32_t limit = 0;
    uint32_t owner = 0;         // Mutant
    int32_t recursion = 0;
    bool finished = false;      // Thread
    bool suspended = false;
    uint32_t exitCode = 0;
    uint32_t kthread = 0;
    uint32_t threadId = 0;
    FILE* file = nullptr;       // File
    std::string path;
    uint64_t size = 0;
    bool rawStream = false;     // .str de mods ja descomprimido (blocos de 128 KB sem zlib): veja RawStreamTake
    bool isDirectory = false;
    bool dirLoaded = false;     // enumeração de diretório (NtQueryDirectoryFile)
    std::vector<std::string> dirEntries;
    size_t dirPos = 0;
    uint32_t guestPtr = 0;      // objeto dispatcher na memória guest (se houver)
    bool deleteOnClose = false; // NtSetInformationFile(FileDispositionInformation): apaga ao fechar

    ~KObject()
    {
        if (file)
            fclose(file);
        if (deleteOnClose && !path.empty())
        {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(path), ec);
        }
    }
};
using KObjectPtr = std::shared_ptr<KObject>;

// Um único mutex + condvar protege o estado dos objetos do kernel. Simples e suficiente.
extern std::mutex g_kmtx;
extern std::condition_variable g_kcv;

uint32_t KNewHandle(const KObjectPtr& obj);
KObjectPtr KGetHandle(uint32_t handle);
bool KCloseHandle(uint32_t handle);
KObjectPtr KGetGuestObject(uint32_t guestPtr);
void KRegisterGuestObject(uint32_t guestPtr, const KObjectPtr& obj);

// timeoutMs < 0 = infinito. Retorna STATUS_SUCCESS + índice, ou STATUS_TIMEOUT.
uint32_t KWait(const std::vector<KObjectPtr>& objects, bool waitAll, int64_t timeoutMs);
int64_t KTimeoutFromGuest(uint32_t timeoutPtr);
// Sleep(0)/yield do guest com espera adaptativa (kernel_misc.cpp)
void GuestYield();

struct GuestThreadExit {};
uint32_t KCreateGuestThread(uint32_t stackSize, uint32_t startup, uint32_t startRoutine, uint32_t startContext,
                            bool suspended, uint32_t* handleOut, uint32_t* tidOut, uint32_t creationFlags = 0);
// affinityMask: processadores permitidos (0 = o mesmo da thread atual)
GuestThread* KSetupGuestThread(uint32_t stackSize, uint32_t affinityMask = 0);
void KSetThreadCpu(GuestThread* gt, uint32_t cpu);
uint32_t KGetThreadCpu(const GuestThread* gt);
GuestThread* KFindThreadByKthread(uint32_t kthread);
void KInitContext(PPCContext& ctx, GuestThread* gt);
void KSetCurrentThread(GuestThread* gt);

// APCs de usuario: enfileira na thread atual; entrega (chama as rotinas) em esperas alertaveis
constexpr uint32_t STATUS_USER_APC = 0x000000C0;
void KQueueApc(uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2);
bool KDeliverApcs(PPCContext& ctx);

std::string KReadGuestAnsi(uint32_t ansiStringPtr);

// Alocações pequenas e permanentes para estruturas do kernel
uint32_t GuestAllocSmall(uint32_t size);

inline void RetStatus(PPCContext& ctx, uint32_t status) { ctx.r3.s64 = (int32_t)status; }
