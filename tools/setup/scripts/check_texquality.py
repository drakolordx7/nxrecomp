#!/usr/bin/env python3
"""Real-data BC7 quality check for tools/setup (ASTC -> BC7 as setup does it) on the game's own textures.

Runs mpr_texq (tools/setup/tools/texq.cpp) over every ASTC texture of an extracted RomFS (level 0, layer 0): ASTC decode ->
BC7 at the setup quality -> reference BC7 decode (bc7enc_rdo bc7decomp_ref) -> PSNR and SSIM against the ASTC decode. Prints
min / mean PSNR RGB, min PSNR alpha and min / mean SSIM per texture class, optionally next to the first-version converter
(--baseline: opaque blocks without mode 6 and without channel rotation), and checks the floors below per texture.

Floors (per texture, setup quality 3):
  PSNR RGB   >= 33.0 dB   the floor of the locked ASTC->BC7 oracle (tools/textures/README.md): above what a broken encoder gives
                          (< 25 dB), below the ~40 dB called visually lossless, because BC7 cannot do better on the noisiest
                          two-channel normal maps (best case measured: 33.6 dB at quality 3, 33.8 at quality 5)
  PSNR alpha >= 32.0 dB   for textures with real alpha (measured worst: 32.6 dB, a 64x16 gradient strip)
  alpha      == 255 exactly on every opaque texture (PSNR alpha 99.00): shaders and blend states may read it
  SSIM luma  >= 0.95      luma only, a rough guide for normal maps; measured worst 0.962
Exemptions (EXEMPT): textures that cannot meet a PSNR floor in BC7 at any quality, each with the measured best value.

Usage: python check_texquality.py [--romfs DIR] [--quality 3] [--stride N] [--baseline] [--out DIR] [--threads N]
Exit 0 PASS, 1 FAIL, 2 tool missing.
"""
import argparse
import csv
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TEXQ = ROOT / "tools/setup/build/release/bin/mpr_texq.exe"

FLOOR_RGB, FLOOR_ALPHA, FLOOR_SSIM = 33.0, 32.0, 0.95
EXACT_ALPHA = 99.0   # texq reports 99.0 dB when the MSE is exactly zero

EXEMPT = {
    # 256x256 Astc5x5Srgb, random RGBA noise (sd about 60 on every channel, mean 128). BC7 stores 8 bits per texel and cannot
    # follow independent per-texel noise: 19.19 dB at quality 3, 19.26 at 5, 19.28 at 6, and perceptual weighting is worse.
    "09c36c30-1b03-472a-bfd1-ca30ef3946d1": "white-noise lookup texture, best BC7 result 19.3 dB (quality 6)",
}


def has_alpha(r):
    """True when at least one texel is not fully opaque (the exact count when the TSV has it)."""
    if "alpha_nonop_n" in r:
        return int(r["alpha_nonop_n"]) > 0
    return float(r["alpha_nonop"]) > 0


def klass(r):
    srgb = r["srgb"] == "1"
    alpha = has_alpha(r)
    name = ("sRGB colour" if srgb else "linear data") + (" + alpha" if alpha else "")
    if not srgb and not alpha and float(r["meanB"]) < 1.0 and float(r["sdB"]) < 1.0:
        name = "linear RG (B = 0, normal maps)"
    return name


def summarize(rows, cfg):
    groups = {}
    for r in rows:
        groups.setdefault(klass(r), []).append(r)
    out = []
    for name, rs in sorted(groups.items()):
        rgb = [float(r[cfg + "_rgb"]) for r in rs]
        ssim = [float(r[cfg + "_ssim"]) for r in rs]
        alpha = [float(r[cfg + "_a"]) for r in rs if has_alpha(r)]
        out.append((name, len(rs), min(rgb), sum(rgb) / len(rgb), min(alpha) if alpha else None, min(ssim), sum(ssim) / len(ssim)))
    rgb = [float(r[cfg + "_rgb"]) for r in rows]
    ssim = [float(r[cfg + "_ssim"]) for r in rows]
    alpha = [float(r[cfg + "_a"]) for r in rows if has_alpha(r)]
    out.append(("ALL", len(rows), min(rgb), sum(rgb) / len(rgb), min(alpha) if alpha else None, min(ssim), sum(ssim) / len(ssim)))
    return out


