# Recompilation (XenonRecomp)

- `xenonrecomp_recompiler.patch`: PowerPC instructions the game uses that XenonRecomp did not have. The list:
  - ADDC, ADDME, SUBFZE, EQV, BDZF, DCBST, LHBRX.
  - LHZU/STHU, LFSU/STFSU, LFDU/STFDU.
  - Indexed loads/stores with update.
  - FRSQRTE, VADDSWS, VSUBSHS, VPKSWSS128, VSEL128, VCMPBFP...

  It is based on commit `ddd128bc` of [XenonRecomp](https://github.com/hedge-dev/XenonRecomp). Apply it with
  `git apply xenonrecomp_recompiler.patch` inside the clone.
- `cars.toml`: XenonRecomp configuration. It has the register save/restore addresses, `setjmp`/`longjmp`, and function
  sizes corrected from the `.pdata`.
- `cars_switch_tables.toml`: 140 jump tables. XenonAnalyse finds none in this game; the compiler of 2009 reorders the
  instructions around the `bctr`. They came from a custom scanner, and `tools/find_missing_switches.py` finds any that
  are still missing.

Put the `.toml` files next to your `default_dec.xex` (decrypted and decompressed, for example with `xextool -e u -c u`).
Then run `XenonRecomp cars.toml <ppc_context.h>`. The generated `ppc/` folder is CMake's `PPC_DIR`.

Still unimplemented in XenonRecomp: `vpkd3d128` (mode 3) and `vupkd3d128` (mode 12) emit `__builtin_debugtrap()`.
The game has not hit them so far.
