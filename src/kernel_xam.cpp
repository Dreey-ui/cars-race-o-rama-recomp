// XAM: perfil local sempre logado, saves (XamContent*) na pasta "savegame" do jogo, enumeradores, notificacoes do
// sistema e telas do sistema (login, escolha de dispositivo, mensagens) respondidas na hora. Semantica do Xenia
// (xam_user.cc, xam_content.cc, xam_ui.cc, xam_enum.cc, xam_notify.cc em C:\DEB\ref\xenia).
//   <jogo>\savegame\perfil.ini         [Perfil] Nome= (gamertag; padrao = usuario do Windows)
//   <jogo>\savegame\<arquivo>\         cada "conteudo" (save) do jogo; o jogo abre como <raiz>:\... (Cars:\PSLOT0.dat)
//   <jogo>\savegame\<arquivo>.info      tipo e nome mostrado do save (para a lista de saves)
// CARS_SAVES=<pasta> troca a pasta (testes).
#include "kernel.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kOk = 0, kFileNotFound = 2, kPathNotFound = 3, kInvalidHandle = 6, kNoMoreFiles = 18, kInvalidParameter = 87,
                   kInsufficientBuffer = 122, kAlreadyExists = 183, kIoPending = 997, kDeviceNotConnected = 0x48F,
                   kNoSuchUser = 0x525, kFunctionFailed = 0x65B;
constexpr uint32_t kDeviceHdd = 1;
constexpr uint64_t kXuid = 0xE000000043617273ull;   // XUID offline ("Cars")
constexpr uint32_t kContentDataSize = 0x134;          // XCONTENT_DATA

uint32_t HResult(uint32_t win32) { return win32 ? 0x80070000u | (win32 & 0xFFFF) : 0; }

std::string GuestCString(uint32_t p, size_t max = 256)
{
    std::string s;
    for (size_t i = 0; p && i < max; i++)
    {
        const char c = char(R8(p + uint32_t(i)));
        if (!c)
            break;
        s += c;
    }
    return s;
}

std::u16string GuestWString(uint32_t p, size_t max = 512)
{
    std::u16string s;
    for (size_t i = 0; p && i < max; i++)
    {
        const char16_t c = char16_t(R16(p + uint32_t(2 * i)));
        if (!c)
            break;
        s += c;
    }
    return s;
}

std::string Utf8(const std::u16string& s)
{
    std::string out;
    if (!s.empty())
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
        out.resize(size_t(n));
        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    }
    return out;
}

std::u16string Utf16(const std::string& s)
{
    std::u16string out;
    if (!s.empty())
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        out.resize(size_t(n));
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), (wchar_t*)out.data(), n);
    }
    return out;
}

// ---- Pasta dos saves e perfil ----
const fs::path& SaveRoot()
{
    static const fs::path root = [] {
        fs::path p = getenv("CARS_SAVES") ? fs::path(getenv("CARS_SAVES")) : fs::path(g_gameRoot) / L"savegame";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }();
    return root;
}

const std::string& ProfileName()
{
    static const std::string name = [] {
        const std::wstring ini = (SaveRoot() / L"perfil.ini").wstring();
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"Perfil", L"Nome", L"", buf, 64, ini.c_str());
        std::wstring w = buf;
        if (w.empty())
        {
            DWORD n = 64;
            if (!GetUserNameW(buf, &n))
                wcscpy_s(buf, L"Jogador");
            w = buf;
            WritePrivateProfileStringW(L"Perfil", L"Nome", w.c_str(), ini.c_str());
        }
        // gamertag: ate 15 caracteres ASCII
        std::string s;
        for (wchar_t c : w)
            if (s.size() < 15)
                s += (c >= 32 && c < 127) ? char(c) : '_';
        if (s.empty())
            s = "Jogador";
        fprintf(stderr, "[perfil] logado localmente como \"%s\"; saves em %s\n", s.c_str(), SaveRoot().string().c_str());
        return s;
    }();
    return name;
}

