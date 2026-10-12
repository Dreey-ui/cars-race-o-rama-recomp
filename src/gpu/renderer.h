#pragma once
#include <cstdint>

// D3D12 graphics backend: receives the Xenos GPU state (registers + PM4 packets from the command processor)
// and draws on the host. The game's shaders are translated by XenosRecomp (gpu/shaders.cpp).

bool RendererInit(bool vsync, uint32_t* registers, const uint8_t* image, size_t imageSize);
bool RendererEnabled();

// Calls from the command processor thread (kernel_gpu.cpp)
void RendererSetShader(bool pixel, const uint8_t* microcode, uint32_t sizeDwords);

struct DrawCall
{
    uint32_t primType;
    uint32_t numIndices;
    bool indexed;
    uint32_t indexAddress;   // physical
    bool index32;
    uint32_t indexEndian;
};
void RendererDraw(const DrawCall& draw);
void RendererSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height);

// The frontbuffer only exists on the host GPU; in guest memory its pages are protected after each swap. When the
// game's CPU reads it (e.g. the loading screen copies the current screen as its background), the exception handler calls
// this: copies the image from the GPU to guest memory (linear BGRA8, like the Xenos resolve with dest_swap) and unprotects the pages.
// Returns true if the (guest) address belonged to the protected frontbuffer.
bool RendererHandleFrontbufferAccess(uint32_t guestAddr);

// GetTickCount64 of the last swap (0 before the first): main.cpp's hang detector warns when the picture stops moving
uint64_t RendererLastSwapTick();
