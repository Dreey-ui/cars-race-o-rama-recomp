// Log de sessao (veja session_log.h). stdout/stderr vao para um pipe; a thread "log da sessao" le, poe o horario no comeco
// de cada linha e grava em jogo.log. Se quem abriu o jogo redirecionou o stderr (tools/run.ps1), o texto original continua
// indo para la tambem. Mantem as ultimas kKeepSessions pastas de sessao.
#include "session_log.h"
#include "settings.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr size_t kKeepSessions = 30;
constexpr uint64_t kMaxLogBytes = 256ull << 20;   // passou disso: so linhas importantes ([CRASH], [MARCA], [sessao]...)

fs::path g_dir;
HANDLE g_pipeRead = nullptr, g_pipeWrite = nullptr;
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_echo = nullptr;                 // stderr original, quando redirecionado
std::atomic<uint64_t> g_flushRequests{ 0 }, g_flushDone{ 0 };
std::atomic<bool> g_ended{ false };
std::chrono::steady_clock::time_point g_t0;
std::mutex g_markMutex;
int g_marks = 0;

fs::path ExeDir()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// AAAA-MM-DD_HH-MM-SS, opcionalmente seguido de _N: so pastas criadas por aqui sao apagadas no rodizio
bool IsSessionName(const std::wstring& n)
{
    static const wchar_t pattern[] = L"0000-00-00_00-00-00";
    if (n.size() < 19)
        return false;
    for (size_t i = 0; i < 19; i++)
        if (pattern[i] == L'0' ? !iswdigit(n[i]) : n[i] != pattern[i])
            return false;
    for (size_t i = 19; i < n.size(); i++)
        if (n[i] != L'_' && !iswdigit(n[i]))
            return false;
    return true;
}

void RemoveOldSessions(const fs::path& root)
{
    std::error_code ec;
    std::vector<fs::path> sessions;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && IsSessionName(e.path().filename().wstring()))
            sessions.push_back(e.path());
    std::sort(sessions.begin(), sessions.end());
    for (size_t i = 0; i + kKeepSessions - 1 < sessions.size(); i++)   // a nova sessao ainda nao foi criada
        fs::remove_all(sessions[i], ec);
}

bool IsImportantLine(const std::string& line)
{
    for (const char* tag : { "[CRASH]", "[MARCA", "[sessao]", "[travou?]", "[lua]" })
        if (line.find(tag) != std::string::npos)
            return true;
    return false;
}

void ReaderThread()
{
    SetThreadDescription(GetCurrentThread(), L"log da sessao");
    std::vector<char> in(64 * 1024);
    std::string out, line, echo;
    uint64_t written = 0;
    bool truncated = false;
    for (;;)
    {
        DWORD n = 0;
        if (!ReadFile(g_pipeRead, in.data(), DWORD(in.size()), &n, nullptr) || n == 0)
            break;
        uint64_t acks = 0;
        out.clear();
        echo.clear();
        for (DWORD i = 0; i < n; i++)
        {
            const char c = in[i];
            if (c == '\x01')   // marcador do SessionLogFlush
            {
                acks++;
                continue;
            }
            if (g_echo)
                echo += c;
            if (line.empty())
            {
                SYSTEMTIME st;
                GetLocalTime(&st);
                char ts[16];
                snprintf(ts, sizeof(ts), "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                line = ts;
            }
            if (c == '\r')
                continue;
            if (c != '\n')
            {
                line += c;
                continue;
            }
            if (written + out.size() + line.size() > kMaxLogBytes && !IsImportantLine(line))
            {
                if (!truncated)
                    out += "[sessao] log passou de 256 MB: daqui em diante so as linhas importantes\r\n";
                truncated = true;
            }
            else
            {
                out += line;
                out += "\r\n";
            }
            line.clear();
        }
        if (!echo.empty())
        {
            DWORD w;
            WriteFile(g_echo, echo.data(), DWORD(echo.size()), &w, nullptr);
        }
        if (!out.empty() && g_file != INVALID_HANDLE_VALUE)
        {
            DWORD w;
            WriteFile(g_file, out.data(), DWORD(out.size()), &w, nullptr);
            written += out.size();
        }
        if (acks)
            g_flushDone += acks;
    }
}

std::string Narrow(const fs::path& p)
{
    const std::wstring w = p.wstring();
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void WriteHeader()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(stderr, "[sessao] Cars Race-O-Rama (recomp) - sessao de %02u/%02u/%04u %02u:%02u:%02u\n", st.wDay, st.wMonth, st.wYear,
            st.wHour, st.wMinute, st.wSecond);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    SYSTEMTIME mt{};
    if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa))
    {
        FILETIME local;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &mt);
    }
    fprintf(stderr, "[sessao] exe: %s (versao de %02u/%02u/%04u %02u:%02u)\n", Narrow(exe).c_str(), mt.wDay, mt.wMonth, mt.wYear,
            mt.wHour, mt.wMinute);

    // Windows (RtlGetVersion: GetVersionEx mente sem manifesto), CPU e memoria
    RTL_OSVERSIONINFOW ver{ sizeof(ver) };
    if (auto rtlGetVersion = reinterpret_cast<LONG(WINAPI*)(RTL_OSVERSIONINFOW*)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        rtlGetVersion(&ver);
    int regs[4];
    char cpu[49] = {};
    for (int i = 0; i < 3; i++)
    {
        __cpuid(regs, int(0x80000002 + i));
        memcpy(cpu + i * 16, regs, 16);
    }
    const char* cpuName = cpu;
    while (*cpuName == ' ')
        cpuName++;
    MEMORYSTATUSEX mem{ sizeof(mem) };
    GlobalMemoryStatusEx(&mem);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    fprintf(stderr, "[sessao] Windows %lu.%lu.%lu | CPU: %s (%lu nucleos logicos) | RAM: %.1f GB (%.1f GB livres)\n", ver.dwMajorVersion,
            ver.dwMinorVersion, ver.dwBuildNumber, cpuName, si.dwNumberOfProcessors, mem.ullTotalPhys / 1073741824.0,
            mem.ullAvailPhys / 1073741824.0);
    fprintf(stderr, "[sessao] config: FPS=%d VSync=%d | pasta: %s\n", SettingsGetInt(L"Video", L"FPS", 60),
            SettingsGetInt(L"Video", L"VSync", 1), Narrow(g_dir).c_str());
    fprintf(stderr, "[sessao] F12 no jogo = marcar algo estranho (salva capturas dos ultimos segundos, o som e o frame em marca_NN)\n");
}
}  // namespace