// ---- XOVERLAPPED {+0 resultado, +4 tamanho, +8 contexto, +0xC evento, +0x10 rotina, +0x14 contexto da rotina, +0x18 erro} ----
void SetEvent_(uint32_t handle, bool signaled)
{
    if (!handle)
        return;
    if (auto ev = KGetHandle(handle))
    {
        std::lock_guard<std::mutex> lock(g_kmtx);
        ev->signaled = signaled;
        if (signaled)
            g_kcv.notify_all();
    }
}

void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t extended, uint32_t length)
{
    W32(ov + 0x04, length);
    W32(ov + 0x18, extended);
    W32(ov + 0x00, result);
    SetEvent_(R32(ov + 0x0C), true);
    if (const uint32_t routine = R32(ov + 0x10))
        KQueueApc(routine, result, length, ov);
}

// Termina uma operacao que pode ter XOVERLAPPED: com ele, completa (resultado 0 ou "falhou" + erro estendido) e devolve
// ERROR_IO_PENDING como o XAM; sem ele, devolve o resultado direto
uint32_t Finish(uint32_t ov, uint32_t result, uint32_t length = 0)
{
    if (!ov)
        return result;
    CompleteOverlapped(ov, result ? kFunctionFailed : kOk, HResult(result), length);
    return kIoPending;
}

// ---- Notificacoes (XamNotifyCreateListener / XNotifyGetNext) ----
struct Listener
{
    uint64_t mask = 0;
    std::deque<std::pair<uint32_t, uint32_t>> queue;
};
std::mutex g_notifyMutex;
std::map<uint32_t, Listener> g_listeners;
bool g_startupNotified = false, g_liveNotified = false;

constexpr uint32_t kXnSysUi = 0x09, kXnSysSigninChanged = 0x0A, kXnLiveConnection = 0x02000001, kXnLiveLink = 0x02000003;

void EnqueueLocked(uint32_t handle, Listener& l, uint32_t id, uint32_t param)
{
    if (!(l.mask & (1ull << ((id >> 25) & 0x3F))))
        return;
    l.queue.push_back({ id, param });
    SetEvent_(handle, true);
}

void Broadcast(uint32_t id, uint32_t param)
{
    std::lock_guard<std::mutex> lock(g_notifyMutex);
    for (auto& [h, l] : g_listeners)
        EnqueueLocked(h, l, id, param);
}

// Tela do sistema "aberta e fechada" na hora (o jogo espera o XN_SYS_UI voltar a 0 para continuar)
void SystemUiFlash()
{
    Broadcast(kXnSysUi, 1);
    Broadcast(kXnSysUi, 0);
}

// ---- Enumeradores (XamEnumerate) ----
struct Enumerator
{
    uint32_t itemSize = 0, perCall = 1;
    std::vector<std::vector<uint8_t>> items;
    size_t pos = 0;
};
std::mutex g_enumMutex;
std::map<uint32_t, std::shared_ptr<Enumerator>> g_enums;

uint32_t NewEnumerator(std::shared_ptr<Enumerator> e)
{
    auto obj = std::make_shared<KObject>();
    obj->type = KType::Event;
    const uint32_t h = KNewHandle(obj);
    std::lock_guard<std::mutex> lock(g_enumMutex);
    g_enums[h] = std::move(e);
    return h;
}

// ---- Saves (XCONTENT_DATA {+0 dispositivo, +4 tipo, +8 nome mostrado u16[128], +0x108 nome do arquivo char[42]}) ----
std::mutex g_contentMutex;
std::map<std::string, fs::path> g_roots;   // "save" -> pasta do save montado

struct ContentData
{
    uint32_t device = 0, type = 0;
    std::u16string display;
    std::string file;
};

ContentData ReadContentData(uint32_t p)
{
    ContentData c;
    c.device = R32(p);
    c.type = R32(p + 4);
    c.display = GuestWString(p + 8, 128);
    for (uint32_t i = 0; i < 42; i++)
    {
        const char ch = char(R8(p + 0x108 + i));
        if (!ch)
            break;
        c.file += ch;
    }
    return c;
}

