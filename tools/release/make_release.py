#!/usr/bin/env python3
"""Build the Windows x64 release of mpr: a release folder and a zip of it, audited before and after the zip is written.

    python tools/release/make_release.py [--out DIR] [--version V] [--no-shader-pack] [--strict-paths]
                                         [--deny-hash-dir DIR ...] [--repo DIR] [--exe F] [--setup-bin DIR]
                                         [--shader-pack F] [--templates DIR] [--controls F] [--license F] [--notices F]

Output, in --out (default <repo>/work/release_out):
    mpr-<version>-windows-x64/              the release folder
    mpr-<version>-windows-x64.zip           the same folder zipped: one top-level folder, sorted entries, fixed timestamps
    mpr-<version>-windows-x64.manifest.txt  the audit table and totals (written only on success)
Without the shader pack the folder and the zip get the suffix -noshaders.

Exit codes: 0 built and audited; 1 an I/O error; 2 a missing input or a bad option; 3 refused by the audit.
A refused or failed build leaves none of its output behind. The .pdb and .map beside mpr.exe are never copied.
"""
import argparse
import fnmatch
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

MIB = 1024 * 1024
KIB = 1024
GIB = 1024 * MIB

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

EXE_NAME = 'mpr.exe'
SETUP_EXES = ('mpr_setup.exe', 'mpr_setup_cli.exe')
DLL_GLOB = 'astcdec-*-shared.dll'
DLL_COUNT = 3
PACK_NAME = 'shader_cache.mprshc'
PACK_MAGIC = b'MPRSHC01'
MANIFEST = 'MANIFEST.sha256'
ZIP_DATE = (2026, 1, 1, 0, 0, 0)
VERSION_RE = r'[A-Za-z0-9][A-Za-z0-9._-]*'

# Licences copied into licenses\ : (source, destination name or None to keep the source's name, required).
# A folder source is copied with everything in it (subfolders included, relative names kept). Edit this list only.
LICENSE_FILES = [
    ('tools/release/licenses', None, True),
]

# Font files shipped in the release: (source, path inside the release, required).
FONT_FILES = [
    ('tools/release/fonts/DejaVuSans.ttf', 'fonts/DejaVuSans.ttf', True),
]

# Size sanity, bytes (min, max or None). Tests replace these with tiny values.
SIZE_RULES = {'exe': (10 * MIB, 400 * MIB), 'setup': (100 * KIB, None), 'pack': (100 * MIB, 1 * GIB)}

DENY_EXT = ('.nsp', '.xci', '.nsz', '.xcz', '.nca', '.nso', '.nro', '.npdm', '.pak', '.bntx', '.keys', '.tik',
            '.cnmt', '.romfs', '.pdb', '.map', '.obj', '.lib', '.ilk', '.exp')
DENY_NAMES = ('prod.keys', 'title.keys', 'dev.keys')
DENY_PARTS = ('romfs', 'exefs', 'bc7_cache', 'game', 'keys')
MAGIC_DENY = [
    (b'NSO0', 'NSO0 (game executable)'),
    (b'NCA2', 'NCA2 (game content archive)'),
    (b'NCA3', 'NCA3 (game content archive)'),
    (b'PFS0', 'PFS0 (game archive)'),
    (b'HFS0', 'HFS0 (game archive)'),
    (b'NXB7', 'NXB7 (BC7 cache)'),
    (b'\x13\xab\xa1\x5c', 'ASTC texture magic'),
    (b'DDS ', 'DDS texture'),
]
TEXT_EXT = ('.txt', '.ini', '.md', '.tsv', '.json')
KEY_TEXT = [
    (rb'(?i)header_key\s*=\s*[0-9a-f]{32,}', 'header_key = <32+ hex digits>'),
    (rb'(?i)aes_kek_generation_source', 'aes_kek_generation_source'),
    (rb'(?i)titlekek_source', 'titlekek_source'),
    (rb'(?i)master_key_00', 'master_key_00'),
    (rb'-----BEGIN', '-----BEGIN'),
]
LEAK_FILES = (EXE_NAME,) + SETUP_EXES
LEAK_PATTERNS = [b'D:/AI', b'D:\\AI', b'drakolord']   # matched case-insensitively, as ASCII and as UTF-16LE


