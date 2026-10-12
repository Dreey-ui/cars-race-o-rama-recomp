# Cars Race-O-Rama — static recompilation for PC

A native Windows runtime that runs the **Xbox 360** version of *Cars Race-O-Rama* (2009), statically recompiled with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), in the style of [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).
XenonRecomp turns the game's PowerPC code into C++. This project is the "console" around it: the Xbox 360 kernel and
XAM, the Xenos GPU on D3D12 (shaders translated by [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)), XMA audio,
input, file I/O, saves and modding tools.

> **This repository does not contain any game files.** To use it you need your own copy of *Cars Race-O-Rama* for
> Xbox 360 (the `default.xex` and the disc files). The code XenonRecomp generates, the XEX and the decompiled game
> scripts are not distributed here.

## What works
- The full game with graphics at 60 fps: Bink intros, menus, story mode, races and the open world.
- Audio (XAudio2, with XMA decoded by FFmpeg), keyboard and controller (XInput).
- A local profile that is always signed in, and **saves** in the game's `savegame` folder (no Xbox account needed).
- Texts without "Xbox 360" (`tools/pc_texts.py`).
- **Mods**: the `mods` folder mirrors the game folder, and Lua scripts can be edited as text.
- **Scenes as loose files**: `geo\*.xng`, `tex\*.dds`, `col\*.gol` and text `.xnm` materials, rebuilt into packages on the fly.
- A log for every session in `logs\<date>\game.log`. F12 marks a moment and saves screenshots and audio.

Known issues: some geometry stretching in races, and achievements/Live are not implemented. Performance drops in very
heavy scenes. See the notes in `CLAUDE.md`.

## Building
Requirements: Windows, Visual Studio 2022/2026 with Clang (ClangCL), CMake, Python 3.

1. Clone [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (with submodules) and
   [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).
2. Patch XenonRecomp with `recomp/xenonrecomp_recompiler.patch`. It adds PowerPC instructions this game uses (see
   [recomp/README.md](recomp/README.md)).
3. Decrypt and decompress your `default.xex` (for example `xextool -e u -c u`). Then generate the game code with
   `recomp/cars.toml` and `recomp/cars_switch_tables.toml`.
4. Download the dependencies into `third_party/` (see [third_party/README.md](third_party/README.md)).
5. Configure and build:
   ```
   cmake -S . -B build -G "Visual Studio 18 2026" -T ClangCL -DXENONRECOMP_DIR=<XenonRecomp clone> ^
         -DXENOSRECOMP_DIR=<XenosRecomp clone> -DPPC_DIR=<generated ppc folder> -DCARS_XEX_PATH=<your default_dec.xex>
   cmake --build build --config Release
   ```
   Compiling the ~78 generated files is slow. After that, runtime-only changes rebuild quickly.
6. `tools\deploy.ps1 -Game "<game folder>"` installs it into the extracted game folder as `Cars Race-O-Rama.exe`.
   The XEX is embedded in the exe, and the script also copies the DLLs, the shader cache and `mods\README.txt`.

Running from the build folder also works:
`build\Release\CarsRecomp.exe <default_dec.xex> "<game folder>" [--trace]`.

## Layout
- `src/` — the runtime:
  - `main.cpp` loads the XEX and handles crashes; `memory.cpp`, `thread.cpp` and `kobjects.cpp` are the basics.
  - `kernel_*.cpp`: sync, memory, file I/O, XAM (profile/saves), misc, GPU interrupts and audio.
  - `gpu/`: the PM4 command processor lives in `kernel_gpu.cpp`. This folder has the D3D12 renderer, textures, shaders,
    the VB/IB cache (`memwatch.cpp`) and model capture.
  - Other files:
    - `xma.cpp`: XMA decoding.
    - `scene_assets.cpp`: scenes as loose files.
    - `game_hooks.cpp`: hooks on game functions.
    - `session_log.cpp`: session log, F12 marks.
    - `input.cpp`, `settings.cpp`.
- `tools/` — debugging and modding scripts:
  - `session.py`: summary of the last session log.
  - `scenes_extract.py` / `scenes_decompile.py`: scene packages → folders / loose files.
  - `xng2obj.py`: `.xng` models and whole scenes → `.obj`.
  - `decompile_lua.ps1`: game Lua scripts → text.
  - `pc_texts.py`: the localization text replacements.
  - `run.ps1`, `fps_summary.ps1`, `symbolize.py`: test runs, performance and profiling.
- `recomp/` — the XenonRecomp patch and configuration for this game.
- `dist/` — files installed next to the game (the mods readme).
- `CLAUDE.md` — detailed technical notes: file formats, guest addresses, the decisions taken and why.

## Debugging
Every run writes `logs\<date>\game.log` next to the exe. Read it with `python -I tools/session.py`; `--mark N` shows
the context of an F12 mark. Environment variables such as `CARS_GPU_LOG`, `CARS_TRACE_FRAME=N`, `CARS_THREADS=1` and
`CARS_HOST_SAMPLE=1` turn on more diagnostics; they are listed in `CLAUDE.md`. `tools\run.ps1 -Seconds N` runs the game
with a hidden window. With no input, it goes into attract mode (3D races), which is good for performance checks.

## Contributing
Issues and pull requests are welcome. Code, comments and logs are in English. The project owner speaks Portuguese, so
a short English description is enough. Please do not commit game files, generated `ppc/` code or decompiled scripts.

## Credits
XenonRecomp and XenosRecomp (hedge-dev), [Xenia](https://github.com/xenia-project/xenia) (the kernel/GPU/XAM
reference), FFmpeg (LGPL) and unluac. *Cars* is a trademark of Disney/Pixar. *Cars Race-O-Rama* was developed by
Incinerator Studios and published by THQ. This is a fan project with no affiliation.
