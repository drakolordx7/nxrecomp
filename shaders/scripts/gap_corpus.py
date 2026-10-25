"""Find the shader code blocks of the player's game that a shader cache has no SPIR-V for, and write them as a harness corpus.

The source of truth is the same scan the setup tool makes for programs.bin (tools/setup/src/programs.cpp): the SNVN records of
MaterialArchive.arc and the program sets embedded in every MTRL asset of every .pak, read straight from the pak files (the META
record in the pak's table of contents names the zlib body). The survey extractor (tools/survey/extract_shaders.py) read the
.MTRL files that retrotool had written to work/survey/paks instead; assets with the same id in several paks landed on one file
name there, so some program sets never reached the survey corpus.

Output folder: manifest.tsv, code_<h>.bin, ctrl_<h>.bin (h = first 16 hex digits of sha1(code), the runtime cache key),
gap_report.json (counts, and for every missing block the pak/asset it came from and the programs that use it).

usage: python -I gap_corpus.py <romfs> --cache <spirv dir> --out <corpus dir> [--programs programs.bin] [--all]
  --all       write every distinct block, not only the ones the cache lacks
"""
import argparse, collections, glob, hashlib, json, os, re, struct, sys, zlib

CODE_MAGICS = (b'\x78\x56\x34\x12', b'\x79\x56\x34\x12')
CTRL_MAGIC = b'\x34\x12\x76\x98'
EMBED_RE = re.compile(rb'\x00(.{4})([\x11\x19\x08]\x00\x00\x00)(.{4})\x78[\x01\x5e\x9c\xda]', re.S)
NVN_STAGE = {0: 'vertex', 1: 'vertex', 2: 'tess_control', 3: 'tess_eval', 4: 'geometry', 5: 'fragment'}


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def uuid_str(b):
    return '%02x%02x%02x%02x-%02x%02x-%02x%02x-%s-%s' % (b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8:10].hex(), b[10:16].hex())


def pak_assets(path):
    """RFRM PACK > TOCC (ADIR + META), as tools/setup/src/pak.cpp. Yields (type, uuid bytes, offset, size, dsize, meta bytes)."""
    with open(path, 'rb') as f:
        hdr = f.read(64)
        if hdr[:4] != b'RFRM' or hdr[20:24] != b'PACK' or hdr[32:36] != b'RFRM' or hdr[52:56] != b'TOCC':
            raise ValueError('not a PACK')
        toc = f.read(u64(hdr, 36))
    adir, meta = [], {}
    off = 0
    while off + 24 <= len(toc):
        cid, size, skip = toc[off:off + 4], u64(toc, off + 4), u64(toc, off + 16)
        body = toc[off + 24 + skip:off + 24 + skip + size]
        if cid == b'ADIR':
            for i in range(u32(body, 0)):
                e = body[4 + 52 * i:4 + 52 * (i + 1)]
                adir.append((e[:4].decode('latin1'), e[4:20], u64(e, 28), u64(e, 44), u64(e, 36)))
        elif cid == b'META':
            for i in range(u32(body, 0)):
                e = body[4 + 20 * i:4 + 20 * (i + 1)]
                mo = u32(e, 16)
                meta[e[:16]] = body[mo + 4:mo + 4 + u32(body, mo)]   # last record wins
        off += 24 + skip + size
    return adir, meta


def mtrl_blob(asset, meta):
    w = meta if len(meta) >= 20 else None
    if w is None:   # footer form (extracted .MTRL files)
        off = 32 + u64(asset, 4)
        fsz = u64(asset, off + 4)
        o = off + 32
        while o + 24 <= off + 32 + fsz:
            csz, sk = u64(asset, o + 4), u64(asset, o + 16)
            if asset[o:o + 4] == b'META':
                w = asset[o + 24 + sk:o + 24 + sk + 20]
                break
            o += 24 + sk + csz
    cs, fo = u32(w, 8), u32(w, 16)
    return zlib.decompress(asset[fo:fo + cs])


def split_raw(raw):
    codes = [j for j in range(0, len(raw) - 3, 4) if raw[j:j + 4] in CODE_MAGICS]
    ctrls = [j for j in range(0, len(raw) - 3, 4) if raw[j:j + 4] == CTRL_MAGIC]
    if not codes or len(codes) != len(ctrls):
        return None
    out = []
    for k in range(len(codes)):
        c_end = codes[k + 1] if k + 1 < len(codes) else ctrls[0]
        t_end = ctrls[k + 1] if k + 1 < len(ctrls) else len(raw)
        code, ctrl = raw[codes[k]:c_end], raw[ctrls[k]:t_end]
        if len(ctrl) < 0x70c or u32(ctrl, 0x704) != len(code):
            return None
        out.append((code, ctrl))
    return out


def embedded_sets(d):
    for m in EMBED_RE.finditer(d):
        zl = u32(m.group(3), 0)
        st = m.start() + 13
        try:
            zo = zlib.decompressobj()
            raw = zo.decompress(d[st:st + zl])
            if zo.unused_data or not zo.eof or raw[:4] not in CODE_MAGICS:
                continue
        except zlib.error:
            continue
        yield m.start(), raw


def stage_of(code, ctrl):
    if u32(code, 0) == 0x12345679:
        return 'compute'
    start = u32(ctrl, 0x708)
    if start + 4 > len(code):
        return '?'
    return NVN_STAGE.get((u32(code, start) >> 10) & 15, '?')


