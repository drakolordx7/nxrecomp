#!/usr/bin/env python3
r"""Build the release of one game (--game DIR, its manifest game.json names every file) for Windows x64 (default) or Linux x86-64
(--platform linux-x86_64): a release folder and an archive of it (zip; tar.xz on Linux), audited before and after the archive is written.

    python tools/release/make_release.py --game DIR [--platform windows-x64|linux-x86_64] [--out DIR] [--version V] [--no-shader-pack]
                                         [--allow-local-paths] [--deny-hash-dir DIR ...] [--repo DIR] [--base DIR] [--exe F] [--setup-bin DIR]
                                         [--shader-pack F] [--templates DIR] [--controls F] [--license F] [--notices F]
                                         [--prompts-dir DIR] [--launcher F]
    python tools/release/make_release.py --print-ffmpeg-pins DIR [--platform P]     SHA-256 of the pinned FFmpeg files in DIR (for ffmpeg_pins.json)

Defaults follow docs/BUILD.md: the exe is NX/build/<main | worktree name>/<id>/relpkg/<files.exe> (game exe built with -Release; Linux:
NX/build/<name>/linux/<id>_relpkg/<exe without .exe>), the setup tools NX/build/setup-<id>/bin (Linux: setup-<id>-linux/bin), the shader pack
<game>/work/setup_test/<files.shader_pack>, the output <game>/work/release_out. NX is $NX_ROOT, else the main checkout (this one, or the parent
of wt/ for a worktree). --base is the base checkout holding LICENSE, runtime/assets/prompts, tools/release/licenses, ffmpeg_pins.json and the
fonts (default: the checkout of this script); --repo is only the repository that git describe names the version after (default: the game folder).

Output, in --out (<platform> = windows-x64 or linux-x86_64):
    <prefix>-<version>-<platform>/                 the release folder
    <prefix>-<version>-<platform>.zip | .tar.xz    the same folder packed: one top-level folder, sorted entries, fixed timestamps (tar: owner root,
                                                   mode 755 for programs and libraries, 644 for the rest)
    <prefix>-<version>-<platform>.manifest.txt     the audit table and totals (written only on success)
    <archive>.sha256                               the archive's SHA-256 in the usual "<hash>  <name>" form
Without the shader pack the folder and the archive get the suffix -noshaders.
The prompt icon pack prompts/prompt_icons.pak and its licence prompts/LICENSE-Kenney-InputPrompts.txt are required,
read from --prompts-dir (default <base>/runtime/assets/prompts).
Windows: the launcher (<id>_launcher.exe) is read from --launcher (default: next to --exe) and ships in the release root beside the game exe, the
ASTC decoder DLLs, the five FFmpeg DLLs and the setup tools (<id>_setup.exe, <id>_setup_cli.exe). Linux: the exe, the ASTC decoder and FFmpeg .so
files (built with -DNXRT_FFMPEG_BUNDLED=ON, found through an $ORIGIN rpath), the setup cli and the launcher when one was built, else a small
placeholder script <id>_launcher that explains how to start the game. Names are those in game.json `files` (Linux: without .exe).
FFmpeg: tools/release/ffmpeg_pins.json names the files and their SHA-256 (Windows; the archives are pinned in runtime/cmake/ffmpeg.cmake).

Exit codes: 0 built and audited; 1 an I/O error; 2 a missing input or a bad option; 3 refused by the audit.
A refused or failed build leaves none of its output behind. The .pdb and .map beside the game exe are never copied.
The Windows audit refuses a shipped .exe or .dll that imports the dynamic MSVC or UCRT runtime (see CRT_PREFIXES); the pinned FFmpeg DLLs
(mingw-built, UCRT) are the one exception, by file name and SHA-256 only. The Linux audit reads every ELF file: x86-64 only, DT_NEEDED limited to
glibc, the X11/Wayland/Vulkan client libraries and libraries of the release itself (no shared libstdc++/libgcc_s: they are linked statically),
RPATH/RUNPATH only $ORIGIN, glibc symbol versions up to MAX_GLIBC.
"""
import argparse
import calendar
import fnmatch
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import json
import zipfile

MIB = 1024 * 1024
KIB = 1024
GIB = 1024 * MIB

BASE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(BASE, 'formats'))
sys.path.insert(0, os.path.join(BASE, 'scripts'))
import nx_formats  # noqa: E402  the shared definition of the shader pack (formats/nx_formats.py)
import nxpaths  # noqa: E402  NX_ROOT

