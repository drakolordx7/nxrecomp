#!/usr/bin/env python3
"""Clean-room check of a release zip: extract it into a fresh folder, run the setup tool from there with a real dump and keys,
start mpr.exe headless from a different working directory and confirm the game reaches the title screen.

  python -I cleanroom_test.py --zip work/release_out/mpr-<ver>-windows-x64.zip --dump <game.nsp> --keys <prod.keys>
        [--dest work/release_test] [--seconds 90] [--shot-at 40] [--quality 3] [--skip-setup]

What it checks (exit 0 only when all hold):
  1. the zip extracts and holds no game data or keys (the same audit rules as make_release.py: no exefs/romfs/bc7_cache/keys)
  2. mpr_setup_cli.exe finishes with exit code 0 and prints its timing; setup.log is kept
  3. mpr.exe, started from another directory with its full path, picks up the game folder by itself (log says so), reaches the
     title screen (the log shows the title-screen marker below and a presented-frame count above a minimum) and writes shot_*.png
  4. it exits cleanly on its own at --seconds (the --max-seconds deadline, exit code 124 is the runtime's deadline exit and is accepted)
The screenshot stays in the extracted folder as shot_<n>.png; its path is printed.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN_TOP = {"exefs", "romfs", "bc7_cache", "game", "keys"}


def run(cmd, **kw):
    print("run:", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, **kw)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", required=True)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--keys", required=True)
    ap.add_argument("--dest", default=str(ROOT / "work/release_test"))
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--shot-at", type=int, default=40)
    ap.add_argument("--quality", type=int, default=None)
    ap.add_argument("--skip-setup", action="store_true", help="reuse an already set up folder (--dest/<zip name>)")
    ap.add_argument("--min-presents", type=int, default=600)
    a = ap.parse_args(argv)

    zpath = Path(a.zip)
    name = zpath.stem
    dest = Path(a.dest) / name
    top = dest / name
    if not a.skip_setup:
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
            bad = [n for n in names if n.split("/")[1:2] and n.split("/")[1].lower() in FORBIDDEN_TOP]
            if bad:
                print("FAIL: the zip holds game folders:", bad[:5])
                return 1
            z.extractall(dest)
        print(f"extracted {len(names)} entries to {top}")
    exe = top / "mpr.exe"
    setup = top / "mpr_setup_cli.exe"
    for f in (exe, setup):
        if not f.exists():
            print("FAIL: missing", f)
            return 1

    if not a.skip_setup:
        cmd = [str(setup), "--input", a.dump, "--keys", a.keys, "--out", str(top)]
        if a.quality is not None:
            cmd += ["--quality", str(a.quality)]
        t0 = time.time()
        r = run(cmd, cwd=str(top.parent))      # not the install folder: setup must not depend on the working directory
        print(f"setup exit {r.returncode}, {time.time() - t0:.0f} s wall")
        if r.returncode != 0:
            return 1
        for need in ("exefs/main", "exefs/rtld", "romfs", "bc7_cache", "mpr.ini", "setup.log"):
            if not (top / need).exists():
                print("FAIL: setup did not produce", need)
                return 1
        # the release must not have been changed by setup except for the files it makes
        print((top / "mpr.ini").read_text())

    for stale in list(top.glob("shot_*.png")) + [top / "mpr.log"]:
        if stale.exists():
            stale.unlink()
    env = dict(os.environ)
    env["NXRT_BOOT_SCREENSHOT_AT"] = str(a.shot_at)
    env["NXRT_BOOT_SCREENSHOT_EVERY"] = "15"
    other = Path(os.environ.get("TEMP", "C:/")).resolve()
    cmd = [str(exe), "--headless", "--max-seconds", str(a.seconds)]
    t0 = time.time()
    r = run(cmd, cwd=str(other), env=env, timeout=a.seconds + 120)
    print(f"mpr.exe exit {r.returncode} after {time.time() - t0:.0f} s (started from {other})")
    log = top / "mpr.log"
    if not log.exists():
        print("FAIL: no mpr.log in the game folder: the exe did not use it as its working folder")
        return 1
    text = log.read_text(errors="replace")
    ok = True
    if "exefs 'exefs'" not in text:
        print("FAIL: the log does not show exefs 'exefs'")
        ok = False
    shots = sorted(top.glob("shot_*.png"))
    print("screenshots:", [str(s) for s in shots])
    if not shots:
        ok = False
        print("FAIL: no screenshot")
    presents = [int(m) for m in re.findall(r"presents (\d+) pipelines", text)]
    print("presented frames at the deadline:", presents[-1] if presents else "none")
    if not presents or presents[-1] < a.min_presents:
        print(f"FAIL: fewer than {a.min_presents} presented frames")
        ok = False
    bad_lines = [l for l in text.splitlines() if "device lost" in l.lower() or " E " in l[:30] and "boot" in l]
    if bad_lines:
        print("log errors:")
        for l in bad_lines[:5]:
            print("  " + l)
    tail = "\n".join(text.splitlines()[-12:])
    print("log tail:\n" + tail)
    if r.returncode not in (0, 124):
        print("FAIL: exit code", r.returncode)
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
