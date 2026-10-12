#pragma once
// Captura de modelos das cenas (F11 liga; F11 de novo salva): cada malha desenhada cujos vertices vem do stream de uma
// cena extraida (mods\...\X.x360.str\) vira um .obj em X.x360.str\_modelos\malhas\ (+ .mtl com a textura .dds da
// pasta), e todas juntas em _modelos\<cena>.obj. Ver model_capture.cpp.
#include <cstdint>
#include <string>

struct DrawCall;
struct GuestShader;

// Um bloco do stream foi copiado para a memoria guest (hook do uncompress em game_hooks.cpp)
void ModelCaptureStreamBlock(uint32_t guest, uint32_t size, const std::string& stream, uint64_t offset);
// Posicao de uma textura no stream (scene_assets.cpp, ao montar a pasta)
void ModelCaptureTexture(const std::string& stream, uint64_t offset, const std::string& name);
bool ModelCaptureActive();
void ModelCaptureDraw(const DrawCall& draw, const GuestShader* vs, const GuestShader* ps, const uint32_t* regs);
// F11: liga a captura, ou salva e desliga; devolve o texto para o titulo da janela
std::wstring ModelCaptureToggle();
// Janela fechando: se a captura estiver ligada, salva
void ModelCaptureFinish();