# The game this release is for: its program and file names, from its manifest games/<id>/game.json (`files` and `release`), named
# by --game DIR or NX_GAME_DIR. set_game() fills the names below.
GAME = None
RELEASE_PREFIX = EXE_NAME = SETUP_GLOB = LAUNCHER_NAME = GAME_INI = PACK_NAME = None   # <prefix>-<version>-<platform>; the launcher
SETUP_EXES = ()                                                                        # sits beside the game exe, built with the runtime
WINDOWS, LINUX = 'windows-x64', 'linux-x86_64'
PLATFORM = WINDOWS                    # the flavour being built: set_game() / the --platform option
DLL_GLOB = 'astcdec-*-shared.dll'     # the ASTC decoder libraries next to the exe (Linux: libastcdec-*-shared.so)
DLL_COUNT = 3
FFMPEG_FILES = ()                     # the FFmpeg libraries this flavour ships next to the exe (tools/release/ffmpeg_pins.json)
FFMPEG_HASHES = {}                    # their pinned SHA-256 (None: not measured yet, only the audit applies)
LAUNCHER_PLACEHOLDER = False          # Linux without a built launcher: the shipped <id>_launcher is a script (no size rule)
EXEC_RULE = ('.so',)                  # Linux: files with these in their name are libraries (mode 755 in the tar, ELF audit)
# glibc symbol versions a shipped binary may need: the oldest system the release runs on. The build box (Ubuntu 22.04) has 2.35.
MAX_GLIBC = (2, 35)
# DT_NEEDED a shipped ELF may name besides the libraries shipped in the release itself: glibc, and the window system and Vulkan loader
# libraries (SDL3 is linked statically and opens X11, Wayland, audio and Vulkan with dlopen, so these only appear when linked on purpose).
ELF_ALLOWED_NEEDED = (r'ld-linux-x86-64[.]so[.]2', r'libc[.]so[.]6', r'libm[.]so[.]6', r'libdl[.]so[.]2', r'libpthread[.]so[.]0',
                      r'librt[.]so[.]1', r'libutil[.]so[.]1', r'libvulkan[.]so[.]1', r'libX11[.]so[.]6', r'libXext[.]so[.]6',
                      r'libXcursor[.]so[.]1', r'libXi[.]so[.]6', r'libXfixes[.]so[.]3', r'libXrandr[.]so[.]2', r'libXss[.]so[.]1',
                      r'libXrender[.]so[.]1', r'libXinerama[.]so[.]1', r'libxcb[A-Za-z0-9-]*[.]so[.][0-9]+', r'libxkbcommon[.]so[.]0',
                      r'libwayland-(client|cursor|egl)[.]so[.][0-9]+')
PACK_MAGICS = (nx_formats.SHADER_PACK_MAGIC, nx_formats.SHADER_PACK_MAGIC_LEGACY)   # shader pack header (formats/nx_formats.h); the v1
PACK_MAGIC = PACK_MAGICS[0]                      # magic is still accepted, new packs are written with the first
PROMPT_FILES = ('prompt_icons.pak', 'LICENSE-Kenney-InputPrompts.txt')   # required, shipped as prompts/<name>
PROMPT_PACK = 'prompts/prompt_icons.pak'        # the only .pak the audit allows, by exact release path
PROMPT_MAGIC = nx_formats.PROMPT_PACK_MAGIC     # header of the prompt icon pack
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
SIZE_RULES = {'exe': (10 * MIB, 400 * MIB), 'setup': (100 * KIB, None), 'pack': (100 * MIB, 1 * GIB),
              'prompt': (KIB, 4 * MIB - 1), 'launcher': (100 * KIB, 40 * MIB)}

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
    (nx_formats.BC7_CACHE_MAGIC, '%s (BC7 cache)' % nx_formats.BC7_CACHE_MAGIC.decode()),
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
LEAK_FILES = ()


def set_game(game_dir, platform=WINDOWS):
    """Read the game's manifest (<game_dir>/game.json) into the names above, spelled for `platform` (Linux: no .exe, the setup cli only)."""
    global GAME, RELEASE_PREFIX, EXE_NAME, SETUP_EXES, SETUP_GLOB, LAUNCHER_NAME, GAME_INI, PACK_NAME, LEAK_FILES, PLATFORM, DLL_GLOB
    path = os.path.join(game_dir, 'game.json')
    if not os.path.isfile(path):
        raise UsageError('game manifest not found: %s (--game names a game folder)' % path)
    with open(path, encoding='utf-8') as f:
        GAME = json.load(f)
    if platform not in (WINDOWS, LINUX):
        raise UsageError('unknown --platform %s (windows-x64 or linux-x86_64)' % platform)
    PLATFORM = platform
    files, rel = GAME['files'], GAME['release']
    RELEASE_PREFIX = rel['prefix']
    GAME_INI = files['game_ini']
    PACK_NAME = files['shader_pack']
    if platform == WINDOWS:
        SETUP_GLOB = rel['setup_glob']
        EXE_NAME = files['exe']
        SETUP_EXES = (files['setup'], files['setup_cli'])
        LAUNCHER_NAME = files['launcher']
        DLL_GLOB = 'astcdec-*-shared.dll'
    else:
        SETUP_GLOB = nxpaths.exe_stem(rel['setup_glob'])
        EXE_NAME = nxpaths.exe_name(files['exe'], 'linux')
        SETUP_EXES = (nxpaths.exe_name(files['setup_cli'], 'linux'),)      # the setup GUI is Windows only
        LAUNCHER_NAME = nxpaths.exe_name(files['launcher'], 'linux')
        DLL_GLOB = 'libastcdec-*-shared.so'
    LEAK_FILES = (EXE_NAME, LAUNCHER_NAME) + SETUP_EXES


