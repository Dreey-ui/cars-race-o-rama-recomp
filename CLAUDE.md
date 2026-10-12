# Cars Race-O-Rama (Xbox 360) — static recompilation para PC

Projeto: recompilação estática do Xbox 360 "Cars Race-O-Rama" (title ID 54510848, PE original `Cars3_final_disc.exe`,
XDK 8276) usando hedge-dev/XenonRecomp, no estilo UnleashedRecomp. O usuário (Drey) fala português; responda em português.
Ambiente: Windows, Visual Studio 2026 Insiders (generator `Visual Studio 18 2026`, `-T ClangCL`, Clang 22.1.3), PowerShell.

## Caminhos na máquina do usuário
- `C:\DEB\cars` — arquivos de trabalho: `default_dec.xex` (XEX descriptografado/descomprimido), `basefile.exe` (imagem crua, NÃO é PE),
  `cars.toml`, `cars_switch_tables.toml`, `xextool.exe`, `XenonRecomp.exe`, `XenonAnalyse.exe`, pasta `ppc\` (código gerado, 78 `ppc_recomp.*.cpp`
  + `ppc_func_mapping.cpp` + `ppc_config.h` + `ppc_recomp_shared.h`).
- `C:\Users\Drey\XenonRecomp` — clone com submódulos (usa-se XenonUtils, thirdparty/disasm, thirdparty/simde).
- `C:\DEB\cars-runtime` — ESTE projeto (runtime host). Build: `cmake -S . -B build -G "Visual Studio 18 2026" -T ClangCL -DXENONRECOMP_DIR=C:/Users/Drey/XenonRecomp -DPPC_DIR=C:/DEB/cars/ppc` e `cmake --build build --config Release`.
- ISO extraída (pasta do jogo): `C:\Games\Cars Race-O-Rama Recomp` (tem C, E, extras, F, G, I, Res, S, config.ini, default.xex).
- Execução: `.\build\Release\CarsRecomp.exe C:\DEB\cars\default_dec.xex "<pasta da ISO>" --trace *> run.log` (as aspas são obrigatórias: o caminho tem espaços e parênteses).

## Estado do recompilador (concluído)
- XenonRecomp rodou limpo (100%, sem `// ERROR`) com `cars.toml`: `file_path="default_dec.xex"`, `out_directory_path="ppc"`,
  `switch_table_file_path="cars_switch_tables.toml"`, otimizações todas false.
  Endereços: savegprlr_14 0x82506410, restgprlr_14 0x82506460, savefpr_14 0x82506EC0, restfpr_14 0x82506F0C,
  savevmx_14 0x82507470, restvmx_14 0x82507708, savevmx_64 0x82507504, restvmx_64 0x8250779C, setjmp 0x825071A0, longjmp 0x825060F0,
  `invalid_instructions=[{data=0,size=4}]`, e 18 overrides em `functions` (tamanhos reais vindos do .pdata).
- O XenonAnalyse NÃO achou jump tables (compilador de 2009 reordena instruções). `cars_switch_tables.toml` (140 tabelas: 51 abs, 26 computed, 23 byte, 40 short)
  foi gerado por um scanner próprio sobre `basefile.exe`; duas entradas escritas à mão (0x823FD84C, 0x824049D0) têm `default = 0`.
- O `recompiler.cpp` do XenonRecomp foi PATCHEADO (ver `recompiler.cpp`/`recompiler.patch` entregues antes) para suportar: ADDC, ADDME, SUBFZE, EQV, BDZF, DCBST, LHBRX,
  LHZU, STHU, LFSU, STFSU, LFDU, STFDU, LBZUX, LWZUX, STBUX, STHUX, STDUX, LFSUX, STFSUX, LFDUX, FRSQRTE, VADDSWS, VSUBSHS, VPKSWSS128, VSEL128, VCMPBFP(128).
  Se regenerar o `ppc/`, use esse recompiler patcheado.
- Ainda NÃO implementados no XenonRecomp: `vpkd3d128` (modo 3) e `vupkd3d128` (modo 12) emitem `__builtin_debugtrap()` (ppc_recomp.30.cpp e ppc_recomp.35.cpp). Implementar se forem atingidos.

## Modelo de memória / convenções do runtime
- Bloco guest de 4 GB reservado (`g_mem.base`, VirtualAlloc MEM_RESERVE), páginas commitadas sob demanda (`g_mem.Commit`). Acessores big-endian R8..R64 / W8..W64 em `runtime.h`.
- Imagem em 0x82000000 (tamanho 0x7D0000); código em 0x82090000; tabela de ponteiros de função em `base + 0x82000000 + 0x7D0000 + (guest - PPC_CODE_BASE)*2`
  (8 bytes por instrução de 4 bytes; macro `PPC_LOOKUP_FUNC`). Preenchida a partir de `PPCFuncMappings[]`.
- Heaps guest: virtual 0x20000000–0x7E000000, pool 0x90000000–0xA0000000, físico 0xA0000000–0xC0000000.
- Imports do kernel = funções `__imp__Nome` com a macro `KERNEL_IMPORT(Nome)` (= `PPC_FUNC(__imp__Nome)`). `stubs_gen.cpp` tem 180 stubs FRACOS (`__attribute__((weak))`)
  que só logam `[import nao implementado]` e retornam 0; definir a função de verdade em outro .cpp sobrescreve o stub.
- Argumentos: r3..r10; 9º argumento em diante na pilha (r1+0x54...). Retorno em r3. Use `RetStatus(ctx, status)` para NTSTATUS.
- PCR em r13: +0x100 KTHREAD atual, +0x10C CPU, +0x150 KPRCB; KTHREAD +0x58/+0x5C limite/base da pilha, +0x14C id da thread. Threads: `KSetupGuestThread`, `KInitContext`, `RunGuestMain`.
- Objetos do kernel: `KObject` (evento, semáforo, mutant, thread, arquivo) com tabela de handles (começam em 0x80000100) e tabela de objetos guest; `KWait` com um mutex+condvar globais.