void WriteContentData(uint32_t p, const ContentData& c)
{
    for (uint32_t i = 0; i < kContentDataSize; i += 4)
        W32(p + i, 0);
    W32(p, c.device);
    W32(p + 4, c.type);
    for (size_t i = 0; i < c.display.size() && i < 127; i++)
        W16(p + 8 + uint32_t(2 * i), uint16_t(c.display[i]));
    for (size_t i = 0; i < c.file.size() && i < 42; i++)
        W8(p + 0x108 + uint32_t(i), uint8_t(c.file[i]));
}

fs::path ContentDir(const std::string& file)
{
    std::string safe = file;
    for (char& ch : safe)
        if (strchr("<>:\"/\\|?*", ch) || (unsigned char)ch < 32)
            ch = '_';
    if (safe.empty())
        safe = "_";
    return SaveRoot() / safe;
}

void WriteInfo(const fs::path& dir, const ContentData& c)
{
    fs::path info = dir;
    info += L".info";
    if (FILE* f = _wfopen(info.c_str(), L"wb"))
    {
        fprintf(f, "tipo=%u\nnome=%s\narquivo=%s\n", c.type, Utf8(c.display).c_str(), c.file.c_str());
        fclose(f);
    }
}

std::vector<ContentData> ListContent(uint32_t type)
{
    std::vector<ContentData> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(SaveRoot(), ec))
    {
        if (!e.is_directory(ec))
            continue;
        ContentData c;
        c.device = kDeviceHdd;
        c.type = 1;
        c.file = e.path().filename().string();
        fs::path info = e.path();
        info += L".info";
        if (FILE* f = _wfopen(info.c_str(), L"rb"))
        {
            char line[600];
            while (fgets(line, sizeof(line), f))
            {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                    l.pop_back();
                if (l.compare(0, 5, "tipo=") == 0)
                    c.type = uint32_t(strtoul(l.c_str() + 5, nullptr, 10));
                else if (l.compare(0, 5, "nome=") == 0)
                    c.display = Utf16(l.substr(5));
                else if (l.compare(0, 8, "arquivo=") == 0)
                    c.file = l.substr(8);
            }
            fclose(f);
        }
        if (c.display.empty())
            c.display = Utf16(c.file);
        if (!type || c.type == type)
            out.push_back(std::move(c));
    }
    return out;
}

void LogSave(const char* fmt, ...)
{
    static std::atomic<int> n{ 0 };
    if (n++ > 200)
        return;
    va_list a;
    va_start(a, fmt);
    fprintf(stderr, "[save] ");
    vfprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    va_end(a);
}

// ---- Configuracoes do perfil (XUSER_PROFILE_SETTING: +0 origem, +8 usuario/xuid, +0x10 id, +0x18 XUSER_DATA {tipo, +8 valor}) ----
struct DefaultSetting
{
    uint32_t id;
    uint32_t value;
};
const DefaultSetting kDefaults[] = {
    { 0x10040002, 0 },   // XPROFILE_GAMER_YAXIS_INVERSION
    { 0x10040003, 3 },   // XPROFILE_OPTION_CONTROLLER_VIBRATION (ligada)
    { 0x10040015, 0 },   // XPROFILE_GAMER_DIFFICULTY (normal)
    { 0x10040018, 0 },   // XPROFILE_GAMER_CONTROL_SENSITIVITY
    { 0x10040021, 1 },   // XPROFILE_GAMER_ACTION_AUTO_AIM
    { 0x10040022, 1 },   // XPROFILE_GAMER_ACTION_AUTO_CENTER
    { 0x10040023, 0 },   // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
    { 0x10040024, 1 },   // XPROFILE_GAMER_RACE_TRANSMISSION (automatica)
    { 0x10040025, 0 },   // XPROFILE_GAMER_RACE_CAMERA_LOCATION
    { 0x10040026, 0 },   // XPROFILE_GAMER_RACE_BRAKE_CONTROL
    { 0x10040027, 0 },   // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
};
}  // namespace