def load_ffmpeg(base, platform=None):
    """The FFmpeg libraries of this flavour from <base>/tools/release/ffmpeg_pins.json into FFMPEG_FILES / FFMPEG_HASHES."""
    global FFMPEG_FILES, FFMPEG_HASHES
    path = os.path.join(base, 'tools', 'release', 'ffmpeg_pins.json')
    if not os.path.isfile(path):
        raise UsageError('FFmpeg pins not found: %s (the release ships the FFmpeg libraries the exe links)' % path)
    with open(path, encoding='utf-8') as f:
        pins = json.load(f)
    entry = pins.get(platform or PLATFORM)
    if not entry or not entry.get('files'):
        raise UsageError('%s has no files for %s' % (path, platform or PLATFORM))
    FFMPEG_FILES = tuple(sorted(entry['files']))
    FFMPEG_HASHES = dict(entry['files'])
    return pins


def _leak_patterns():
    # This machine's own paths and user name must never end up in a release: the checkout's parent folders (as Windows, WSL and Linux
    # spell them), the user name and anything listed in NX_LEAK_PATTERNS (comma separated).
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parts = root.replace('\\', '/').split('/')
    spellings = {root, '/'.join(parts[:2])}
    for p in list(spellings):
        q = p.replace('\\', '/')
        if len(q) > 2 and q[1] == ':':
            spellings.add('/mnt/%s%s' % (q[0].lower(), q[2:]))      # the same folder seen from WSL: what a Linux build records
    pats = {*spellings, os.environ.get('USERNAME', ''), '' if os.environ.get('USER') == 'root' else os.environ.get('USER', ''), *os.environ.get('NX_LEAK_PATTERNS', '').split(',')}
    out = set()
    for p in pats:
        p = p.strip()
        if len(p) >= 4:
            out |= {p.encode(), p.replace('\\', '/').encode(), p.replace('/', '\\').encode()}
    return sorted(out)


LEAK_PATTERNS = _leak_patterns()   # matched case-insensitively, as ASCII and as UTF-16LE


PE_EXT = ('.exe', '.dll')                       # files whose import tables the audit reads
CRT_PREFIXES = ('vcruntime', 'msvcp', 'msvcr', 'vcomp', 'concrt', 'ucrtbase', 'api-ms-win-crt-')   # dynamic MSVC / UCRT runtime


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
    """Today plus `git describe` of the repository `repo` (the game's)."""
    today = time.strftime('%Y-%m-%d')
    try:
        r = subprocess.run(['git', '-C', repo, 'describe', '--always', '--dirty'], capture_output=True, text=True)
        desc = r.stdout.strip() if r.returncode == 0 else ''
    except OSError:
        desc = ''
    return '%s-%s' % (today, desc) if desc else today


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=os.environ.get('NX_GAME_DIR'), help='game folder with the manifest game.json (default NX_GAME_DIR)')
    ap.add_argument('--platform', default=WINDOWS, choices=(WINDOWS, LINUX),
                    help='the flavour to build: windows-x64 (zip) or linux-x86_64 (tar.xz; the files come from a Linux build, run this anywhere)')
    ap.add_argument('--print-ffmpeg-pins', metavar='DIR',
                    help='print the SHA-256 of the pinned FFmpeg libraries found in DIR as JSON (to fill ffmpeg_pins.json) and exit')
    ap.add_argument('--repo', help='the repository git describe names the version after (default: the game folder)')
    ap.add_argument('--base', default=BASE, help='base checkout holding LICENSE, runtime/assets/prompts, tools/release/licenses and the fonts')
    ap.add_argument('--out', help='output folder (default <game>/work/release_out)')
    ap.add_argument('--version', help='version in the names (default: today plus git describe --always --dirty)')
    ap.add_argument('--exe', help='the game exe of the package build (default NX/build/<game folder name>/relpkg/<exe>, made by runtime/scripts/build_game_exe.ps1 -Release; '
                                  'Linux: NX/build/<name>/linux/<id>_relpkg/<exe>); the astcdec and FFmpeg libraries come from its folder')
    ap.add_argument('--launcher', help='the launcher exe (default: the one in the folder of --exe; Linux: a placeholder script when there is none)')
    ap.add_argument('--setup-bin', help='folder with the setup exes (default NX/build/setup-<id>/bin; Linux: setup-<id>-linux/bin)')
    ap.add_argument('--shader-pack', help='translated shader pack, copied under the shader_pack name of game.json '
                                          '(default <game>/work/setup_test/<shader_pack>)')
    ap.add_argument('--no-shader-pack', action='store_true', help='build the slim variant without the shader pack (-noshaders)')
    ap.add_argument('--templates', help='templates folder: README.txt, HELP.txt, THIRD_PARTY_NOTICES.txt, optional controls.ini and <game ini>.example '
                                        '(default <game folder>/release/templates)')
    ap.add_argument('--controls', help='controls file shipped as controls.ini (default templates/controls.ini when present)')
    ap.add_argument('--license', help='GPL-3.0 licence text (default <base>/LICENSE)')
    ap.add_argument('--notices', help='THIRD_PARTY_NOTICES.txt (default <templates>/THIRD_PARTY_NOTICES.txt; required)')
    ap.add_argument('--prompts-dir', help='folder with prompt_icons.pak and LICENSE-Kenney-InputPrompts.txt, shipped under prompts/ '
                                          '(default <base>/runtime/assets/prompts; both required)')
    ap.add_argument('--deny-hash-dir', action='append', default=[], metavar='DIR',
                    help='refuse any release file whose sha256 equals a file directly inside DIR (repeatable, not recursive)')
    ap.add_argument('--allow-local-paths', action='store_true',
                    help='a local-path hit in the executables is only a warning (for development builds)')
    ap.add_argument('--strict-paths', action='store_true', help=argparse.SUPPRESS)   # old name: now the default, no-op
    return ap.parse_args(argv)


