"""Unit tests for make_release.py. Every build runs on a fake repository in a temp folder; nothing real is read or written."""
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import struct
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock

import make_release as mr

ROOT = 'demo-test-windows-x64'
MANIFEST = mr.MANIFEST
FFMPEG_DLLS = ('avcodec-62.dll', 'avformat-62.dll', 'avutil-60.dll', 'swresample-6.dll', 'swscale-9.dll')
FFMPEG_SOS = ('libavcodec.so.62', 'libavformat.so.62', 'libavutil.so.60', 'libswresample.so.6', 'libswscale.so.9')
UCRT = 'api-ms-win-crt-runtime-l1-1-0.dll'          # what the mingw-built FFmpeg DLLs import: allowed for the pinned files only
EXPECTED = sorted([
    'demo.exe', 'astcdec-avx2-shared.dll', 'astcdec-sse2-shared.dll', 'astcdec-sse4.1-shared.dll', *FFMPEG_DLLS,
    'demo_setup.exe', 'demo_setup_cli.exe', 'demo_launcher.exe', 'shader_cache.demoshc', 'README.txt', 'HELP.txt', 'controls.ini',
    'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'licenses/LICENSE.astc-encoder.Apache-2.0', 'licenses/LICENSE.zstd',
    'fonts/DejaVuSans.ttf', 'prompts/prompt_icons.pak', 'prompts/LICENSE-Kenney-InputPrompts.txt', MANIFEST,
])
TINY_RULES = {'exe': (0, None), 'setup': (0, None), 'pack': (0, None), 'launcher': (0, None)}
BUILD_DEMO = 'build/' + mr.nxpaths.CHECKOUT_NAME + '/demo'   # the build folder of this checkout: NX/build/<main | worktree name>/<id>
EXE_REL = BUILD_DEMO + '/relpkg/demo.exe'
GAME_JSON = (b'{"id": "demo", "files": {"exe": "demo.exe", "setup": "demo_setup.exe", "setup_cli": "demo_setup_cli.exe", '
             b'"launcher": "demo_launcher.exe", "game_ini": "demo.ini", "shader_pack": "shader_cache.demoshc"}, '
             b'"release": {"prefix": "demo", "setup_glob": "demo_setup*.exe"}}')   # games/demo of the fake NX folder
_GAME_DIR = tempfile.mkdtemp(prefix='demo-release-game-')   # the fixture manifest, for the tests that call the audit directly
with open(os.path.join(_GAME_DIR, 'game.json'), 'wb') as _f:
    _f.write(GAME_JSON)
mr.set_game(_GAME_DIR)
LOCAL_PATH = b'D:' + b'/AI/x'   # a developer-machine path as the audit sees it (spelled in two pieces so the source holds no such path)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def make_fake_repo(root):
    """An NX folder with just the inputs the builder reads, all tiny: games/demo, base, build/<checkout>/demo/relpkg, build/setup-demo/bin."""
    def w(rel, data):
        write(os.path.join(root, *rel.split('/')), data)

    w('base/LICENSE', b'GNU GENERAL PUBLIC LICENSE, fake text for the test\n')
    w('games/demo/game.json', GAME_JSON)
    w(EXE_REL, b'MZ' + b'\x90' * 64 + b'fake demo')
    w(BUILD_DEMO + '/relpkg/demo.pdb', b'PDB fake, must never ship')
    w(BUILD_DEMO + '/relpkg/demo.map', b'MAP fake, must never ship')
    for n in ('astcdec-avx2-shared.dll', 'astcdec-sse2-shared.dll', 'astcdec-sse4.1-shared.dll'):
        w(BUILD_DEMO + '/relpkg/' + n, b'MZ fake dll')
    win_pins = {}
    for n in FFMPEG_DLLS:
        data = make_pe(['kernel32.dll', UCRT]) + n.encode()
        w(BUILD_DEMO + '/relpkg/' + n, data)
        win_pins[n] = hashlib.sha256(data).hexdigest()
    w('base/tools/release/ffmpeg_pins.json', json.dumps({'windows-x64': {'files': win_pins},
                                                          'linux-x86_64': {'files': {n: None for n in FFMPEG_SOS}}}).encode())
    w('build/setup-demo/bin/demo_setup.exe', b'MZ fake setup')
    w('build/setup-demo/bin/demo_setup_cli.exe', b'MZ fake setup cli')
    w(BUILD_DEMO + '/relpkg/demo_launcher.exe', b'MZ fake launcher')
    w('build/setup-demo/bin/astc2bc7.exe', b'MZ tool that must not ship')
    w('games/demo/work/setup_test/shader_cache.demoshc', mr.PACK_MAGIC + b'\x00' * 1024)
    w('games/demo/release/templates/README.txt', b'readme\n')
    w('games/demo/release/templates/HELP.txt', b'help\n')
    w('games/demo/release/templates/THIRD_PARTY_NOTICES.txt', b'notices\n')
    w('games/demo/release/templates/controls.ini', b'[controls]\n')
    w('base/tools/release/licenses/LICENSE.astc-encoder.Apache-2.0', b'Apache licence text\n')
    w('base/tools/release/licenses/LICENSE.zstd', b'zstd licence text\n')
    w('base/tools/release/fonts/DejaVuSans.ttf', b'\x00\x01\x00\x00' + b'\x00' * 64)
    return root


