"""Unit tests for hifps_probe (script templates and trace analyzers on synthetic traces, stdlib unittest only)."""
import math
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

import hifps_probe  # noqa: E402

RATES = (60, 120, 144, 240)
BASE_RATE = 60
VMAX = 6.0          # units / s
TAU_UP = 0.25       # s, acceleration time constant
TAU_DOWN = 0.15     # s, deceleration time constant after release
JUMP_V0 = 4.0       # units / s
JUMP_G = 20.0       # units / s^2
GROUND_Z = 10.0
YAW_TARGET = -0.7676


def walk_trace(rate):
    """3 s standing, stick held 3 s (v = VMAX*(1-exp(-t/TAU_UP))), then released for 1.5 s (v *= exp(-dt/TAU_DOWN) per frame)."""
    dt = 1.0 / rate
    stand = hifps_probe.frames(3.0, rate)
    held = hifps_probe.frames(3.0, rate)
    after = hifps_probe.frames(1.5, rate)
    rows = []
    y = 0.0
    v = 0.0
    for f in range(stand + held + after):
        if stand <= f < stand + held:
            t = (f - stand + 1) * dt
            v = VMAX * (1.0 - math.exp(-t / TAU_UP))
        elif f >= stand + held:
            v *= math.exp(-dt / TAU_DOWN)
        y += v * dt
        rows.append({'f': f, 'pos': [0.0, y, 0.0]})
    return rows


def jump_trace(rate):
    """Standing on the ground, lift at 3 s, z(t) = 4t - 10t^2 for 0 <= t < 0.4, ground height again after landing."""
    lift = hifps_probe.frames(3.0, rate)
    rows = []
    for f in range(lift + hifps_probe.frames(2.0, rate)):
        z = GROUND_Z
        if f >= lift:
            t = (f - lift) / rate
            if t < 0.4:
                z = GROUND_Z + JUMP_V0 * t - 0.5 * JUMP_G * t * t
        rows.append({'f': f, 'pos': [0.0, 0.0, z], 'yaw': 0.0})
    return rows


def turn_trace(rate):
    """Yaw 0 until 3 s, then linear to YAW_TARGET over 0.2 s, constant after."""
    lift = hifps_probe.frames(3.0, rate)
    ramp = hifps_probe.frames(0.2, rate)
    rows = []
    for f in range(lift + hifps_probe.frames(0.5, rate)):
        k = min(max(f - lift, 0), ramp)
        rows.append({'f': f, 'pos': [0.0, 0.0, 0.0], 'yaw': YAW_TARGET * k / ramp})
    return rows


def assert_rel(test, value, ref, tol, what):
    rel = abs(value - ref) / abs(ref)
    test.assertLessEqual(
        rel, tol,
        '%s: actual %.6f, expected %.6f, relative error %.4f exceeds %.4f' % (what, value, ref, rel, tol))


def script_lines(text):
    return [ln.strip() for ln in text.splitlines() if ln.strip()]