def resolve(o):
    if not o.game:
        raise UsageError('no game: pass --game DIR (or set NX_GAME_DIR)')
    o.game = os.path.abspath(o.game)
    set_game(o.game, o.platform)
    o.base = os.path.abspath(o.base)
    load_ffmpeg(o.base, o.platform)
    nx = nxpaths.find_root()

    def under_game(*parts):
        return os.path.join(o.game, *parts)

    def under_nx(*parts):
        if not nx:
            raise UsageError('no NX folder around %s: pass the paths (--exe, --setup-bin) or set NX_ROOT' % BASE)
        return os.path.join(nx, *parts)

    if o.no_shader_pack and o.shader_pack:
        raise UsageError('--no-shader-pack and --shader-pack cannot be given together')
    if PLATFORM == WINDOWS:
        o.exe = os.path.abspath(o.exe or under_nx('build', nxpaths.CHECKOUT_NAME, GAME['id'], 'relpkg', EXE_NAME))
        o.setup_bin = os.path.abspath(o.setup_bin or under_nx('build', 'setup-' + GAME['id'], 'bin'))
    else:
        o.exe = os.path.abspath(o.exe or under_nx('build', nxpaths.CHECKOUT_NAME, 'linux', GAME['id'] + '_relpkg', EXE_NAME))
        o.setup_bin = os.path.abspath(o.setup_bin or under_nx('build', 'setup-%s-linux' % GAME['id'], 'bin'))
    o.launcher = os.path.abspath(o.launcher or os.path.join(os.path.dirname(o.exe), LAUNCHER_NAME))
    if o.no_shader_pack:
        o.shader_pack = None
    else:
        o.shader_pack = os.path.abspath(o.shader_pack or under_game('work', 'setup_test', PACK_NAME))
    o.templates = os.path.abspath(o.templates or os.path.join(o.game, 'release', 'templates'))
    o.license = os.path.abspath(o.license or os.path.join(o.base, 'LICENSE'))
    o.notices = os.path.abspath(o.notices or os.path.join(o.templates, 'THIRD_PARTY_NOTICES.txt'))
    o.prompts_dir = os.path.abspath(o.prompts_dir or os.path.join(o.base, 'runtime', 'assets', 'prompts'))
    o.controls = os.path.abspath(o.controls) if o.controls else None
    o.out = os.path.abspath(o.out or under_game('work', 'release_out'))
    o.version = o.version or default_version(os.path.abspath(o.repo or o.game))
    o.deny_hash_dir = [os.path.abspath(d) for d in o.deny_hash_dir]
    return o


