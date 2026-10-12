#pragma once
// Log de sessao: cada vez que o jogo abre cria <pasta do exe>\logs\AAAA-MM-DD_HH-MM-SS\ com jogo.log (tudo que o runtime
// escreve em stdout/stderr, com o horario em cada linha). F12 cria marca_NN\ dentro dela (capturas, som, trace do frame).
#include <cstdint>
#include <filesystem>

void SessionLogStart();                                   // primeira coisa do main
const std::filesystem::path& SessionDir();                // vazio se a pasta nao pode ser criada
void SessionLogFlush();                                   // espera tudo chegar no arquivo (crash, fim)
void SessionLogEnd(const char* reason);                   // linha final "[sessao] fim" + flush
std::filesystem::path SessionNewMark(int& index);         // cria marca_NN_HH-MM-SS e devolve o caminho
// Imagem 24 bits BGR (linhas de cima para baixo) em PNG ou JPEG (WIC)
bool WriteImageFile(const std::filesystem::path& path, const uint8_t* bgr, uint32_t w, uint32_t h, uint32_t stride, bool jpeg);
