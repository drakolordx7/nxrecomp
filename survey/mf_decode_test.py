"""Decode every extracted movie (work/survey/fmv/*.mp4) through Windows Media Foundation (OpenCV's CAP_MSMF backend) and compare
the decoded frame count with ffprobe's. Proves the Windows H.264 MFT handles these streams (Main L4.2, 1600x900, 60 fps, B-frames).
usage: python mf_decode_test.py [--dir work/survey/fmv] [--ffprobe <path>]"""
import argparse, glob, os, subprocess, sys, time
import cv2

ap = argparse.ArgumentParser()
ap.add_argument('--dir', default='work/survey/fmv')
ap.add_argument('--ffprobe', default=os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\WinGet\Links\ffprobe.exe'))
a = ap.parse_args()
ok = bad = 0
tot_frames = 0
t_all = time.perf_counter()
for f in sorted(glob.glob(os.path.join(a.dir, '*.mp4'))):
    n_probe = int(subprocess.run([a.ffprobe, '-v', 'error', '-count_frames', '-select_streams', 'v:0', '-show_entries',
                                  'stream=nb_read_frames', '-of', 'csv=p=0', f], capture_output=True, text=True).stdout.strip() or 0)
    cap = cv2.VideoCapture(f, cv2.CAP_MSMF)
    if not cap.isOpened():
        print('OPEN FAIL', os.path.basename(f))
        bad += 1
        continue
    n = 0
    shape = None
    t = time.perf_counter()
    while True:
        r, fr = cap.read()
        if not r:
            break
        n += 1
        shape = fr.shape
    dt = time.perf_counter() - t
    cap.release()
    good = n == n_probe
    ok += good
    bad += not good
    tot_frames += n
    print(f'{os.path.basename(f)[:12]} msmf frames {n} ffprobe {n_probe} shape {shape} {n / dt:.0f} fps decode {"OK" if good else "MISMATCH"}')
print(f'{ok} ok, {bad} bad; {tot_frames} frames in {time.perf_counter() - t_all:.1f} s (includes ffprobe -count_frames)')