## Arquivos do runtime (src/)
main.cpp (carga do XEX, mapeamento, handler de crash/auto-map, thread monitor), memory.cpp, thread.cpp, kobjects.cpp, kernel_sync.cpp (critical sections, TLS, eventos, semáforos, mutants, threads, waits),
kernel_mem.cpp (NtAllocateVirtualMemory, MmAllocatePhysicalMemoryEx, ExAllocatePool...), kernel_io.cpp (Nt*File, `game:\`/`\Device\Cdrom0\`/`d:\` → pasta da ISO; NtQueryDirectoryFile),
kernel_misc.cpp (tempo, config, XGetVideoMode, entrada: controle fixo sem botões, XamUser*, notificações), kernel_gpu.cpp (thread de vblank 60 Hz chamando o callback de `VdSetGraphicsInterruptCallback`), stubs_gen.cpp.

## Histórico de bugs já resolvidos (não repetir)
- Seção `.reloc` do XEX passa do fim do buffer → crash no memcpy; ela é ignorada em main.cpp. Cópia das seções limitada ao fim da imagem.
- Nomes de seção impressos com lixo (não terminam em zero): inofensivo.
- PowerShell: comentários `#` colados não copiam arquivos; usar `Copy-Item`/`Expand-Archive` explícitos.

## Estado atual (09/10/2026)
O jogo roda com GRAFICOS: intros (Pixar/THQ/Incinerator, Bink), loading (~20 s), tela de titulo 3D, modo attract e corridas 3D com HUD
renderizam pelo backend D3D12. Teclado/controle funcionam. Problemas abertos (o usuario achou "cagado"):
- Tela de titulo 3D: ainda pode sair parcial; corridas: faixas/estiramentos horizontais no carro (geometria esticada), possivel erro de vertex fetch/indice.
- Desempenho: com 50-70 mil draws/s cai para ~18-22 fps; o custo e copiar vertex buffers por draw (vb ~350 ms/s) e hash de texturas. Cachear VBs/IBs por (endereco, tamanho, hash).
- Sem som (XAudio render driver so descarta amostras; XMA nao decodificado). (Saves/perfil: RESOLVIDO em 12/10, veja "Perfil e saves".)
- 37 `// ERROR` pre-existentes no codigo gerado (ppc_recomp.33/34/40/41/42/52) - nao investigados.

## Arquitetura de GPU (src/gpu + kernel_gpu.cpp) - estilo Xenia + shaders XenosRecomp
- kernel_gpu.cpp: processador PM4 com semantica do Xenia (pm4_command_processor_implement.h em C:\DEB\ref\xenia). Registradores em g_regs[0x8000].
  Trata SET_CONSTANT/LOAD_ALU_CONSTANT/SET_SHADER_CONSTANTS, IM_LOAD(_IMMEDIATE), DRAW_INDX(_2), predicacao de pacotes (bit 0, SET_BIN_MASK/SELECT
  0x50/0x51/0x60-0x63) usada pelo predicated tiling, scratch writeback, EVENT_WRITE_SHD/EXT(0x5A), INTERRUPT (callback r3=fonte, r4=dados, CPU no PCR+0x10C).
  VdSwap escreve pacote XE_SWAP (0x64, 'SWAP', frontbuffer, w, h) como o Xenia. CP gira com yield ~4 ms apos trabalho (timeBeginPeriod(1)):
  o D3D espera fences em sequencia (sub_822BBC38) e latencia alta deixava o loading em ~70 s.
- MMIO simulado: 0x1951=1 (vblank), 0x1961=1280x720, RB_EDRAM_TIMING etc. (sem isso o callback nunca ve vblank e o CP espera [0x40B000]).
- VdIsHSIOTrainingSucceeded DEVE retornar 1 (senao o D3D entra em modo degradado e para de chutar o ring). VdQueryVideoMode precisa de pixel_rate 0x8A em +0x1C (o D3D divide por ele).
- Memoria fisica: secao de 512 MB mapeada 3x (0xA0000000, 0xC0000000, 0xE0000000) em memory.cpp.
- gpu/shaders.cpp: indexa containers (.vsh/.psh da ISO + varredura do XEX e da memoria guest quando ha shader desconhecido) por hash do microcodigo;
  traduz com o shader_recompiler.cpp do XenosRecomp (C:\Users\Drey\XenosRecomp, compilado dentro do runtime com src/gpu/xenos_recomp_pch.h) e compila
  com dxcompiler.dll em runtime (cache em build/Release/shader_cache, versao kShaderGenVersion). Pos-processamento do HLSL: XeTransformPos (viewport
  Xenos -> host, VTE/window offset/pixel center, vtx_xy_fmt), alpha test com funcao, entradas inteiras -> int4, oPos=0, atribuicoes vazias removidas.
  - VS: o D3D REESCREVE os vfetch com a vertex declaration -> busca ignorando slots de vfetch e cria variante com o microcodigo vivo.
  - Microcodigo sem container (UI/D3D interno, ex. 0x82050DC8): container SINTETIZADO (constantes C[256], entradas TEXCOORDi, interpoladores
    ligados por indice ao PS) - ShadersLinkRawVertexShader compila um por PS.
- gpu/renderer.cpp: D3D12 bindless (root sig = b0/b1/b2 space4 + tabelas t0 space0..2 e s0 space3, como o HLSL do XenosRecomp). RT do host por
  (base EDRAM, pitch, formato DXGI) - formatos Xenos com os mesmos bits (2/10, 0/1) compartilham RT. Rect list via geometry shader; quad/fan -> indices.
  Resolve: copia RT -> textura por endereco; destinos dentro de uma textura existente viram deslocamento por macro tile 32x32 (tiling).
  Swap: blit da textura resolvida do frontbuffer. textures.cpp: untiling 2D do Xenia, endian por XOR de endereco, formatos -> DXGI.
- Outros: kernel_audio.cpp (callback do XAudio render driver a cada 5,333 ms - sem ele o motor de audio nao anda e o loading demora),
  input.cpp (XInput + teclado), APCs de usuario (NtReadFile com rotina de conclusao, esperas alertaveis).

## Desempenho / tempo (10/10/2026) - NAO regredir
- mftb: o XenonRecomp gera `__rdtsc()` (TSC do PC, GHz). `src/guest_timebase.h` e incluido a forca (/FI, CMakeLists) em todo o ppc/
  e troca por `GuestTimebase()` (QPC escalado para 50 MHz = KeQueryPerformanceFrequency). Sem isso o jogo roda acelerado.
- Processador da thread (PCR+0x10C e KTHREAD+0xBF): vem do byte alto dos flags do ExCreateThread (mascara) ou do KeSetAffinityThread
  (menor bit); sem mascara herda o da thread criadora (como o Xenia). O XAudio2 (sub_822D3AF8) e os jobs usam esse numero em barreiras
  de espera ativa: com numeros errados (antes era threadId % 6) as threads de audio giravam a 100% em 2 nucleos.
- KeDelayExecutionThread(0) (Sleep(0) em laco das 4 threads de jobs 0x820B02F8/0x820B0398): GuestYield (kernel_misc.cpp) cede a vez por
  1 ms e depois dorme 0,2 ms (timer de alta resolucao). Antes eram 4 nucleos a 95% e o thread da GPU ficava sem CPU.
- Shaders: compilacao DXC em threads de trabalho (o draw e pulado ate ficar pronto; DXIL em cache no disco carrega na hora) e a
  varredura de containers na memoria guest roda numa thread propria, so nas faixas alocadas (GuestAllocatedRanges) com busca SIMD.
- Renderer: untiling de textura com pshufb direto no buffer de upload; VB com pshufb + stream; constantes VS/PS so recopiadas quando
  escritas (g_aluDirty no kernel_gpu.cpp); estado D3D12 redundante filtrado (BoundState). Resultado: 30 fps ate ~175 mil draws/s.
- O thread principal de render do jogo (0x820A8440) gira dentro do Swap do D3D (sub_822BBC38/sub_822C2670) esperando a fence da GPU.
- Proximos passos de desempenho: cache de VB/IB entre frames (vb ainda ~250 ms/s em 180 mil draws), PSOs assincronos.

## Audio (10/10/2026)
- kernel_audio.cpp: XAudioSubmitRenderDriverFrame (6 canais x 256 amostras, float BE planar, 48 kHz) -> voz 5.1 do XAudio2 do Windows
  (downmix automatico); o callback do driver segue a fila do dispositivo (alvo 6 frames). CARS_AUDIO_DUMP=1 grava audio_dump.raw
  (tools/audio_check.py: nivel/ZCR por segundo + .wav).
- xma.cpp: hardware XMA. Tabela de 320 contextos (XMA_CONTEXT_DATA do Xenia) na memoria fisica; endereco em 0x7FEA1800 em LITTLE-endian
  (o jogo le com lwbrx), 0x7FEA1818 = 0. O XAudio2 do jogo nao usa XMA* alem de Create/Release: escreve registradores como indice*4
  com stwbrx (Kick 0x7FEA1940 em sub_822D55E8, Lock 0x7FEA1A40 em sub_822D5340, Clear 0x7FEA1A80 em sub_822D5120). Hook no kick
  decodifica na hora (como o Xenia) com o xma2 do FFmpeg, pacote de 2 KB por vez; PCM 16 bits BE intercalado no anel de saida.
  Loops sao aproximados por pacote (pode haver clique no ponto de loop). FFmpeg 8.1 LGPL em third_party/ffmpeg (DLLs copiadas no build).
- tools/gen_d3d_profile.ps1 exclui funcoes que ja tem PPC_FUNC(sub_X) em outro arquivo (senao: simbolo duplicado no link).

## Instalacao na pasta do jogo (10/10/2026)
- O XEX descriptografado vai EMBUTIDO no exe (recurso RCDATA 101 gerado de src/xex_image.rc.in, CMake CARS_XEX_PATH). Exe com
  subsistema Windows (WIN32_EXECUTABLE + /ENTRY:mainCRTStartup): sem argumentos usa o XEX embutido e pasta do jogo = pasta do exe;
  o log vai para logs\<data>\jogo.log (ver "Log de sessao"); erros fatais em MessageBox. Argumentos antigos (<xex> <pasta>) continuam valendo.
- tools/deploy.ps1 instala em "<ISO>\Cars Race-O-Rama.exe" + DLLs (dxcompiler, dxil, avcodec-62, avutil-60, swresample-6) +
  shader_cache + mods\ (nao apaga mods existentes) + lua_src\. Rodar depois de cada build (feche o jogo antes).
- Testado: o jogo nao abre o default.xex da ISO em nenhum momento (2447 aberturas de arquivo com --trace).

## Mods e scripts Lua (10/10/2026)
- kernel_io.cpp: <pasta do exe>\mods (ou CARS_MODS) espelha a pasta do jogo; arquivo la substitui o original em aberturas de leitura
  (DoOpen e NtQueryFullAttributesFile). "X.lua" em texto substitui "X.lua.bin": o runtime monta um arquivo temporario com 4 bytes BE
  de tamanho + fonte (o parser Lua do jogo esta no exe; FE_FrontEnd.lua.bin original ja e texto). Log: [mods].
- Scripts: 324 *.lua.bin = 4 bytes BE de tamanho + bytecode Lua 5.1 (lua_Number float de 4 bytes, flag little-endian), Lua modificado
  (aceita !=). lua_src\ = todos descompilados (tools/decompile_lua.ps1: unluac em third_party\unluac + JRE portatil third_party\jre;
  tools/fix_lua_goto.py reescreve os 4 trechos com goto que o unluac gera). Validado: os 324 em mods\ rodam ate a demo sem erro.
- game_hooks.cpp: sub_82284D80 = luaX_lexerror -> loga "[lua] erro de sintaxe" (LexState +4 linha, +44 TString* nome, texto em +16).
- tools/mod_bisect.ps1 -All | -Files "a.lua;b.lua": roda com esses mods e diz se a demo 3D ainda comeca (sem input).
- F12 no jogo: marca do log de sessao (abaixo); inclui o trace do proximo frame em marca_NN\frame.txt.
- Telas de carregamento (RESOLVIDO): o jogo desenha load_screen_N_w.dds (DXT5) 1 frame e a thread de render (0x820A8440,
  lr 823DD0AC chamado de 820A4198) LE o frontbuffer (fisico 00416000, tiled, k_8_8_8_8 BGRA) com a CPU para usar como fundo.
  Como os resolves so existem na GPU do host, ficava preto. renderer.cpp: depois de cada swap as paginas do frontbuffer ficam
  PAGE_NOACCESS nas 3 vistas fisicas; o CrashHandler chama RendererHandleFrontbufferAccess -> copia a textura do resolve (lista de
  comandos/fence proprias) para a memoria guest em layout tiled (TiledOffset2D) BGRA8 e libera. ~13 leituras em 2 min (so nos loadings).
- Cores R/B invertidas em render-to-texture (RESOLVIDO): quase todo resolve tem RB_COPY_DEST_INFO.copy_dest_swap (bit 24); o Xenos
  grava BGRA e o fetch usa swizzle A0A (ZYX) para destrocar. A copia do host e direta, entao HostTexture::rbSwap troca X<->Z no
  mapeamento do SRV (GetSrv); a apresentacao do frontbuffer usa GetSrv(..., false). Afetava HUD (velocimetro laranja saia ciano).
- 60 fps (10/10/2026): o D3D enfileira cada flip em sub_822C4708(valor: fb | intervalo<<8 | limite de tearing); o jogo usa
  intervalo 2 (30 fps). Hook (game_hooks.cpp) troca para 1 e chama GpuImmediateVblank (kernel_gpu.cpp: callback de vblank
  na hora, com mutex reentrante e preservando a CPU do PCR) -- sem isso a GPU esperava o flag 00410004 (flip pendente) a cada
  frame e perdia vblanks. O ritmo fica com o vsync do host + LimitFrameRate (renderer.cpp; tambem com janela oculta).
  A logica do jogo e por tempo (QPC): velocidade certa a 60 fps (cronometro conferido). Config: "<exe>.ini" ([Video] FPS=60|30,
  VSync=1|0), src/settings.cpp. CARS_VBLANK_HZ, CARS_LOG_WAITS, CARS_LOG_SLOW_WAITS=swap: diagnostico.
- Cache de VB/IB entre frames (gpu/memwatch.cpp + renderer.cpp): copias convertidas num pool persistente (upload heap, 1 GB),
  paginas guest PAGE_READONLY nas 3 vistas; escrita da CPU -> excecao (main.cpp CrashHandler) marca suja; epoca global evita
  checar paginas por draw; paginas reescritas em < 30 frames viram dinamicas por 600 frames. NtReadFile chama MemWatchInvalidate
  (ReadFile nao gera excecao). CARS_NO_BUFFER_CACHE=1 desliga. vb caiu de ~200 para ~80 ms/s. tools/fps_summary.ps1 resume o log.
- Pasta do jogo agora: C:\Games\Cars Race-O-Rama Recomp (sem default.xex). tools/dds2png.py decodifica DXT1/3/5 para conferir cores.
- Diagnostico: CARS_LOG_FULLSCREEN=1 (texturas 1280x720 usadas por frame), CARS_WATCH=end,tam (bytes nao-zero a cada 10 frames),
  CARS_TRAP=fisico,tam + CARS_TRAP_DELAY=ms (loga o primeiro acesso da CPU a uma faixa da memoria fisica, com pilha guest).

## Cenas extraidas (11/10/2026) - mods\C\Scenes (o jogo le PASTAS, nao os pacotes)
- O usuario quer que o jogo leia as pastas extraidas, nao .str/.res. tools/scenes_extract.py gera, para os 30 pacotes
  de C\Scenes: mods\...\X.x360.res\ (arquivos internos, ZLIBME descomprimidos, _indice.txt = ordem) e
  mods\...\X.x360.str\ (cada textura como .dds de PC no caminho do jogo + _dados\XXXXXXXX.bin com o resto do stream:
  geometria e enchimento dos tiles + _indice.txt "tamanho / textura <fetch 6 dwords> <tam> <posicoes> <arquivo> /
  dados <pos> <arquivo>"); confere que a pasta remonta o stream byte a byte (scenes_textures.build_from_folder).
  Backup dos originais em "<jogo>\_backup_original\C\Scenes". Testado: FE (menu), HUB (mundo aberto), corrida.
- Runtime: src/scene_assets.cpp monta cada pasta UMA vez por execucao num arquivo temporario (FILE_FLAG_DELETE_ON_CLOSE,
  aberto de novo a cada NtCreateFile do jogo): .res v3 sem compressao (flags 0) e stream cru (zera, copia _dados,
  escreve os blocos de cada .dds com tiling/endian/swizzle do Xenos; aceita DDS com cabecalho DX10). kernel_io.cpp:
  FindModOverride reconhece pasta X.res / X.str; streams marcam rawStream (RawStreamRegister + hook do uncompress).
  `CarsRecomp.exe --montar <pasta> <saida>` monta uma pasta sem abrir o jogo (teste: igual ao .str cru de referencia).
- .res v3 (leitor sub_821C3580 cabecalho/diretorio, sub_821C1AA0 leitura): +0 versao, +4 tamanho do cabecalho apos +8,
  +8 flags (0x100 blocos zlib de 0x6000, 0x40 tabela u16 de tamanhos, 0x200 tamanhos-1, 0x80/0x400 XOR), diretorio
  {u32 n, u32 tamanho do diretorio, n x {u32 tam. nome, nome, u32 posicao, u32 tamanho}}, dados depois (LE). Sem 0x100 o
  jogo le cada arquivo direto do disco. Arquivos internos "ZLIBME" (+8 comp BE, +12 descomp BE, zlib em +16): o carregador
  (sub_821FF8B8) aceita sem compressao. .xnm.bpb = material (INI binario "BPBh": shaders, nomes de textura).
- .str: registros de stream no .toc (BE): {posicao no .str, total, descomp A, descomp B, comp A, comp B, blocos A,
  blocos B, tabela de tamanhos}; HUB tem 72 (zonas), ShadowMap ~100. Leitor sub_820B0398: 0/0 blocos = le direto;
  senao le cada bloco num buffer de 0x20000 (0x8259FA00, 4 slots) e o job sub_820B02F8 chama uncompress
  sub_822702F8(dst, src, len). O stream "cru" dos mods tem os blocos descomprimidos alinhados a 0x1000 e os registros do
  .toc reescritos (o .toc da pasta .res ja e o reescrito). MiniMap .str ja e cru no disco (sem registros): fica o original.
- Texturas: .soi tem {nome[0x100], ..., cabecalho D3D do 360 com BaseFlush/MipFlush FFFF0000 FFFF0000 em +0x124 e a
  fetch constant de 6 dwords em +0x12C}; .toc tem {nome[0x100], 0, indice, indice, posicao no trecho A, tamanho, 1} logo
  depois do registro de stream da zona. Base address 0; mip_address relativo a textura; 1945 texturas (DXT1/3/5, ARGB,
  2 cubos). Layout dos mips = GetGuestTextureLayout do Xenia (tools/scenes_textures.py e scene_assets.cpp iguais).
  Tiles de mips tem restos de imagem no enchimento: ficam nos _dados (por isso a remontagem e exata).
- Modelos (12/10/2026): o .toc tem um registro por modelo {nome[0x100], 0, FFFFFFFF, indice, posicao do bloco no stream,
  tamanho, 0}; o .soi tem o cabecalho "xgs" de cada modelo (= .xng sem os dados; nome interno em xgs+0x0C casa com o
  fim do nome do .toc). Formato .xng/.xgs (BE): 'xng\0', u32 versao 5, u32 objetos; por objeto nome[0x80], matriz 4x4,
  limites (7 floats), FFFFFFFF (+0xE8), u32 materiais, materiais x nome[0x40], u8 LODs(+3), por LOD f32 distancia + u32
  submalhas, por submalha {u32 material, u32 streams, u8 n, u8, u8 (0 = dados no arquivo, 1 = no stream), u8, u16,
  n x f32, u16 vertices, u16 indices} e no .xng os dados logo depois: indices u16 em tira (FFFF reinicia) alinhados a 4 +
  streams nao intercalados na ordem posicao 0x01 (3f), normal 0x02 (3f), cor 0x08 (ARGB), UV0 0x04 (2f), UV1 0x10,
  UV2 0x4000, tangente 0x20 (3f). No stream o bloco do modelo e a concatenacao dos dados das submalhas.
  Vertices em espaco do objeto: o jogo posiciona pelo [ModelN] do .scn (Position, LookVector = Z, UpVector = Y; os
  dois vetores carregam a ESCALA no comprimento).
- Zip do amigo do usuario (Downloads\Scenes.zip, extraido em "<jogo>\cenas_soltas"): C\Scenes de ORIGEM em arquivos
  soltos (9494 .xng, 7701 .dds de PC, .xnm em texto, .gol colisao, .pco fisica, .scn COMPLETOS com [ModelN]) para as
  cenas do jogo + RS/TF/OV (PS2), PC_*, RR_*, CD, *_Night. Bate com o jogo (texturas identicas byte a byte; todos os
  modelos da FE). tools/xng2obj.py <pasta> [--so X]: <cena>\_obj\modelos\*.obj, <cena>\_obj\<cena>.obj (cena montada
  pelo .scn) e materiais.mtl (map_Kd = .dds via TextureMap_1 do .xnm). Conferido: render com textura igual ao menu.
- O jogo retail NAO carrega cena de arquivos soltos: sem o .str fica no LOADING (testado com mods\...\X.nao_existe, que
  esconde um arquivo do jogo: kernel_io.cpp HiddenByMods). C\Debug\config.txt (EnableAllResFiles, ResourceFileOverride,
  BuildResourceFiles...) e o config de desenvolvedor, mas o exe retail nao le esse arquivo (nao ha a string).
- F11 = captura de modelos pela GPU (gpu/model_capture.cpp): malhas desenhadas que vem do stream de uma cena extraida ->
  mods\...\X.x360.str\_modelos (espaco do objeto, com o layout do draw em comentarios). Serviu de gabarito para decifrar
  o .xgs; com o zip ela e redundante. CARS_MODELOS_AT=s1,s2 liga/salva sem tecla (testes).
- Descompilacao completa (12/10/2026): `python -I tools/scenes_decompile.py [--so CR_01,HUB] [--comparar "<jogo>\cenas_soltas"]`
  -> "<jogo>\cenas_extraidas\<Cena>\..." no layout do zip (Geo\*.xng, Tex\*.dds de PC + *.xnm em texto, colisoes .gol,
  Eco/Anim/Skies/.scn/.got/.mcf/.tsf copiados de C\Scenes, _pacote\ = .soi/.toc). Le as pastas de mods (monta o stream com
  `CarsRecomp.exe --montar`). Resultado: 5201 xng, 3453 dds, 1610 xnm, 800 gol. Iguais ao zip byte a byte na maioria;
  diferencas = zip veio do Hi-Octane (Mater-National): HUB tem 32 modelos diferentes, ~148 materiais adaptados, mips
  pequenos dos .dds do zip com lixo/zero. Um material fica .bpb (nao converte).
  - Tipo do recurso no .toc (+0x114): 0 modelo, 1 textura, 2 colisao (modelo/colisao tem FFFFFFFF em +0x104). Trecho A
    do registro de stream = dados da GPU (texturas, modelos); trecho B = CPU (colisoes). Nomes "C\" ou "c\" (CT_SM),
    espacos no .toc que o xgs nao tem ("wood catwalk01") -> comparar sem espacos.
  - Colisao: cabecalho no .soi 00 00 04 D2 'col\0', versao 2, flag +0x0C (9 = no stream, 0 = .gol), +0x48/+0x4C/+0x50 =
    vertices/nos/triangulos; nome ate 0x400 bytes antes. Blob no stream: vertices float4, nos alinhados a 32 (64 B {min4,
    max4, filhos, A, B, 0, pad}), triangulos 32 B {normal4, d, i0 i1 i2, pad}. .gol = cabecalho 0x58 (flag 0) + vertices
    float3 + u32 0 + nos 32 B {A, min3, max3, filhos} + triangulos 22 B {d, n3, idx3}.
  - BPB (INI binario, "BPBh" ou "BPB\0"): cabecalho >IIHHIIII, registros de 16 B {hash, nome | tipo (0 texto, 2 int,
    3 float), sufixo (ffff = nenhum), contagem/primeiro ou valor}; secoes primeiro.
  - MiniMap: stream sem registros; cada peca tem o seu "Section_N" no .toc terminando em {posicao, total, total, 0} logo
    antes do nome (posicao da textura relativa a ela). CR_01 guarda as pecas como DDS de PC inteiros no .str. As pecas
    ficam de baixo para cima (linha 0 = embaixo).
- CENA EM ARQUIVOS SOLTOS (12/10/2026): mods\C\Scenes\<Cena> = copia de cenas_extraidas (geo\*.xng, tex\*.dds + *.xnm,
  col\*.gol, Eco, Anim, .scn...) + _pacote\X.x360.soi/.toc (esqueleto). Quando o jogo abre C\Scenes\...\X.x360.res/.str
  e existe <pasta>\_pacote\X.x360.soi, scene_assets.cpp (BuildLoosePack) gera .soi/.toc/.str novos a partir dos arquivos
  (kernel_io.cpp FindModOverride: isLoose; prioridade: arquivo > pasta X.res/X.str antiga > _pacote). Os .xnm.bpb NAO entram
  no .res: o jogo procura X.xnm.bpb e cai no X.xnm em texto (mesmo leitor de INI do config.ini). Modelo/textura/colisao de
  outro tamanho: o recurso e reempacotado no setor (A 0x1000, B 0x80), blocos/tabela do .toc refeitos, vaga de memoria
  (soi +0x50, indice no cabecalho do setor) aumentada. .dds de outro tamanho/mips/formato: fetch nova (MakeFetch).
  Se faltar/estiver invalido um arquivo, o log diz "[mods] ERRO cena solta ..." e o jogo usa o pacote original.
  `CarsRecomp --montar-solto <raiz que espelha o jogo> C\Scenes\X\X.x360.res <saida>` gera <saida>.res/.str sem o jogo.
  Conferido nos 30 pacotes: .soi e .toc identicos, modelos identicos, colisoes identicas (6 com 1 ULP no raio dos nos),
  texturas iguais menos o enchimento dos tiles. Pastas antigas X.x360.res\/X.x360.str\ movidas para
  "<jogo>\_mods_pastas_antigas" (scenes_extract.py ainda gera esse formato; scenes_decompile.py le dali, --origem).
  No jogo: FE montada em ~65 ms, tela de titulo igual; teste com o logo (PS360_RaceORama_LOGO.xng com x*0.5 e RoR_Logo.dds
  trocado por 256x128 listrado, original 512x256) apareceu certo. _pacote\X.arquivos.txt = CRC32 de cada arquivo como foi
  extraido: o runtime loga "(modificado)" no que mudou. Sem input o attract agora toca attract_720p.bik (nao corrida).
  - scenes_decompile.py: mesmo recurso em dois pacotes da pasta (X e X_Landmark) -> fica o do 1o (first_write); texturas
    so no .soi (86 DDS no HUB) exportadas; bug corrigido: a posicao "Section_N" do MiniMap so vale sem registros
    (antes deslocava texturas de pacotes normais quando os 16 bytes antes do nome pareciam um registro).
  - .soi (gramatica conferida em todos): cabecalho 15 u32 [versao 0x102 (0x101 = MiniMap, stream sem blocos: registro do
    setor no .toc so {pos, total, A, B}), 0x810, setores, colisoes, modelos, 0, texturas do stream, texturas so no .soi,
    vagas A, vagas B, pos modelos, (= pos modelos), pos colisoes, pos texturas, 0]; posicoes relativas a 0x3C apontam o
    indice do 1o registro. Info em 0x3C (0x50: tamanho de cada vaga A e depois B). Registro = 0x3C de posicionamento
    {0, Position, LookVector, UpVector, 0, 0} ANTES dele (o 1o fica na info) + u32 indice (= indice do .toc; -1 nas texturas
    so do .soi) + nome[0x100] + u32 0 + u32 0/-1 + u32 nprops + nprops x 0x208 {nome[0x100], valor[0x100], 8} + conteudo:
    textura 0x38 (fetch em +0x20) ou {u32 tamanho, DDS de PC} (86 no HUB, depois das texturas do stream; MiniMap CR_01),
    xgs, colisao 0x7C (cabecalho do .gol com flag 9 + 0 + no raiz). Arquivo termina no fim do ultimo conteudo.
  - .toc: setores {nome[0x100], 21 u32 [0, nres, nA, nB, -1, vaga A, vaga B, 8 vizinhos, caixa 6 floats], registro
    {pos no .str, total, A, B, A no .str, B no .str, blocos A, blocos B} + tabela, nres x 0x118}; sem offsets absolutos.
  - No da arvore de colisao no stream (64 B): {minimo xyz,1, tamanho xyz,1, filhos u32, A = 8*sx*sy*sz, B = sqrtf((sx*sx +
    sy*sy) + sz*sz) em float, 0, 16 zeros}; vertices xyz,1; triangulos {normal,1, d, 3 u16, 6 zeros}.
- Diagnostico: CARS_LOG_READS=texto;texto (leituras com offset/tamanho/pilha guest), CARS_LOG_INFLATE=1 (as duas copias
  do inflate do zlib 1.0: sub_822731D8 e sub_823CF728); "[mods] AVISO: uncompress de dados sem cabecalho zlib" = bloco
  cru nao rastreado. Testes com run.ps1 usam build\Release\mods: passe CARS_MODS="<jogo>\mods".

## Perfil e saves (12/10/2026) - src/kernel_xam.cpp
- Perfil local SEMPRE logado no controle 1 (XamUserGetSigninState = 1, sem o aviso "not signed in"): gamertag = usuario do
  Windows (<jogo>\savegame\perfil.ini [Perfil] Nome=), XUID offline E000000043617273. XamUserReadProfileSettings
  devolve padroes (vibracao 3 etc.; o jogo so le 10040003); sem conquistas (enumerador vazio).
- Saves: XamContentCreateEx monta a raiz que o jogo pede ("Cars") na pasta <jogo>\savegame\<arquivo>\ (o usuario pediu
  a pasta do jogo, nao Documentos) + <arquivo>.info com tipo/nome mostrado (para XamContentCreateEnumerator/XamEnumerate).
  kernel_io ResolvePath chama
  XamResolveContentPath ("Cars:\PSLOT0.dat"). O jogo grava PSLOT0.dat..N (modo historia, ~312 KB, "SAVE 1 - A") e
  GlobalData ("HALL OF FAME"). XamShowDeviceSelectorUI = disco rigido; XamShowMessageBoxUIEx responde o botao ativo
  (loga o texto); telas do sistema mandam XN_SYS_UI 1/0. Notificacoes como o Xenia (XN_SYS_UI 0 + SIGNINCHANGED 1 para o
  1o ouvinte do sistema, Live desconectado). XMsgStartIORequest(Ex) completa o XOVERLAPPED na hora (app FB msg B0006 etc.).
  XOVERLAPPED: +0 resultado, +4 tamanho, +0xC evento, +0x10 rotina (APC: resultado, tamanho, ov), +0x18 erro estendido.
  CARS_SAVES=<pasta> troca o lugar (testes). Testado: criar (historia -> pista CR_01 carregou) e carregar ("Loading Game").
- kernel_io: _wfopen (caminhos com acento), FILE_DIRECTORY_FILE cria pasta, NtSetInformationFile 20/19 (tamanho),
  13 (apagar ao fechar: KObject::deleteOnClose) e 10 (renomear).
- Testes com run.ps1: o XInput le o CONTROLE FISICO do usuario mesmo com a janela oculta ([input] que nao sao do teste).
- Textos sem "Xbox 360" (12/10/2026): `python -I tools/textos_pc.py [--desfazer]` grava em mods\<L>\Loc\ os
  <L>_L_TRC_X360.bin e <L>_L_CarsFrontEndUI.bin (E/F/G/I/S) com "console Xbox 360" -> computer/ordinateur/Computer/
  computer/ordenador e "controle Xbox 360" -> so "controle". Formato: u32 bytes do bloco, BOM FF FE, textos UTF-16LE
  separados por 0 (o jogo pega pelo indice; C\Loc\X.loc = quantidade + nomes STR_*), depois 1 byte de atributos por
  texto (mantidos). So esses dois bancos falam de Xbox; o resto ("xbox360_A_button"...) sao nomes de textura.

## Log de sessao (10/10/2026) - ONDE OLHAR QUANDO O USUARIO ACHAR ALGO ESTRANHO
- Cada vez que o jogo abre: "C:\Games\Cars Race-O-Rama Recomp\logs\AAAA-MM-DD_HH-MM-SS\jogo.log" (ficam as 30 ultimas sessoes).
  session_log.cpp: stdout/stderr -> pipe -> thread "log da sessao" poe HH:MM:SS.mmm (hora local) em cada linha; com stderr
  redirecionado (tools/run.ps1) o texto original continua indo para run.err.log tambem (sem horario). Limite 256 MB.
- Conteudo: cabecalho [sessao] (exe, Windows, CPU, RAM, GPU/driver, monitor, FPS/VSync), [gpu] por segundo, [input] cada
  botao apertado/solto (input.cpp), [arquivo] cada arquivo aberto (ate 10x o mesmo; kernel_io.cpp), [janela] foco/minimizar,
  [travou?] nenhum swap ha 6 s + pilhas guest (MonitorThread), [CRASH] com pilha guest, "[sessao] fim (motivo)".
  Sem a linha de fim = processo morto/fechado a forca.
- F12 = marca: "[MARCA NN]" no log + pasta marca_NN_HH-MM-SS com tela_01..20 (JPEG, ultimos 5 s, 4/s: anel de capturas no
  renderer, namespace capture, thread "capturas"), tela_agora.png, som_ultimos_10s.wav (kernel_audio.cpp, anel estereo 16
  bits), frame.txt (trace completo do frame seguinte) e frame_N*.png (frame, texturas e RT de cada resolve). Titulo da janela
  avisa "MARCA NN salva". Custo: ~0,5 s de engasgo no F12 (copias da GPU do trace); as capturas periodicas nao custam fps.
- Ler: `python -I tools/sessao.py` (ultima sessao: duracao, como terminou, fps, problemas, marcas com contexto),
  `--lista`, `-n 2` (penultima), `--marca 3` (tudo dos 20 s antes da marca), ou um caminho de pasta.
- CARS_MARK_AT=segundos[,...]: marca sem tecla (so para testar com a janela minimizada; nao e input do jogo).
- Dumps (CARS_DUMP_FRAMES, CARS_DUMP_RESOLVE, F12) agora saem em PNG (WIC, WriteImageFile em session_log.cpp).

## Ferramentas de depuracao
- NAO deixar input automatico no jogo (o usuario joga no teclado; um Start automatico de teste abria o menu de pausa a cada 3 s).
- Estatistica `[gpu]` por segundo: fps, pior frame, draws, "CP trabalho" (tempo real do processador de comandos) e "esperando CPU"
  (WAIT_REG_MEM), shaders compilados e varreduras. CARS_THREADS=1: uso de CPU por thread a cada 5 s (`[threads]`, nomes guest N (entrada)).
- CARS_HOST_SAMPLE=1 (todas) ou =texto do nome da thread (ex. 820A8440, "gpu cp"): profiler nativo por RIP; simbolizar com
  `python -I tools/symbolize.py build/CarsRecomp.map run.err.log all -1` (funcoes recompiladas aparecem como sub_XXXXXXXX).
- CARS_LOG_PAUSE=1: loga quem chama a pausa nativa (sub_82162350) e os eventos de sistema (sub_820F8758).
- tools/run.ps1 -Seconds N [-Dump "f1,f2"] [-Trace] [-GpuLog] [-Extra "--novsync"]: roda com janela minimizada (CARS_HIDDEN_WINDOW, ignora WM_CLOSE).
  Sem input, depois do titulo o jogo entra no modo demonstracao (corridas 3D): bom para medir desempenho.
- Variaveis: CARS_DUMP_FRAMES, CARS_DUMP_RESOLVE=enderecos, CARS_TRACE_FRAME=N (todos os draws/resolves do frame,
  dumps do RT em cada resolve), CARS_GPU_LOG, CARS_STACKS (pilhas guest a cada 12 s), CARS_SAMPLE (profiler estatistico),
  CARS_D3D12_DEBUG. Crash handler mostra rip, registradores, lr guest; build/CarsRecomp.map (lld /lldmap) para simbolizar.
- tools/find_missing_switches.py: acha jump tables que faltam no cars_switch_tables.toml (2 inline foram adicionadas: 0x82167698, 0x8217CEDC;
  override de tamanho 0x8217CEC8 no cars.toml). Backup antes disso em C:\DEB\cars\backup_20261009.
- Teclado: Enter=Start, Tab=Back, Espaco=A, Esc/Backspace/X=B, C=X, V=Y, Q/E=LB/RB, setas=direcional/analogico, W=acelerar, S=freio, IJKL=analogico dir.

## Mapa do D3D (D3D9LTCG estatico, sem simbolos)
- Hooks: `sub_X` no codigo gerado e alias FRACO de `__imp__sub_X`; definir `PPC_FUNC(sub_X)` no runtime e chamar `__imp__sub_X` dentro
  sobrescreve a funcao (inclusive chamadas indiretas). `tools/gen_d3d_profile.ps1` gera `src/d3d_profile_gen.cpp` com wrappers para os arquivos 30 e 31
  (0x822BABF8..0x822E4C70); `--profile` grava `d3d_profile.txt` (chamadas/frame, chamadores externos) e loga `[args]` (lista g_logAddrs em d3d_profile.cpp).
- O D3D vai de ~0x822BABF8 a ~0x822D3000. Device = 0x201F2D80 (r3). Frontbuffer: textura 1280x720 em fisico 0x00411000.
- Funcoes (tipos de primitiva: 4 TRILIST, 5 TRIFAN, 6 TRISTRIP, 8 RECTLIST, 13 QUADLIST):
  - 0x822BD308 DrawIndexed (de 0x820B1DC4); 0x822BC560 DrawVerticesUP-like; 0x822BCF18 QUADLIST (UI, de 0x820A2BB0)
  - 0x822C1738 Resolve; 0x822C4DF0 Swap (de 0x820BB4E4); 0x822C48B8 pre-swap; 0x822BBB38 kick do ring (bit 0x02 em device+10941 = modo degradado)
  - 0x822BBC38 espera de fence ([device+10896] >= alvo); 0x822C2670 detector de GPU travada (laco de espera)
  - 0x822C68A0 grava blocos de registradores; 0x822C6C38 grava constantes; 0x822BD978/0x822BDC78/0x822BDF08 headers de textura
  - 0x822BC0F8 cria device; 0x822BBF50 callback de interrupcao; 0x822CA610 VdInitializeEngines; 0x822CFAD0/0x822D0110 setup do scaler (usam VdQueryVideoMode)
- Com LTCG, SetRenderState/SetTexture sao inline no jogo: por isso o backend le o estado dos registradores PM4 (como o Xenia), nao de hooks.
## Proximos passos sugeridos
1. Corrigir geometria esticada nas corridas (conferir vfetch reescrito: offset/stride/formato por elemento; indices; VGT_INDX_OFFSET).
2. Desempenho: cache de vertex/index buffers e de texturas sem re-hash por frame.
3. Audio: enviar XAudioSubmitRenderDriverFrame ao host (WASAPI/XAudio2) e decodificar XMA (xma_context do Xenia em C:\DEB\ref\xenia\src\xenia\apu).
4. XAM: perfil, saves (XamContent*), XamShow*UI com XOVERLAPPED (layout: +0 resultado, +4 tamanho, +0xC evento, +0x10 rotina) - ver xam_*.cc do Xenia.
## Dicas de trabalho
- Compilar os 78 arquivos gerados é lento; mudanças só no runtime recompilam rápido. Verificação de sintaxe sem Clang: g++ -std=c++20 -msse4.1 -fsyntax-only com um shim de simde.
- `findstr` do Windows: use `findstr /s /m /c:"texto" ppc\*.cpp`.
- Nunca execute greps gigantes em todos os ppc_recomp.*.cpp sem timeout.
