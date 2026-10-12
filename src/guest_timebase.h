// Incluido a forca (/FI) em todo o codigo gerado pelo XenonRecomp.
// O XenonRecomp traduz mftb para __rdtsc(): o TSC do PC (GHz), mas o jogo espera o timebase do Xbox 360
// (50 MHz, o mesmo valor de KeQueryPerformanceFrequency). Sem isso o tempo do jogo passa dezenas de vezes mais rapido.
#pragma once
#include <cstdint>
#include <intrin.h>
#include <immintrin.h>
#include <x86intrin.h>

uint64_t GuestTimebase();

#define __rdtsc() GuestTimebase()