def print_table(title, table):
    print(f"\n{title}")
    print(f"  {'class':32s} {'n':>5s} {'rgb min':>8s} {'rgb mean':>9s} {'alpha min':>10s} {'ssim min':>9s} {'ssim mean':>10s}")
    for name, n, mn, mean, amin, smin, smean in table:
        a = f"{amin:10.2f}" if amin is not None else f"{'-':>10s}"
        print(f"  {name:32s} {n:5d} {mn:8.2f} {mean:9.2f} {a} {smin:9.4f} {smean:10.4f}")


def count_below(rows, cfg, limit):
    return sum(1 for r in rows if float(r[cfg + "_rgb"]) < limit)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--romfs", default=str(ROOT / "game/romfs"))
    ap.add_argument("--quality", type=int, default=3)
    ap.add_argument("--stride", type=int, default=1, help="probe every Nth texture (1 = all)")
    ap.add_argument("--baseline", action="store_true", help="also measure the first-version converter (opaque: no mode 6, no rotation)")
    ap.add_argument("--out", default=str(ROOT / "work/texq"))
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--tsv", help="use an existing mpr_texq TSV instead of running it (columns q<N> and q<N>l)")
    ap.add_argument("--texq", default=str(TEXQ))
    a = ap.parse_args()

    cfg = f"q{a.quality}"
    base = f"q{a.quality}l"
    tsv = Path(a.tsv) if a.tsv else Path(a.out) / f"quality_{cfg}.tsv"
    if not a.tsv:
        if not Path(a.texq).exists():
            print(f"FAIL: {a.texq} not found (pwsh tools\\setup\\scripts\\build.ps1 -Target mpr_texq)")
            return 2
        Path(a.out).mkdir(parents=True, exist_ok=True)
        cmd = [a.texq, "--romfs", a.romfs, "--configs", cfg + ("," + base if a.baseline else ""), "--stride", str(a.stride),
               "--out", str(tsv), "--threads", str(a.threads)]
        t0 = time.time()
        print("running:", " ".join(cmd))
        subprocess.run(cmd, check=True)
        print(f"probe took {time.time() - t0:.0f} s")
    rows = list(csv.DictReader(open(tsv), delimiter="\t"))
    if not rows:
        print("FAIL: no textures probed")
        return 1
    has_base = (base + "_rgb") in rows[0]
    print(f"{len(rows)} textures (level 0), floors: RGB {FLOOR_RGB} dB, alpha {FLOOR_ALPHA} dB, opaque alpha exact, SSIM {FLOOR_SSIM}")
    if has_base:
        print_table(f"BEFORE (first-version converter, quality {a.quality})", summarize(rows, base))
    print_table(f"AFTER (current converter, quality {a.quality})", summarize(rows, cfg))
    if has_base:
        print(f"\ntextures under 40 dB / 36 dB / 33 dB: before {count_below(rows, base, 40)} / {count_below(rows, base, 36)} / "
              f"{count_below(rows, base, 33)}, after {count_below(rows, cfg, 40)} / {count_below(rows, cfg, 36)} / {count_below(rows, cfg, 33)}")

    bad = []
    exempt_seen = []
    for r in rows:
        alpha_here = has_alpha(r)
        rgb, al, ss = float(r[cfg + "_rgb"]), float(r[cfg + "_a"]), float(r[cfg + "_ssim"])
        problems = []
        if rgb < FLOOR_RGB:
            problems.append(f"PSNR RGB {rgb:.2f} < {FLOOR_RGB}")
        if alpha_here and al < FLOOR_ALPHA:
            problems.append(f"PSNR alpha {al:.2f} < {FLOOR_ALPHA}")
        if not alpha_here and al < EXACT_ALPHA:
            problems.append(f"opaque texture, alpha not exactly 255 (PSNR alpha {al:.2f})")
        if ss < FLOOR_SSIM:
            problems.append(f"SSIM {ss:.4f} < {FLOOR_SSIM}")
        if not problems:
            continue
        if r["id"] in EXEMPT:
            exempt_seen.append((r["id"], EXEMPT[r["id"]], rgb))
            continue
        bad.append((r["id"], r["fmt"], r["w"], r["h"], problems))
    for tid, why, rgb in exempt_seen:
        print(f"exempt: {tid} ({why}); measured {rgb:.2f} dB")
    if bad:
        print(f"\nFAIL: {len(bad)} texture(s) below a floor")
        for tid, fmt, w, h, p in bad[:40]:
            print(f"  {tid} {fmt} {w}x{h}: {'; '.join(p)}")
        return 1
    print("\nPASS: every texture meets the floors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
