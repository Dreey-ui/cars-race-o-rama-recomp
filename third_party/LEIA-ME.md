# Dependências binárias (não vão para o repositório)

| Pasta | O que é | De onde |
|---|---|---|
| `third_party/ffmpeg/` (`include/`, `lib/`, `bin/`) | FFmpeg 8.1 **LGPL shared**: decodificador `xma2` do áudio XMA. As DLLs (`avcodec-62`, `avutil-60`, `swresample-6`) são copiadas ao lado do exe no build. | [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases), arquivo `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`, extraído aqui com o nome `ffmpeg` |
| `third_party/unluac/unluac_*.jar` | Descompilador de Lua 5.1 (só para `tools/decompile_lua.ps1`). | [unluac](https://sourceforge.net/projects/unluac/) |
| `third_party/jre/` | Java portátil para rodar o unluac (só para `tools/decompile_lua.ps1`). | [Adoptium OpenJDK 21 JRE](https://adoptium.net/temurin/releases/), zip x64 Windows extraído aqui com o nome `jre` |

Sem o FFmpeg o runtime não compila. O unluac e o JRE só são necessários para descompilar os scripts Lua do jogo.
