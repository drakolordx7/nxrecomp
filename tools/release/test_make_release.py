"""Unit tests for make_release.py. Every build runs on a fake repository in a temp folder; nothing real is read or written."""
import contextlib
import hashlib
import io
import os
import shutil
import tempfile
import unittest
import zipfile
from unittest import mock

import make_release as mr

ROOT = 'mpr-test-windows-x64'
MANIFEST = mr.MANIFEST
EXPECTED = sorted([
    'mpr.exe', 'astcdec-avx2-shared.dll', 'astcdec-sse2-shared.dll', 'astcdec-sse4.1-shared.dll',
    'mpr_setup.exe', 'mpr_setup_cli.exe', 'shader_cache.mprshc', 'README.txt', 'controls.ini',
    'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'licenses/LICENSE.astc-encoder.Apache-2.0', 'licenses/LICENSE.zstd',
    'fonts/DejaVuSans.ttf', 'prompts/prompt_icons.pak', 'prompts/LICENSE-Kenney-InputPrompts.txt', MANIFEST,
])
TINY_RULES = {'exe': (0, None), 'setup': (0, None), 'pack': (0, None)}
EXE_REL = 'game-build/build/release/mpr.exe'


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def make_fake_repo(root):
    """A repository with just the inputs the builder reads, all tiny."""
    def w(rel, data):
        write(os.path.join(root, *rel.split('/')), data)

    w('LICENSE', b'GNU GENERAL PUBLIC LICENSE, fake text for the test\n')
    w(EXE_REL, b'MZ' + b'\x90' * 64 + b'fake mpr')
    w('game-build/build/release/mpr.pdb', b'PDB fake, must never ship')
    w('game-build/build/release/mpr.map', b'MAP fake, must never ship')
    for n in ('astcdec-avx2-shared.dll', 'astcdec-sse2-shared.dll', 'astcdec-sse4.1-shared.dll'):
        w('game-build/build/release/' + n, b'MZ fake dll')
    w('tools/setup/build/release/bin/mpr_setup.exe', b'MZ fake setup')
    w('tools/setup/build/release/bin/mpr_setup_cli.exe', b'MZ fake setup cli')
    w('tools/setup/build/release/bin/astc2bc7.exe', b'MZ tool that must not ship')
    w('work/setup_test/shader_cache.mprshc', mr.PACK_MAGIC + b'\x00' * 1024)
    w('tools/release/templates/README.txt', b'readme\n')
    w('tools/release/templates/THIRD_PARTY_NOTICES.txt', b'notices\n')
    w('tools/release/templates/controls.ini', b'[controls]\n')
    w('tools/release/licenses/LICENSE.astc-encoder.Apache-2.0', b'Apache licence text\n')
    w('tools/release/licenses/LICENSE.zstd', b'zstd licence text\n')
    w('tools/release/fonts/DejaVuSans.ttf', b'\x00\x01\x00\x00' + b'\x00' * 64)
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


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mpr-release-test-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo = make_fake_repo(os.path.join(self.tmp, 'repo'))
        self.out = os.path.join(self.tmp, 'out')
        self.prompts = make_fake_prompts(os.path.join(self.tmp, 'prompts'))

    def run_build(self, extra=(), repo=None, out=None, hook=None, real_sizes=False, prompts=None):
        argv = ['--repo', repo or self.repo, '--out', out or self.out, '--version', 'test',
                '--prompts-dir', prompts or self.prompts] + list(extra)
        buf = io.StringIO()
        rules = mock.patch.dict(mr.SIZE_RULES, {}) if real_sizes else mock.patch.dict(mr.SIZE_RULES, TINY_RULES)
        with rules, contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = mr.main(argv, stage_hook=hook)
        return code, buf.getvalue()

    @staticmethod
    def leftovers(out):
        """Names of release outputs left in out (must be empty after a refusal)."""
        if not os.path.isdir(out):
            return []
        return sorted(n for n in os.listdir(out) if n.startswith('mpr-'))

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
        self.assertEqual(names, sorted(base + '/' + r for r in EXPECTED if r != 'shader_cache.mprshc'))

    CASES = [
        ('key file name', 'tools/release/licenses/prod.keys', b'not really keys', 'prod.keys'),
        ('.nsp extension', 'tools/release/licenses/title.nsp', b'nsp', '.nsp'),
        ('romfs folder', 'tools/release/licenses/romfs/x.bin', b'bin', 'romfs'),
        ('NSO0 magic', 'tools/release/licenses/fake.bin', b'NSO0' + bytes(60), 'NSO0'),
        ('NXB7 magic', 'tools/release/licenses/cache.dat', b'NXB7' + bytes(60), 'NXB7'),
        ('header_key text', 'tools/release/licenses/notes.txt',
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
        write(os.path.join(deny, 'dumped.bin'), read_bytes(os.path.join(self.repo, 'LICENSE')))
        code, log = self.run_build(['--deny-hash-dir', deny])
        self.assertEqual(code, 3, log)
        self.assertIn('same sha256 as', log)
        self.assertIn('dumped.bin', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_deny_dir_is_not_recursive(self):
        deny = os.path.join(self.tmp, 'exefs')
        write(os.path.join(deny, 'sub', 'dumped.bin'), read_bytes(os.path.join(self.repo, 'LICENSE')))
        code, log = self.run_build(['--deny-hash-dir', deny])
        self.assertEqual(code, 0, log)

    def test_pack_with_other_name_is_refused(self):
        other = os.path.join(self.repo, 'work', 'setup_test', 'other.mprshc')
        shutil.copyfile(os.path.join(self.repo, 'work', 'setup_test', 'shader_cache.mprshc'), other)
        code, log = self.run_build(['--shader-pack', other])
        self.assertEqual(code, 3, log)
        self.assertIn('shader_cache.mprshc', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_pack_with_wrong_magic_is_refused(self):
        pack = os.path.join(self.repo, 'work', 'setup_test', 'shader_cache.mprshc')
        write(pack, b'XXXXXXXX' + b'\x00' * 1024)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('MPRSHC01', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_local_path_is_warning_unless_strict(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at C:\\dev\\x' + b'\x00' * 8)
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        self.assertIn('mpr.exe contains', log)
        out2 = os.path.join(self.tmp, 'out-strict')
        code, log = self.run_build(['--strict-paths'], out=out2)
        self.assertEqual(code, 3, log)
        self.assertIn('paths: mpr.exe contains', log)
        self.assertEqual(self.leftovers(out2), [])

    def test_real_size_rules_refuse_small_exe(self):
        code, log = self.run_build(real_sizes=True)
        self.assertEqual(code, 3, log)
        self.assertIn('mpr.exe is', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_dll_count_other_than_three_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, 'game-build', 'build', 'release', 'astcdec-sse2-shared.dll'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_missing_notices_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, 'tools', 'release', 'templates', 'THIRD_PARTY_NOTICES.txt'))
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


if __name__ == '__main__':
    unittest.main()
