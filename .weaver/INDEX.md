# tools/.weaver index

- `shaders/oracle/run_oracle.py --dksh shaders/corpus/sascha_dksh --out <dir> --expect shaders/oracle/expected_sascha.json`: both Maxwell->SPIR-V translators + spirv-val, PASS/FAIL against the locked baseline.
- `shaders/scripts/build_corpus.py`: Vulkan GLSL -> uam dialect -> .dksh (uam runs in WSL).
- `shaders/scripts/dksh_to_corpus.py`, `corpus_to_mxs.py`: .dksh -> harness corpus layout -> smo stage files.
- `textures/scripts/build.ps1`: build astc2bc7 (CMake+Ninja+clang-cl). `textures/scripts/make_astc_corpus.py`: encode the CC0 corpus to ASTC.
- `textures/oracle/run_oracle.py`: ASTC->BC7 PSNR/SSIM/alpha/passthrough oracle; `--sweep` for the quality table.
- `tiling/scripts/build.ps1`, `tiling/oracle/run_oracle.py`: tile/untile library build and cross-check against Ryujinx.
- `survey/` (S4c asset survey; run order and numbers in `docs/mpr/survey/SURVEY.md`): `extract_all.ps1` (all paks -> work/survey/paks via retrotool), `inventory.py` (per-type counts/sizes from the PACK directory), `extract_shaders.py <MaterialArchive.arc> --out <corpus> [--paks work/survey/paks]` (NVN program sets -> harness corpus), `txtr_tally.py` (TXTR header tally), `txtr_pipeline.py layout|samples|bulk` (untile/ASTC->BC7/PSNR/timing on real textures), `movies.py` + `mf_decode_test.cpp` (FMV0 -> mp4, ffprobe, Media Foundation decode), `audio_symbols.py` (ELF audio symbols), `ui_survey.py` (GUIF/Scaleform search).