// Caminho "<raiz>:\..." de um save montado (XamContentCreateEx) -> pasta no PC. Chamado por ResolvePath (kernel_io.cpp).
bool XamResolveContentPath(const std::string& xboxPath, fs::path& out)
{
    std::string p = xboxPath;
    for (char& c : p)
        if (c == '\\')
            c = '/';
    if (p.compare(0, 4, "/??/") == 0)
        p = p.substr(4);
    const size_t colon = p.find(':');
    if (colon == std::string::npos || colon == 0 || p.find('/') < colon)
        return false;
    std::string root = p.substr(0, colon);
    for (char& c : root)
        c = char(tolower((unsigned char)c));
    std::lock_guard<std::mutex> lock(g_contentMutex);
    auto it = g_roots.find(root);
    if (it == g_roots.end())
        return false;
    std::string rest = p.substr(colon + 1);
    while (!rest.empty() && rest[0] == '/')
        rest.erase(0, 1);
    out = rest.empty() ? it->second : it->second / fs::path(rest);
    return true;
}

// ---- Usuario ----
KERNEL_IMPORT(XamUserGetSigninState)
{
    // 1 = logado localmente (sem Live) - so o controle 1
    ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0;
    (void)ProfileName();
}

KERNEL_IMPORT(XamUserGetName)
{
    // (usuario, buffer, tamanho com o zero)
    const uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32;
    if (user != 0)
    {
        if (buf && len)
            W8(buf, 0);
        ctx.r3.u64 = kNoSuchUser;
        return;
    }
    const std::string& name = ProfileName();
    const uint32_t n = std::min<uint32_t>(len ? len - 1 : 0, uint32_t(name.size()));
    for (uint32_t i = 0; i < n; i++)
        W8(buf + i, uint8_t(name[i]));
    if (len)
        W8(buf + n, 0);
    ctx.r3.u64 = kOk;
}

KERNEL_IMPORT(XamUserReadProfileSettings)
{
    // (titulo, usuario, n xuids, xuids*, n configuracoes, ids*, tamanho do buffer*, buffer, [pilha] overlapped)
    const uint32_t user = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    const uint32_t sizePtr = ctx.r9.u32, buffer = ctx.r10.u32, ov = R32(ctx.r1.u32 + 0x54);
    if (count < 1 || count > 32 || !sizePtr || !ids)
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    const uint32_t users = xuids ? std::max(xuidCount, 1u) : 1;
    uint32_t extra = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = R32(ids + 4 * i), type = id >> 28, size = (id >> 16) & 0xFFF;
        if (type == 4 || type == 6)   // texto / binario: dados depois dos cabecalhos
            extra += size;
    }
    const uint32_t need = 8 + users * (count * 40 + extra);
    const uint32_t have = R32(sizePtr);
    if (!buffer || have < need)
    {
        if (!have)
            W32(sizePtr, need);
        ctx.r3.u64 = kInsufficientBuffer;
        return;
    }
    static std::atomic<int> logged{ 0 };
    if (logged++ < 4)
    {
        std::string list;
        for (uint32_t i = 0; i < count; i++)
        {
            char b[12];
            snprintf(b, sizeof(b), " %08X", R32(ids + 4 * i));
            list += b;
        }
        fprintf(stderr, "[perfil] jogo leu %u configuracao(oes) do perfil:%s\n", count, list.c_str());
    }
    if (user != 0 && !xuids)
    {
        ctx.r3.u64 = Finish(ov, kNoSuchUser);
        return;
    }
    W32(buffer, count);
    W32(buffer + 4, buffer + 8);
    uint32_t data = buffer + 8 + count * 40;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t s = buffer + 8 + i * 40, id = R32(ids + 4 * i), type = id >> 28, size = (id >> 16) & 0xFFF;
        for (uint32_t k = 0; k < 40; k += 4)
            W32(s + k, 0);
        uint32_t source = 0;   // sem valor
        uint32_t value = 0;
        for (const auto& d : kDefaults)
            if (d.id == id)
            {
                source = 1;   // padrao do sistema
                value = d.value;
            }
        W32(s + 0x00, source);
        if (xuids)
            W64(s + 0x08, kXuid);
        else
            W32(s + 0x08, user);
        W32(s + 0x10, id);
        W8(s + 0x18, uint8_t(type));
        if (type == 4 || type == 6)
        {
            W32(s + 0x20, 0);   // vazio
            W32(s + 0x24, data);
            data += size;
        }
        else
            W32(s + 0x20, value);
    }
    ctx.r3.u64 = Finish(ov, kOk);
}