def make_fake_prompts(folder):
    """The two prompt files the release needs: a tiny pack with the real MPIP magic, and a licence text."""
    write(os.path.join(folder, 'prompt_icons.pak'), mr.PROMPT_MAGIC + b'\x00' * 1024)
    write(os.path.join(folder, 'LICENSE-Kenney-InputPrompts.txt'), b'CC0 1.0 fake text for the test\n')
    return folder


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def sha(path):
    return hashlib.sha256(read_bytes(path)).hexdigest()


def make_pe(imports=(), delay=()):
    """A minimal PE32+ file with one section; its import and delay-import tables name the given DLLs."""
    imp_rva = 0x1000
    dly_rva = imp_rva + 20 * (len(imports) + 1)
    str_rva = dly_rva + 32 * (len(delay) + 1)
    names = list(imports) + list(delay)
    strings, rvas = b'', []
    for n in names:
        rvas.append(str_rva + len(strings))
        strings += n.encode() + b'\0'
    sec = b''
    for i in range(len(imports)):
        sec += struct.pack('<IIIII', 0, 0, 0, rvas[i], 0)          # OriginalFirstThunk, Time, Forwarder, Name, FirstThunk
    sec += bytes(20)
    for j in range(len(delay)):
        sec += struct.pack('<IIIIIIII', 1, rvas[len(imports) + j], 0, 0, 0, 0, 0, 0)   # Attributes=1: RVA form
    sec += bytes(32) + strings
    assert len(sec) <= 0x200
    dos = bytearray(0x40)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x40)
    coff = struct.pack('<HHIIIHH', 0x8664, 1, 0, 0, 0, 240, 0x0022)
    opt = bytearray(240)
    struct.pack_into('<H', opt, 0, 0x20B)
    struct.pack_into('<Q', opt, 24, 0x140000000)
    struct.pack_into('<II', opt, 32, 0x1000, 0x200)
    struct.pack_into('<I', opt, 56, 0x2000)
    struct.pack_into('<I', opt, 60, 0x200)
    struct.pack_into('<I', opt, 108, 16)
    struct.pack_into('<II', opt, 112 + 8, imp_rva, 20 * (len(imports) + 1))          # directory 1: imports
    if delay:
        struct.pack_into('<II', opt, 112 + 8 * 13, dly_rva, 32 * (len(delay) + 1))   # directory 13: delay imports
    sect = bytearray(40)
    sect[:8] = b'.idata\0\0'
    struct.pack_into('<IIIIIIHHI', sect, 8, len(sec), 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0x40000040)
    head = bytes(dos) + b'PE\0\0' + bytes(coff) + bytes(opt) + bytes(sect)
    return head + bytes(0x200 - len(head)) + sec