void SessionLogStart()
{
    g_t0 = std::chrono::steady_clock::now();
    const fs::path root = ExeDir() / L"logs";
    std::error_code ec;
    fs::create_directories(root, ec);
    RemoveOldSessions(root);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[40];
    swprintf(name, 40, L"%04u-%02u-%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fs::path dir = root / name;
    for (int i = 2; fs::exists(dir, ec) && i < 100; i++)
        dir = root / (std::wstring(name) + L"_" + std::to_wstring(i));
    if (fs::create_directory(dir, ec))
        g_dir = dir;

    // stderr ja redirecionado por quem abriu (tools/run.ps1, console): continua recebendo o texto sem horario
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN)
        DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &g_echo, 0, FALSE, DUPLICATE_SAME_ACCESS);

    HANDLE crtHandle = nullptr;
    if (!g_dir.empty())
        g_file = CreateFileW((g_dir / L"jogo.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE || !CreatePipe(&g_pipeRead, &g_pipeWrite, nullptr, 1 << 20) ||
        !DuplicateHandle(GetCurrentProcess(), g_pipeWrite, GetCurrentProcess(), &crtHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        // Sem pasta de logs (ex. pasta sem permissao de escrita): <exe>.log como antes, se ninguem redirecionou
        if (!g_echo)
        {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const fs::path log = fs::path(exe).replace_extension(L".log");
            _wfreopen(log.c_str(), L"w", stderr);
            _wfreopen(log.c_str(), L"a", stdout);
        }
        setvbuf(stderr, nullptr, _IONBF, 0);
        setvbuf(stdout, nullptr, _IONBF, 0);
        g_pipeRead = g_pipeWrite = nullptr;
        fprintf(stderr, "[sessao] nao consegui criar a pasta de logs em %s\n", Narrow(root).c_str());
        return;
    }
    // App sem console: stdout/stderr nao tem descritor; o freopen para NUL cria um e o _dup2 aponta ele para o pipe
    const int fd = _open_osfhandle(intptr_t(crtHandle), _O_BINARY | _O_WRONLY);
    FILE* f = nullptr;
    freopen_s(&f, "NUL", "wb", stderr);
    freopen_s(&f, "NUL", "wb", stdout);
    _dup2(fd, _fileno(stderr));
    _dup2(fd, _fileno(stdout));
    _close(fd);
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetStdHandle(STD_ERROR_HANDLE, g_pipeWrite);
    SetStdHandle(STD_OUTPUT_HANDLE, g_pipeWrite);
    std::thread(ReaderThread).detach();
    WriteHeader();
}

const fs::path& SessionDir() { return g_dir; }

void SessionLogFlush()
{
    fflush(stdout);
    fflush(stderr);
    if (!g_pipeWrite)
        return;
    // O pipe e uma fila: quando a thread de log processar este byte, tudo que veio antes ja esta no arquivo
    const uint64_t id = ++g_flushRequests;
    DWORD w;
    if (!WriteFile(g_pipeWrite, "\x01", 1, &w, nullptr))
        return;
    for (int i = 0; i < 400 && g_flushDone.load() < id; i++)
        Sleep(5);
}

void SessionLogEnd(const char* reason)
{
    if (g_ended.exchange(true))
        return;
    const auto s = uint64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count());
    fprintf(stderr, "[sessao] fim (%s) depois de %02llu:%02llu:%02llu\n", reason, (unsigned long long)(s / 3600),
            (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    SessionLogFlush();
}

fs::path SessionNewMark(int& index)
{
    std::lock_guard<std::mutex> lock(g_markMutex);
    index = ++g_marks;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[48];
    swprintf(name, 48, L"marca_%02d_%02u-%02u-%02u", index, st.wHour, st.wMinute, st.wSecond);
    const fs::path base = g_dir.empty() ? fs::current_path() : g_dir;
    std::error_code ec;
    fs::create_directories(base / name, ec);
    return base / name;
}

bool WriteImageFile(const fs::path& path, const uint8_t* bgr, uint32_t w, uint32_t h, uint32_t stride, bool jpeg)
{
    static thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE tambem serve
    (void)com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &props)))
        return false;
    if (jpeg && props)
    {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v{};
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        props->Write(1, &option, &v);
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat24bppBGR ||
        FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgr))) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()))
        return false;
    return true;
}
