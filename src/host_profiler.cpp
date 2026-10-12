// Profiler estatistico do codigo do HOST (CARS_HOST_SAMPLE=1): amostra o RIP (e 3 retornos, via unwind) das threads
// registradas a cada ~1 ms e imprime os RVAs mais frequentes a cada 20 s. Simbolizar: tools/symbolize.py
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
struct Target
{
    HANDLE thread;
    const char* name;
};
std::mutex g_mutex;
std::vector<Target> g_targets;
bool g_started = false;

constexpr int kDepth = 4;

int Capture(HANDLE t, uint64_t (&frames)[kDepth])
{
    if (SuspendThread(t) == DWORD(-1))
        return 0;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int n = 0;
    if (GetThreadContext(t, &c))
    {
        // Sem alocacao enquanto a thread esta suspensa (ela pode estar com o lock do heap)
        for (; n < kDepth; n++)
        {
            frames[n] = c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fn)
            {
                if (c.Rsp == 0)
                    break;
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);   // funcao folha
                c.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
            if (c.Rip == 0)
            {
                n++;
                break;
            }
        }
    }
    ResumeThread(t);
    return n;
}
}  // namespace

// Uso de CPU por thread do processo (CARS_THREADS=1), a cada 5 s: mostra quem e o gargalo
void HostThreadUsageStart()
{
    if (!getenv("CARS_THREADS"))
        return;
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"monitor de threads");
        std::unordered_map<DWORD, uint64_t> last;
        auto lastWall = std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto now = std::chrono::steady_clock::now();
            const double wallMs = std::chrono::duration<double, std::milli>(now - lastWall).count();
            lastWall = now;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                continue;
            THREADENTRY32 te{ sizeof(te) };
            std::vector<std::pair<double, std::string>> rows;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != GetCurrentProcessId())
                    continue;
                HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!t)
                    continue;
                FILETIME c, e, k, u;
                if (GetThreadTimes(t, &c, &e, &k, &u))
                {
                    const uint64_t cpu = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
                    auto it = last.find(te.th32ThreadID);
                    if (it != last.end())
                    {
                        const double pct = (cpu - it->second) / 10000.0 / wallMs * 100.0;
                        if (pct >= 3.0)
                        {
                            PWSTR desc = nullptr;
                            char name[96] = "?";
                            if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc)
                            {
                                WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name), nullptr, nullptr);
                                LocalFree(desc);
                            }
                            char buf[160];
                            snprintf(buf, sizeof(buf), "%s [tid %lu]", name[0] ? name : "?", te.th32ThreadID);
                            rows.emplace_back(pct, buf);
                        }
                    }
                    last[te.th32ThreadID] = cpu;
                }
                CloseHandle(t);
            }
            CloseHandle(snap);
            std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
            fprintf(stderr, "[threads]");
            for (const auto& [pct, name] : rows)
                fprintf(stderr, " | %s %.0f%%", name.c_str(), pct);
            fprintf(stderr, "\n");
        }
    }).detach();
}

void HostProfilerRegisterCurrentThread(const char* name)
{
    wchar_t wname[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
    SetThreadDescription(GetCurrentThread(), wname);
    // CARS_HOST_SAMPLE=1: todas as threads registradas; outro valor: so as que tem esse texto no nome
    // (ex.: CARS_HOST_SAMPLE=822D3158 para a thread guest com essa entrada, "gpu cp" para o processador de comandos)
    const char* filter = getenv("CARS_HOST_SAMPLE");
    if (!filter || (strcmp(filter, "1") != 0 && !strstr(name, filter)))
        return;
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_targets.push_back({ h, name });
    if (g_started)
        return;
    g_started = true;
    std::thread([] {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        // Enderecos fora do exe: "dll+offset" (modulo descoberto depois de soltar a thread)
        std::unordered_map<uint64_t, std::string> modNames;
        auto describe = [&](uint64_t addr) -> std::string {
            HMODULE m = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(addr), &m) || !m)
                return "?";
            auto it = modNames.find(uint64_t(m));
            if (it == modNames.end())
            {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(m, path, MAX_PATH);
                const char* slash = strrchr(path, '\\');
                it = modNames.emplace(uint64_t(m), slash ? slash + 1 : path).first;
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "+%llX", (unsigned long long)(addr - uint64_t(m)));
            return it->second + buf;
        };
        std::unordered_map<uint64_t, uint32_t> self, incl;
        std::unordered_map<std::string, uint32_t> dllSelf;
        uint32_t samples = 0;
        auto last = std::chrono::steady_clock::now();
        timeBeginPeriod(1);
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::vector<Target> targets;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                targets = g_targets;
            }
            for (const auto& t : targets)
            {
                uint64_t frames[kDepth];
                const int n = Capture(t.thread, frames);
                if (n == 0)
                    continue;
                samples++;
                self[frames[0] - base]++;
                if (frames[0] - base > 0x40000000)
                {
                    // primeiro endereco de retorno dentro do exe = quem chamou a DLL
                    std::string key = describe(frames[0]);
                    const std::string mod = key.substr(0, key.find('+'));
                    for (int i = 1; i < n; i++)
                        if (frames[i] - base < 0x40000000)
                        {
                            char buf[32];
                            snprintf(buf, sizeof(buf), " <- rva=%llX", (unsigned long long)(frames[i] - base));
                            dllSelf[mod + buf]++;
                            break;
                        }
                }
                // "inclusivo" aproximado: cada endereco de retorno distinto da pilha curta
                for (int i = 1; i < n; i++)
                    incl[frames[i] - base]++;
            }
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(20))
            {
                auto dump = [&](const char* title, std::unordered_map<uint64_t, uint32_t>& m) {
                    std::vector<std::pair<uint64_t, uint32_t>> v(m.begin(), m.end());
                    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                    for (size_t i = 0; i < v.size() && i < 60; i++)
                        fprintf(stderr, "[host-%s] %5.2f%% rva=%llX\n", title, 100.0 * v[i].second / samples,
                                (unsigned long long)v[i].first);
                    m.clear();
                };
                fprintf(stderr, "[host] %u amostras\n", samples);
                dump("self", self);
                dump("pilha", incl);
                {
                    std::vector<std::pair<std::string, uint32_t>> v(dllSelf.begin(), dllSelf.end());
                    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                    for (size_t i = 0; i < v.size() && i < 40; i++)
                        fprintf(stderr, "[host-dll] %5.2f%% %s\n", 100.0 * v[i].second / samples, v[i].first.c_str());
                    dllSelf.clear();
                }
                samples = 0;
                last = std::chrono::steady_clock::now();
            }
        }
    }).detach();
}
