#pragma once
// Scene model capture (F11 starts; F11 again saves): every drawn mesh whose vertices come from the stream of an
// extracted scene (mods\...\X.x360.str\) becomes an .obj in X.x360.str\_models\meshes\ (+ .mtl with the folder's .dds
// texture), and all of them together in _models\<scene>.obj. See model_capture.cpp.
#include <cstdint>
#include <string>

struct DrawCall;
struct GuestShader;

// A stream block was copied into guest memory (uncompress hook in game_hooks.cpp)
void ModelCaptureStreamBlock(uint32_t guest, uint32_t size, const std::string& stream, uint64_t offset);
// Position of a texture in the stream (scene_assets.cpp, while building the folder)
void ModelCaptureTexture(const std::string& stream, uint64_t offset, const std::string& name);
bool ModelCaptureActive();
void ModelCaptureDraw(const DrawCall& draw, const GuestShader* vs, const GuestShader* ps, const uint32_t* regs);
// F11: starts capturing, or saves and stops; returns the text for the window title
std::wstring ModelCaptureToggle();
// Window closing: if capturing, save
void ModelCaptureFinish();