def snvn_sets(arc):
    b = open(arc, 'rb').read()
    off = 32
    while off + 24 <= len(b):
        size, skip = u64(b, off + 4), u64(b, off + 16)
        body = off + 24 + skip
        if b[off:off + 4] == b'SNVN':
            p = body + 8 + u32(b, body + 4)
            n = u32(b, p)
            p += 4
            for i in range(n):
                sl = u32(b, p + 1)
                p += 5 + sl
                dl = u32(b, p)
                p += 4
                d = b[p:p + dl]
                p += dl
                yield i, zlib.decompressobj().decompress(d[13:13 + u32(d, 9)])
            return
        off = body + size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('romfs')
    ap.add_argument('--cache', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--programs')
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    have = {f[:-4] for f in os.listdir(a.cache) if f.endswith('.spv')}
    os.makedirs(a.out, exist_ok=True)
    blocks = {}                                   # h -> (stage, code, ctrl)
    where = collections.defaultdict(set)          # h -> {"pak:asset"}
    programs = set()                              # (vs, fs, gs) as hex
    st = collections.Counter()

    def take(raw, src):
        s = split_raw(raw)
        if s is None:
            st['invalid_sets'] += 1
            return
        st['sets'] += 1
        prog = {}
        for code, ctrl in s:
            h = hashlib.sha1(code).hexdigest()[:16]
            stg = stage_of(code, ctrl)
            blocks.setdefault(h, (stg, code, ctrl))
            where[h].add(src)
            prog.setdefault(stg, []).append(h)
        if 'vertex' in prog and 'fragment' in prog and all(len(v) == 1 for v in prog.values()) and \
                not ({'compute', 'tess_control', 'tess_eval'} & prog.keys()):
            programs.add((prog['vertex'][0], prog['fragment'][0], prog.get('geometry', ['0' * 16])[0]))

    arc = os.path.join(a.romfs, 'MaterialArchive.arc')
    for i, raw in snvn_sets(arc):
        take(raw, 'MaterialArchive.arc:%d' % i)
    paks = sorted(glob.glob(os.path.join(a.romfs, '**', '*.pak'), recursive=True))
    ids = collections.Counter()
    for pk in paks:
        rel = os.path.relpath(pk, a.romfs).replace('\\', '/')
        adir, meta = pak_assets(pk)
        with open(pk, 'rb') as f:
            for typ, uid, off, size, dsize, in adir:
                if typ != 'MTRL':
                    continue
                st['mtrl_assets'] += 1
                ids[uid] += 1
                if size != dsize:
                    st['mtrl_pak_compressed'] += 1
                    continue
                f.seek(off)
                asset = f.read(size)
                try:
                    blob = mtrl_blob(asset, meta.get(uid, b''))
                except Exception:
                    st['mtrl_failed'] += 1
                    continue
                for o, raw in embedded_sets(blob):
                    take(raw, '%s:%s@%x' % (rel, uuid_str(uid), o))
    st['mtrl_distinct_ids'] = len(ids)
    st['mtrl_ids_in_several_paks'] = sum(1 for v in ids.values() if v > 1)
    missing = sorted(h for h in blocks if h not in have)
    write = sorted(blocks) if a.all else missing
    rows = []
    for h in write:
        stg, code, ctrl = blocks[h]
        open(os.path.join(a.out, 'code_%s.bin' % h), 'wb').write(code)
        open(os.path.join(a.out, 'ctrl_%s.bin' % h), 'wb').write(ctrl)
        rows.append('\t'.join(['gap_' + h, stg, '0x0', '%#x' % len(code), '0x0', '%#x' % len(ctrl), '%#x' % u32(ctrl, 0x708),
                               '%#x' % u32(ctrl, 0x6f8), '%#x' % u32(ctrl, 0x700), '%#x' % u32(ctrl, 0x6fc), h,
                               'code_%s.bin' % h, 'ctrl_%s.bin' % h]))
    with open(os.path.join(a.out, 'manifest.tsv'), 'w', newline='\n') as m:
        m.write('id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\t'
                'code_sha1_16\tcode_file\tctrl_file\n')
        m.write(''.join(r + '\n' for r in rows))
    mset = set(missing)
    prog_missing = [p for p in programs if mset & set(p)]
    listed = None
    if a.programs:
        d = open(a.programs, 'rb').read()
        listed = {tuple('%016x' % x for x in struct.unpack_from('<QQQ', d, 16 + 24 * i)) for i in range(u32(d, 8))}
    rep = dict(stats=dict(st), distinct_blocks=len(blocks), cache_spv=len(have), missing_blocks=len(missing),
               missing_by_stage=dict(collections.Counter(blocks[h][0] for h in missing)), programs=len(programs),
               programs_with_missing_stage=len(prog_missing),
               programs_bin=None if listed is None else dict(count=len(listed), equal_to_scan=listed == programs,
                                                             only_in_bin=len(listed - programs), only_in_scan=len(programs - listed)),
               missing={h: dict(stage=blocks[h][0], code_size=len(blocks[h][1]), sources=sorted(where[h]),
                                programs=sum(1 for p in prog_missing if h in p)) for h in missing})
    json.dump(rep, open(os.path.join(a.out, 'gap_report.json'), 'w'), indent=1)
    print(json.dumps({k: v for k, v in rep.items() if k != 'missing'}, indent=1))


if __name__ == '__main__':
    sys.exit(main())
