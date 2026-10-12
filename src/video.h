#pragma once
#include <cstdint>

// Profiler for the game's D3D functions (d3d_profile.cpp)
void D3DProfileStart();

// GPU registers (kernel_gpu.cpp)
uint32_t* GpuRegisters();