# ---------------------------------------------------------------------------------------------- plan
def plan_items(o):
    """(path inside the release, source path) for every file the release is made of."""
    def need(path, what):
        if not os.path.isfile(path):
            raise UsageError('%s not found: %s' % (what, path))
        return path

    global LAUNCHER_PLACEHOLDER
    exe = need(o.exe, EXE_NAME)
    items = [(EXE_NAME, exe)]
    dlls = sorted(glob.glob(os.path.join(glob.escape(os.path.dirname(exe)), DLL_GLOB)))
    if len(dlls) != DLL_COUNT:
        raise UsageError('expected exactly %d %s next to %s, found %d' % (DLL_COUNT, DLL_GLOB, exe, len(dlls)))
    items += [(os.path.basename(d), d) for d in dlls]
    for name in FFMPEG_FILES:        # the FFmpeg libraries the exe links (LGPL: shipped as separate files)
        items.append((name, need(os.path.join(os.path.dirname(exe), name), 'FFmpeg library %s' % name)))
    for name in SETUP_EXES:
        items.append((name, need(os.path.join(o.setup_bin, name), name)))
    LAUNCHER_PLACEHOLDER = PLATFORM == LINUX and not os.path.isfile(o.launcher)
    if LAUNCHER_PLACEHOLDER:         # no Linux launcher yet: a script that says how to start the game
        items.append((LAUNCHER_NAME, launcher_placeholder()))
    else:
        items.append((LAUNCHER_NAME, need(o.launcher, LAUNCHER_NAME)))
    if o.shader_pack:
        items.append((PACK_NAME, need(o.shader_pack, 'shader pack')))
    def template(name):          # templates/linux/<name> first on Linux, then templates/<name>
        specific = os.path.join(o.templates, 'linux', name)
        return specific if PLATFORM == LINUX and os.path.isfile(specific) else os.path.join(o.templates, name)

    for name in ('README.txt', 'HELP.txt'):
        items.append((name, need(template(name), '%s template' % name)))
    if o.controls:
        items.append(('controls.ini', need(o.controls, 'controls file')))
    elif os.path.isfile(template('controls.ini')):
        items.append(('controls.ini', template('controls.ini')))
    if os.path.isfile(template(GAME_INI + '.example')):
        items.append((GAME_INI + '.example', template(GAME_INI + '.example')))
    items.append(('LICENSE', need(o.license, 'LICENSE')))
    items.append(('THIRD_PARTY_NOTICES.txt', need(o.notices, 'THIRD_PARTY_NOTICES.txt')))
    for name in PROMPT_FILES:
        items.append(('prompts/' + name, need(os.path.join(o.prompts_dir, name), 'prompt icon file %s' % name)))
    for src, dst, required in LICENSE_FILES:
        p = os.path.join(o.base, *src.split('/'))
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
        p = os.path.join(o.base, *src.split('/'))
        if os.path.isfile(p):
            items.append((dst, p))
        elif required:
            raise UsageError('font source not found: %s' % p)
    rels = [rel for rel, _ in items]
    dups = sorted({r for r in rels if rels.count(r) > 1})
    if dups:
        raise UsageError('two sources map to the same release path: %s' % ', '.join(dups))
    return items


def launcher_placeholder():
    """The script shipped as <id>_launcher on Linux until a launcher is built: it only explains and starts nothing by itself."""
    setup = SETUP_EXES[-1]
    lines = ['#!/bin/sh',
             '# Placeholder: the launcher is not available on Linux yet.',
             'echo "The launcher is not available on Linux yet."',
             'echo "1. ./%s --input <your dump> --out <game folder>     (once; --help lists the options)"' % setup,
             'echo "2. ./%s     (settings: %s next to it)"' % (EXE_NAME, GAME_INI),
             'exit 1',
             '']
    return '\n'.join(lines).encode('utf-8')


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
        if head not in PACK_MAGICS:
            problems.append('pack-magic: %s does not start with %s' % (o.shader_pack, PACK_MAGIC.decode()))
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
    if ext in DENY_EXT and not (ext == '.pak' and '/'.join(parts) == PROMPT_PACK):
        return 'file type %s' % ext
    for p in parts:
        if p.lower() in DENY_PARTS:
            return 'path component %s' % p
    return None


def is_library(rel):
    """A shared library of the Linux release (libfoo.so, libfoo.so.62) in the release root."""
    return PLATFORM == LINUX and '/' not in rel and re.fullmatch(r'lib.+[.]so([.][0-9]+)*', rel) is not None


def magic_problems(rel, head):
    if rel == PACK_NAME:
        return [] if head.startswith(PACK_MAGICS) else ['%s does not start with %s' % (rel, PACK_MAGIC.decode())]
    if rel == PROMPT_PACK:
        out = [] if head.startswith(PROMPT_MAGIC) else ['%s does not start with MPIP (prompt icon pack)' % rel]
        out += ['%s starts with %s' % (rel, label) for magic, label in MAGIC_DENY if head.startswith(magic)]
        return out
    root = '/' not in rel
    exempt = root and (rel in (EXE_NAME, LAUNCHER_NAME) or rel.lower().endswith('.dll') or is_library(rel)
                       or fnmatch.fnmatch(rel.lower(), SETUP_GLOB))
    if exempt:
        return []
    out = []
    if head.startswith(PACK_MAGICS):
        out.append('%s starts with the shader pack magic %s (only %s may)' % (rel, PACK_MAGIC.decode(), PACK_NAME))
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


