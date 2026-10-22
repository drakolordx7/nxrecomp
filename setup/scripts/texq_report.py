#!/usr/bin/env python3
"""Summarise a mpr_texq TSV: min/mean PSNR RGB, PSNR A and SSIM per texture class and configuration, plus the worst files.
usage: texq_report.py file.tsv [--worst N] [--cfg NAME]"""
import sys, argparse
import numpy as np
import csv

def classify(r):
    fmt = r["fmt"]; srgb = r["srgb"] == "1"
    alpha = float(r["alpha_nonop"]) > 0.001
    cls = ("srgb" if srgb else "linear") + ("+alpha" if alpha else "")
    return cls

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("tsv"); ap.add_argument("--worst", type=int, default=15)
    ap.add_argument("--by", default="class")
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.tsv), delimiter="\t"))
    cfgs = [k[:-4] for k in rows[0] if k.endswith("_rgb")]
    print(f"{len(rows)} textures, configs {cfgs}")
    def key(r):
        if a.by == "fmt": return r["fmt"]
        if a.by == "size": return f"{max(int(r['w']),int(r['h']))}"
        return classify(r)
    groups = {}
    for r in rows: groups.setdefault(key(r), []).append(r)
    for c in cfgs:
        print(f"\n== {c}: class n | rgb min mean | a min | ssim min mean | ms/tex")
        for g, rs in sorted(groups.items()):
            rgb = np.array([float(r[c+"_rgb"]) for r in rs]); al = np.array([float(r[c+"_a"]) for r in rs])
            ss = np.array([float(r[c+"_ssim"]) for r in rs]); ms = np.array([float(r[c+"_ms"]) for r in rs])
            print(f"  {g:16s} {len(rs):4d} | {rgb.min():6.2f} {rgb.mean():6.2f} | {al.min():6.2f} | {ss.min():.4f} {ss.mean():.4f} | {ms.sum():.0f}")
        rgb = np.array([float(r[c+"_rgb"]) for r in rows]); ss = np.array([float(r[c+"_ssim"]) for r in rows])
        print(f"  ALL {len(rows)} | rgb min {rgb.min():.2f} mean {rgb.mean():.2f} p1 {np.percentile(rgb,1):.2f} | ssim min {ss.min():.4f} | <40dB: {(rgb<40).sum()}  <36: {(rgb<36).sum()}  <33: {(rgb<33).sum()}")
    c = cfgs[0]
    print(f"\nworst {a.worst} by {c}_rgb:")
    for r in sorted(rows, key=lambda r: float(r[c+"_rgb"]))[:a.worst]:
        print(f"  {r['id']} {r['fmt']:14s} {r['w']}x{r['h']} kind{r['kind']} " + " ".join(f"{k}={r[k+'_rgb']}/{r[k+'_ssim']}" for k in cfgs) +
              f" mean=({r['meanR']},{r['meanG']},{r['meanB']},{r['meanA']}) sd=({r['sdR']},{r['sdG']},{r['sdB']},{r['sdA']}) anop={r['alpha_nonop']} unit={r['unit_xyz']} grey={r['grey']} modes={r[c+'_modes']}")
main()
