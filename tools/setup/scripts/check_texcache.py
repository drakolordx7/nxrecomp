#!/usr/bin/env python3
"""Quality sample of the BC7 texture cache written by tools/setup (src/texcache.cpp).

Input: a cache folder (NXB7 .bc7 files, from test_texcache) and a folder of reference sidecars (<key>.ref written by
test_texcache: u32 width, u32 height (little endian), then width*height*4 RGBA8 = the ASTC decode of level 0, layer 0).
For a sample of cache files (sorted by name, every k-th that has a reference) the level-0 layer-0 BC7 blocks are wrapped in
a DX10 DDS, decoded by the oracle bc7dec.exe (tools/textures/build/oracle) and compared with the reference:
  per file: PSNR RGB >= --min-psnr, PSNR alpha >= --min-alpha-psnr, SSIM(luma) >= --min-ssim
  sample:   mean PSNR RGB >= --min-mean-psnr
PSNR and SSIM are the functions of tools/textures/oracle/metrics.py (imported, not rewritten); the DDS header is the one
of tools/textures/oracle/run_oracle.py (make_dds).

Usage: python check_texcache.py --cache-dir <dir> --ref-dir <dir> [--romfs <dir>] [--n 40]
Exit 0 on PASS, 1 on FAIL, 2 when bc7dec or the python dependencies are missing.
"""

import argparse
import math
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path("D:/AI/Active/Metroid-Prime")
ORACLE = ROOT / "tools/textures/oracle"

try:
    import numpy as np
    sys.path.insert(0, str(ORACLE))
    import metrics  # oracle/metrics.py: psnr, ssim, decode_dds, metrics
    import run_oracle  # oracle/run_oracle.py: make_dds (DX10 DDS header)
except ImportError as exc:  # pragma: no cover
    print(f"FAIL: python dependency missing: {exc}")
    sys.exit(2)

VK_BC7_UNORM = 145
VK_BC7_SRGB = 146
HEADER = struct.Struct("<4sIQIII")        # magic, version, key, VkFormat, levels, layers (28 bytes)
LEVEL = struct.Struct("<IIIQQ")           # width, height, depth, offset, size (28 bytes)


