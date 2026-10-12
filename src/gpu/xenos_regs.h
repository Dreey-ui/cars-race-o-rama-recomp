// Xenos GPU registers used by the backend (indices from Xenia's register_table.inc)
#pragma once
#include <cstdint>
#include <cstring>

namespace xreg
{
constexpr uint32_t SCRATCH_UMSK = 0x01DC;
constexpr uint32_t SCRATCH_ADDR = 0x01DD;
constexpr uint32_t SCRATCH_REG0 = 0x0578;
constexpr uint32_t COHER_STATUS_HOST = 0x0A31;

constexpr uint32_t RB_SURFACE_INFO = 0x2000;
constexpr uint32_t RB_COLOR_INFO = 0x2001;
constexpr uint32_t RB_DEPTH_INFO = 0x2002;
constexpr uint32_t RB_COLOR1_INFO = 0x2003;
constexpr uint32_t RB_COLOR2_INFO = 0x2004;
constexpr uint32_t RB_COLOR3_INFO = 0x2005;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x200F;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100;
constexpr uint32_t VGT_MIN_VTX_INDX = 0x2101;
constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t RB_COLOR_MASK = 0x2104;
constexpr uint32_t RB_BLEND_RED = 0x2105;
constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C;
constexpr uint32_t RB_STENCILREFMASK = 0x210D;
constexpr uint32_t RB_ALPHA_REF = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;
constexpr uint32_t PA_CL_VPORT_XOFFSET = 0x2110;
constexpr uint32_t PA_CL_VPORT_YSCALE = 0x2111;
constexpr uint32_t PA_CL_VPORT_YOFFSET = 0x2112;
constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113;
constexpr uint32_t PA_CL_VPORT_ZOFFSET = 0x2114;
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
constexpr uint32_t VGT_DMA_BASE = 0x21FA;
constexpr uint32_t VGT_DMA_SIZE = 0x21FB;
constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;
constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202;
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
constexpr uint32_t RB_MODECONTROL = 0x2208;
constexpr uint32_t RB_BLENDCONTROL1 = 0x2209;
constexpr uint32_t RB_BLENDCONTROL2 = 0x220A;
constexpr uint32_t RB_BLENDCONTROL3 = 0x220B;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
constexpr uint32_t RB_COPY_CONTROL = 0x2318;
constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
constexpr uint32_t RB_COLOR_CLEAR_LO = 0x231F;
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380;
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381;

constexpr uint32_t ALU_CONSTANTS = 0x4000;    // 512 float4: VS 0..255, PS 256..511
constexpr uint32_t FETCH_CONSTANTS = 0x4800;  // 32 x 6 dwords (textures) = 96 x 2 dwords (vertices)
constexpr uint32_t BOOL_CONSTANTS = 0x4900;   // 8 dwords = 256 bits
constexpr uint32_t LOOP_CONSTANTS = 0x4908;   // 32 dwords

constexpr uint32_t kRegisterCount = 0x8000;
}

inline uint32_t Bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t SBits(uint32_t v, uint32_t lo, uint32_t n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }
inline float AsFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

// GPU physical addresses -> guest address (physical memory mapped at 0xA0000000)
inline uint32_t GpuPhysToGuest(uint32_t p) { return 0xA0000000u | (p & 0x1FFFFFFFu); }