KERNEL_IMPORT(XamUserCreateAchievementEnumerator)
{
    // (titulo, usuario, xuid, flags, inicio, quantidade, tamanho do buffer*, handle*): sem conquistas
    const uint32_t flags = ctx.r6.u32, count = ctx.r8.u32, sizePtr = ctx.r9.u32, handlePtr = ctx.r10.u32;
    if (!count || !sizePtr || !handlePtr)
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    auto e = std::make_shared<Enumerator>();
    e->itemSize = 36 + ((flags & 7) ? 464 : 0);
    e->perCall = count;
    W32(sizePtr, e->itemSize * count);
    W32(handlePtr, NewEnumerator(e));
    ctx.r3.u64 = kOk;
}

// ---- Telas do sistema ----
KERNEL_IMPORT(XamShowSigninUI)
{
    // (usuarios necessarios, flags): o perfil local ja esta logado
    SystemUiFlash();
    Broadcast(kXnSysSigninChanged, 1);
    ctx.r3.u64 = kOk;
}

KERNEL_IMPORT(XamShowDeviceSelectorUI)
{
    // (usuario, tipo de conteudo, flags, bytes pedidos, id do dispositivo*, overlapped): sempre o "disco rigido"
    const uint32_t devPtr = ctx.r7.u32, ov = ctx.r8.u32;
    if (!devPtr || !ov)
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    W32(devPtr, kDeviceHdd);
    LogSave("escolha de dispositivo: disco rigido (%s)", SaveRoot().string().c_str());
    SystemUiFlash();
    ctx.r3.u64 = Finish(ov, kOk);
}

KERNEL_IMPORT(XamShowMessageBoxUIEx)
{
    // (usuario, titulo, texto, n botoes, botoes*, botao ativo, flags, ?, [pilha] resultado*, overlapped): escolhe o botao ativo
    const std::string title = Utf8(GuestWString(ctx.r4.u32)), text = Utf8(GuestWString(ctx.r5.u32));
    const uint32_t active = ctx.r8.u32, resultPtr = R32(ctx.r1.u32 + 0x54), ov = R32(ctx.r1.u32 + 0x58);
    fprintf(stderr, "[xam] mensagem do sistema \"%s\": %s (respondido: botao %u)\n", title.c_str(), text.c_str(), active);
    if (resultPtr)
        W32(resultPtr, active);
    SystemUiFlash();
    ctx.r3.u64 = Finish(ov, kOk);
}

// ---- Dispositivos e saves ----
KERNEL_IMPORT(XamContentGetDeviceState)
{
    // (dispositivo, overlapped)
    const uint32_t r = ctx.r3.u32 == kDeviceHdd ? kOk : kDeviceNotConnected;
    ctx.r3.u64 = Finish(ctx.r4.u32, r);
}

KERNEL_IMPORT(XamContentGetDeviceData)
{
    // (dispositivo, X_CONTENT_DEVICE_DATA* {id, tipo, total u64, livre u64, nome u16[28]})
    const uint32_t p = ctx.r4.u32;
    if (ctx.r3.u32 != kDeviceHdd || !p)
    {
        ctx.r3.u64 = kDeviceNotConnected;
        return;
    }
    for (uint32_t i = 0; i < 0x50; i += 4)
        W32(p + i, 0);
    ULARGE_INTEGER freeBytes{}, total{};
    if (!GetDiskFreeSpaceExW(SaveRoot().c_str(), &freeBytes, &total, nullptr))
    {
        total.QuadPart = 16ull << 30;
        freeBytes.QuadPart = 8ull << 30;
    }
    W32(p + 0, kDeviceHdd);
    W32(p + 4, 1);   // disco rigido
    W64(p + 8, total.QuadPart);
    W64(p + 16, freeBytes.QuadPart);
    const char16_t name[] = u"Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        W16(p + 24 + 2 * i, uint16_t(name[i]));
    ctx.r3.u64 = kOk;
}