def parse_cache(path):
    """Returns (key, vk_format, layers, (w, h, depth, layer0_bytes)) of level 0, after checking the whole file layout."""
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("file shorter than its header")
    magic, version, key, vk, levels, layers = HEADER.unpack_from(data, 0)
    if magic != b"NXB7" or version != 1:
        raise ValueError(f"bad magic/version {magic!r} {version}")
    if vk not in (VK_BC7_UNORM, VK_BC7_SRGB) or levels < 1 or layers < 1:
        raise ValueError(f"bad format/levels/layers vk={vk} levels={levels} layers={layers}")
    head = HEADER.size + levels * LEVEL.size
    table = [LEVEL.unpack_from(data, HEADER.size + i * LEVEL.size) for i in range(levels)]
    end = head + table[-1][3] + table[-1][4]
    if len(data) != end:
        raise ValueError(f"file is {len(data)} bytes, its table says {end}")
    w, h, d, off, size = table[0]
    if off != 0 or size % layers:
        raise ValueError("level 0 does not start at 0 or does not split into layers")
    expect = math.ceil(w / 4) * math.ceil(h / 4) * 16 * d * layers
    if size != expect:
        raise ValueError(f"level 0 holds {size} bytes, {w}x{h}x{d} x{layers} BC7 needs {expect}")
    layer0 = data[head + off: head + off + size // layers]
    return key, vk, layers, (w, h, d, layer0)


def parse_ref(path):
    raw = path.read_bytes()
    w, h = struct.unpack_from("<II", raw, 0)
    rgba = np.frombuffer(raw, dtype=np.uint8, offset=8)
    if rgba.size != w * h * 4:
        raise ValueError(f"reference holds {rgba.size} bytes, {w}x{h} needs {w * h * 4}")
    return rgba.reshape(h, w, 4)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--ref-dir", required=True)
    ap.add_argument("--romfs", default=str(ROOT / "game/romfs"), help="RomFS folder (reported only)")
    ap.add_argument("--n", type=int, default=40, help="sample size")
    ap.add_argument("--min-psnr", type=float, default=33.0)
    ap.add_argument("--min-alpha-psnr", type=float, default=33.0)
    ap.add_argument("--min-ssim", type=float, default=0.97)
    ap.add_argument("--min-mean-psnr", type=float, default=45.0)
    ap.add_argument("--bc7dec", default=str(metrics.DEFAULT_BC7DEC))
    args = ap.parse_args(argv)

    bc7dec = Path(args.bc7dec)
    if not bc7dec.exists():
        print(f"FAIL: bc7dec not found: {bc7dec}")
        return 2
    cache_dir = Path(args.cache_dir)
    ref_dir = Path(args.ref_dir)
    refs = {p.stem for p in ref_dir.glob("*.ref")}
    cands = sorted(p for p in cache_dir.rglob("*.bc7") if p.stem in refs)
    if not cands:
        print(f"FAIL: no cache file in {cache_dir} has a reference in {ref_dir}")
        return 1
    k = max(1, len(cands) // args.n)
    picks = cands[::k][: args.n]
    print(f"sample: {len(picks)} of {len(cands)} cache files with references (every {k} file(s), sorted by name); "
          f"RomFS folder {'present' if Path(args.romfs).is_dir() else 'missing'}")
    if len(picks) < args.n:
        print(f"NOTE: only {len(picks)} files available, sample is smaller than {args.n}")

    rows = []
    failures = []
    with tempfile.TemporaryDirectory() as td:
        for path in picks:
            name = path.name
            try:
                key, vk, _layers, (w, h, d, layer0) = parse_cache(path)
                if f"{key:016x}" != path.stem:
                    raise ValueError(f"key {key:016x} does not match the file name")
                ref = parse_ref(ref_dir / f"{path.stem}.ref")
                if ref.shape[1] != w or ref.shape[0] != h:
                    raise ValueError(f"reference is {ref.shape[1]}x{ref.shape[0]}, cache level 0 is {w}x{h}")
                if d != 1:
                    raise ValueError("3D level 0 is not sampled")
                dds = Path(td) / f"{path.stem}.dds"
                run_oracle.make_dds(dds, 99 if vk == VK_BC7_SRGB else 98, w, h, layer0)
                dec, _srgb = metrics.decode_dds(dds, bc7dec)
                m = metrics.metrics(ref, dec)
            except Exception as exc:  # report and count as a failure
                failures.append(f"{name}: {exc}")
                print(f"  {name}: ERROR {exc}")
                continue
            ok = m["psnr_rgb"] >= args.min_psnr and m["psnr_a"] >= args.min_alpha_psnr and m["ssim_luma"] >= args.min_ssim
            rows.append((name, w, h, m["psnr_rgb"], m["psnr_a"], m["ssim_luma"]))
            print(f"  {name} {w}x{h}: psnr_rgb {m['psnr_rgb']:.2f}  psnr_a {m['psnr_a']:.2f}  ssim_luma {m['ssim_luma']:.4f}"
                  f"  {'ok' if ok else 'BELOW THRESHOLD'}")
            if not ok:
                failures.append(f"{name}: psnr_rgb {m['psnr_rgb']:.2f}, psnr_a {m['psnr_a']:.2f}, ssim {m['ssim_luma']:.4f}")

    if not rows:
        print("FAIL: no file could be checked")
        return 1
    rgb = [r[3] for r in rows]
    mean_rgb = sum(rgb) / len(rgb)
    min_alpha = min(r[4] for r in rows)
    min_ssim = min(r[5] for r in rows)
    print(f"summary: {len(rows)} files, psnr_rgb min {min(rgb):.2f} mean {mean_rgb:.2f}, "
          f"psnr_a min {min_alpha:.2f}, ssim_luma min {min_ssim:.4f}")
    if mean_rgb < args.min_mean_psnr:
        failures.append(f"mean psnr_rgb {mean_rgb:.2f} < {args.min_mean_psnr}")
    if failures:
        print(f"FAIL: {len(failures)} problem(s)")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"PASS: {len(rows)} files, mean psnr_rgb {mean_rgb:.2f} dB (>= {args.min_mean_psnr}), min psnr_rgb {min(rgb):.2f} "
          f"(>= {args.min_psnr}), min psnr_a {min_alpha:.2f} (>= {args.min_alpha_psnr}), min ssim_luma {min_ssim:.4f} "
          f"(>= {args.min_ssim})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
