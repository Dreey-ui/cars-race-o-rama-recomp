#pragma once
// Pastas extraidas das cenas (tools/scenes_extract.py) no lugar dos pacotes do jogo:
//   mods\...\X.x360.res\  arquivos internos do pacote -> .res sem compressao
//   mods\...\X.x360.str\  texturas .dds + _dados\*.bin (geometria) -> stream cru (blocos de 128 KB sem zlib)
// Cena em arquivos soltos (tools/scenes_decompile.py, layout de "<jogo>\cenas_extraidas"):
//   mods\C\Scenes\<Cena>\_pacote\X.x360.soi/.toc + geo\*.xng, tex\*.dds, col\*.gol, tex\*.xnm -> .res e .str montados
// Cada pasta e montada uma vez por execucao num arquivo temporario (apagado ao fechar o jogo); editar um arquivo vale na
// proxima vez que o jogo abrir.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

bool SceneFolderSize(const std::filesystem::path& folder, bool stream, uint64_t& size);
FILE* SceneFolderOpen(const std::filesystem::path& folder, bool stream, uint64_t& size);

// packDir = pasta do pacote nos mods (mods\C\Scenes\CR_01, ...\ShadowMap), stem = "CR_01.x360"; root = pasta dos mods
// (espelho da pasta do jogo). Se a montagem falhar (arquivo faltando/invalido), Open devolve nullptr e o erro vai pro log.
bool LooseSceneExists(const std::filesystem::path& packDir, const std::string& stem);
FILE* LooseSceneOpen(const std::filesystem::path& root, const std::filesystem::path& packDir, const std::string& stem, bool stream,
                     uint64_t& size, bool& rawBlocks);
bool LooseSceneSize(const std::filesystem::path& root, const std::filesystem::path& packDir, const std::string& stem, bool stream,
                    uint64_t& size);
bool LooseSceneBuildFiles(const std::filesystem::path& root, const std::filesystem::path& packDir, const std::string& stem,
                          const std::filesystem::path& outRes, const std::filesystem::path& outStr);