KERNEL_IMPORT(XamContentCreateEnumerator)
{
    // (usuario, dispositivo, tipo, flags, itens por chamada, tamanho do buffer*, handle*)
    const uint32_t device = ctx.r4.u32, type = ctx.r5.u32, perCall = ctx.r7.u32, sizePtr = ctx.r8.u32, handlePtr = ctx.r9.u32;
    if (!handlePtr || (device && device != kDeviceHdd))
    {
        if (sizePtr)
            W32(sizePtr, 0);
        ctx.r3.u64 = HResult(kInvalidParameter);
        return;
    }
    auto e = std::make_shared<Enumerator>();
    e->itemSize = kContentDataSize;
    e->perCall = std::max(perCall, 1u);
    std::lock_guard<std::mutex> lock(g_contentMutex);
    for (const auto& c : ListContent(type))
    {
        std::vector<uint8_t> item(kContentDataSize, 0);
        auto put32 = [&](size_t o, uint32_t v) { item[o] = uint8_t(v >> 24); item[o + 1] = uint8_t(v >> 16); item[o + 2] = uint8_t(v >> 8); item[o + 3] = uint8_t(v); };
        put32(0, kDeviceHdd);
        put32(4, c.type);
        for (size_t i = 0; i < c.display.size() && i < 127; i++)
        {
            item[8 + 2 * i] = uint8_t(c.display[i] >> 8);
            item[9 + 2 * i] = uint8_t(c.display[i]);
        }
        for (size_t i = 0; i < c.file.size() && i < 42; i++)
            item[0x108 + i] = uint8_t(c.file[i]);
        e->items.push_back(std::move(item));
    }
    LogSave("lista de saves (tipo %u): %zu encontrado(s)", type, e->items.size());
    if (sizePtr)
        W32(sizePtr, kContentDataSize * e->perCall);
    W32(handlePtr, NewEnumerator(e));
    ctx.r3.u64 = kOk;
}

KERNEL_IMPORT(XamEnumerate)
{
    // (handle, flags, buffer, tamanho, itens devolvidos*, overlapped)
    const uint32_t handle = ctx.r3.u32, buffer = ctx.r5.u32, length = ctx.r6.u32, itemsPtr = ctx.r7.u32, ov = ctx.r8.u32;
    std::shared_ptr<Enumerator> e;
    {
        std::lock_guard<std::mutex> lock(g_enumMutex);
        auto it = g_enums.find(handle);
        if (it != g_enums.end())
            e = it->second;
    }
    if (!e)
    {
        ctx.r3.u64 = kInvalidHandle;
        return;
    }
    uint32_t n = 0, result = kOk;
    if (!buffer)
        result = kInvalidParameter;
    else
    {
        const size_t room = e->itemSize ? length / e->itemSize : 0;
        const size_t take = std::min({ size_t(e->perCall), e->items.size() - e->pos, room });
        if (!take)
            result = kNoMoreFiles;
        for (size_t k = 0; k < take; k++, e->pos++)
        {
            const auto& item = e->items[e->pos];
            memcpy(g_mem.base + buffer + n * e->itemSize, item.data(), item.size());
            n++;
        }
    }
    if (ov)
    {
        CompleteOverlapped(ov, result ? kFunctionFailed : kOk, HResult(result), n);
        ctx.r3.u64 = kIoPending;
        return;
    }
    if (itemsPtr)
        W32(itemsPtr, result ? 0 : n);
    ctx.r3.u64 = result;
}