def pe_imports(data):
    """Lower-case DLL names in the import and delay-import directories of a PE32+ file, in order, each once.

    None when data is not a PE32+ file or is cut short. The caller treats None as unreadable, not as clean.
    """
    try:
        return _pe_imports(data)
    except (struct.error, ValueError, IndexError):
        return None


def _pe_imports(data):
    if data[:2] != b'MZ':
        return None
    lfanew, = struct.unpack_from('<I', data, 0x3C)
    if data[lfanew:lfanew + 4] != b'PE\0\0':
        return None
    nsec, = struct.unpack_from('<H', data, lfanew + 6)
    opt_size, = struct.unpack_from('<H', data, lfanew + 20)
    opt = lfanew + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x20B:          # PE32+ only: the release is x64
        return None
    image_base, = struct.unpack_from('<Q', data, opt + 24)
    ndirs, = struct.unpack_from('<I', data, opt + 108)
    dirs = opt + 112
    sections = []
    for i in range(nsec):
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, opt + opt_size + 40 * i + 8)
        sections.append((va, max(vsize, rawsize), rawptr))

    def offset(rva):
        for va, span, raw in sections:
            if va <= rva < va + span:
                return rva - va + raw
        raise ValueError('rva %#x is outside every section' % rva)

    def name_at(rva):
        o = offset(rva)
        return data[o:data.index(b'\0', o)].decode('ascii', 'replace').lower()

    def directory(index):
        return struct.unpack_from('<II', data, dirs + 8 * index) if index < ndirs else (0, 0)

    names = []

    def add(name):
        if name not in names:
            names.append(name)

    rva, _ = directory(1)                                       # normal imports: 20-byte descriptors
    while rva:
        desc = data[offset(rva):offset(rva) + 20]
        if len(desc) != 20:
            raise ValueError('truncated import descriptor')
        if desc == bytes(20):
            break
        add(name_at(struct.unpack_from('<I', desc, 12)[0]))
        rva += 20

    rva, _ = directory(13)                                      # delay imports: 32-byte descriptors
    while rva:
        desc = data[offset(rva):offset(rva) + 32]
        if len(desc) != 32:
            raise ValueError('truncated delay-import descriptor')
        if desc == bytes(32):
            break
        attrs, dll = struct.unpack_from('<II', desc, 0)
        if not attrs & 1:                                       # old form: the name is an absolute address
            dll -= image_base
        add(name_at(dll))
        rva += 32
    return names


def ffmpeg_pinned(rel, sha):
    """True for a root file that is one of the pinned FFmpeg libraries with exactly the pinned SHA-256."""
    return '/' not in rel and FFMPEG_HASHES.get(rel) is not None and FFMPEG_HASHES[rel] == sha


def crt_problems(rel, data, a, sha=None):
    """Refuse a shipped .exe or .dll that imports the dynamic MSVC or UCRT runtime; warn when it cannot be read.

    The one exception: the pinned FFmpeg DLLs (built with mingw against the UCRT), by file name and SHA-256. Our own binaries never get it.
    """
    names = pe_imports(data)
    if names is None:
        a.warnings.append('%s is not a readable PE file, so its imports were not checked' % rel)
        return
    if sha is not None and ffmpeg_pinned(rel, sha):
        return
    for name in names:
        if name.startswith(CRT_PREFIXES):
            a.problems.append('crt-import: %s imports %s' % (rel, name))


ELF_MAGIC = bytes([0x7F]) + b'ELF'


def elf_info(data):
    """What the audit needs from an ELF64 little-endian file: {'machine', 'needed', 'rpath', 'soname', 'glibc', 'cxx'}.

    needed: DT_NEEDED names; rpath: the DT_RPATH and DT_RUNPATH entries; glibc: the highest GLIBC_x.y symbol version required (a tuple, or None);
    cxx: the GLIBCXX / CXXABI versions required (the shared C++ runtime). None when data is not such a file or is cut short.
    """
    try:
        return _elf_info(data)
    except (struct.error, ValueError, IndexError):
        return None


