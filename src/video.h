#pragma once
#include <cstdint>

// Profiler das funcoes do D3D do jogo (d3d_profile.cpp)
void D3DProfileStart();

// Registradores da GPU (kernel_gpu.cpp)
uint32_t* GpuRegisters();