class UsageError(Exception):
    """A missing input or a bad option: exit 2, nothing written."""


class Refused(Exception):
    """The audit refused the release: exit 3, nothing left in the output."""

    def __init__(self, problems, warnings=()):
        super().__init__('; '.join(problems))
        self.problems = list(problems)
        self.warnings = list(warnings)


def say(msg=''):
    print(msg, flush=True)


def fail(msg=''):
    print(msg, file=sys.stderr, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def default_version(repo):
    today = time.strftime('%Y-%m-%d')
    try:
        r = subprocess.run(['git', '-C', repo, 'describe', '--always', '--dirty'], capture_output=True, text=True)
        desc = r.stdout.strip() if r.returncode == 0 else ''
    except OSError:
        desc = ''
    return '%s-%s' % (today, desc) if desc else today


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default=REPO, help='repository root; every default path below is relative to it')
    ap.add_argument('--out', help='output folder (default <repo>/work/release_out)')
    ap.add_argument('--version', help='version in the names (default: today plus git describe --always --dirty)')
    ap.add_argument('--exe', help='mpr.exe (default <repo>/game-build/build/release/mpr.exe); the astcdec DLLs come from its folder')
    ap.add_argument('--setup-bin', help='folder with mpr_setup.exe and mpr_setup_cli.exe (default <repo>/tools/setup/build/release/bin)')
    ap.add_argument('--shader-pack', help='translated shader pack, copied as shader_cache.mprshc '
                                          '(default <repo>/work/setup_test/shader_cache.mprshc)')
    ap.add_argument('--no-shader-pack', action='store_true', help='build the slim variant without the shader pack (-noshaders)')
    ap.add_argument('--templates', help='templates folder (default <repo>/tools/release/templates)')
    ap.add_argument('--controls', help='controls file shipped as controls.ini (default templates/controls.ini when present)')
    ap.add_argument('--license', help='GPL-3.0 licence text (default <repo>/LICENSE)')
    ap.add_argument('--notices', help='THIRD_PARTY_NOTICES.txt (default templates/THIRD_PARTY_NOTICES.txt; required)')
    ap.add_argument('--deny-hash-dir', action='append', default=[], metavar='DIR',
                    help='refuse any release file whose sha256 equals a file directly inside DIR (repeatable, not recursive)')
    ap.add_argument('--strict-paths', action='store_true',
                    help='a local-path hit in mpr.exe or mpr_setup*.exe is an error instead of a warning')
    return ap.parse_args(argv)


def resolve(o):
    o.repo = os.path.abspath(o.repo)

    def under_repo(*parts):
        return os.path.join(o.repo, *parts)

    if o.no_shader_pack and o.shader_pack:
        raise UsageError('--no-shader-pack and --shader-pack cannot be given together')
    o.exe = os.path.abspath(o.exe or under_repo('game-build', 'build', 'release', EXE_NAME))
    o.setup_bin = os.path.abspath(o.setup_bin or under_repo('tools', 'setup', 'build', 'release', 'bin'))
    if o.no_shader_pack:
        o.shader_pack = None
    else:
        o.shader_pack = os.path.abspath(o.shader_pack or under_repo('work', 'setup_test', PACK_NAME))
    o.templates = os.path.abspath(o.templates or under_repo('tools', 'release', 'templates'))
    o.license = os.path.abspath(o.license or under_repo('LICENSE'))
    o.notices = os.path.abspath(o.notices or os.path.join(o.templates, 'THIRD_PARTY_NOTICES.txt'))
    o.controls = os.path.abspath(o.controls) if o.controls else None
    o.out = os.path.abspath(o.out or under_repo('work', 'release_out'))
    o.version = o.version or default_version(o.repo)
    o.deny_hash_dir = [os.path.abspath(d) for d in o.deny_hash_dir]
    return o


