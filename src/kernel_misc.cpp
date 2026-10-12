// Time, XAM (input, configuration) and basic video
#include "kernel.h"
#include "input.h"

#include <atomic>
#include <chrono>
#include <windows.h>

constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_ = 0x48F;
constexpr uint32_t ERROR_NOT_FOUND_            = 0x490;

// ---- Time ----
// Xbox 360 timebase (mftb / the game's QueryPerformanceCounter): 50 MHz, derived from the host QPC
constexpr uint64_t kGuestTimebaseHz = 50000000;

uint64_t GuestTimebase()
{
    static const struct Scale
    {
        uint64_t start, mul;   // tb = (qpc - start) * mul >> 32
        Scale()
        {
            LARGE_INTEGER f, c;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);
            start = uint64_t(c.QuadPart);
            mul = (kGuestTimebaseHz << 32) / uint64_t(f.QuadPart);
        }
    } s;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // starts at ~1 s (some games treat 0 as "not initialized")
    return uint64_t((unsigned __int128)(uint64_t(c.QuadPart) - s.start) * s.mul >> 32) + kGuestTimebaseHz;
}

// A truly short sleep (~0.2 ms) with a high-resolution timer (Windows 10 1803+); otherwise Sleep(1)
static void ShortHostSleep()
{
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -2000;   // 200 us (100 ns units, relative)
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(1);
}

// Sleep(0) in a loop: on the 360 the game's worker threads wait for tasks like this, each on its own hardware thread.
// On PC this turned into yielding at 100% CPU on 4 threads, stealing the processor from the game and the GPU. Adaptive wait:
// the first consecutive calls only yield (fast response to a new task); if the thread keeps polling for
// more than ~1 ms, it starts sleeping 0.2 ms per call until it does work again.
void GuestYield()
{
    thread_local int64_t last = 0;
    thread_local int64_t pollingSince = 0;
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return int64_t(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t now = c.QuadPart;
    if (now - last > freq / 10000)   // more than 100 us since the last call: the thread did some work
        pollingSince = now;
    if (now - pollingSince < freq / 1000)
        SwitchToThread();
    else
        ShortHostSleep();
    QueryPerformanceCounter(&c);
    last = c.QuadPart;
}

KERNEL_IMPORT(KeQueryPerformanceFrequency)  { ctx.r3.u64 = kGuestTimebaseHz; }

KERNEL_IMPORT(KeQuerySystemTime)
{
    using namespace std::chrono;
    // FILETIME: 100 ns units since 1601-01-01
    const uint64_t t = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ull;
    W64(ctx.r3.u32, t);
}

// ---- Strings ----
KERNEL_IMPORT(XexCheckExecutablePrivilege)  { ctx.r3.u64 = 0; }
KERNEL_IMPORT(XamGetSystemVersion)          { ctx.r3.u64 = 0x20DC8000; }
KERNEL_IMPORT(NetDll_WSAStartup)            { ctx.r3.u64 = 0; }
KERNEL_IMPORT(FscSetCacheElementCount)      { ctx.r3.u64 = 0; }
KERNEL_IMPORT(ExRegisterTitleTerminateNotification)  { }
KERNEL_IMPORT(KiApcNormalRoutineNop)        { }
KERNEL_IMPORT(XamLoaderGetLaunchData)       { ctx.r3.u64 = ERROR_NOT_FOUND_; }
KERNEL_IMPORT(XamLoaderSetLaunchData)       { ctx.r3.u64 = 0; }
KERNEL_IMPORT(XamLoaderTerminateTitle)      { fprintf(stderr, "[xam] XamLoaderTerminateTitle\n"); fflush(stderr); exit(0); }

// ---- Configuration / region / language ----
KERNEL_IMPORT(XGetLanguage)    { ctx.r3.u64 = 1; }        // English
KERNEL_IMPORT(XGetGameRegion)  { ctx.r3.u64 = 0x00FF; }   // NTSC-U
KERNEL_IMPORT(XGetAVPack)      { ctx.r3.u64 = 3; }        // HDMI

KERNEL_IMPORT(ExGetXConfigSetting)
{
    // (category, setting, buffer*, bufferSize, requiredSize*)
    const uint32_t cat = ctx.r3.u32, setting = ctx.r4.u32, buf = ctx.r5.u32, size = ctx.r6.u32, req = ctx.r7.u32;
    uint32_t value = 0;
    if (cat == 3 && setting == 9)    // user language
        value = 1;
    if (cat == 3 && setting == 0xA)  // video flags (widescreen/HD)
        value = 0x00060000;
    if (buf && size)
    {
        for (uint32_t i = 0; i < size; i++)
            W8(buf + i, 0);
        if (size >= 4)
            W32(buf, value);
        else
            W8(buf + size - 1, uint8_t(value));
    }
    if (req)
        W32(req, size);
    ctx.r3.u64 = 0;
}

// ---- Basic video (X_VIDEO_MODE structure) ----
static void FillVideoMode(uint32_t p)
{
    for (uint32_t i = 0; i < 0x30; i += 4)
        W32(p + i, 0);
    W32(p + 0x00, 1280);
    W32(p + 0x04, 720);
    W32(p + 0x08, 0);          // interlaced
    W32(p + 0x0C, 1);          // widescreen
    W32(p + 0x10, 1);          // HD
    const float refresh = 60.0f;
    uint32_t bits;
    memcpy(&bits, &refresh, 4);
    W32(p + 0x14, bits);
    W32(p + 0x18, 1);          // NTSC-M
    W32(p + 0x1C, 0x8A);       // pixel rate (D3D divides by it)
    W32(p + 0x20, 0x01);       // flag widescreen
}
KERNEL_IMPORT(XGetVideoMode)    { FillVideoMode(ctx.r3.u32); ctx.r3.u64 = 0; }
KERNEL_IMPORT(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }
KERNEL_IMPORT(VdQueryRealVideoMode) { FillVideoMode(ctx.r3.u32); }

// ---- Input (controller) ----
KERNEL_IMPORT(XamInputGetCapabilities)
{
    // (userIndex, flags, caps*)
    const uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (user != 0 && user != 0xFF)
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
        return;
    }
    for (uint32_t i = 0; i < 0x14; i += 4)
        W32(caps + i, 0);
    W8 (caps + 0, 1);          // type: gamepad
    W8 (caps + 1, 1);          // subtype: gamepad
    W16(caps + 4, 0xF3FF);     // buttons
    W8 (caps + 6, 0xFF);       // triggers
    W8 (caps + 7, 0xFF);
    W16(caps + 8,  0x7FFF);    // sticks
    W16(caps + 10, 0x7FFF);
    W16(caps + 12, 0x7FFF);
    W16(caps + 14, 0x7FFF);
    W16(caps + 16, 0xFFFF);    // vibration
    W16(caps + 18, 0xFFFF);
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XamInputGetState)
{
    // (userIndex, flags, state*): keyboard + XInput (input.cpp)
    static std::atomic<uint32_t> packet{ 1 };
    uint32_t user = ctx.r3.u32;
    const uint32_t state = ctx.r5.u32;
    if (user == 0xFF)
        user = 0;
    HostPadState pad;
    if (user > 3 || !HostInputGetState(user, pad))
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
        return;
    }
    W32(state + 0, packet++);
    W16(state + 4, pad.buttons);
    W8 (state + 6, pad.leftTrigger);
    W8 (state + 7, pad.rightTrigger);
    W16(state + 8, uint16_t(pad.thumbLX));
    W16(state + 10, uint16_t(pad.thumbLY));
    W16(state + 12, uint16_t(pad.thumbRX));
    W16(state + 14, uint16_t(pad.thumbRY));
    ctx.r3.u64 = 0;
}

