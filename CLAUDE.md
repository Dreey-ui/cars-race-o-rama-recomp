# Cars Race-O-Rama (Xbox 360) — static recompilation for PC

Project: static recompilation of the Xbox 360 game "Cars Race-O-Rama" (title ID 54510848, original PE `Cars3_final_disc.exe`,
XDK 8276) with hedge-dev/XenonRecomp, in the style of UnleashedRecomp. The owner (Drey) speaks Portuguese: answer him in
Portuguese. The project itself (code, comments, logs, docs, tool options) is in ENGLISH because other people help with it;
keep new code and log strings in English.
Environment: Windows, Visual Studio 2026 Insiders (generator `Visual Studio 18 2026`, `-T ClangCL`, Clang 22.1.3), PowerShell.
Repository: https://github.com/Dreey-ui/cars-race-o-rama-recomp (public; commits use the GitHub noreply e-mail
340763992+Dreey-ui@users.noreply.github.com). Never commit game files, the generated ppc/ code or decompiled scripts.

## Paths on the user's machine
- `C:\DEB\cars` — work files: `default_dec.xex` (decrypted/decompressed XEX), `basefile.exe` (raw image, NOT a PE),
  `cars.toml`, `cars_switch_tables.toml`, `xextool.exe`, `XenonRecomp.exe`, `XenonAnalyse.exe`, `ppc\` folder (generated code, 78 `ppc_recomp.*.cpp`
  + `ppc_func_mapping.cpp` + `ppc_config.h` + `ppc_recomp_shared.h`). Copies of the .toml files and the recompiler patch are in `recomp/`.
- `C:\Users\Drey\XenonRecomp` — clone with submodules (XenonUtils, thirdparty/disasm and thirdparty/simde are used).
- `C:\Users\Drey\XenosRecomp` — shader translator (shader_recompiler.cpp is compiled inside the runtime).
- `C:\DEB\cars-runtime` — THIS project (host runtime). Build: `cmake -S . -B build -G "Visual Studio 18 2026" -T ClangCL -DXENONRECOMP_DIR=C:/Users/Drey/XenonRecomp -DPPC_DIR=C:/DEB/cars/ppc` and `cmake --build build --config Release`.
- Game folder (extracted ISO): `C:\Games\Cars Race-O-Rama Recomp` (C, E, extras, F, G, I, Res, S, config.ini; no default.xex).
- Running from the build: `.\build\Release\CarsRecomp.exe C:\DEB\cars\default_dec.xex "<game folder>" --trace *> run.log` (the quotes are required: the path has spaces).
  The installed game is `<game folder>\Cars Race-O-Rama.exe` (tools/deploy.ps1), run with no arguments.
- `C:\DEB\ref\xenia` — Xenia source, the reference for kernel/GPU/XAM/APU behaviour.

## Recompiler state (done)
- XenonRecomp ran clean (100%, no `// ERROR`) with `cars.toml`: `file_path="default_dec.xex"`, `out_directory_path="ppc"`,
  `switch_table_file_path="cars_switch_tables.toml"`, all optimizations false.
  Addresses: savegprlr_14 0x82506410, restgprlr_14 0x82506460, savefpr_14 0x82506EC0, restfpr_14 0x82506F0C,
  savevmx_14 0x82507470, restvmx_14 0x82507708, savevmx_64 0x82507504, restvmx_64 0x8250779C, setjmp 0x825071A0, longjmp 0x825060F0,
  `invalid_instructions=[{data=0,size=4}]`, and 18 overrides in `functions` (real sizes from the .pdata).
- XenonAnalyse found NO jump tables (the 2009 compiler reorders instructions). `cars_switch_tables.toml` (140 tables: 51 abs, 26 computed, 23 byte, 40 short)
  came from a custom scanner over `basefile.exe`; two hand-written entries (0x823FD84C, 0x824049D0) have `default = 0`.
- XenonRecomp's `recompiler.cpp` is PATCHED (`recomp/xenonrecomp_recompiler.patch`, base commit ddd128bc) to support: ADDC, ADDME, SUBFZE, EQV, BDZF, DCBST, LHBRX,
  LHZU, STHU, LFSU, STFSU, LFDU, STFDU, LBZUX, LWZUX, STBUX, STHUX, STDUX, LFSUX, STFSUX, LFDUX, FRSQRTE, VADDSWS, VSUBSHS, VPKSWSS128, VSEL128, VCMPBFP(128).
  If you regenerate `ppc/`, use the patched recompiler.
- Still NOT implemented in XenonRecomp: `vpkd3d128` (mode 3) and `vupkd3d128` (mode 12) emit `__builtin_debugtrap()` (ppc_recomp.30.cpp and ppc_recomp.35.cpp). Implement them if they are hit.

## Memory model / runtime conventions
- 4 GB guest block reserved (`g_mem.base`, VirtualAlloc MEM_RESERVE), pages committed on demand (`g_mem.Commit`). Big-endian accessors R8..R64 / W8..W64 in `runtime.h`.
- Image at 0x82000000 (size 0x7D0000); code at 0x82090000; function pointer table at `base + 0x82000000 + 0x7D0000 + (guest - PPC_CODE_BASE)*2`
  (8 bytes per 4-byte instruction; macro `PPC_LOOKUP_FUNC`). Filled from `PPCFuncMappings[]`.