# ---------------------------------------------------------------------------------------------- plan
def plan_items(o):
    """(path inside the release, source path) for every file the release is made of."""
    def need(path, what):
        if not os.path.isfile(path):
            raise UsageError('%s not found: %s' % (what, path))
        return path

    exe = need(o.exe, EXE_NAME)
    items = [(EXE_NAME, exe)]
    dlls = sorted(glob.glob(os.path.join(glob.escape(os.path.dirname(exe)), DLL_GLOB)))
    if len(dlls) != DLL_COUNT:
        raise UsageError('expected exactly %d %s next to %s, found %d' % (DLL_COUNT, DLL_GLOB, exe, len(dlls)))
    items += [(os.path.basename(d), d) for d in dlls]
    for name in SETUP_EXES:
        items.append((name, need(os.path.join(o.setup_bin, name), name)))
    if o.shader_pack:
        items.append((PACK_NAME, need(o.shader_pack, 'shader pack')))
    items.append(('README.txt', need(os.path.join(o.templates, 'README.txt'), 'README.txt template')))
    if o.controls:
        items.append(('controls.ini', need(o.controls, 'controls file')))
    elif os.path.isfile(os.path.join(o.templates, 'controls.ini')):
        items.append(('controls.ini', os.path.join(o.templates, 'controls.ini')))
    if os.path.isfile(os.path.join(o.templates, 'mpr.ini.example')):
        items.append(('mpr.ini.example', os.path.join(o.templates, 'mpr.ini.example')))
    items.append(('LICENSE', need(o.license, 'LICENSE')))
    items.append(('THIRD_PARTY_NOTICES.txt', need(o.notices, 'THIRD_PARTY_NOTICES.txt')))
    for src, dst, required in LICENSE_FILES:
        p = os.path.join(o.repo, *src.split('/'))
        if os.path.isdir(p):
            found = []
            for dp, _, names in os.walk(p):
                for n in names:
                    f = os.path.join(dp, n)
                    found.append((os.path.relpath(f, p).replace(os.sep, '/'), f))
            if not found and required:
                raise UsageError('licence folder has no files: %s' % p)
            items += [('licenses/' + rel, f) for rel, f in sorted(found)]
        elif os.path.isfile(p):
            items.append(('licenses/' + (dst or os.path.basename(p)), p))
        elif required:
            raise UsageError('licence source not found: %s' % p)
    for src, dst, required in FONT_FILES:
        p = os.path.join(o.repo, *src.split('/'))
        if os.path.isfile(p):
            items.append((dst, p))
        elif required:
            raise UsageError('font source not found: %s' % p)
    rels = [rel for rel, _ in items]
    dups = sorted({r for r in rels if rels.count(r) > 1})
    if dups:
        raise UsageError('two sources map to the same release path: %s' % ', '.join(dups))
    return items


def deny_hashes(dirs):
    """sha256 -> path of every non-empty file directly inside each DIR."""
    out = {}
    for d in dirs:
        if not os.path.isdir(d):
            raise UsageError('--deny-hash-dir not a folder: %s' % d)
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if os.path.isfile(p) and os.path.getsize(p) > 0:
                out.setdefault(sha256_file(p), p)
    return out


def source_checks(o):
    """Refusals decided on the inputs before anything is written."""
    problems = []
    if o.shader_pack:
        base = os.path.basename(o.shader_pack)
        if base != PACK_NAME:
            problems.append('pack-name: the shader pack must be named exactly %s (it is %s)' % (PACK_NAME, base))
        with open(o.shader_pack, 'rb') as f:
            head = f.read(len(PACK_MAGIC))
        if head != PACK_MAGIC:
            problems.append('pack-magic: %s does not start with MPRSHC01' % o.shader_pack)
    if problems:
        raise Refused(problems)