def _elf_info(data):
    if data[:4] != ELF_MAGIC or data[4] != 2 or data[5] != 1:
        return None
    machine, = struct.unpack_from('<H', data, 0x12)
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    loads, dyn = [], None
    for i in range(phnum):
        p_type, _, p_off, p_vaddr, _, p_filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if p_type == 1:
            loads.append((p_vaddr, p_filesz, p_off))
        elif p_type == 2:
            dyn = (p_off, p_filesz)
    info = {'machine': machine, 'needed': [], 'rpath': [], 'soname': None, 'glibc': None, 'cxx': []}
    if dyn is None:
        return info

    def offset(vaddr):
        for va, size, off in loads:
            if va <= vaddr < va + size:
                return vaddr - va + off
        raise ValueError('address %#x is outside every segment' % vaddr)

    tags = {}
    needed_offs, rpath_offs = [], []
    pos = dyn[0]
    while pos + 16 <= dyn[0] + dyn[1]:
        tag, val = struct.unpack_from('<qQ', data, pos)
        pos += 16
        if tag == 0:
            break
        if tag == 1:
            needed_offs.append(val)
        elif tag in (15, 29):
            rpath_offs.append(val)
        else:
            tags[tag] = val
    if 5 not in tags:
        return info
    strtab = offset(tags[5])

    def cstr(o):
        s = strtab + o
        return data[s:data.index(b'\0', s)].decode('ascii', 'replace')

    info['needed'] = [cstr(o) for o in needed_offs]
    info['rpath'] = [e for o in rpath_offs for e in cstr(o).split(':') if e]
    if 14 in tags:
        info['soname'] = cstr(tags[14])
    verneed, count = tags.get(0x6FFFFFFE), tags.get(0x6FFFFFFF, 0)
    if verneed and count:
        vn = offset(verneed)
        for _ in range(count):
            _, cnt, _, aux, nxt = struct.unpack_from('<HHIII', data, vn)
            va = vn + aux
            for _ in range(cnt):
                _, _, _, name, anext = struct.unpack_from('<IHHII', data, va)
                ver = cstr(name)
                m = re.fullmatch(r'GLIBC_(\d+)\.(\d+)(?:\.(\d+))?', ver)
                if m:
                    v = (int(m.group(1)), int(m.group(2)))
                    if info['glibc'] is None or v > info['glibc']:
                        info['glibc'] = v
                elif ver.startswith(('GLIBCXX_', 'CXXABI_')):
                    info['cxx'].append(ver)
                va += anext
            if not nxt:
                break
            vn += nxt
    return info


def elf_problems(rel, data, a, shipped):
    """The Linux audit of one shipped ELF file (see the module doc); `shipped`: names of the libraries in the release root."""
    info = elf_info(data)
    if info is None:
        a.warnings.append('%s is not a readable ELF64 file, so its dependencies were not checked' % rel)
        return
    if info['machine'] != 62:
        a.problems.append('elf-arch: %s is not an x86-64 file (e_machine %d)' % (rel, info['machine']))
    for name in info['needed']:
        if name in shipped or any(re.fullmatch(rx, name) for rx in ELF_ALLOWED_NEEDED):
            continue
        shared_cxx = name in ('libstdc++.so.6', 'libgcc_s.so.1')
        a.problems.append('elf-needed: %s needs %s%s' % (rel, name, ' (the C++ runtime must be linked statically)' if shared_cxx else ''))
    for e in info['rpath']:
        if e != '$ORIGIN' and not e.startswith('$ORIGIN/'):
            a.problems.append('elf-rpath: %s has the search path %s (only $ORIGIN is allowed)' % (rel, e))
    if info['cxx']:
        a.problems.append('elf-needed: %s needs the shared C++ runtime (%s)' % (rel, info['cxx'][-1]))
    if info['glibc'] is not None and info['glibc'] > MAX_GLIBC:
        a.problems.append('elf-glibc: %s needs GLIBC_%d.%d, newer than the allowed GLIBC_%d.%d' % ((rel,) + info['glibc'] + MAX_GLIBC))


def size_problem(rel, size):
    if rel == EXE_NAME:
        key = 'exe'
    elif '/' not in rel and fnmatch.fnmatch(rel.lower(), SETUP_GLOB):
        key = 'setup'
    elif rel == LAUNCHER_NAME:
        if LAUNCHER_PLACEHOLDER:
            return None
        key = 'launcher'
    elif rel == PACK_NAME:
        key = 'pack'
    elif rel == PROMPT_PACK:
        key = 'prompt'
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
    """Local paths or the developer's name inside the executables: problems, or warnings with --allow-local-paths."""
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
    shipped = {r for r in have if is_library(r)}      # a shipped ELF may need these
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
        if '/' not in rel and rel in FFMPEG_HASHES and FFMPEG_HASHES[rel] is not None and sha != FFMPEG_HASHES[rel]:
            a.problems.append('ffmpeg-pin: %s is not the pinned FFmpeg build (sha256 %s, pinned %s)' % (rel, sha[:12], FFMPEG_HASHES[rel][:12]))
        if rel.lower().endswith(PE_EXT):
            with ref.opener() as f:
                crt_problems(rel, f.read(), a, sha)
        elif PLATFORM == LINUX and head[:4] == ELF_MAGIC:
            with ref.opener() as f:
                elf_problems(rel, f.read(), a, shipped)
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
        if isinstance(src, bytes):                       # generated content (the Linux launcher placeholder)
            with open(dst, 'wb') as f:
                f.write(src)
            origins[rel] = 'generated placeholder'
        else:
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


def is_program(rel):
    """Files that get mode 755 in the Linux tarball: the programs, the launcher and the shared libraries (release root only)."""
    return '/' not in rel and (rel in (EXE_NAME, LAUNCHER_NAME) or rel in SETUP_EXES or is_library(rel))


