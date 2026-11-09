# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks nx_formats.py against nx_formats.h (the constants are parsed out of the header) and against the fixed vectors that
tools/setup/tests/test_formats.cpp checks on the C++ side. usage: python test_formats.py"""
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nx_formats as py  # noqa: E402

# The same numbers are in tools/setup/tests/test_formats.cpp.
VECTOR_HASH = 0xa0bd9a2db08e831f
VECTOR_KEY = 0xa6f65dbf8e2792ed


def vector(n):
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


def header():
    with open(os.path.join(HERE, "nx_formats.h"), encoding="utf-8") as f:
        return f.read()


class Formats(unittest.TestCase):
    def test_magics_match_header(self):
        h = header()
        for name, val in (("kProgramsMagic", py.PROGRAMS_MAGIC), ("kProgramsMagicLegacy", py.PROGRAMS_MAGIC_LEGACY),
                          ("kShaderPackMagic", py.SHADER_PACK_MAGIC), ("kShaderPackMagicLegacy", py.SHADER_PACK_MAGIC_LEGACY)):
            m = re.search(name + r"\[8\] = \{([^}]*)\}", h)
            self.assertIsNotNone(m, name)
            self.assertEqual("".join(c.strip().strip("'") for c in m.group(1).split(",")).encode(), val, name)

    def test_sizes_match_header(self):
        h = header()
        for name, val in (("kBc7CacheVersion", py.BC7_CACHE_VERSION), ("kProgramsHeadBytes", py.PROGRAMS_HEAD_BYTES),
                          ("kProgramsRecordBytes", py.PROGRAMS_RECORD_BYTES), ("kShaderPackHeadBytes", py.SHADER_PACK_HEAD_BYTES)):
            m = re.search(name + r" = (\d+)", h)
            self.assertIsNotNone(m, name)
            self.assertEqual(int(m.group(1)), val, name)
        for name, val in (("kP1", py.P1), ("kP2", py.P2), ("kP3", py.P3), ("kP4", py.P4), ("kP5", py.P5)):
            m = re.search(name + r" = 0x([0-9A-Fa-f]+)ull", h)
            self.assertIsNotNone(m, name)
            self.assertEqual(int(m.group(1), 16), val, name)

    def test_hash_and_key_vectors(self):
        self.assertEqual(py.hash_bytes(vector(75)), VECTOR_HASH)
        self.assertEqual(py.bc7_cache_key(vector(64), 0x7F, 8, 8, 1, 4, 1, 1, 0, 3), VECTOR_KEY)

    def test_path_and_programs(self):
        self.assertEqual(py.bc7_cache_rel_path(0x0123456789abcdef), "01/0123456789abcdef.bc7")
        b = py.programs_encode([(1, 2, 0), (1, 3, 0)])
        self.assertEqual(b[:8], py.PROGRAMS_MAGIC)
        self.assertEqual(len(b), py.PROGRAMS_HEAD_BYTES + 2 * py.PROGRAMS_RECORD_BYTES)
        self.assertEqual(struct.unpack_from("<I", b, 8)[0], 2)


if __name__ == "__main__":
    unittest.main()
