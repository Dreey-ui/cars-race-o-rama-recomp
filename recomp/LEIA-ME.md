# Recompilação (XenonRecomp)

- `xenonrecomp_recompiler.patch` — instruções PowerPC que o jogo usa e o XenonRecomp não tinha (ADDC, ADDME, SUBFZE, EQV,
  BDZF, DCBST, LHBRX, LHZU/STHU, LFSU/STFSU, LFDU/STFDU, carregamentos/gravações indexadas com update, FRSQRTE, VADDSWS,
  VSUBSHS, VPKSWSS128, VSEL128, VCMPBFP...). Feito sobre o commit `ddd128bc` do
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): `git apply xenonrecomp_recompiler.patch` dentro do clone.
- `cars.toml` — configuração do XenonRecomp (endereços de save/restore de registradores, `setjmp`/`longjmp`, tamanhos de
  funções corrigidos pelo `.pdata`).
- `cars_switch_tables.toml` — 140 tabelas de salto (o XenonAnalyse não acha nenhuma neste jogo).

Coloque os `.toml` ao lado do seu `default_dec.xex` (descriptografado e descomprimido, por exemplo com o `xextool -e u -c u`)
e rode `XenonRecomp cars.toml <ppc_context.h>`; a pasta `ppc/` gerada é o `PPC_DIR` do CMake.
