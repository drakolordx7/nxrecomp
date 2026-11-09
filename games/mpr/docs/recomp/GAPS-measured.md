# Lifter gaps measured on Metroid Prime Remastered

Moved from the base's `recomp/docs/GAPS.md` (the general gaps stay there). Paths like `docs/S1b.md` now mean
`docs/recomp/S1b.md` in this repo; `debug 007` / `debug 008` are the base's `recomp/docs/debug/` notes.

## Measured on the game (S1b, docs/recomp/S1b.md)

- `cover` is 100% on `main`, `sdk` and `subsdk0`; code outside every discovered function (hand-written SDK stubs) was found by a separate gap scan and is lifted (debug 008).
- Jump tables: 881 / 622 / 335 resolved; every other `br` is an indirect tail call (debug 007), 6 keep the dispatch fallback.
- Compile time and size: 6.5 M instructions lift to 453 MB of C++ and compile cold in about two minutes (docs/S1b.md); no function reaches `optnone_insns`.
- C++ exceptions: only `sdk` has LSDAs; no typed catch and 13 empty `throw()` specifications. Class-hierarchy matching and the unwinder are the runtime's.
- Still not covered: hook sites on the real game (none configured), FPCR (3 writes, all in unreferenced SDK stubs), and anything that only a run can show.