# ---------------------------------------------------------------------------------------------- audit
class FileRef:
    def __init__(self, rel, size, opener):
        self.rel, self.size, self.opener = rel, size, opener


class Audit:
    def __init__(self):
        self.problems, self.warnings = [], []
        self.hashes, self.sizes = {}, {}


def folder_refs(folder):
    refs = {}
    for dp, _, names in os.walk(folder):
        for n in names:
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, folder).replace(os.sep, '/')
            refs[rel] = FileRef(rel, os.path.getsize(p), lambda p=p: open(p, 'rb'))
    return refs


def zip_refs(z, root, problems):
    refs = {}
    seen = set()
    for info in z.infolist():
        name = info.filename
        if name in seen:
            problems.append('zip: duplicate entry %s' % name)
        seen.add(name)
        if name.endswith('/'):
            problems.append('zip: directory entry %s (only files are allowed)' % name)
            continue
        if not name.startswith(root + '/'):
            problems.append('zip: entry outside the release folder %s' % name)
            continue
        rel = name[len(root) + 1:]
        refs[rel] = FileRef(rel, info.file_size, lambda n=name: z.open(n))
    return refs


def read_ref(ref, keep_body):
    """(first 16 bytes, sha256 hex, whole content if keep_body else None), one read of the file."""
    h = hashlib.sha256()
    body = bytearray() if keep_body else None
    with ref.opener() as f:
        head = f.read(16)
        while len(head) < 16:
            more = f.read(16 - len(head))
            if not more:
                break
            head += more
        h.update(head)
        if body is not None:
            body.extend(head)
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            if body is not None:
                body.extend(chunk)
    return head, h.hexdigest(), (bytes(body) if body is not None else None)


def denied_name(rel):
    parts = rel.split('/')
    low = parts[-1].lower()
    if low in DENY_NAMES:
        return 'key file name'
    ext = os.path.splitext(low)[1]
    if ext in DENY_EXT:
        return 'file type %s' % ext
    for p in parts:
        if p.lower() in DENY_PARTS:
            return 'path component %s' % p
    return None


def magic_problems(rel, head):
    if rel == PACK_NAME:
        return [] if head.startswith(PACK_MAGIC) else ['%s does not start with MPRSHC01' % rel]
    root = '/' not in rel
    exempt = root and (rel == EXE_NAME or rel.lower().endswith('.dll') or fnmatch.fnmatch(rel.lower(), 'mpr_setup*.exe'))
    if exempt:
        return []
    out = []
    if head.startswith(PACK_MAGIC):
        out.append('%s starts with the shader pack magic MPRSHC01 (only %s may)' % (rel, PACK_NAME))
    for magic, label in MAGIC_DENY:
        if head.startswith(magic):
            out.append('%s starts with %s' % (rel, label))
    return out


def text_problems(rel, body):
    if not rel.lower().endswith(TEXT_EXT):
        return []
    out = []
    for rx, label in KEY_TEXT:
        m = re.search(rx, body)
        if m:
            out.append('%s contains key material: %s (offset %#x)' % (rel, label, m.start()))
    return out


def size_problem(rel, size):
    if rel == EXE_NAME:
        key = 'exe'
    elif '/' not in rel and fnmatch.fnmatch(rel.lower(), 'mpr_setup*.exe'):
        key = 'setup'
    elif rel == PACK_NAME:
        key = 'pack'
    else:
        return None
    lo, hi = SIZE_RULES[key]
    if size < lo or (hi is not None and size > hi):
        return '%s is %s bytes, outside the allowed %s to %s' % (
            rel, format(size, ','), format(lo, ','), 'no limit' if hi is None else format(hi, ','))
    return None