KERNEL_IMPORT(XamInputSetState)
{
    // (userIndex, flags, vibration* { u16 left, u16 right })
    uint32_t user = ctx.r3.u32 == 0xFF ? 0 : ctx.r3.u32;
    if (ctx.r5.u32 && user < 4)
        HostInputSetVibration(user, R16(ctx.r5.u32), R16(ctx.r5.u32 + 2));
    ctx.r3.u64 = 0;
}

// User, saves, notifications and system UI: kernel_xam.cpp

KERNEL_IMPORT(XamGetExecutionId)  { ctx.r3.u64 = ERROR_NOT_FOUND_; }

// ---- XMsg (system apps) ----
// 0xFA = XMP (system music player). 0x70009 = XMPGetStatus(client, state ptr)
KERNEL_IMPORT(XMsgInProcessCall)
{
    const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, buf = ctx.r5.u32;
    if (app == 0xFA && msg == 0x00070009 && buf)
    {
        const uint32_t statePtr = R32(buf + 4);
        if (statePtr)
            W32(statePtr, 0);   // 0 = stopped (the game plays its own music)
        ctx.r3.u64 = 0;
        return;
    }
    static std::atomic<int> logged{ 0 };
    if (logged++ < 10)
        fprintf(stderr, "[xmsg] app=%X msg=%08X buf=%08X (ignored)\n", app, msg, buf);
    ctx.r3.u64 = 0;
}