- Guest heaps: virtual 0x20000000–0x7E000000, pool 0x90000000–0xA0000000, physical 0xA0000000–0xC0000000.
- Kernel imports = `__imp__Name` functions with the `KERNEL_IMPORT(Name)` macro (= `PPC_FUNC(__imp__Name)`). `stubs_gen.cpp` has 180 WEAK stubs (`__attribute__((weak))`)
  that only log `[unimplemented import]` and return 0; defining the real function in another .cpp overrides the stub.
- Arguments: r3..r10; 9th argument onwards on the stack (r1+0x54...). Return in r3. Use `RetStatus(ctx, status)` for NTSTATUS.
- PCR in r13: +0x100 current KTHREAD, +0x10C CPU, +0x150 KPRCB; KTHREAD +0x58/+0x5C stack limit/base, +0x14C thread id. Threads: `KSetupGuestThread`, `KInitContext`, `RunGuestMain`.
- Kernel objects: `KObject` (event, semaphore, mutant, thread, file) with a handle table (starting at 0x80000100) and a guest object table; `KWait` with a global mutex+condvar.

## Runtime files (src/)
main.cpp (XEX loading, mapping, crash/auto-map handler, monitor thread, `--build-folder`/`--build-loose`), memory.cpp, thread.cpp, kobjects.cpp,
kernel_sync.cpp (critical sections, TLS, events, semaphores, mutants, threads, waits), kernel_mem.cpp (NtAllocateVirtualMemory, MmAllocatePhysicalMemoryEx, ExAllocatePool...),
kernel_io.cpp (Nt*File, `game:\`/`\Device\Cdrom0\`/`d:\` → game folder; NtQueryDirectoryFile; mods), kernel_xam.cpp (profile, saves, XamShow*UI, notifications),
kernel_misc.cpp (time, config, XGetVideoMode, input, XamUser* leftovers), kernel_gpu.cpp (PM4 command processor, vblank), kernel_audio.cpp, xma.cpp,
input.cpp, settings.cpp, session_log.cpp, scene_assets.cpp, game_hooks.cpp, host_profiler.cpp, d3d_profile*.cpp, gpu/*, stubs_gen.cpp.

## Bugs already fixed (do not repeat)
- The XEX `.reloc` section goes past the end of the buffer → crash in memcpy; it is ignored in main.cpp. Section copies are clamped to the end of the image.
- Section names printed with garbage (not NUL-terminated): harmless.
- PowerShell: pasted `#` comments do not copy files; use explicit `Copy-Item`/`Expand-Archive`.

## State (12/10/2026)
The game is fully playable at 60 fps on the D3D12 backend: Bink intros, loading screens, 3D title screen, attract mode, menus,
story mode, races and the open world (HUB), with audio (XMA), keyboard/controller, saves and mods.
Older open issues (from 09/10/2026, not re-checked since):
- Races: horizontal stripes/stretching on the car (stretched geometry), maybe a vertex fetch/index error.
- 37 pre-existing `// ERROR` in the generated code (ppc_recomp.33/34/40/41/42/52) - not investigated.
- Achievements and Live are not implemented (the achievement enumerator is empty).

## GPU architecture (src/gpu + kernel_gpu.cpp) - Xenia style + XenosRecomp shaders
- kernel_gpu.cpp: PM4 processor with Xenia's semantics (pm4_command_processor_implement.h in C:\DEB\ref\xenia). Registers in g_regs[0x8000].
  Handles SET_CONSTANT/LOAD_ALU_CONSTANT/SET_SHADER_CONSTANTS, IM_LOAD(_IMMEDIATE), DRAW_INDX(_2), packet predication (bit 0, SET_BIN_MASK/SELECT
  0x50/0x51/0x60-0x63) used by predicated tiling, scratch writeback, EVENT_WRITE_SHD/EXT(0x5A), INTERRUPT (callback r3=source, r4=data, CPU in PCR+0x10C).
  VdSwap writes an XE_SWAP packet (0x64, 'SWAP', frontbuffer, w, h) like Xenia. The CP spins with a ~4 ms yield after work (timeBeginPeriod(1)):
  the D3D waits for fences in sequence (sub_822BBC38) and high latency made the loading take ~70 s.
- Simulated MMIO: 0x1951=1 (vblank), 0x1961=1280x720, RB_EDRAM_TIMING etc. (without it the callback never sees vblank and the CP waits on [0x40B000]).
- VdIsHSIOTrainingSucceeded MUST return 1 (otherwise the D3D goes into degraded mode and stops kicking the ring). VdQueryVideoMode needs pixel_rate 0x8A at +0x1C (the D3D divides by it).
- Physical memory: 512 MB section mapped 3x (0xA0000000, 0xC0000000, 0xE0000000) in memory.cpp.
- gpu/shaders.cpp: indexes containers (.vsh/.psh from the ISO + a scan of the XEX and of guest memory when a shader is unknown) by microcode hash;
  translates with XenosRecomp's shader_recompiler.cpp (compiled inside the runtime with src/gpu/xenos_recomp_pch.h) and compiles
  with dxcompiler.dll at runtime (cache in build/Release/shader_cache, version kShaderGenVersion). HLSL post-processing: XeTransformPos (Xenos
  viewport -> host, VTE/window offset/pixel center, vtx_xy_fmt), alpha test with function, integer inputs -> int4, oPos=0, empty assignments removed.
  - VS: the D3D REWRITES the vfetches with the vertex declaration -> lookup ignoring vfetch slots, creating a variant with the live microcode.
  - Microcode without a container (UI/internal D3D, e.g. 0x82050DC8): SYNTHESIZED container (constants C[256], TEXCOORDi inputs, interpolators
    linked by index to the PS) - ShadersLinkRawVertexShader compiles one per PS.
- gpu/renderer.cpp: bindless D3D12 (root sig = b0/b1/b2 space4 + tables t0 space0..2 and s0 space3, like XenosRecomp's HLSL). Host RT per
  (EDRAM base, pitch, DXGI format) - Xenos formats with the same bits (2/10, 0/1) share an RT. Rect lists via a geometry shader; quad/fan -> indices.
  Resolve: copies RT -> texture by address; destinations inside an existing texture become an offset in 32x32 macro tiles (tiling).
  Swap: blit of the resolved frontbuffer texture. textures.cpp: Xenia's 2D untiling, endian by address XOR, formats -> DXGI.
- Others: kernel_audio.cpp (XAudio render driver callback every 5.333 ms - without it the audio engine does not advance and loading takes long),
  input.cpp (XInput + keyboard), user APCs (NtReadFile with a completion routine, alertable waits).

## Performance / timing (10/10/2026) - do NOT regress
- mftb: XenonRecomp emits `__rdtsc()` (PC TSC, GHz). `src/guest_timebase.h` is force-included (/FI, CMakeLists) in all of ppc/
  and replaces it with `GuestTimebase()` (QPC scaled to 50 MHz = KeQueryPerformanceFrequency). Without it the game runs too fast.
- Thread processor (PCR+0x10C and KTHREAD+0xBF): comes from the high byte of the ExCreateThread flags (mask) or from KeSetAffinityThread
  (lowest bit); with no mask it inherits the creating thread's (like Xenia). XAudio2 (sub_822D3AF8) and the jobs use this number in
  busy-wait barriers: with wrong numbers (it used to be threadId % 6) the audio threads spun at 100% on 2 cores.
- KeDelayExecutionThread(0) (Sleep(0) in a loop in the 4 job threads 0x820B02F8/0x820B0398): GuestYield (kernel_misc.cpp) yields for
  1 ms and then sleeps 0.2 ms (high resolution timer). Before, 4 cores sat at 95% and the GPU thread starved.
- Shaders: DXC compilation in worker threads (the draw is skipped until it is ready; DXIL cached on disk loads immediately) and the
  container scan of guest memory runs in its own thread, only over allocated ranges (GuestAllocatedRanges) with a SIMD search.
- Renderer: texture untiling with pshufb straight into the upload buffer; VB with pshufb + stream; VS/PS constants only re-copied when
  written (g_aluDirty in kernel_gpu.cpp); redundant D3D12 state filtered (BoundState). Result: 30 fps up to ~175k draws/s.
- The game's main render thread (0x820A8440) spins inside the D3D Swap (sub_822BBC38/sub_822C2670) waiting for the GPU fence.
- Next performance steps: async PSOs; texture hashing without re-hash per frame.

## Audio (10/10/2026)
- kernel_audio.cpp: XAudioSubmitRenderDriverFrame (6 channels x 256 samples, planar BE float, 48 kHz) -> a 5.1 voice on Windows XAudio2
  (automatic downmix); the driver callback follows the device queue (target 6 frames). CARS_AUDIO_DUMP=1 writes audio_dump.raw
  (tools/audio_check.py: level/ZCR per second + .wav).
- xma.cpp: XMA hardware. Table of 320 contexts (Xenia's XMA_CONTEXT_DATA) in physical memory; address at 0x7FEA1800 in LITTLE-endian
  (the game reads it with lwbrx), 0x7FEA1818 = 0. The game's XAudio2 does not use XMA* beyond Create/Release: it writes registers as index*4
  with stwbrx (Kick 0x7FEA1940 in sub_822D55E8, Lock 0x7FEA1A40 in sub_822D5340, Clear 0x7FEA1A80 in sub_822D5120). A hook on the kick
  decodes right away (like Xenia) with FFmpeg's xma2, one 2 KB packet at a time; 16-bit BE interleaved PCM into the output ring.
  Loops are approximated per packet (there can be a click at the loop point). FFmpeg 8.1 LGPL in third_party/ffmpeg (DLLs copied by the build).
- tools/gen_d3d_profile.ps1 excludes functions that already have PPC_FUNC(sub_X) in another file (otherwise: duplicate symbol at link time).

## Installing into the game folder (10/10/2026)
- The decrypted XEX is EMBEDDED in the exe (RCDATA resource 101 generated from src/xex_image.rc.in, CMake CARS_XEX_PATH). The exe uses the
  Windows subsystem (WIN32_EXECUTABLE + /ENTRY:mainCRTStartup): with no arguments it uses the embedded XEX and game folder = exe folder;
  the log goes to logs\<date>\game.log (see "Session log"); fatal errors in a MessageBox. The old arguments (<xex> <folder>) still work.
- tools/deploy.ps1 installs "<game>\Cars Race-O-Rama.exe" + DLLs (dxcompiler, dxil, avcodec-62, avutil-60, swresample-6) +
  shader_cache + mods\ (never deletes existing mods; copies dist\mods\README.txt) + lua_src\. Run it after each build (close the game first).
- Tested: the game never opens the ISO's default.xex (2447 file opens with --trace).

## Mods and Lua scripts (10/10/2026)
- kernel_io.cpp: <exe folder>\mods (or CARS_MODS) mirrors the game folder; a file there replaces the original on read opens
  (DoOpen and NtQueryFullAttributesFile). A text "X.lua" replaces "X.lua.bin": the runtime builds a temp file with a 4-byte BE
  size + source (the game's Lua parser is in the exe; the original FE_FrontEnd.lua.bin is already text). Log: [mods].
  mods\...\X.hidden hides the game file X (HiddenByMods; the game sees it as missing).
- Scripts: 324 *.lua.bin = 4-byte BE size + Lua 5.1 bytecode (4-byte float lua_Number, little-endian flag), modified Lua
  (accepts !=). lua_src\ = all of them decompiled (tools/decompile_lua.ps1: unluac in third_party\unluac + portable JRE in third_party\jre;
  tools/fix_lua_goto.py rewrites the 4 goto spots unluac generates). Validated: all 324 in mods\ run up to the demo without errors.
- game_hooks.cpp: sub_82284D80 = luaX_lexerror -> logs "[lua] syntax error" (LexState +4 line, +44 TString* name, text at +16).
- tools/mod_bisect.ps1 -All | -Files "a.lua;b.lua": runs with those mods and says whether the 3D demo still starts (no input).
- F12 in game: session log mark (below); includes the trace of the next frame in mark_NN\frame.txt.
- Loading screens (FIXED): the game draws load_screen_N_w.dds (DXT5) for 1 frame and the render thread (0x820A8440,
  lr 823DD0AC called from 820A4198) READS the frontbuffer (physical 00416000, tiled, k_8_8_8_8 BGRA) with the CPU to use as background.
  Since resolves only exist on the host GPU, it was black. renderer.cpp: after each swap the frontbuffer pages become
  PAGE_NOACCESS in the 3 physical views; the CrashHandler calls RendererHandleFrontbufferAccess -> copies the resolve texture (own command
  list/fence) to guest memory in tiled layout (TiledOffset2D) BGRA8 and unlocks. ~13 reads in 2 min (only during loading).
- R/B swapped in render-to-texture (FIXED): almost every resolve has RB_COPY_DEST_INFO.copy_dest_swap (bit 24); the Xenos
  writes BGRA and the fetch uses swizzle A0A (ZYX) to swap back. The host copy is direct, so HostTexture::rbSwap swaps X<->Z in
  the SRV mapping (GetSrv); frontbuffer presentation uses GetSrv(..., false). It affected the HUD (orange speedometer came out cyan).
- 60 fps (10/10/2026): the D3D queues each flip in sub_822C4708(value: fb | interval<<8 | tearing limit); the game uses
  interval 2 (30 fps). A hook (game_hooks.cpp) changes it to 1 and calls GpuImmediateVblank (kernel_gpu.cpp: vblank callback
  right away, with a reentrant mutex and preserving the PCR CPU) -- without it the GPU waited on flag 00410004 (flip pending) every
  frame and lost vblanks. Pacing comes from the host vsync + LimitFrameRate (renderer.cpp; also with a hidden window).
  The game logic is time based (QPC): correct speed at 60 fps (checked with the race timer). Config: "<exe>.ini" ([Video] FPS=60|30,
  VSync=1|0), src/settings.cpp. CARS_VBLANK_HZ, CARS_LOG_WAITS, CARS_LOG_SLOW_WAITS=swap: diagnostics.
- VB/IB cache across frames (gpu/memwatch.cpp + renderer.cpp): converted copies in a persistent pool (upload heap, 1 GB),
  guest pages PAGE_READONLY in the 3 views; a CPU write -> exception (main.cpp CrashHandler) marks it dirty; a global epoch avoids
  checking pages per draw; pages rewritten within < 30 frames become dynamic for 600 frames. NtReadFile calls MemWatchInvalidate
  (ReadFile does not raise an exception). CARS_NO_BUFFER_CACHE=1 turns it off. vb went from ~200 to ~80 ms/s. tools/fps_summary.ps1 summarizes the log.
- tools/dds2png.py decodes DXT1/3/5 to check colors.
- Diagnostics: CARS_LOG_FULLSCREEN=1 (1280x720 textures used per frame), CARS_WATCH=end,size (non-zero bytes every 10 frames),
  CARS_TRAP=physical,size + CARS_TRAP_DELAY=ms (logs the first CPU access to a physical memory range, with the guest stack).

## Extracted scene folders (11/10/2026) - older format, superseded by loose scenes (below)
- tools/scenes_extract.py writes, for the 30 packages in C\Scenes: mods\...\X.x360.res\ (inner files, ZLIBME decompressed,
  _index.txt = order) and mods\...\X.x360.str\ (each texture as a PC .dds at its game path + _data\XXXXXXXX.bin with the rest of the
  stream: geometry and tile padding + _index.txt "size / texture <fetch 6 dwords> <size> <positions> <file> / data <pos> <file>");
  it checks that the folder rebuilds the stream byte for byte (scenes_textures.build_from_folder). Folders from older builds have
  _indice.txt/_dados and Portuguese keywords (tamanho/textura/dados): both readers (Python and C++) still accept them.
  Backup of the originals in "<game>\_backup_original\C\Scenes".
- Runtime: src/scene_assets.cpp builds each folder ONCE per run into a temp file (FILE_FLAG_DELETE_ON_CLOSE, reopened on each
  NtCreateFile of the game): uncompressed .res v3 (flags 0) and a raw stream (zeroes, copies _data, writes each .dds's blocks with
  Xenos tiling/endian/swizzle; accepts DDS with a DX10 header). kernel_io.cpp: FindModOverride recognizes X.res / X.str folders;
  streams set rawStream (RawStreamRegister + uncompress hook).
  `CarsRecomp.exe --build-folder <folder> <out>` builds a folder without starting the game (test: equal to the raw reference .str).
- .res v3 (reader sub_821C3580 header/directory, sub_821C1AA0 read): +0 version, +4 header size after +8,
  +8 flags (0x100 zlib blocks of 0x6000, 0x40 u16 size table, 0x200 sizes-1, 0x80/0x400 XOR), directory
  {u32 n, u32 directory size, n x {u32 name length, name, u32 offset, u32 size}}, data after (LE). Without 0x100 the
  game reads each file straight from disk. Inner "ZLIBME" files (+8 comp BE, +12 decomp BE, zlib at +16): the loader
  (sub_821FF8B8) accepts them uncompressed. .xnm.bpb = material (binary INI "BPBh": shaders, texture names).
- .str: stream records in the .toc (BE): {position in the .str, total, decomp A, decomp B, comp A, comp B, blocks A,
  blocks B, size table}; HUB has 72 (zones), ShadowMap ~100. Reader sub_820B0398: 0/0 blocks = reads directly;
  otherwise reads each block into a 0x20000 buffer (0x8259FA00, 4 slots) and the job sub_820B02F8 calls uncompress
  sub_822702F8(dst, src, len). The "raw" mod stream has the decompressed blocks aligned to 0x1000 and the .toc records
  rewritten (the .toc in the .res folder is already the rewritten one). The MiniMap .str is already raw on disk (no records): the original stays.
- Textures: the .soi has {name[0x100], ..., 360 D3D header with BaseFlush/MipFlush FFFF0000 FFFF0000 at +0x124 and the
  6-dword fetch constant at +0x12C}; the .toc has {name[0x100], 0, index, index, position in part A, size, 1} right
  after the zone's stream record. Base address 0; mip_address relative to the texture; 1945 textures (DXT1/3/5, ARGB,
  2 cubes). Mip layout = Xenia's GetGuestTextureLayout (tools/scenes_textures.py and scene_assets.cpp match).
  Mip tiles have image leftovers in the padding: they stay in _data (that is why the rebuild is exact).
- Models (12/10/2026): the .toc has one record per model {name[0x100], 0, FFFFFFFF, index, block position in the stream,
  size, 0}; the .soi has the "xgs" header of each model (= .xng without the data; the internal name at xgs+0x0C matches the
  end of the .toc name). .xng/.xgs format (BE): 'xng\0', u32 version 5, u32 objects; per object name[0x80], 4x4 matrix,
  bounds (7 floats), FFFFFFFF (+0xE8), u32 materials, materials x name[0x40], u8 LODs(+3), per LOD f32 distance + u32
  submeshes, per submesh {u32 material, u32 streams, u8 n, u8, u8 (0 = data in the file, 1 = in the stream), u8, u16,
  n x f32, u16 vertices, u16 indices} and in the .xng the data right after: u16 strip indices (FFFF restarts) aligned to 4 +
  non-interleaved streams in the order position 0x01 (3f), normal 0x02 (3f), color 0x08 (ARGB), UV0 0x04 (2f), UV1 0x10,
  UV2 0x4000, tangent 0x20 (3f). In the stream the model block is the concatenation of the submesh data.
  Vertices in object space: the game places them with the .scn [ModelN] (Position, LookVector = Z, UpVector = Y; both
  vectors carry the SCALE in their length).
- Zip from the user's friend (Downloads\Scenes.zip, extracted to "<game>\cenas_soltas"): SOURCE C\Scenes as loose
  files (9494 .xng, 7701 PC .dds, text .xnm, .gol collision, .pco physics, COMPLETE .scn with [ModelN]) for the
  game's scenes + RS/TF/OV (PS2), PC_*, RR_*, CD, *_Night. It matches the game (textures identical byte for byte; all
  FE models). tools/xng2obj.py <folder> [--only X]: <scene>\_obj\models\*.obj, <scene>\_obj\<scene>.obj (scene assembled
  from the .scn) and materials.mtl (map_Kd = .dds via TextureMap_1 of the .xnm). Checked: textured render equal to the menu.
- The retail game does NOT load scenes from loose files: without the .str it stays on LOADING (tested with mods\...\X.hidden, which
  hides a game file: kernel_io.cpp HiddenByMods). C\Debug\config.txt (EnableAllResFiles, ResourceFileOverride,
  BuildResourceFiles...) is the developer config, but the retail exe does not read that file (the string is not there).
- F11 = model capture through the GPU (gpu/model_capture.cpp): drawn meshes that come from an extracted scene's stream ->
  mods\...\X.x360.str\_models (object space, with the draw layout in comments). It served as the reference to decode
  the .xgs; with the zip it is redundant. CARS_MODELS_AT=s1,s2 turns it on/saves without a key (tests).
- Full decompilation (12/10/2026): `python -I tools/scenes_decompile.py [--only CR_01,HUB] [--compare "<game>\cenas_soltas"]`
  -> "<game>\extracted_scenes\<Scene>\..." in the zip's layout (Geo\*.xng, Tex\*.dds PC + text *.xnm, .gol collisions,
  Eco/Anim/Skies/.scn/.got/.mcf/.tsf copied from C\Scenes, _package\ = .soi/.toc). It reads the extracted folders (--source; default
  <game>\_old_mod_folders) and builds the stream with `CarsRecomp.exe --build-folder`. Result: 5201 xng, 3453 dds, 1610 xnm, 800 gol.
  Mostly byte-identical to the zip; differences = the zip came from Hi-Octane (Mater-National): HUB has 32 different models,
  ~148 adapted materials, small mips of the zip's .dds with garbage/zero. One material stays .bpb (does not convert).
  - Resource type in the .toc (+0x114): 0 model, 1 texture, 2 collision (model/collision have FFFFFFFF at +0x104). Part A
    of the stream record = GPU data (textures, models); part B = CPU (collisions). Names "C\" or "c\" (CT_SM),
    spaces in the .toc that the xgs does not have ("wood catwalk01") -> compare without spaces.
  - Collision: header in the .soi 00 00 04 D2 'col\0', version 2, flag +0x0C (9 = in the stream, 0 = .gol), +0x48/+0x4C/+0x50 =
    vertices/nodes/triangles; name up to 0x400 bytes before. Blob in the stream: float4 vertices, nodes aligned to 32 (64 B {min4,
    max4, children, A, B, 0, pad}), 32 B triangles {normal4, d, i0 i1 i2, pad}. .gol = 0x58 header (flag 0) + float3
    vertices + u32 0 + 32 B nodes {A, min3, max3, children} + 22 B triangles {d, n3, idx3}.
  - BPB (binary INI, "BPBh" or "BPB\0"): header >IIHHIIII, 16 B records {hash, name | type (0 text, 2 int,
    3 float), suffix (ffff = none), count/first or value}; sections first.
  - MiniMap: stream without records; each piece has its own "Section_N" in the .toc ending in {position, total, total, 0} right
    before the name (texture position relative to it). CR_01 keeps the pieces as whole PC DDS in the .str. The pieces
    go bottom to top (row 0 = bottom).

## SCENES AS LOOSE FILES (12/10/2026) - what the game uses now
- mods\C\Scenes\<Scene> = copy of extracted_scenes (geo\*.xng, tex\*.dds + *.xnm, col\*.gol, Eco, Anim, .scn...) + _package\X.x360.soi/.toc
  (skeleton) + _package\X.files.txt (CRC32 of each file as extracted). When the game opens C\Scenes\...\X.x360.res/.str
  and <folder>\_package\X.x360.soi exists, scene_assets.cpp (BuildLoosePack) generates new .soi/.toc/.str from the files
  (kernel_io.cpp FindModOverride: isLoose; priority: file > old X.res/X.str folder > _package; builds before the English
  translation used "_pacote" and ".arquivos.txt": the user's folders were renamed on 12/10/2026). The .xnm.bpb are NOT put in the .res: the game looks for X.xnm.bpb and falls
  back to the text X.xnm (same INI reader as config.ini). A model/texture/collision of another size: the resource is repacked
  in the sector (A 0x1000, B 0x80), the .toc blocks/table rebuilt, the memory slot (soi +0x50, index in the sector header)
  enlarged. A .dds of another size/mips/format: new fetch (MakeFetch).
  If a file is missing/invalid, the log says "[mods] ERROR loose scene ..." and the game uses the original package.
  The runtime logs "(modified)" / "(new size)" for files whose CRC differs from X.files.txt.
  `CarsRecomp --build-loose <root that mirrors the game> C\Scenes\X\X.x360.res <out>` writes <out>.res/.str without the game.
  Checked on all 30 packages: .soi and .toc identical, models identical, collisions identical (6 with 1 ULP in the node radius),
  textures equal except the tile padding. The old X.x360.res\/X.x360.str\ folders were moved to
  "<game>\_old_mod_folders" (scenes_extract.py still writes that format; scenes_decompile.py reads it from there, --source).
  In game: FE built in ~65 ms, title screen unchanged; a test with the logo (PS360_RaceORama_LOGO.xng with x*0.5 and RoR_Logo.dds
  replaced by a striped 256x128, original 512x256) showed up right. With no input the attract now plays attract_720p.bik (not a race).
  - scenes_decompile.py: the same resource in two packages of the folder (X and X_Landmark) -> the first one's wins (first_write);
    .soi-only textures (86 DDS in HUB) are exported; fixed bug: the MiniMap "Section_N" position only applies without records
    (it used to shift textures of normal packages when the 16 bytes before the name looked like a record).
  - .soi (grammar checked on all of them): header 15 u32 [version 0x102 (0x101 = MiniMap, unblocked stream: the sector record in
    the .toc only {pos, total, A, B}), 0x810, sectors, collisions, models, 0, stream textures, .soi-only textures,
    A slots, B slots, models pos, (= models pos), collisions pos, textures pos, 0]; positions relative to 0x3C point to the
    index of the 1st record. Info at 0x3C (0x50: size of each A slot and then B). Record = 0x3C of placement
    {0, Position, LookVector, UpVector, 0, 0} BEFORE it (the 1st one is in the info) + u32 index (= .toc index; -1 for .soi-only
    textures) + name[0x100] + u32 0 + u32 0/-1 + u32 nprops + nprops x 0x208 {name[0x100], value[0x100], 8} + content:
    texture 0x38 (fetch at +0x20) or {u32 size, PC DDS} (86 in HUB, after the stream textures; MiniMap CR_01),
    xgs, collision 0x7C (.gol header with flag 9 + 0 + root node). The file ends at the end of the last content.
  - .toc: sectors {name[0x100], 21 u32 [0, nres, nA, nB, -1, slot A, slot B, 8 neighbors, box 6 floats], record
    {pos in the .str, total, A, B, A in the .str, B in the .str, blocks A, blocks B} + table, nres x 0x118}; no absolute offsets.
  - Collision tree node in the stream (64 B): {min xyz,1, size xyz,1, children u32, A = 8*sx*sy*sz, B = sqrtf((sx*sx +
    sy*sy) + sz*sz) in float, 0, 16 zeros}; vertices xyz,1; triangles {normal,1, d, 3 u16, 6 zeros}.
- Diagnostics: CARS_LOG_READS=text;text (reads with offset/size/guest stack), CARS_LOG_INFLATE=1 (both copies
  of zlib 1.0's inflate: sub_822731D8 and sub_823CF728); "[mods] WARNING: uncompress of data without a zlib header" = untracked
  raw block. Tests with run.ps1 use build\Release\mods: pass CARS_MODS="<game>\mods".

## Profile and saves (12/10/2026) - src/kernel_xam.cpp
- Local profile ALWAYS signed in on controller 1 (XamUserGetSigninState = 1, no "not signed in" warning): gamertag = Windows
  user name (<game>\savegame\profile.ini [Profile] Name=; the old perfil.ini [Perfil] Nome= is no longer read), offline XUID
  E000000043617273. XamUserReadProfileSettings returns defaults (vibration 3 etc.; the game only reads 10040003); no
  achievements (empty enumerator).
- Saves: XamContentCreateEx mounts the root the game asks for ("Cars") at <game>\savegame\<file>\ (the user asked for the
  game folder, not Documents) + <file>.info with type/display name (for XamContentCreateEnumerator/XamEnumerate).
  kernel_io ResolvePath calls XamResolveContentPath ("Cars:\PSLOT0.dat"). The game writes PSLOT0.dat..N (story mode, ~312 KB,
  "SAVE 1 - A") and GlobalData ("HALL OF FAME"). XamShowDeviceSelectorUI = hard drive; XamShowMessageBoxUIEx answers the
  active button (logs the text); system UIs send XN_SYS_UI 1/0. Notifications like Xenia (XN_SYS_UI 0 + SIGNINCHANGED 1 for the
  1st system listener, Live disconnected). XMsgStartIORequest(Ex) completes the XOVERLAPPED right away (app FB msg B0006 etc.).
  XOVERLAPPED: +0 result, +4 length, +0xC event, +0x10 routine (APC: result, length, ov), +0x18 extended error.
  CARS_SAVES=<folder> changes the location (tests). Tested: create (story -> track CR_01 loaded) and load ("Loading Game").
- kernel_io: _wfopen (paths with accents), FILE_DIRECTORY_FILE creates the folder, NtSetInformationFile 20/19 (size),
  13 (delete on close: KObject::deleteOnClose) and 10 (rename).
- Tests with run.ps1: XInput reads the user's PHYSICAL controller even with the window hidden ([input] lines that are not from the test).
- Texts without "Xbox 360" (12/10/2026): `python -I tools/pc_texts.py [--undo]` writes into mods\<L>\Loc\ the
  <L>_L_TRC_X360.bin and <L>_L_CarsFrontEndUI.bin (E/F/G/I/S) with "Xbox 360 console" -> computer/ordinateur/Computer/
  computer/ordenador and "Xbox 360 controller" -> just "controller". Format: u32 bytes of the block, BOM FF FE, UTF-16LE strings
  separated by 0 (the game gets them by index; C\Loc\X.loc = count + STR_* names), then 1 attribute byte per string
  (kept). Only those two banks mention Xbox; the rest ("xbox360_A_button"...) are texture names.

## Session log (10/10/2026) - WHERE TO LOOK WHEN THE USER FINDS SOMETHING ODD
- Every time the game starts: "C:\Games\Cars Race-O-Rama Recomp\logs\YYYY-MM-DD_HH-MM-SS\game.log" (the last 30 sessions are kept;
  sessions from older builds have jogo.log). session_log.cpp: stdout/stderr -> pipe -> "session log" thread prefixes HH:MM:SS.mmm
  (local time) to each line; with stderr redirected (tools/run.ps1) the original text also goes to run.err.log (no time). Limit 256 MB.
- Contents: [session] header (exe, Windows, CPU, RAM, GPU/driver, monitor, FPS/VSync), [gpu] per second, [input] each
  button pressed/released (input.cpp), [file] each file opened (up to 10x the same; kernel_io.cpp), [window] focus/minimize,
  [hang?] no swap for 6 s + guest stacks (MonitorThread), [CRASH] with guest stack, "[session] end (reason)".
  No end line = process killed/closed by force.
- F12 = mark: "[MARK NN]" in the log + folder mark_NN_HH-MM-SS with the last 5 s of screenshots (JPEG, 4/s: capture ring in the
  renderer, namespace capture, "captures" thread), a current PNG screenshot, the last 10 s of audio as .wav (kernel_audio.cpp, 16-bit
  stereo ring), frame.txt (full trace of the next frame) and frame_N*.png (frame, textures and RT of each resolve). The window title
  says the mark was saved. Cost: ~0.5 s hitch on F12 (GPU copies of the trace); the periodic captures cost no fps.
- Read it: `python -I tools/session.py` (last session: duration, how it ended, fps, problems, marks with context),
  `--list`, `-n 2` (second to last), `--mark 3` (everything in the 20 s before the mark), or a folder path. It also reads
  logs from older builds (Portuguese tags).
- CARS_MARK_AT=seconds[,...]: mark without a key (only for testing with the window minimized; it is not game input).
- Dumps (CARS_DUMP_FRAMES, CARS_DUMP_RESOLVE, F12) are written as PNG (WIC, WriteImageFile in session_log.cpp).

## Debugging tools
- Do NOT leave automatic input in the game (the user plays on the keyboard; an automatic test Start opened the pause menu every 3 s).
- `[gpu]` stats per second: fps, worst frame, draws, "CP work" (real time of the command processor) and "waiting for CPU"
  (WAIT_REG_MEM), compiled shaders and scans. CARS_THREADS=1: CPU use per thread every 5 s (`[threads]`, guest names N (entry)).
- CARS_HOST_SAMPLE=1 (all) or =text of the thread name (e.g. 820A8440, "gpu cp"): native profiler by RIP; symbolize with
  `python -I tools/symbolize.py build/CarsRecomp.map run.err.log all -1` (recompiled functions show up as sub_XXXXXXXX).
- CARS_LOG_PAUSE=1: logs who calls the native pause (sub_82162350) and the system events (sub_820F8758).
- tools/run.ps1 -Seconds N [-Dump "f1,f2"] [-Trace] [-GpuLog] [-Extra "--novsync"]: runs with a minimized window (CARS_HIDDEN_WINDOW, ignores WM_CLOSE).
  With no input, after the title the game goes into attract mode: good for measuring performance.
- Variables: CARS_DUMP_FRAMES, CARS_DUMP_RESOLVE=addresses, CARS_TRACE_FRAME=N (all draws/resolves of the frame,
  RT dumps at each resolve), CARS_GPU_LOG, CARS_STACKS (guest stacks every 12 s), CARS_SAMPLE (statistical profiler),
  CARS_D3D12_DEBUG. The crash handler shows rip, registers, guest lr; build/CarsRecomp.map (lld /lldmap) to symbolize.
- tools/find_missing_switches.py: finds jump tables missing from cars_switch_tables.toml (2 inline ones were added: 0x82167698, 0x8217CEDC;
  size override 0x8217CEC8 in cars.toml). Backup before that in C:\DEB\cars\backup_20261009.
- Keyboard: Enter=Start, Tab=Back, Space=A, Esc/Backspace/X=B, C=X, V=Y, Q/E=LB/RB, arrows=d-pad/stick, W=accelerate, S=brake, IJKL=right stick.

## D3D map (static D3D9LTCG, no symbols)
- Hooks: `sub_X` in the generated code is a WEAK alias of `__imp__sub_X`; defining `PPC_FUNC(sub_X)` in the runtime and calling `__imp__sub_X` inside
  overrides the function (including indirect calls). `tools/gen_d3d_profile.ps1` generates `src/d3d_profile_gen.cpp` with wrappers for files 30 and 31
  (0x822BABF8..0x822E4C70); `--profile` writes `d3d_profile.txt` (calls/frame, external callers) and logs `[args]` (list g_logAddrs in d3d_profile.cpp).
- The D3D goes from ~0x822BABF8 to ~0x822D3000. Device = 0x201F2D80 (r3). Frontbuffer: 1280x720 texture at physical 0x00411000.
- Functions (primitive types: 4 TRILIST, 5 TRIFAN, 6 TRISTRIP, 8 RECTLIST, 13 QUADLIST):
  - 0x822BD308 DrawIndexed (from 0x820B1DC4); 0x822BC560 DrawVerticesUP-like; 0x822BCF18 QUADLIST (UI, from 0x820A2BB0)
  - 0x822C1738 Resolve; 0x822C4DF0 Swap (from 0x820BB4E4); 0x822C48B8 pre-swap; 0x822BBB38 ring kick (bit 0x02 in device+10941 = degraded mode)
  - 0x822BBC38 fence wait ([device+10896] >= target); 0x822C2670 hung GPU detector (wait loop)
  - 0x822C68A0 writes register blocks; 0x822C6C38 writes constants; 0x822BD978/0x822BDC78/0x822BDF08 texture headers
  - 0x822BC0F8 creates the device; 0x822BBF50 interrupt callback; 0x822CA610 VdInitializeEngines; 0x822CFAD0/0x822D0110 scaler setup (use VdQueryVideoMode)
- With LTCG, SetRenderState/SetTexture are inlined in the game: that is why the backend reads state from the PM4 registers (like Xenia), not from hooks.

## Suggested next steps
1. Fix the stretched geometry in races (check the rewritten vfetch: offset/stride/format per element; indices; VGT_INDX_OFFSET).
2. Performance: async PSOs, texture cache without re-hashing every frame.
3. New objects in loose scenes (today a scene only has the skeleton's objects).
4. Achievements (XamUserCreateAchievementEnumerator / XamUserWriteAchievements) if anyone cares.

## Work tips
- Compiling the 78 generated files is slow; runtime-only changes rebuild fast. Syntax check without Clang: g++ -std=c++20 -msse4.1 -fsyntax-only with a simde shim.
- Windows `findstr`: use `findstr /s /m /c:"text" ppc\*.cpp`.
- Never run huge greps over all ppc_recomp.*.cpp without a timeout.
- Bash heredocs mangle backslashes in Python/C strings: write scripts with the Write tool.
