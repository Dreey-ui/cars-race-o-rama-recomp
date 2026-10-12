#pragma once
#include <cstdint>

// CPU write tracking on 4 KB pages of guest physical memory, for the GPU caches (vertex/index buffers):
// a watched page is PAGE_READONLY in the 3 physical views; the first CPU write raises an exception that marks it
// dirty and re-enables writes. Pages written often become "dynamic" for a while and are not watched (the cache
// takes the uncached path for them).

// Are all pages in the range watched and unwritten since they started being watched?
bool MemWatchIsClean(uint32_t phys, uint32_t size);
// Is any page in the range dynamic right now?
bool MemWatchIsDynamic(uint32_t phys, uint32_t size);
// Start watching the range (after copying the data). false if some page is dynamic (nothing is watched)
bool MemWatchProtect(uint32_t phys, uint32_t size);
// Exception handler: write (guestAddr = guest address in any view) to a watched page. true = handled
bool MemWatchHandleWriteFault(uint32_t guestAddr);
// Before a write that raises no exception (Windows ReadFile straight into guest memory): unprotect and mark dirty
void MemWatchInvalidate(uint32_t guestAddr, uint32_t size);
// Changes whenever any watched page is written: a cache that stored the value and sees the same one is still valid
uint32_t MemWatchEpoch();
// Frame counter (for the dynamic-page heuristic)
void MemWatchSetFrame(uint64_t frame);
// Statistics: write exceptions since the last call
uint32_t MemWatchTakeFaults();
