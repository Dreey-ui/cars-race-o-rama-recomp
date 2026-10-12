#pragma once
#include <cstdint>

// Vigilancia de escrita da CPU em paginas (4 KB) da memoria fisica guest, para os caches da GPU (vertex/index buffers):
// uma pagina vigiada fica PAGE_READONLY nas 3 vistas fisicas; a primeira escrita da CPU gera uma excecao que a marca como
// suja e libera a escrita. Paginas escritas com frequencia viram "dinamicas" por um tempo e nao sao vigiadas (o cache
// usa o caminho sem cache para elas).

// Todas as paginas da faixa estao vigiadas e sem escrita desde que foram vigiadas?
bool MemWatchIsClean(uint32_t phys, uint32_t size);
// Alguma pagina da faixa e dinamica agora?
bool MemWatchIsDynamic(uint32_t phys, uint32_t size);
// Passa a vigiar a faixa (depois de copiar os dados). false se alguma pagina e dinamica (nada e vigiado)
bool MemWatchProtect(uint32_t phys, uint32_t size);
// Handler de excecao: escrita (guestAddr = endereco guest em qualquer vista) numa pagina vigiada. true = tratado
bool MemWatchHandleWriteFault(uint32_t guestAddr);
// Antes de uma escrita que nao gera excecao (ReadFile do Windows direto na memoria guest): libera e marca suja
void MemWatchInvalidate(uint32_t guestAddr, uint32_t size);
// Muda sempre que alguma pagina vigiada e escrita: um cache que guardou o valor e ve o mesmo ainda esta valido
uint32_t MemWatchEpoch();
// Contador de frames (para a heuristica de paginas dinamicas)
void MemWatchSetFrame(uint64_t frame);
// Estatistica: excecoes de escrita desde a ultima chamada
uint32_t MemWatchTakeFaults();
