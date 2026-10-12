// Entrada do host: controle XInput (controle de Xbox no PC) + teclado quando a janela do jogo tem foco.
// Teclado: Enter = Start, Tab = Back, Espaco = A, Esc/Backspace/X = B, C = X, V = Y, Q/E = LB/RB,
//          setas = direcional + analogico esquerdo, W = acelerar (RT), S = freio/re (LT), I/J/K/L = analogico direito.
#include "input.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* XInputSetStateFn)(DWORD, XINPUT_VIBRATION*);
static XInputGetStateFn g_xinputGetState = nullptr;
static XInputSetStateFn g_xinputSetState = nullptr;
static bool g_inputInit = false;

static void InitInput()
{
    if (g_inputInit)
        return;
    g_inputInit = true;
    for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
    {
        if (HMODULE m = LoadLibraryW(dll))
        {
            g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
            g_xinputSetState = reinterpret_cast<XInputSetStateFn>(GetProcAddress(m, "XInputSetState"));
            if (g_xinputGetState)
                break;
        }
    }
}

static bool OurWindowHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Log de sessao: botoes que mudaram desde a ultima leitura do jogo ("[input] controle 0: +A -START"); gatilhos e analogicos
// contam como apertados passando da metade
static void LogInputChanges(uint32_t user, bool connected, const HostPadState& s)
{
    static const char* const kNames[] = { "CIMA", "BAIXO", "ESQ", "DIR", "START", "BACK", "L3", "R3", "LB", "RB", "?10", "?11",
                                          "A", "B", "X", "Y", "LT", "RT", "LS_ESQ", "LS_DIR", "LS_CIMA", "LS_BAIXO",
                                          "RS_ESQ", "RS_DIR", "RS_CIMA", "RS_BAIXO" };
    static std::mutex mutex;
    static uint32_t lastBits[4];
    static bool lastConnected[4];
    if (user >= 4)
        return;
    const int t = 16384;
    uint32_t bits = s.buttons;
    bits |= (s.leftTrigger > 128 ? 1u : 0u) << 16 | (s.rightTrigger > 128 ? 1u : 0u) << 17;
    bits |= (s.thumbLX < -t ? 1u : 0u) << 18 | (s.thumbLX > t ? 1u : 0u) << 19 | (s.thumbLY > t ? 1u : 0u) << 20 | (s.thumbLY < -t ? 1u : 0u) << 21;
    bits |= (s.thumbRX < -t ? 1u : 0u) << 22 | (s.thumbRX > t ? 1u : 0u) << 23 | (s.thumbRY > t ? 1u : 0u) << 24 | (s.thumbRY < -t ? 1u : 0u) << 25;
    std::lock_guard<std::mutex> lock(mutex);
    if (connected != lastConnected[user])
    {
        if (user != 0)   // o 0 sempre existe (teclado)
            fprintf(stderr, "[input] controle %u %s\n", user, connected ? "conectado" : "desconectado");
        lastConnected[user] = connected;
    }
    const uint32_t changed = bits ^ lastBits[user];
    if (!changed)
        return;
    std::string text;
    for (uint32_t i = 0; i < 26; i++)
        if (changed & (1u << i))
        {
            text += (bits & (1u << i)) ? " +" : " -";
            text += kNames[i];
        }
    fprintf(stderr, "[input] controle %u:%s\n", user, text.c_str());
    lastBits[user] = bits;
}

bool HostInputGetState(uint32_t user, HostPadState& out)
{
    InitInput();
    out = HostPadState{};
    bool connected = false;
    if (g_xinputGetState)
    {
        XINPUT_STATE s{};
        if (g_xinputGetState(user, &s) == ERROR_SUCCESS)
        {
            connected = true;
            out.buttons = s.Gamepad.wButtons;
            out.leftTrigger = s.Gamepad.bLeftTrigger;
            out.rightTrigger = s.Gamepad.bRightTrigger;
            out.thumbLX = s.Gamepad.sThumbLX;
            out.thumbLY = s.Gamepad.sThumbLY;
            out.thumbRX = s.Gamepad.sThumbRX;
            out.thumbRY = s.Gamepad.sThumbRY;
        }
    }
    // O teclado sempre conta como o controle 0
    if (user == 0)
    {
        connected = true;
        if (OurWindowHasFocus())
        {
            uint16_t b = 0;
            if (Key(VK_RETURN)) b |= XINPUT_GAMEPAD_START;
            if (Key(VK_TAB)) b |= XINPUT_GAMEPAD_BACK;
            if (Key(VK_SPACE)) b |= XINPUT_GAMEPAD_A;
            if (Key('X') || Key(VK_ESCAPE) || Key(VK_BACK)) b |= XINPUT_GAMEPAD_B;
            if (Key('C')) b |= XINPUT_GAMEPAD_X;
            if (Key('V')) b |= XINPUT_GAMEPAD_Y;
            if (Key('Q')) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (Key('E')) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (Key(VK_UP)) b |= XINPUT_GAMEPAD_DPAD_UP;
            if (Key(VK_DOWN)) b |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (Key(VK_LEFT)) b |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (Key(VK_RIGHT)) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
            out.buttons |= b;
            if (Key('W')) out.rightTrigger = 255;
            if (Key('S')) out.leftTrigger = 255;
            if (Key(VK_LEFT)) out.thumbLX = -32767;
            if (Key(VK_RIGHT)) out.thumbLX = 32767;
            if (Key(VK_UP)) out.thumbLY = 32767;
            if (Key(VK_DOWN)) out.thumbLY = -32767;
            if (Key('J')) out.thumbRX = -32767;
            if (Key('L')) out.thumbRX = 32767;
            if (Key('I')) out.thumbRY = 32767;
            if (Key('K')) out.thumbRY = -32767;
        }
    }
    LogInputChanges(user, connected, out);
    return connected;
}

void HostInputSetVibration(uint32_t user, uint16_t left, uint16_t right)
{
    InitInput();
    if (!g_xinputSetState)
        return;
    XINPUT_VIBRATION v{ left, right };
    g_xinputSetState(user, &v);
}
