# SPDX-License-Identifier: GPL-3.0-or-later
"""Python twin of nx_formats.h: the hash of the BC7 texture cache key, and the magics and layouts of programs.bin, the shader pack and the
prompt icon pack.
The same constants and the same functions; test_formats.py checks this file against the header. Used by
tools/setup/scripts/make_shader_pack.py, runtime/scripts/build_prompt_pack.py and tools/release/make_release.py."""
import struct

M64 = (1 << 64) - 1
P1, P2, P3, P4, P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _lane(acc, v):
    acc = (acc + v * P2) & M64
    return (_rotl(acc, 31) * P1) & M64


def _merge(h, v):
    return (((h ^ _lane(0, v)) * P1) + P4) & M64


def hash_seeded(data, seed=0):
    data = bytes(data)
    n = len(data)
    p = 0
    if n >= 32:
        v1, v2, v3, v4 = (seed + P1 + P2) & M64, (seed + P2) & M64, seed & M64, (seed - P1) & M64
        while n - p >= 32:
            a, b, c, d = struct.unpack_from("<4Q", data, p)
            v1, v2, v3, v4 = _lane(v1, a), _lane(v2, b), _lane(v3, c), _lane(v4, d)
            p += 32
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & M64
        for v in (v1, v2, v3, v4):
            h = _merge(h, v)
    else:
        h = (seed + P5) & M64
    h = (h + n) & M64
    while n - p >= 8:
        h ^= _lane(0, struct.unpack_from("<Q", data, p)[0])
        h = (_rotl(h, 27) * P1 + P4) & M64
        p += 8
    if n - p >= 4:
        h ^= (struct.unpack_from("<I", data, p)[0] * P1) & M64
        h = (_rotl(h, 23) * P2 + P3) & M64
        p += 4
    while p < n:
        h ^= (data[p] * P5) & M64
        h = (_rotl(h, 11) * P1) & M64
        p += 1
    h ^= h >> 33
    h = (h * P2) & M64
    h ^= h >> 29
    h = (h * P3) & M64
    h ^= h >> 32
    return h


def hash_bytes(data):
    return hash_seeded(data, 0)


BC7_CACHE_VERSION = 1
BC7_CACHE_MAGIC = b"NXB7"
BC7_HEAD_BYTES = 28
BC7_LEVEL_BYTES = 28


def bc7_cache_key(mem, nvn_format, width, height, depth, levels, layers, block_height, pitch, quality):
    meta = struct.pack("<10Q", BC7_CACHE_VERSION, nvn_format, width, height, depth, levels, layers, block_height, pitch, quality)
    return hash_seeded(mem, hash_bytes(meta))


def bc7_cache_rel_path(key):
    name = "%016x" % key
    return "%s/%s.bc7" % (name[:2], name)


PROGRAMS_MAGIC = b"NXPRG002"
PROGRAMS_MAGIC_LEGACY = b"MPRPRG01"             # v1 magic; same layout
PROGRAMS_HEAD_BYTES = 16
PROGRAMS_RECORD_BYTES = 24

SHADER_PACK_MAGIC = b"NXSHC002"
SHADER_PACK_MAGIC_LEGACY = b"MPRSHC01"           # v1 magic; same layout
SHADER_PACK_HEAD_BYTES = 16
SHADER_RESULTS_NAME = "results.tsv"


PROMPT_PACK_MAGIC = b"MPIP"
PROMPT_PACK_VERSION = 1
PROMPT_PACK_HEAD_BYTES = 24
PROMPT_PACK_ENTRY_BYTES = 48
PROMPT_PACK_NAME_BYTES = 40


def shader_pack_magic_ok(head):
    return bytes(head[:8]) in (SHADER_PACK_MAGIC, SHADER_PACK_MAGIC_LEGACY)


def programs_encode(programs):
    """programs: sorted, unique (vs, fs, gs) tuples."""
    out = bytearray(PROGRAMS_MAGIC + struct.pack("<II", len(programs), 0))
    for p in programs:
        out += struct.pack("<3Q", *p)
    return bytes(out)