class TestScripts(unittest.TestCase):
    def test_frames(self):
        self.assertEqual(hifps_probe.frames(3.0, 60), 180)
        self.assertEqual(hifps_probe.frames(3.0, 144), 432)
        self.assertEqual(hifps_probe.frames(0.1, 240), 24)
        self.assertEqual(hifps_probe.frames(0.001, 60), 1)

    def test_walk_script_scales_with_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                lines = script_lines(hifps_probe.script_walk(rate))
                settle = 'wait %d' % hifps_probe.frames(3.0, rate)
                self.assertGreaterEqual(lines.count(settle), 2, 'expected "%s" at least twice' % settle)
                self.assertIn('stick left 0 1.0', lines)
                self.assertIn('stick left 0 0.0', lines)
                self.assertIn('@warp frigate frigate_start', lines)
                self.assertEqual(lines[-1], '@pass')

    def test_jump_script_scales_with_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                lines = script_lines(hifps_probe.script_jump(rate))
                self.assertIn('press B for %d' % hifps_probe.frames(0.4, rate), lines)

    def test_turn_script_scales_with_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                text = hifps_probe.script_turn(rate)
                m = re.search(r'^mouse (\S+) 0 for (\d+)$', text, re.M)
                self.assertIsNotNone(m, 'no "mouse <dx> 0 for <n>" line in script at %d Hz' % rate)
                dx, n = float(m.group(1)), int(m.group(2))
                self.assertEqual(n, hifps_probe.frames(0.2, rate))
                self.assertLess(abs(dx * n - 1000.0), 1e-3,
                                'dx*n: actual %.6f, expected 1000.0 at %d Hz' % (dx * n, rate))

    def test_menu_script_scales_with_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                lines = script_lines(hifps_probe.script_menu(rate))
                self.assertIn('@frame %d' % hifps_probe.frames(22.0, rate), lines)
                self.assertIn('press A for %d' % hifps_probe.frames(0.1, rate), lines)


class TestMetrics(unittest.TestCase):
    def test_walk_metrics_match_60hz(self):
        base = hifps_probe.walk_metrics(walk_trace(BASE_RATE), BASE_RATE)
        self.assertIsNotNone(base)
        for rate in RATES:
            if rate == BASE_RATE:
                continue
            with self.subTest(rate=rate):
                m = hifps_probe.walk_metrics(walk_trace(rate), rate)
                self.assertIsNotNone(m)
                assert_rel(self, m['steady'], base['steady'], 0.02, 'walk steady speed %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['dist'], base['dist'], 0.03, 'walk distance %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['t95'], base['t95'], 0.08, 'walk t95 %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['slide'], base['slide'], 0.08, 'walk slide %d Hz vs 60 Hz' % rate)

    def test_walk_steady_speed_is_vmax(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                m = hifps_probe.walk_metrics(walk_trace(rate), rate)
                self.assertIsNotNone(m)
                assert_rel(self, m['steady'], VMAX, 0.03, 'walk steady speed %d Hz vs vmax' % rate)

    def test_jump_metrics_match_60hz(self):
        base = hifps_probe.jump_metrics(jump_trace(BASE_RATE), BASE_RATE)
        self.assertIsNotNone(base)
        for rate in RATES:
            if rate == BASE_RATE:
                continue
            with self.subTest(rate=rate):
                m = hifps_probe.jump_metrics(jump_trace(rate), rate)
                self.assertIsNotNone(m)
                assert_rel(self, m['peak'], base['peak'], 0.02, 'jump peak %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['air'], base['air'], 0.03, 'jump air %d Hz vs 60 Hz' % rate)

    def test_jump_peak_is_analytic(self):
        # 4^2 / (2 * 20) = 0.4
        for rate in RATES:
            with self.subTest(rate=rate):
                m = hifps_probe.jump_metrics(jump_trace(rate), rate)
                self.assertIsNotNone(m)
                assert_rel(self, m['peak'], 0.4, 0.03, 'jump peak %d Hz vs 0.4' % rate)

    def test_turn_yaw_matches_target(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                m = hifps_probe.turn_metrics(turn_trace(rate), rate)
                self.assertIsNotNone(m)
                assert_rel(self, m['yaw'], YAW_TARGET, 0.01, 'turn yaw %d Hz vs %.4f' % (rate, YAW_TARGET))

    def test_too_few_rows_return_none(self):
        short = [{'f': i, 'pos': [0.0, float(i), 0.0], 'yaw': 0.0} for i in range(9)]
        for name in ('walk_metrics', 'jump_metrics', 'turn_metrics'):
            fn = getattr(hifps_probe, name)
            with self.subTest(analyzer=name, rows=9):
                self.assertIsNone(fn(short, 60))
            with self.subTest(analyzer=name, rows=0):
                self.assertIsNone(fn([], 60))


if __name__ == '__main__':
    unittest.main()