def reset_state():
    """make_release keeps the flavour in module globals: back to the Windows demo game, no FFmpeg pins, for the tests that call the audit directly."""
    mr.set_game(_GAME_DIR)
    mr.FFMPEG_FILES, mr.FFMPEG_HASHES, mr.LAUNCHER_PLACEHOLDER = (), {}, False


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='demo-release-test-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.addCleanup(reset_state)
        self.repo = make_fake_repo(os.path.join(self.tmp, 'repo'))
        self.out = os.path.join(self.tmp, 'out')
        self.prompts = make_fake_prompts(os.path.join(self.tmp, 'prompts'))

    def run_build(self, extra=(), repo=None, out=None, hook=None, real_sizes=False, prompts=None, size_rules=None):
        nx = repo or self.repo
        argv = ['--game', os.path.join(nx, 'games', 'demo'), '--base', os.path.join(nx, 'base'), '--out', out or self.out,
                '--version', 'test', '--prompts-dir', prompts or self.prompts] + list(extra)
        buf = io.StringIO()
        if size_rules is None:
            size_rules = {} if real_sizes else TINY_RULES
        rules = mock.patch.dict(mr.SIZE_RULES, size_rules)
        with rules, mock.patch.dict(os.environ, {'NX_ROOT': nx}), contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = mr.main(argv, stage_hook=hook)
        return code, buf.getvalue()

    @staticmethod
    def leftovers(out):
        """Names of release outputs left in out (must be empty after a refusal)."""
        if not os.path.isdir(out):
            return []
        return sorted(n for n in os.listdir(out) if n.startswith('demo-'))

    def test_clean_build_passes_and_zip_holds_exactly_the_manifest_files(self):
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        zpath = os.path.join(self.out, ROOT + '.zip')
        self.assertTrue(os.path.isfile(zpath))
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
            manifest = z.read(ROOT + '/' + MANIFEST).decode('utf-8')
            for info in z.infolist():
                self.assertEqual(info.date_time, mr.ZIP_DATE)
        self.assertEqual(names, sorted(ROOT + '/' + r for r in EXPECTED))
        listed = {line.split('  ', 1)[1] for line in manifest.splitlines()}
        self.assertEqual(listed, set(EXPECTED) - {MANIFEST})
        self.assertFalse(any(n.endswith(('.pdb', '.map')) for n in names))
        self.assertFalse(any('astc2bc7' in n for n in names))
        self.assertTrue(os.path.isfile(os.path.join(self.out, ROOT + '.manifest.txt')))
        with open(os.path.join(self.out, ROOT + '.zip.sha256'), encoding='utf-8') as f:
            self.assertEqual(f.read(), '%s  %s\n' % (sha(os.path.join(self.out, ROOT + '.zip')), ROOT + '.zip'))

    def test_zip_is_deterministic(self):
        out2 = os.path.join(self.tmp, 'out2')
        self.assertEqual(self.run_build()[0], 0)
        self.assertEqual(self.run_build(out=out2)[0], 0)
        self.assertEqual(sha(os.path.join(self.out, ROOT + '.zip')), sha(os.path.join(out2, ROOT + '.zip')))

    def test_no_shader_pack_variant_gets_suffix_and_no_pack(self):
        code, log = self.run_build(['--no-shader-pack'])
        self.assertEqual(code, 0, log)
        base = ROOT + '-noshaders'
        with zipfile.ZipFile(os.path.join(self.out, base + '.zip')) as z:
            names = z.namelist()
        self.assertEqual(names, sorted(base + '/' + r for r in EXPECTED if r != 'shader_cache.demoshc'))

    CASES = [
        ('key file name', 'base/tools/release/licenses/prod.keys', b'not really keys', 'prod.keys'),
        ('.nsp extension', 'base/tools/release/licenses/title.nsp', b'nsp', '.nsp'),
        ('romfs folder', 'base/tools/release/licenses/romfs/x.bin', b'bin', 'romfs'),
        ('NSO0 magic', 'base/tools/release/licenses/fake.bin', b'NSO0' + bytes(60), 'NSO0'),
        ('NXB7 magic', 'base/tools/release/licenses/cache.dat', b'NXB7' + bytes(60), 'NXB7'),
        ('header_key text', 'base/tools/release/licenses/notes.txt',
         b'header_key = 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n', 'header_key'),
    ]

    def test_planted_files_are_refused_with_no_output(self):
        for label, rel, data, needle in self.CASES:
            with self.subTest(label):
                repo = make_fake_repo(os.path.join(self.tmp, 'case-' + str(abs(hash(label)))))
                write(os.path.join(repo, *rel.split('/')), data)
                out = os.path.join(self.tmp, 'out-' + str(abs(hash(label))))
                code, log = self.run_build(repo=repo, out=out)
                self.assertEqual(code, 3, log)
                self.assertIn(needle, log)
                self.assertEqual(self.leftovers(out), [])

    def test_unexpected_extra_file_is_refused_with_no_output(self):
        def hook(folder):
            write(os.path.join(folder, 'extra.bin'), b'not in the allowlist')
        code, log = self.run_build(hook=hook)
        self.assertEqual(code, 3, log)
        self.assertIn('unexpected file extra.bin', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_file_with_hash_of_deny_dir_file_is_refused(self):
        deny = os.path.join(self.tmp, 'exefs')
        write(os.path.join(deny, 'dumped.bin'), read_bytes(os.path.join(self.repo, 'base', 'LICENSE')))
        code, log = self.run_build(['--deny-hash-dir', deny])
        self.assertEqual(code, 3, log)
        self.assertIn('same sha256 as', log)
        self.assertIn('dumped.bin', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_deny_dir_is_not_recursive(self):
        deny = os.path.join(self.tmp, 'exefs')
        write(os.path.join(deny, 'sub', 'dumped.bin'), read_bytes(os.path.join(self.repo, 'base', 'LICENSE')))
        code, log = self.run_build(['--deny-hash-dir', deny])
        self.assertEqual(code, 0, log)

    def test_pack_with_other_name_is_refused(self):
        other = os.path.join(self.repo, 'games', 'demo', 'work', 'setup_test', 'other.demoshc')
        shutil.copyfile(os.path.join(self.repo, 'games', 'demo', 'work', 'setup_test', 'shader_cache.demoshc'), other)
        code, log = self.run_build(['--shader-pack', other])
        self.assertEqual(code, 3, log)
        self.assertIn('shader_cache.demoshc', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_pack_with_wrong_magic_is_refused(self):
        pack = os.path.join(self.repo, 'games', 'demo', 'work', 'setup_test', 'shader_cache.demoshc')
        write(pack, b'XXXXXXXX' + b'\x00' * 1024)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('NXSHC002', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_local_path_in_exe_is_refused_by_default(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at ' + LOCAL_PATH + b'\x00' * 8)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('paths: demo.exe contains', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_local_path_in_exe_only_warns_with_allow_local_paths(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at ' + LOCAL_PATH + b'\x00' * 8)
        code, log = self.run_build(['--allow-local-paths'])
        self.assertEqual(code, 0, log)
        self.assertIn('demo.exe contains', log)
        self.assertNotIn('paths: ', log)
        self.assertNotIn('REFUSED', log)
        self.assertIn('RELEASE OK', log)

    def test_strict_paths_alias_is_accepted_and_is_the_default(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at ' + LOCAL_PATH + b'\x00' * 8)
        code, log = self.run_build(['--strict-paths'])
        self.assertEqual(code, 3, log)
        self.assertIn('paths: demo.exe contains', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_local_path_in_launcher_is_refused_by_default(self):
        write(os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'demo_launcher.exe'),
              b'MZ' + b'launcher built at ' + LOCAL_PATH + b'\x00' * 8)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('paths: demo_launcher.exe contains', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_real_size_rules_refuse_small_exe(self):
        code, log = self.run_build(real_sizes=True)
        self.assertEqual(code, 3, log)
        self.assertIn('demo.exe is', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_dll_count_other_than_three_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'astcdec-sse2-shared.dll'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_launcher_ships_beside_demo_exe_and_in_the_zip(self):
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        src = os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'demo_launcher.exe')
        self.assertTrue(os.path.isfile(os.path.join(self.out, ROOT, 'demo_launcher.exe')))
        with zipfile.ZipFile(os.path.join(self.out, ROOT + '.zip')) as z:
            self.assertIn(ROOT + '/demo_launcher.exe', z.namelist())
            self.assertEqual(z.read(ROOT + '/demo_launcher.exe'), read_bytes(src))

    def test_missing_launcher_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'demo_launcher.exe'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertIn('demo_launcher.exe not found', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_launcher_importing_vcruntime_is_refused(self):
        write(os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'demo_launcher.exe'),
              make_pe(['kernel32.dll', 'vcruntime140.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: demo_launcher.exe imports vcruntime140.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_launcher_below_size_minimum_is_refused(self):
        code, log = self.run_build(size_rules=dict(TINY_RULES, launcher=(100 * mr.KIB, 40 * mr.MIB)))
        self.assertEqual(code, 3, log)
        self.assertIn('size: demo_launcher.exe is', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_missing_notices_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, 'games', 'demo', 'release', 'templates', 'THIRD_PARTY_NOTICES.txt'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_release_carries_prompt_pack_and_licence(self):
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        with zipfile.ZipFile(os.path.join(self.out, ROOT + '.zip')) as z:
            names = z.namelist()
            pak = z.read(ROOT + '/prompts/prompt_icons.pak')
            lic = z.read(ROOT + '/prompts/LICENSE-Kenney-InputPrompts.txt')
        self.assertIn(ROOT + '/prompts/prompt_icons.pak', names)
        self.assertIn(ROOT + '/prompts/LICENSE-Kenney-InputPrompts.txt', names)
        self.assertEqual(pak, read_bytes(os.path.join(self.prompts, 'prompt_icons.pak')))
        self.assertEqual(lic, read_bytes(os.path.join(self.prompts, 'LICENSE-Kenney-InputPrompts.txt')))

    def test_missing_prompt_files_are_usage_errors(self):
        code, log = self.run_build(prompts=os.path.join(self.tmp, 'no-such-prompts'))
        self.assertEqual(code, 2, log)
        self.assertIn('prompt icon', log)
        self.assertEqual(self.leftovers(self.out), [])
        os.remove(os.path.join(self.prompts, 'LICENSE-Kenney-InputPrompts.txt'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertIn('LICENSE-Kenney-InputPrompts.txt', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_pak_under_any_other_path_is_refused(self):
        def hook(folder):
            write(os.path.join(folder, 'prompts', 'other.pak'), mr.PROMPT_MAGIC + b'\x00' * 64)
            write(os.path.join(folder, 'prompts', 'sub', 'prompt_icons.pak'), mr.PROMPT_MAGIC + b'\x00' * 64)
            write(os.path.join(folder, 'licenses', 'game.pak'), b'\x00' * 64)
        code, log = self.run_build(hook=hook)
        self.assertEqual(code, 3, log)
        for rel in ('prompts/other.pak', 'prompts/sub/prompt_icons.pak', 'licenses/game.pak'):
            self.assertIn('denylist: %s: file type .pak' % rel, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_prompt_pack_too_large_or_wrong_magic_is_refused(self):
        pak = os.path.join(self.prompts, 'prompt_icons.pak')
        write(pak, mr.PROMPT_MAGIC + b'\x00' * mr.MIB * 4)           # 4 MiB + 4 bytes: over the limit
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('prompts/prompt_icons.pak is', log)
        self.assertEqual(self.leftovers(self.out), [])
        for head, label in ((b'XXXX', None), (mr.PACK_MAGIC, None), (b'NSO0', 'NSO0 (game executable)')):
            with self.subTest(head=head):
                write(pak, head + b'\x00' * 1024)
                code, log = self.run_build()
                self.assertEqual(code, 3, log)
                self.assertIn('prompts/prompt_icons.pak does not start with MPIP', log)
                if label:
                    self.assertIn(label, log)
                self.assertEqual(self.leftovers(self.out), [])

    def test_exe_importing_only_kernel32_passes(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['kernel32.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        self.assertNotIn('crt-import', log)

    def test_exe_importing_vcruntime_is_refused(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['kernel32.dll', 'vcruntime140.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: demo.exe imports vcruntime140.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_exe_importing_ucrt_api_set_is_refused(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['api-ms-win-crt-runtime-l1-1-0.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: demo.exe imports api-ms-win-crt-runtime-l1-1-0.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_dll_with_crt_delay_import_is_refused(self):
        dll = os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'astcdec-avx2-shared.dll')
        write(dll, make_pe(['kernel32.dll'], delay=['MSVCP140.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: astcdec-avx2-shared.dll imports msvcp140.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_unreadable_dll_is_a_warning_only(self):
        dll = os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'astcdec-avx2-shared.dll')
        for label, data in (('not a PE file', b'plain text, not a program'),
                            ('cut short', make_pe(['vcruntime140.dll'])[:300])):
            with self.subTest(label):
                write(dll, data)
                code, log = self.run_build()
                self.assertEqual(code, 0, log)
                self.assertIn('astcdec-avx2-shared.dll is not a readable PE file', log)
                self.assertNotIn('crt-import', log)


    def test_pinned_ffmpeg_dll_may_import_the_ucrt_but_our_binaries_may_not(self):
        code, log = self.run_build()
        self.assertEqual(code, 0, log)                       # the fixture FFmpeg DLLs import the UCRT and are pinned
        self.assertNotIn('crt-import', log)
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['kernel32.dll', UCRT]))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: demo.exe imports ' + UCRT, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_ffmpeg_dll_that_is_not_the_pinned_build_is_refused(self):
        dll = os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'avcodec-62.dll')
        write(dll, make_pe(['kernel32.dll', UCRT]) + b'a different build')
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('ffmpeg-pin: avcodec-62.dll is not the pinned FFmpeg build', log)
        self.assertIn('crt-import: avcodec-62.dll imports ' + UCRT, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_the_pin_does_not_cover_another_file_name(self):
        dll = os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'astcdec-avx2-shared.dll')
        write(dll, read_bytes(os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'avcodec-62.dll')))   # same bytes, not a pinned name
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: astcdec-avx2-shared.dll imports ' + UCRT, log)

    def test_missing_ffmpeg_dll_or_pins_are_usage_errors(self):
        os.remove(os.path.join(self.repo, *BUILD_DEMO.split('/'), 'relpkg', 'swscale-9.dll'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertIn('swscale-9.dll', log)
        os.remove(os.path.join(self.repo, 'base', 'tools', 'release', 'ffmpeg_pins.json'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertIn('FFmpeg pins not found', log)
        self.assertEqual(self.leftovers(self.out), [])


def real_pins():
    with open(os.path.join(mr.BASE, 'tools', 'release', 'ffmpeg_pins.json'), encoding='utf-8') as f:
        return json.load(f)


class FfmpegPinTests(unittest.TestCase):
    def test_pins_file_names_the_same_archives_as_the_cmake_that_fetches_them(self):
        pins = real_pins()
        with open(os.path.join(mr.BASE, 'runtime', 'cmake', 'ffmpeg.cmake'), encoding='utf-8') as f:
            cmake = f.read()
        for plat in (mr.WINDOWS, mr.LINUX):
            self.assertIn(pins[plat]['archive_sha256'], cmake, plat)
        self.assertIn(pins['version'], cmake)
        self.assertIn(pins['tag'], cmake)

    def test_every_windows_file_is_pinned_by_hash(self):
        pins = real_pins()
        files = pins[mr.WINDOWS]['files']
        self.assertEqual(sorted(files), sorted(FFMPEG_DLLS))
        for name, h in files.items():
            self.assertRegex(h, r'^[0-9a-f]{64}$', name)


class LauncherSizeRuleTests(unittest.TestCase):
    def test_launcher_size_limits_are_the_shipped_rule(self):
        self.assertIn('outside the allowed', mr.size_problem('demo_launcher.exe', 100 * mr.KIB - 1))
        self.assertIsNone(mr.size_problem('demo_launcher.exe', 100 * mr.KIB))
        self.assertIsNone(mr.size_problem('demo_launcher.exe', 40 * mr.MIB))
        self.assertIn('outside the allowed', mr.size_problem('demo_launcher.exe', 40 * mr.MIB + 1))


class PeImportTests(unittest.TestCase):
    def test_reader_returns_normal_and_delay_imports_lower_cased_in_order(self):
        data = make_pe(['KERNEL32.dll', 'vcruntime140.dll', 'kernel32.DLL'],
                       delay=['API-MS-WIN-CRT-runtime-l1-1-0.dll', 'Vulkan-1.dll'])
        self.assertEqual(mr.pe_imports(data), ['kernel32.dll', 'vcruntime140.dll',
                                               'api-ms-win-crt-runtime-l1-1-0.dll', 'vulkan-1.dll'])

    def test_reader_gives_none_for_data_that_is_not_a_readable_pe(self):
        self.assertIsNone(mr.pe_imports(b''))
        self.assertIsNone(mr.pe_imports(b'plain text, not a program'))
        self.assertIsNone(mr.pe_imports(make_pe(['kernel32.dll'])[:300]))
        self.assertIsNone(mr.pe_imports(make_pe(['kernel32.dll'])[:0x50]))

    def test_reader_handles_a_pe_with_no_imports(self):
        self.assertEqual(mr.pe_imports(make_pe([])), [])

    def test_zip_side_audit_refuses_crt_import_once_per_dll(self):
        tmp = tempfile.mkdtemp(prefix='demo-crt-zip-')
        self.addCleanup(shutil.rmtree, tmp, True)
        zpath = os.path.join(tmp, 'release.zip')
        with zipfile.ZipFile(zpath, 'w') as z:
            z.writestr(ROOT + '/demo.exe', make_pe(['vcruntime140.dll', 'msvcp140.dll'], delay=['vcruntime140.dll']))
        with zipfile.ZipFile(zpath) as z, mock.patch.dict(mr.SIZE_RULES, TINY_RULES):
            zip_problems = []
            a = mr.audit_files(mr.zip_refs(z, ROOT, zip_problems), {'demo.exe'}, {})
        self.assertEqual(zip_problems, [])
        self.assertEqual(a.problems, ['crt-import: demo.exe imports vcruntime140.dll',
                                      'crt-import: demo.exe imports msvcp140.dll'])


def make_elf(needed=(), rpath=(), runpath=(), glibc=(), cxx=(), machine=62, soname=None):
    """A minimal ELF64 x86-64 shared object: one PT_LOAD over the whole file, a PT_DYNAMIC with DT_NEEDED / RPATH / RUNPATH / SONAME / STRTAB and a
    version-needs table naming libc.so.6 with the GLIBC_x.y versions (and GLIBCXX_ / CXXABI_ names for `cxx`)."""
    strs, offs = bytearray(b'\0'), {}

    def add(text):
        if text not in offs:
            offs[text] = len(strs)
            strs.extend(text.encode() + b'\0')
        return offs[text]

    needed_o = [add(n) for n in needed]
    rpath_o, runpath_o = [add(':'.join(rpath))] if rpath else [], [add(':'.join(runpath))] if runpath else []
    soname_o = add(soname) if soname else None
    versions = ['GLIBC_%d.%d' % v for v in glibc] + list(cxx)
    file_o = add('libc.so.6')
    ver_o = [add(v) for v in versions]
    base = 0x100
    dynstr_at = base
    verneed_at = (dynstr_at + len(strs) + 7) & ~7
    verneed = b''
    if versions:
        verneed = struct.pack('<HHIII', 1, len(versions), file_o, 16, 0)
        for i, vo in enumerate(ver_o):
            verneed += struct.pack('<IHHII', 0, 0, 0, vo, 16 if i + 1 < len(ver_o) else 0)
    dynamic_at = (verneed_at + len(verneed) + 15) & ~15
    dyn = [(1, o) for o in needed_o] + [(15, o) for o in rpath_o] + [(29, o) for o in runpath_o] + [(5, dynstr_at)]
    if soname_o is not None:
        dyn.append((14, soname_o))
    if versions:
        dyn += [(0x6FFFFFFE, verneed_at), (0x6FFFFFFF, 1)]
    dyn.append((0, 0))
    dynamic = b''.join(struct.pack('<qQ', t, v) for t, v in dyn)
    total = dynamic_at + len(dynamic)
    ehdr = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8) + struct.pack('<HHIQQQIHHHHHH', 3, machine, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    ph = struct.pack('<IIQQQQQQ', 1, 5, 0, 0, 0, total, total, 0x1000)
    ph += struct.pack('<IIQQQQQQ', 2, 6, dynamic_at, dynamic_at, dynamic_at, len(dynamic), len(dynamic), 8)
    out = bytearray(total)
    out[:len(ehdr)] = ehdr
    out[64:64 + len(ph)] = ph
    out[dynstr_at:dynstr_at + len(strs)] = strs
    out[verneed_at:verneed_at + len(verneed)] = verneed
    out[dynamic_at:dynamic_at + len(dynamic)] = dynamic
    return bytes(out)


LROOT = 'demo-test-linux-x86_64'
LBUILD = 'build/' + mr.nxpaths.CHECKOUT_NAME + '/linux/demo_relpkg'
LINUX_ASTC = ('libastcdec-avx2-shared.so', 'libastcdec-sse2-shared.so', 'libastcdec-sse4.1-shared.so')
LINUX_EXPECTED = sorted([
    'demo', *LINUX_ASTC, *FFMPEG_SOS, 'demo_setup_cli', 'demo_launcher', 'shader_cache.demoshc', 'README.txt', 'HELP.txt', 'controls.ini',
    'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'licenses/LICENSE.astc-encoder.Apache-2.0', 'licenses/LICENSE.zstd',
    'fonts/DejaVuSans.ttf', 'prompts/prompt_icons.pak', 'prompts/LICENSE-Kenney-InputPrompts.txt', MANIFEST,
])
GOOD_EXE = dict(needed=['libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'libvulkan.so.1', 'libavcodec.so.62'], runpath=['$ORIGIN'],
                glibc=[(2, 2), (2, 17), (2, 34)])


class LinuxBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='demo-release-linux-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.addCleanup(reset_state)
        self.repo = make_fake_repo(os.path.join(self.tmp, 'repo'))
        self.out = os.path.join(self.tmp, 'out')
        self.prompts = make_fake_prompts(os.path.join(self.tmp, 'prompts'))
        self.put(LBUILD + '/demo', make_elf(**GOOD_EXE))
        for n in LINUX_ASTC + FFMPEG_SOS:
            self.put(LBUILD + '/' + n, make_elf(needed=['libc.so.6', 'libm.so.6'], runpath=['$ORIGIN'], glibc=[(2, 17)], soname=n))
        self.put('build/setup-demo-linux/bin/demo_setup_cli', make_elf(needed=['libc.so.6'], glibc=[(2, 34)]))

    def put(self, rel, data):
        write(os.path.join(self.repo, *rel.split('/')), data)

    def run_build(self, extra=(), hook=None):
        argv = ['--game', os.path.join(self.repo, 'games', 'demo'), '--base', os.path.join(self.repo, 'base'), '--out', self.out,
                '--version', 'test', '--prompts-dir', self.prompts, '--platform', 'linux-x86_64'] + list(extra)
        buf = io.StringIO()
        with mock.patch.dict(mr.SIZE_RULES, TINY_RULES), mock.patch.dict(os.environ, {'NX_ROOT': self.repo}), \
                contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = mr.main(argv, stage_hook=hook)
        return code, buf.getvalue()

    def leftovers(self):
        return sorted(n for n in os.listdir(self.out) if n.startswith('demo-')) if os.path.isdir(self.out) else []

    def test_tarball_holds_the_linux_files_with_programs_executable(self):
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        tpath = os.path.join(self.out, LROOT + '.tar.xz')
        with tarfile.open(tpath) as t:
            members = t.getmembers()
        self.assertEqual([m.name for m in members], sorted(LROOT + '/' + r for r in LINUX_EXPECTED))
        for m in members:
            rel = m.name[len(LROOT) + 1:]
            self.assertEqual((m.uid, m.gid, m.mtime), (0, 0, 1767225600), m.name)
            programs = rel in ('demo', 'demo_setup_cli', 'demo_launcher') or rel.startswith('lib')
            self.assertEqual(m.mode, 0o755 if programs else 0o644, m.name)
        self.assertFalse(any(m.name.endswith(('.exe', '.dll')) for m in members))
        with open(tpath + '.sha256', encoding='utf-8') as f:
            self.assertEqual(f.read(), '%s  %s\n' % (sha(tpath), LROOT + '.tar.xz'))
        self.assertFalse(os.path.exists(os.path.join(self.out, LROOT + '.zip')))

    def test_tarball_is_deterministic(self):
        self.assertEqual(self.run_build()[0], 0)
        first = sha(os.path.join(self.out, LROOT + '.tar.xz'))
        shutil.rmtree(self.out)
        self.assertEqual(self.run_build()[0], 0)
        self.assertEqual(first, sha(os.path.join(self.out, LROOT + '.tar.xz')))

    def test_missing_launcher_ships_a_placeholder_script_and_a_built_one_ships_as_is(self):
        self.assertEqual(self.run_build()[0], 0)
        with tarfile.open(os.path.join(self.out, LROOT + '.tar.xz')) as t:
            text = t.extractfile(LROOT + '/demo_launcher').read().decode()
        self.assertTrue(text.startswith('#!/bin/sh'))
        self.assertIn('./demo_setup_cli', text)
        shutil.rmtree(self.out)
        real = make_elf(needed=['libc.so.6'])
        self.put(LBUILD + '/demo_launcher', real)
        self.assertEqual(self.run_build()[0], 0)
        with tarfile.open(os.path.join(self.out, LROOT + '.tar.xz')) as t:
            self.assertEqual(t.extractfile(LROOT + '/demo_launcher').read(), real)

    def test_the_setup_gui_and_windows_files_are_not_shipped(self):
        self.put('build/setup-demo-linux/bin/demo_setup', make_elf(needed=['libc.so.6']))
        self.assertEqual(self.run_build()[0], 0)
        with tarfile.open(os.path.join(self.out, LROOT + '.tar.xz')) as t:
            names = t.getnames()
        self.assertNotIn(LROOT + '/demo_setup', names)

    def refused(self, rel, data, needle):
        self.put(rel, data)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn(needle, log)
        self.assertEqual(self.leftovers(), [])

    def test_elf_audit_refuses_shared_cxx_runtime_and_unlisted_libraries(self):
        self.refused(LBUILD + '/demo', make_elf(**dict(GOOD_EXE, needed=GOOD_EXE['needed'] + ['libstdc++.so.6'])),
                     'elf-needed: demo needs libstdc++.so.6 (the C++ runtime must be linked statically)')
        self.refused(LBUILD + '/demo', make_elf(**dict(GOOD_EXE, needed=GOOD_EXE['needed'] + ['libSDL3.so.0'])), 'elf-needed: demo needs libSDL3.so.0')
        self.refused(LBUILD + '/demo', make_elf(**dict(GOOD_EXE, cxx=['GLIBCXX_3.4.29'])), 'needs the shared C++ runtime (GLIBCXX_3.4.29)')

    def test_elf_audit_refuses_absolute_rpath_new_glibc_and_wrong_arch(self):
        self.refused(LBUILD + '/demo', make_elf(**dict(GOOD_EXE, runpath=['/mnt/d/AI/build/lib'])), 'elf-rpath: demo has the search path /mnt/d/AI/build/lib')
        self.refused(LBUILD + '/demo', make_elf(**dict(GOOD_EXE, glibc=[(2, 17), (2, 36)])), 'elf-glibc: demo needs GLIBC_2.36')
        self.refused(LBUILD + '/demo', make_elf(**dict(GOOD_EXE, machine=183)), 'elf-arch: demo is not an x86-64 file')

    def test_elf_audit_lets_a_file_need_the_libraries_shipped_beside_it_only(self):
        self.refused(LBUILD + '/libavcodec.so.62', make_elf(needed=['libc.so.6', 'libavutil.so.99'], runpath=['$ORIGIN']),
                     'elf-needed: libavcodec.so.62 needs libavutil.so.99')

    def test_unreadable_elf_is_a_warning_only(self):
        self.put(LBUILD + '/libswscale.so.9', b'\x7fELF but cut short')
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        self.assertIn('libswscale.so.9 is not a readable ELF64 file', log)

    def test_wsl_spelling_of_the_checkout_path_in_the_exe_is_a_leak(self):
        root = mr._leak_patterns()
        self.assertTrue(any(p.startswith(b'/mnt/') for p in root) or mr.BASE[1:2] != ':')
        data = make_elf(**GOOD_EXE) + b'built in /mnt/' + mr.BASE[0].lower().encode() + mr.BASE[2:].replace(os.sep, '/').encode() + b'\0'
        if mr.BASE[1:2] == ':':
            self.refused(LBUILD + '/demo', data, 'paths: demo contains')

    def test_print_ffmpeg_pins_lists_the_hashes_of_the_files_in_a_folder(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = mr.main(['--print-ffmpeg-pins', os.path.join(self.repo, *LBUILD.split('/')), '--platform', 'linux-x86_64',
                            '--base', os.path.join(self.repo, 'base')])
        self.assertEqual(code, 0)
        files = json.loads(buf.getvalue())['linux-x86_64']['files']
        self.assertEqual(sorted(files), sorted(FFMPEG_SOS))
        self.assertEqual(files['libavcodec.so.62'], sha(os.path.join(self.repo, *LBUILD.split('/'), 'libavcodec.so.62')))


class ElfReaderTests(unittest.TestCase):
    def test_reader_returns_needed_rpath_soname_and_the_highest_glibc(self):
        info = mr.elf_info(make_elf(needed=['libc.so.6', 'libm.so.6'], rpath=['$ORIGIN', '/x'], runpath=['$ORIGIN/lib'], glibc=[(2, 2), (2, 34), (2, 17)],
                                    cxx=['CXXABI_1.3'], soname='libfoo.so.1'))
        self.assertEqual(info['needed'], ['libc.so.6', 'libm.so.6'])
        self.assertEqual(info['rpath'], ['$ORIGIN', '/x', '$ORIGIN/lib'])
        self.assertEqual(info['soname'], 'libfoo.so.1')
        self.assertEqual(info['glibc'], (2, 34))
        self.assertEqual(info['cxx'], ['CXXABI_1.3'])
        self.assertEqual(info['machine'], 62)

    def test_reader_gives_none_for_data_that_is_not_a_readable_elf(self):
        self.assertIsNone(mr.elf_info(b''))
        self.assertIsNone(mr.elf_info(b'MZ plain text'))
        self.assertIsNone(mr.elf_info(make_elf(needed=['libc.so.6'])[:100]))

    def test_reader_handles_a_file_with_no_dynamic_section_data(self):
        info = mr.elf_info(make_elf())
        self.assertEqual((info['needed'], info['rpath'], info['glibc']), ([], [], None))


if __name__ == '__main__':
    unittest.main()
