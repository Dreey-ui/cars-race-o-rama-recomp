#pragma once
// Folders extracted from the scenes (tools/scenes_extract.py) instead of the game's packages:
//   mods\...\X.x360.res\  files inside the package -> uncompressed .res
//   mods\...\X.x360.str\  .dds textures + _data\*.bin (geometry) -> raw stream (128 KB blocks without zlib)
// Scene as loose files (tools/scenes_decompile.py, layout of "<game>\extracted_scenes"):
//   mods\C\Scenes\<Scene>\_package\X.x360.soi/.toc + geo\*.xng, tex\*.dds, col\*.gol, tex\*.xnm -> built .res and .str
// Each folder is built once per run into a temporary file (deleted when the game closes); editing a file takes effect
// the next time the game starts.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

bool SceneFolderSize(const std::filesystem::path& folder, bool stream, uint64_t& size);
FILE* SceneFolderOpen(const std::filesystem::path& folder, bool stream, uint64_t& size);

// packDir = package folder in mods (mods\C\Scenes\CR_01, ...\ShadowMap), stem = "CR_01.x360"; root = mods folder
// (mirror of the game folder). If building fails (missing/invalid file), Open returns nullptr and the error goes to the log.
bool LooseSceneExists(const std::filesystem::path& packDir, const std::string& stem);
FILE* LooseSceneOpen(const std::filesystem::path& root, const std::filesystem::path& packDir, const std::string& stem, bool stream,
                     uint64_t& size, bool& rawBlocks);
bool LooseSceneSize(const std::filesystem::path& root, const std::filesystem::path& packDir, const std::string& stem, bool stream,
                    uint64_t& size);
bool LooseSceneBuildFiles(const std::filesystem::path& root, const std::filesystem::path& packDir, const std::string& stem,
                          const std::filesystem::path& outRes, const std::filesystem::path& outStr);
