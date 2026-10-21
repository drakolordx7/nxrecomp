# NVN struct sizes: cross-check

Source A is `refs/mp1r-practice-mod/src_framework/nvn/nvn.h`. Generated sizes come from `scripts/gen_nvn_table.py` (run output in `src/gfx/nvn/nvn_sizes.inc`). Only sizes that another source states explicitly are listed. Each row cites file:line.

| struct | size in A | size in other source (file:line) | agree? |
|---|---|---|---|
| NVNsampler | 0x30 (`char reserved[0x30]`) | 0x30: `smo-recomp/runtime/gfx/nvn_front.h:22`, `ssbu-recomp/runtime/nvn_front.h:22` ("48 bytes = NVNsampler (0x30)") | yes |
| NVNtextureView | 0x28 (`char reserved[0x28]`) | 0x28: `smo-recomp/runtime/gfx/nvn_front.h:23`, `ssbu-recomp/runtime/nvn_front.h:23`, `LASRecomp/runtime/nvn/nvn.h:49` | yes |
| NVNtextureBuilder | 0x80 (`char reserved[0x80]`) | 0x80: `smo-recomp/runtime/gfx/nvn_front.h:24`, `ssbu-recomp/runtime/nvn_front.h:24` | yes |
| NVNqueueBuilder | 0x40 (`char reserved[0x40]`) | 0x40: `smo-recomp/runtime/gfx/nvn_gl.c:4970` | yes |
| NVNtextureSparseTileLayout | none: A has only `char dummy[1]` (placeholder), so the generator skips it | 16 bytes: `smo-recomp/runtime/gfx/nvn_gl.c:4428` ("the 16 bytes"); "4 words": `smo-recomp/runtime/gfx/nvn_sdk.h:17` (word size not stated) | not comparable (A gives no size) |

Result: 8 explicit size claims in the scanned sources, all 8 agree with A. Zero disagreements.

Notes:
- The smo and ssbu `nvn_front.h` comments are the same text, so they count as two copies of one claim, not two independent sources.
- `refs/SuperMarioWonderRecomp/docs/nvn/*.md`, `refs/ScarletNX/docs`, `refs/deko3d`: no NVN struct sizes stated.
- `refs/deko3d` (Primer.md, README.md) has no NVN object layout facts. It is a separate API.
- Structs skipped by the generator (placeholder `dummy` arrays, or a `layout[8]` placeholder) are listed in the generator's stdout, not in this table.
