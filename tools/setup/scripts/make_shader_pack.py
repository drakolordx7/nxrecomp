#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds an MPRSHC01 shader pack from a shader cache folder (layout: src/shadercache.h).

usage: make_shader_pack.py <src folder> <out pack> [--zstd] [--level N]

The folder must contain results.tsv and <16 lower-case hex digits>.spv files; a manifest.sha256 in the folder is not packed.
Records are written sorted by name with results.tsv first. Stored entries (method 0) are the default. --zstd writes method 1
entries (zstd, multi-threaded, level 12 unless --level N is given, 1-22) and needs the 'zstandard' module
(pip install zstandard). The setup tool reads method 1 only when it was built with zstd (MPRS_HAVE_ZSTD).
Entries are compressed and written one at a time; the record table is written last, at the front of the file.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
import time

MAGIC = b"MPRSHC01"
SPV_RE = re.compile(r"^[0-9a-f]{16}\.spv$")
RESULTS = "results.tsv"
MANIFEST = "manifest.sha256"
DEFAULT_LEVEL = 12


def valid_name(name):
    return name == RESULTS or SPV_RE.match(name) is not None


def fail(msg):
    print("make_shader_pack: " + msg, file=sys.stderr)
    sys.exit(1)


def main(argv):
    ap = argparse.ArgumentParser(description="Build an MPRSHC01 shader pack from a shader cache folder.")
    ap.add_argument("src", help="shader cache folder (results.tsv + .spv files)")
    ap.add_argument("out", help="pack file to write")
    ap.add_argument("--zstd", action="store_true", help="compress entries with zstd (method 1)")
    ap.add_argument("--level", type=int, default=None, help="zstd level 1-22, default %d (needs --zstd)" % DEFAULT_LEVEL)
    a = ap.parse_args(argv[1:])
    if a.level is not None and not a.zstd:
        ap.error("--level needs --zstd")
    level = a.level if a.level is not None else DEFAULT_LEVEL
    if not 1 <= level <= 22:
        ap.error("--level must be between 1 and 22")
    src, out, use_zstd = a.src, a.out, a.zstd

    cctx = None
    if use_zstd:
        try:
            import zstandard
        except ImportError:
            fail("--zstd needs the 'zstandard' Python module (pip install zstandard); it is not installed here")
        cctx = zstandard.ZstdCompressor(level=level, threads=-1)

    if not os.path.isdir(src):
        fail("source folder not found: " + src)
    names = sorted(n for n in os.listdir(src) if n != MANIFEST)
    if RESULTS not in names:
        fail(src + " does not contain a shader cache (results.tsv is missing)")
    for n in names:
        if os.path.isdir(os.path.join(src, n)) or not valid_name(n):
            fail("not part of a shader cache: " + n)
    names.remove(RESULTS)
    names = [RESULTS] + names

    # The table size depends only on the names, so the data can start at a known offset.
    name_bytes = [n.encode("ascii") for n in names]
    table_size = sum(2 + len(nb) + 8 + 8 + 8 + 32 + 1 for nb in name_bytes)
    data_start = 16 + table_size

    t0 = time.time()
    raw_total = stored_total = 0
    records = []   # (name bytes, sha256 of raw, raw size, method, data offset, stored size)
    tmp = out + ".part"
    with open(tmp, "wb") as f:
        f.write(b"\0" * data_start)   # header and table are filled in once every entry is known
        offset = data_start
        for n, nb in zip(names, name_bytes):
            with open(os.path.join(src, n), "rb") as s:
                raw = s.read()
            stored = cctx.compress(raw) if use_zstd else raw
            method = 1 if use_zstd else 0
            f.write(stored)
            records.append((nb, hashlib.sha256(raw).digest(), len(raw), method, offset, len(stored)))
            offset += len(stored)
            raw_total += len(raw)
            stored_total += len(stored)

        table = bytearray()
        for nb, digest, raw_len, method, off, stored_len in records:
            table += struct.pack("<H", len(nb)) + nb
            table += struct.pack("<QQQ", off, stored_len, raw_len)
            table += digest
            table += struct.pack("<B", method)
        if len(table) != table_size:
            fail("internal error: record table is %d bytes, expected %d" % (len(table), table_size))
        f.seek(0)
        f.write(MAGIC + struct.pack("<II", len(records), 0) + table)
    os.replace(tmp, out)
    print("wrote %s: %d entries, %s, raw %.1f MB -> stored %.1f MB, %.1f s" % (
        out, len(records), "zstd level %d" % level if use_zstd else "stored",
        raw_total / 1e6, stored_total / 1e6, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
