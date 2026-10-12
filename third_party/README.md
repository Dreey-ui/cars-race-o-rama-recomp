# Binary dependencies (not in the repository)

| Folder | What it is | Where from |
|---|---|---|
| `third_party/ffmpeg/` (`include/`, `lib/`, `bin/`) | FFmpeg 8.1 **LGPL shared**: the `xma2` decoder for the XMA audio. The DLLs (`avcodec-62`, `avutil-60`, `swresample-6`) are copied next to the exe by the build. | [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases), file `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`, extracted here as `ffmpeg` |
| `third_party/unluac/unluac_*.jar` | Lua 5.1 decompiler (only for `tools/decompile_lua.ps1`). | [unluac](https://sourceforge.net/projects/unluac/) |
| `third_party/jre/` | Portable Java to run unluac (only for `tools/decompile_lua.ps1`). | [Adoptium OpenJDK 21 JRE](https://adoptium.net/temurin/releases/), x64 Windows zip extracted here as `jre` |

The runtime does not build without FFmpeg. unluac and the JRE are only needed to decompile the game's Lua scripts.
