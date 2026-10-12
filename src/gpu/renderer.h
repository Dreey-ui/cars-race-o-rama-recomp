#pragma once
#include <cstdint>

// Backend grafico D3D12: recebe o estado da GPU Xenos (registradores + pacotes PM4 do processador de comandos)
// e desenha no host. Os shaders do jogo sao traduzidos pelo XenosRecomp (gpu/shaders.cpp).

bool RendererInit(bool vsync, uint32_t* registers, const uint8_t* image, size_t imageSize);
bool RendererEnabled();

// Chamadas da thread do processador de comandos (kernel_gpu.cpp)
void RendererSetShader(bool pixel, const uint8_t* microcode, uint32_t sizeDwords);

struct DrawCall
{
    uint32_t primType;
    uint32_t numIndices;
    bool indexed;
    uint32_t indexAddress;   // fisico
    bool index32;
    uint32_t indexEndian;
};
void RendererDraw(const DrawCall& draw);
void RendererSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height);

// O frontbuffer so existe na GPU do host; na memoria guest as paginas dele ficam protegidas depois de cada swap. Quando a
// CPU do jogo le (ex.: a tela de carregamento copia a tela atual como fundo), o handler de excecao chama isto: copia a imagem
// da GPU para a memoria guest (BGRA8 linear, como o resolve do Xenos com dest_swap) e libera as paginas.
// Retorna true se o endereco (guest) era do frontbuffer protegido.
bool RendererHandleFrontbufferAccess(uint32_t guestAddr);

// GetTickCount64 do ultimo swap (0 antes do primeiro): o detector de travada do main.cpp avisa quando a imagem para de andar
uint64_t RendererLastSwapTick();