def write_tar(tpath, folder, root, rels):
    """A deterministic .tar.xz: one top-level folder, sorted file entries only, fixed mtime, owner root, mode 755 or 644."""
    mtime = int(calendar.timegm(ZIP_DATE + (0, 0, 0)))
    with tarfile.open(tpath, 'w:xz', format=tarfile.PAX_FORMAT, preset=6) as t:
        for rel in sorted(rels):
            path = os.path.join(folder, *rel.split('/'))
            ti = tarfile.TarInfo(root + '/' + rel)
            ti.size = os.path.getsize(path)
            ti.mtime = mtime
            ti.mode = 0o755 if is_program(rel) else 0o644
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = 'root'
            ti.pax_headers = {}
            with open(path, 'rb') as f:
                t.addfile(ti, f)


def tar_refs(t, root, problems):
    refs, seen = {}, set()
    for m in t.getmembers():
        name = m.name
        if name in seen:
            problems.append('tar: duplicate entry %s' % name)
        seen.add(name)
        if not m.isfile():
            problems.append('tar: %s is not a regular file (only files are allowed)' % name)
            continue
        if name.startswith('/') or '..' in name.split('/') or not name.startswith(root + '/'):
            problems.append('tar: entry outside the release folder %s' % name)
            continue
        rel = name[len(root) + 1:]
        refs[rel] = FileRef(rel, m.size, lambda m=m: t.extractfile(m))
        if is_program(rel) != bool(m.mode & 0o111):
            problems.append('tar: %s has mode %o (%s)' % (name, m.mode, 'programs and libraries must be executable, the rest not'))
    return refs


def remove_output(p):
    if os.path.isdir(p) and not os.path.islink(p):
        shutil.rmtree(p, ignore_errors=True)
    elif os.path.lexists(p):
        try:
            os.remove(p)
        except OSError:
            pass


def render_table(audit, origins, zpath, zsize, zsha):
    kind = 'tar.xz' if zpath.endswith('.tar.xz') else 'zip'
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
              '%s: %s   %s bytes (%.1f MiB)' % (kind, os.path.basename(zpath), format(zsize, ','), zsize / MIB),
              '%s sha256: %s' % (kind, zsha)]
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

    base = '%s-%s-%s%s' % (RELEASE_PREFIX, o.version, PLATFORM, '' if o.shader_pack else '-noshaders')
    folder = os.path.join(o.out, base)
    tarball = PLATFORM == LINUX
    zpath = folder + ('.tar.xz' if tarball else '.zip')      # the archive (the variable keeps its old name)
    table = folder + '.manifest.txt'
    sums = zpath + '.sha256'          # "<sha256>  <archive name>": what `sha256sum -c` and Get-FileHash comparisons expect
    outputs = [folder, zpath, table, sums]
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

        fa = audit_files(folder_refs(folder), expected, deny, leak=True, strict=not o.allow_local_paths)
        if fa.problems:
            raise Refused(fa.problems, fa.warnings)

        zip_problems = []
        if tarball:
            write_tar(zpath, folder, base, expected)
            with tarfile.open(zpath, 'r:xz') as t:
                za = audit_files(tar_refs(t, base, zip_problems), expected, deny)
        else:
            write_zip(zpath, folder, base, expected)
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
        with open(sums, 'w', encoding='utf-8', newline='\n') as f:
            f.write('%s  %s\n' % (zsha, os.path.basename(zpath)))
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
    say('%s: %s' % ('tar.xz' if tarball else 'zip', zpath))
    say('table: %s' % table)
    say('checksum: %s' % sums)
    return 0


def print_ffmpeg_pins(o):
    """--print-ffmpeg-pins DIR: the SHA-256 of every pinned FFmpeg file of the flavour that is in DIR, as ffmpeg_pins.json spells them."""
    load_ffmpeg(os.path.abspath(o.base), o.platform)
    found = {}
    for name in FFMPEG_FILES:
        path = os.path.join(o.print_ffmpeg_pins, name)
        if not os.path.isfile(path):
            raise UsageError('%s not found in %s' % (name, o.print_ffmpeg_pins))
        found[name] = sha256_file(path)
    say(json.dumps({o.platform: {'files': found}}, indent=2))
    return 0


def main(argv=None, stage_hook=None):
    """Build the release. stage_hook(folder), for tests only, runs after the manifest is written and before the audit."""
    o = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if o.print_ffmpeg_pins:
            return print_ffmpeg_pins(o)
        return build(o, stage_hook)
    except UsageError as e:
        fail('error: %s' % e)
        return 2
    except Refused as r:                       # refused on the inputs, before anything was written
        report_refused(r.problems, r.warnings)
        return 3


if __name__ == '__main__':
    sys.exit(main())
