# Cars Race-O-Rama — recompilação estática para PC

Runtime nativo para Windows que roda a versão de **Xbox 360** de *Cars Race-O-Rama* (2009) recompilada estaticamente com
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), no estilo do [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).
O código PowerPC do jogo vira C++ (gerado pelo XenonRecomp); este projeto é o "console" em volta: kernel/XAM do Xbox 360,
GPU Xenos em D3D12 (shaders traduzidos pelo [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)), áudio XMA, entrada,
arquivos, saves e ferramentas de mod.

> **Este repositório não contém nenhum arquivo do jogo.** Para usar, você precisa da sua própria cópia de
> *Cars Race-O-Rama* para Xbox 360 (o `default.xex` e os arquivos do disco). O código gerado pelo XenonRecomp, o XEX e
> os scripts do jogo descompilados não são distribuídos aqui.

## O que já funciona
- Jogo completo com gráficos (intros em Bink, menus, modo história, corridas, mundo aberto) a 60 fps.
- Áudio (XAudio2 + XMA decodificado com o FFmpeg), teclado e controle (XInput).
- Perfil local sempre logado e **saves** na pasta `savegame` do jogo (sem conta Xbox).
- Textos sem "Xbox 360" (`tools/textos_pc.py`).
- **Mods**: a pasta `mods` espelha a pasta do jogo; scripts Lua em texto; **cenas em arquivos soltos**
  (`geo\*.xng`, `tex\*.dds`, `col\*.gol`, materiais `.xnm` em texto), remontadas em pacotes na hora.
- Log de cada sessão em `logs\<data>\jogo.log` (F12 marca um momento com capturas de tela e som).

## Compilar
Requisitos: Windows, Visual Studio 2022/2026 com Clang (ClangCL), CMake, Python 3.

1. Clone o [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (com submódulos) e o
   [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).
2. Gere o código do jogo com o XenonRecomp a partir do seu `default.xex` descriptografado (veja `CLAUDE.md`: endereços
   das funções de save/restore, `setjmp`/`longjmp` e as tabelas de salto). O recompilador precisa do patch que acrescenta
   instruções que o jogo usa (lista no `CLAUDE.md`).
3. Baixe as dependências de `third_party/` (veja [third_party/LEIA-ME.md](third_party/LEIA-ME.md)).
4. Configure e compile:
   ```
   cmake -S . -B build -G "Visual Studio 18 2026" -T ClangCL -DXENONRECOMP_DIR=<clone do XenonRecomp> ^
         -DXENOSRECOMP_DIR=<clone do XenosRecomp> -DPPC_DIR=<pasta ppc gerada> -DCARS_XEX_PATH=<seu default_dec.xex>
   cmake --build build --config Release
   ```
5. `tools\deploy.ps1` instala na pasta do jogo como `Cars Race-O-Rama.exe` (o XEX vai embutido no exe).

## Estrutura
- `src/` — runtime: `kernel_*.cpp` (kernel, memória, threads, arquivos, XAM), `gpu/` (processador de comandos PM4,
  renderer D3D12, texturas, shaders), `xma.cpp` (áudio), `scene_assets.cpp` (cenas em arquivos soltos), `game_hooks.cpp`.
- `tools/` — scripts de depuração e de mod (extração/descompilação de cenas, conversão `.xng` → `.obj`, análise de log).
- `CLAUDE.md` — notas técnicas detalhadas de tudo que foi descoberto (formatos, endereços, decisões).

## Créditos
XenonRecomp e XenosRecomp (hedge-dev), [Xenia](https://github.com/xenia-project/xenia) (referência de kernel/GPU/XAM),
FFmpeg (LGPL), unluac. *Cars* é marca da Disney/Pixar; *Cars Race-O-Rama* foi desenvolvido pela Incinerator Studios e
publicado pela THQ. Projeto de fã, sem afiliação.
