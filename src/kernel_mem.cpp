// Virtual/physical memory and kernel pools
#include "kernel.h"

constexpr uint32_t MEM_COMMIT  = 0x1000;
constexpr uint32_t MEM_RESERVE = 0x2000;
constexpr uint32_t MEM_RELEASE = 0x8000;

KERNEL_IMPORT(NtAllocateVirtualMemory)
{
    // (uint32_t* PBaseAddress, uint32_t* PRegionSize, AllocationType, Protect, DebugMemory)
    const uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    uint32_t addr = R32(pBase);
    const uint32_t size = R32(pSize);
    if (g_traceImports) fprintf(stderr, "[trace] NtAllocateVirtualMemory(addr=%08X size=%08X type=%08X prot=%08X)\n", addr, size, ctx.r5.u32, ctx.r6.u32);
    if (size == 0) { RetStatus(ctx, STATUS_INVALID_PARAMETER); return; }

    if (addr)
    {
        if (!g_virtualHeap.AllocFixed(addr, size))
        {
            // already reserved before: a commit over an existing range counts as success
            if (!g_virtualHeap.Contains(addr) || !g_mem.Commit(addr, size))
            {
                RetStatus(ctx, STATUS_NO_MEMORY);
                return;
            }
        }
    }
    else
    {
        addr = g_virtualHeap.Alloc(size, 0x10000);
        if (!addr) { RetStatus(ctx, STATUS_NO_MEMORY); return; }
        W32(pBase, addr);
    }
    W32(pSize, (size + 0xFFF) & ~0xFFFu);
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(NtFreeVirtualMemory)
{
    // (uint32_t* PBaseAddress, uint32_t* PRegionSize, FreeType)
    if (ctx.r5.u32 & MEM_RELEASE)
        g_virtualHeap.Free(R32(ctx.r3.u32));
    RetStatus(ctx, STATUS_SUCCESS);
}

KERNEL_IMPORT(MmAllocatePhysicalMemoryEx)
{
    // (Type, Size, Protect, MinAddress, MaxAddress, Alignment) -> address
    uint32_t align = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    ctx.r3.u64 = g_physicalHeap.Alloc(ctx.r4.u32, align);
    if (g_traceImports) fprintf(stderr, "[trace] MmAllocatePhysicalMemoryEx(size=%08X) -> %08X\n", ctx.r4.u32, ctx.r3.u32);
}

KERNEL_IMPORT(MmFreePhysicalMemory)  { g_physicalHeap.Free(ctx.r4.u32); }
KERNEL_IMPORT(MmGetPhysicalAddress)  { ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; }

KERNEL_IMPORT(ExAllocatePoolTypeWithTag)
{
    const uint32_t sz = ctx.r3.u32;
    ctx.r3.u64 = g_poolHeap.Alloc(sz, 0x1000);
    if (g_traceImports) fprintf(stderr, "[trace] ExAllocatePool(size=%08X) -> %08X\n", sz, ctx.r3.u32);
}

KERNEL_IMPORT(ExFreePool)  { g_poolHeap.Free(ctx.r3.u32); }