def manifest_problems(text, hashes):
    out, listed = [], {}
    for n, line in enumerate(text.decode('utf-8', 'replace').splitlines(), 1):
        m = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not m:
            out.append('manifest: line %d is not "<sha256>  <path>"' % n)
            continue
        listed[m.group(2)] = m.group(1)
    for rel in sorted(set(hashes) - {MANIFEST} - set(listed)):
        out.append('manifest: %s is not listed' % rel)
    for rel in sorted(set(listed) - set(hashes)):
        out.append('manifest: %s is listed but not in the release' % rel)
    for rel in sorted(set(listed) & set(hashes)):
        if listed[rel] != hashes[rel]:
            out.append('manifest: sha256 of %s does not match the file' % rel)
    return out


def leak_scan(refs, audit, strict):
    """Local paths or the developer's name inside the executables: warnings, or problems with --strict-paths."""
    for rel in LEAK_FILES:
        if rel not in refs:
            continue
        with refs[rel].opener() as f:
            low = f.read().lower()
        for pat in LEAK_PATTERNS:
            for enc, label in ((pat, 'ASCII'), (pat.decode().encode('utf-16-le'), 'UTF-16LE')):
                i = low.find(enc.lower())
                if i >= 0:
                    msg = '%s contains "%s" (%s) at offset %#x' % (rel, pat.decode(), label, i)
                    (audit.problems if strict else audit.warnings).append(('paths: ' if strict else '') + msg)


def audit_files(refs, expected, deny, leak=False, strict=False):
    a = Audit()
    have = set(refs)
    for rel in sorted(have - expected):
        a.problems.append('allowlist: unexpected file %s' % rel)
    for rel in sorted(expected - have):
        a.problems.append('allowlist: missing file %s' % rel)
    manifest_text = None
    for rel in sorted(have):
        ref = refs[rel]
        a.sizes[rel] = ref.size
        why = denied_name(rel)
        if why:
            a.problems.append('denylist: %s: %s' % (rel, why))
        keep = rel == MANIFEST or rel.lower().endswith(TEXT_EXT)
        head, sha, body = read_ref(ref, keep)
        a.hashes[rel] = sha
        if rel == MANIFEST:
            manifest_text = body
        for p in magic_problems(rel, head):
            a.problems.append('magic: ' + p)
        if body is not None:
            for p in text_problems(rel, body):
                a.problems.append('text: ' + p)
        sp = size_problem(rel, ref.size)
        if sp:
            a.problems.append('size: ' + sp)
        if sha in deny:
            a.problems.append('deny-hash: %s has the same sha256 as %s' % (rel, deny[sha]))
    if MANIFEST in have and manifest_text is not None:
        for p in manifest_problems(manifest_text, a.hashes):
            a.problems.append(p)
    if leak:
        leak_scan(refs, a, strict)
    return a