KERNEL_IMPORT(XamContentCreateEx)
{
    // (usuario, nome da raiz, XCONTENT_DATA*, flags, disposicao*, licenca*, cache, tamanho, [pilha] overlapped)
    const std::string root = GuestCString(ctx.r4.u32, 64);
    const ContentData c = ReadContentData(ctx.r5.u32);
    const uint32_t flags = ctx.r6.u32, dispPtr = ctx.r7.u32, licensePtr = ctx.r8.u32, ov = R32(ctx.r1.u32 + 0x54);
    if (root.empty() || c.file.empty())
    {
        ctx.r3.u64 = 0x7B;   // ERROR_INVALID_NAME
        return;
    }
    std::string key = root;
    if (!key.empty() && key.back() == ':')
        key.pop_back();
    for (char& ch : key)
        ch = char(tolower((unsigned char)ch));
    const fs::path dir = ContentDir(c.file);
    std::error_code ec;
    uint32_t result = kOk, disposition = 0;   // 1 criado, 2 aberto
    {
        std::lock_guard<std::mutex> lock(g_contentMutex);
        const bool exists = fs::is_directory(dir, ec);
        switch (flags & 0xF)
        {
        case 1: if (exists) result = kAlreadyExists; else disposition = 1; break;                 // criar novo
        case 2: if (exists) fs::remove_all(dir, ec); disposition = 1; break;                       // criar sempre
        case 3: if (!exists) result = kPathNotFound; else disposition = 2; break;                  // abrir existente
        case 4: disposition = exists ? 2 : 1; break;                                               // abrir sempre
        case 5: if (!exists) result = kPathNotFound; else { fs::remove_all(dir, ec); disposition = 1; } break;   // truncar
        default: result = kInvalidParameter; break;
        }
        if (!result && disposition == 1)
        {
            fs::create_directories(dir, ec);
            WriteInfo(dir, c);
            if (ec)
                result = kPathNotFound;
        }
        if (!result)
            g_roots[key] = dir;
    }
    LogSave("XamContentCreateEx \"%s\" (tipo %u, \"%s\") como %s: -> %s (%s)", c.file.c_str(), c.type, Utf8(c.display).c_str(),
            root.c_str(), dir.string().c_str(), result ? "falhou" : disposition == 1 ? "criado" : "aberto");
    if (dispPtr && !result)
        W32(dispPtr, disposition);
    if (licensePtr && !result)
        W32(licensePtr, 0);
    if (ov)
    {
        CompleteOverlapped(ov, result ? kFunctionFailed : kOk, HResult(result), disposition);
        ctx.r3.u64 = kIoPending;
        return;
    }
    ctx.r3.u64 = result;
}

KERNEL_IMPORT(XamContentClose)
{
    // (nome da raiz, overlapped)
    std::string key = GuestCString(ctx.r3.u32, 64);
    if (!key.empty() && key.back() == ':')
        key.pop_back();
    for (char& ch : key)
        ch = char(tolower((unsigned char)ch));
    {
        std::lock_guard<std::mutex> lock(g_contentMutex);
        g_roots.erase(key);
    }
    LogSave("XamContentClose %s", key.c_str());
    const uint32_t ov = ctx.r4.u32;
    if (ov)
    {
        CompleteOverlapped(ov, kOk, kOk, 0);
        ctx.r3.u64 = kIoPending;
        return;
    }
    ctx.r3.u64 = kOk;
}

KERNEL_IMPORT(XamContentDelete)
{
    // (usuario, XCONTENT_DATA*, overlapped)
    const ContentData c = ReadContentData(ctx.r4.u32);
    const fs::path dir = ContentDir(c.file);
    std::error_code ec;
    uint32_t result = kOk;
    {
        std::lock_guard<std::mutex> lock(g_contentMutex);
        if (!fs::is_directory(dir, ec))
            result = kFileNotFound;
        else
        {
            fs::remove_all(dir, ec);
            fs::path info = dir;
            info += L".info";
            fs::remove(info, ec);
        }
    }
    LogSave("XamContentDelete \"%s\": %s", c.file.c_str(), result ? "nao existe" : "apagado");
    ctx.r3.u64 = Finish(ctx.r5.u32, result);
}

