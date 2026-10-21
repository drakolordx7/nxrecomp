# The instruction-family loop (for workers)

You own one family of AArch64 instructions in the lifter. The oracle is a differential test: every instruction shape of the
family is executed by Unicorn (the reference) and by the C++ the lifter generates, from the same machine state; registers,
flags, TPIDR/FPCR and a memory window must be bit-identical.

## Paths

    RECOMP = D:/AI/Active/Metroid-Prime/recomp          PY = D:/AI/Active/Metroid-Prime/tools/.venv/Scripts/python.exe
    run from RECOMP:  PY oracle/difftest/difftest.py <cmd> --filter '<regex>' --work D:/tmp_e2e/dt_<yourname>

## Commands

- `all` = gen + build + check. gen lifts every corpus word whose SHAPE KEY matches the regex and writes the tests; build
  compiles them (clang-cl, takes a minute or two for a big filter); check runs Unicorn and the lifted code and compares.
- A shape key is `<mnemonic> <operand shape>` e.g. `fadd v.4s, v.4s, v.4s` or `ldr x, [x, #]!`, so a regex like
  `'^(fadd|fsub) '` selects by mnemonic (note the trailing space) and `'^fadd v'` only the vector forms.
- The last lines of `check` are `tests: N identical: M different: K untranslated corpus words: U (S of T shapes)` and a
  `DIFF <shape> <count> e.g. #<test index> <word>` line per failing shape. YOU ARE DONE when K = 0 and U = 0 for your
  filter. `kind special` tests feed NaN / infinity / zero / denormal operands: they matter as much as the others.
- `PY oracle/difftest/difftest.py show <test index> --work <dir>` prints the lifted statement and, for each register that
  differs, `init` (before), `ref` (Unicorn) and `host` (lifted). Use it on one failing test per failing shape.
- `<dir>/bad.json` maps failing shape keys to test indices; `<dir>/coverage.json` lists untranslated corpus words with the
  reason (`t2:<mnemonic>` = tier 2 has no handler; `t2:<mnemonic>:<why>` = a handler refused the form).
- A generated statement that does not compile is reported as `(generated code does not compile)`; the real compiler error is
  in the build output (`cmake --build` text; rerun `build` alone to see it).

## How the lifter is organised

- Tier 1: `nxrecomp/lift/a64.py` (bit-field decoder; class `Fn`) tries each word first. What it cannot do raises `Unimpl`.
- Tier 2: `nxrecomp/lift/tier2/` (Capstone-driven handlers `i_<mnemonic>(self, f, i, ops)` returning one C++ statement).
  The files `x_<name>.py` are mixin classes that come FIRST in the class's base list: a method with the same name there
  OVERRIDES the ported handler in `ext.py`, `fp.py`, `int.py`, `neon.py`, `neon_wide.py`, `vldst.py`, `misc.py`, `lse.py`.
  Do not edit those ported files: copy the handler you need to fix into your own `x_<name>.py` and change it there.
- Helper functions the generated C++ calls live in headers: `include/nxrecomp/ctx.h` (C/C++, shared with the runtime: do
  NOT edit), `cxx.h` (ported helpers in `namespace nx`, do not edit), `fpexact.h` / `neon.h` (exact FP and smo's SIMD helpers),
  and one header per worker stream `cxx_<name>.h` (inside `namespace nx { }`, already included at the end of cxx.h).
- Generated code is C++20 compiled by clang-cl with `-ffp-contract=off -fno-strict-aliasing`. State is `Ctx *c`: `c->x[n]`
  (x31 is SP, XZR is never stored: use the `Reg` helper of tier 2 which already handles it), `c->v[n]` (a `V128` union:
  `.u8 .s8 .u16 .s16 .u32 .s32 .u64 .s64 .f32 .f64`), flags `c->N c->Z c->C c->V` (0 or 1). Guest memory is host memory:
  `nx::ld<T>(addr)` / `nx::st<T>(addr, v)`.
- A write to a scalar FP register or a narrower-than-128-bit vector write zeroes the rest of the register (`nx::vzero()`).

## Rules

1. NEVER edit anything under `oracle/` (the oracle scripts are hash-locked; weakening a test to make it pass is forbidden)
   and never edit `include/nxrecomp/ctx.h`. Edit only the files your brief lists as yours.
2. The architectural pseudocode (Arm ARM) is the specification; Unicorn is the reference. Match Unicorn bit for bit,
   including NaN payloads/signs and the zero upper lanes. If you believe Unicorn is wrong for an encoding, report that case
   (index, word, why) instead of special-casing it.
3. No inline assembly. No libc beyond what C++20 `<cmath>`, `<bit>`, `<cstring>` give. Deterministic code only.
4. Work in BATCHES: first write the handlers/helpers for ALL mnemonics of your family, only then run `all` once, then read
   the DIFF lines, fix, run again. Do not run the test after each single handler. Use a small filter while debugging one
   shape, the full family filter for the final check.
5. If your edit of a shared file fails because it changed since you read it (other workers run in parallel), re-read it and
   redo the edit; keep edits small and local.
6. Do not git commit. Do not run other workers' filters. Use only your own `--work` directory.
7. Report: the final `tests: ... untranslated ...` line of your filter, the list of files you changed, any shape you could not
   finish with the test index, the Unicorn-vs-lifted values and your analysis.