# ---------------------------------------------------------------------------------------------- build
def stage(items, folder):
    origins = {}
    for rel, src in items:
        dst = os.path.join(folder, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        origins[rel] = src
    return origins


def write_manifest(folder):
    rels = sorted(folder_refs(folder))
    lines = ['%s  %s\n' % (sha256_file(os.path.join(folder, *rel.split('/'))), rel) for rel in rels]
    with open(os.path.join(folder, MANIFEST), 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(lines)


def write_zip(zpath, folder, root, rels):
    with zipfile.ZipFile(zpath, 'w') as z:
        for rel in sorted(rels):
            zi = zipfile.ZipInfo(root + '/' + rel, date_time=ZIP_DATE)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3
            zi.external_attr = 0o644 << 16
            with open(os.path.join(folder, *rel.split('/')), 'rb') as src, z.open(zi, 'w') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)


def remove_output(p):
    if os.path.isdir(p) and not os.path.islink(p):
        shutil.rmtree(p, ignore_errors=True)
    elif os.path.lexists(p):
        try:
            os.remove(p)
        except OSError:
            pass


def render_table(audit, origins, zpath, zsize, zsha):
    rels = sorted(audit.sizes)
    pw = max([len('path')] + [len(r) for r in rels])
    hdr = '%-*s  %12s  %-12s  %-16s  %s' % (pw, 'path', 'size', 'sha256', 'status', 'origin')
    lines = [hdr, '-' * len(hdr)]
    for rel in rels:
        warned = any(w.startswith(rel + ':') for w in audit.warnings)
        lines.append('%-*s  %12s  %-12s  %-16s  %s' % (
            pw, rel, format(audit.sizes[rel], ','), audit.hashes[rel][:12], 'WARN: local path' if warned else 'ok',
            origins.get(rel, '?')))
    total = sum(audit.sizes.values())
    lines += ['',
              'files: %d   folder: %s bytes (%.1f MiB)' % (len(rels), format(total, ','), total / MIB),
              'zip: %s   %s bytes (%.1f MiB)' % (os.path.basename(zpath), format(zsize, ','), zsize / MIB),
              'zip sha256: %s' % zsha]
    lines += ['warning: %s' % w for w in audit.warnings]
    return '\n'.join(lines) + '\n'


def report_refused(problems, warnings):
    fail('REFUSED: no release written, %d problem(s):' % len(problems))
    for p in problems:
        fail('  - ' + p)
    for w in warnings:
        fail('  (warning) ' + w)


def build(o, stage_hook=None):
    o = resolve(o)
    if not re.fullmatch(VERSION_RE, o.version):
        raise UsageError('bad --version %r: use letters, digits, dot, dash and underscore only' % o.version)
    items = plan_items(o)
    deny = deny_hashes(o.deny_hash_dir)
    source_checks(o)

    base = 'mpr-%s-windows-x64%s' % (o.version, '' if o.shader_pack else '-noshaders')
    folder = os.path.join(o.out, base)
    zpath = folder + '.zip'
    table = folder + '.manifest.txt'
    outputs = [folder, zpath, table]
    try:
        for p in outputs:
            remove_output(p)
        os.makedirs(o.out, exist_ok=True)
        os.makedirs(folder)
        origins = stage(items, folder)
        write_manifest(folder)
        origins[MANIFEST] = 'generated by make_release.py'
        if stage_hook:
            stage_hook(folder)
        expected = set(origins)

        fa = audit_files(folder_refs(folder), expected, deny, leak=True, strict=o.strict_paths)
        if fa.problems:
            raise Refused(fa.problems, fa.warnings)

        write_zip(zpath, folder, base, expected)
        zip_problems = []
        with zipfile.ZipFile(zpath) as z:
            za = audit_files(zip_refs(z, base, zip_problems), expected, deny)
        problems = zip_problems + za.problems
        if problems:
            raise Refused(problems, fa.warnings)

        zsize = os.path.getsize(zpath)
        zsha = sha256_file(zpath)
        text = render_table(fa, origins, zpath, zsize, zsha)
        with open(table, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    except Refused as r:
        for p in outputs:
            remove_output(p)
        report_refused(r.problems, r.warnings)
        return 3
    except OSError as e:
        for p in outputs:
            remove_output(p)
        fail('I/O error, nothing written: %s' % e)
        return 1

    say(text)
    say('RELEASE OK: %s' % folder)
    say('zip: %s' % zpath)
    say('table: %s' % table)
    return 0


def main(argv=None, stage_hook=None):
    """Build the release. stage_hook(folder), for tests only, runs after the manifest is written and before the audit."""
    o = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return build(o, stage_hook)
    except UsageError as e:
        fail('error: %s' % e)
        return 2
    except Refused as r:                       # refused on the inputs, before anything was written
        report_refused(r.problems, r.warnings)
        return 3


if __name__ == '__main__':
    sys.exit(main())