KERNEL_IMPORT(XamContentGetCreator)
{
    // (usuario, XCONTENT_DATA*, e o criador*, xuid do criador*, overlapped)
    const ContentData c = ReadContentData(ctx.r4.u32);
    const uint32_t isCreator = ctx.r5.u32, xuidPtr = ctx.r6.u32, ov = ctx.r7.u32;
    if (!isCreator)
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    std::error_code ec;
    uint32_t result = kOk;
    if (!fs::is_directory(ContentDir(c.file), ec))
        result = kPathNotFound;
    else
    {
        W32(isCreator, c.type == 1 ? 1 : 0);
        if (xuidPtr)
            W64(xuidPtr, c.type == 1 ? kXuid : 0);
    }
    ctx.r3.u64 = Finish(ov, result);
}

// ---- Notificacoes ----
KERNEL_IMPORT(XamNotifyCreateListener)
{
    // (mascara u64, versao maxima)
    auto obj = std::make_shared<KObject>();
    obj->type = KType::Event;
    obj->manualReset = true;
    const uint32_t h = KNewHandle(obj);
    std::lock_guard<std::mutex> lock(g_notifyMutex);
    Listener& l = g_listeners[h];
    l.mask = ctx.r3.u64;
    // o que o XAM manda na partida (como o Xenia): tela do sistema fechada, perfil logado, Live desconectado
    if (!g_startupNotified && (l.mask & 1))
    {
        g_startupNotified = true;
        EnqueueLocked(h, l, kXnSysUi, 0);
        EnqueueLocked(h, l, kXnSysSigninChanged, 1);
    }
    if (!g_liveNotified && (l.mask & 2))
    {
        g_liveNotified = true;
        EnqueueLocked(h, l, kXnLiveConnection, 0x001510F1);
        EnqueueLocked(h, l, kXnLiveLink, 0);
    }
    ctx.r3.u64 = h;
}

KERNEL_IMPORT(XNotifyGetNext)
{
    // (handle, id procurado (0 = qualquer), id*, parametro*)
    const uint32_t handle = ctx.r3.u32, match = ctx.r4.u32, idPtr = ctx.r5.u32, paramPtr = ctx.r6.u32;
    if (paramPtr)
        W32(paramPtr, 0);
    if (!idPtr)
    {
        ctx.r3.u64 = 0;
        return;
    }
    W32(idPtr, 0);
    std::lock_guard<std::mutex> lock(g_notifyMutex);
    auto it = g_listeners.find(handle);
    if (it == g_listeners.end())
    {
        ctx.r3.u64 = 0;
        return;
    }
    auto& q = it->second.queue;
    bool got = false;
    for (auto n = q.begin(); n != q.end(); ++n)
        if (!match || n->first == match)
        {
            W32(idPtr, n->first);
            if (paramPtr)
                W32(paramPtr, n->second);
            q.erase(n);
            got = true;
            break;
        }
    if (q.empty())
        SetEvent_(handle, false);
    ctx.r3.u64 = got ? 1 : 0;
}

// ---- XMsg assincrono (apps do sistema: conquistas, sessoes, player de musica) ----
static void XMsgAsync(PPCContext& ctx, const char* name)
{
    // (app, mensagem, overlapped, buffer, tamanho[, args]): aceito e completado na hora
    const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, ov = ctx.r5.u32;
    static std::atomic<int> logged{ 0 };
    if (logged++ < 12)
        fprintf(stderr, "[xmsg] %s app=%X msg=%08X buf=%08X tam=%X (completado sem efeito)\n", name, app, msg, ctx.r6.u32, ctx.r7.u32);
    if (ov)
    {
        CompleteOverlapped(ov, kOk, kOk, 0);
        ctx.r3.u64 = kIoPending;
        return;
    }
    ctx.r3.u64 = kOk;
}

KERNEL_IMPORT(XMsgStartIORequest) { XMsgAsync(ctx, "XMsgStartIORequest"); }
KERNEL_IMPORT(XMsgStartIORequestEx) { XMsgAsync(ctx, "XMsgStartIORequestEx"); }
