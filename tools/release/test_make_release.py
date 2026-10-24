"""Unit tests for make_release.py. Every build runs on a fake repository in a temp folder; nothing real is read or written."""
import contextlib
import hashlib
import io
import os
import shutil
import struct
import tempfile
import unittest
import zipfile
from unittest import mock

import make_release as mr

ROOT = 'mpr-test-windows-x64'
MANIFEST = mr.MANIFEST
EXPECTED = sorted([
    'mpr.exe', 'astcdec-avx2-shared.dll', 'astcdec-sse2-shared.dll', 'astcdec-sse4.1-shared.dll',
    'mpr_setup.exe', 'mpr_setup_cli.exe', 'mpr_launcher.exe', 'shader_cache.mprshc', 'README.txt', 'controls.ini',
    'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'licenses/LICENSE.astc-encoder.Apache-2.0', 'licenses/LICENSE.zstd',
    'fonts/DejaVuSans.ttf', 'prompts/prompt_icons.pak', 'prompts/LICENSE-Kenney-InputPrompts.txt', MANIFEST,
])
TINY_RULES = {'exe': (0, None), 'setup': (0, None), 'pack': (0, None), 'launcher': (0, None)}
EXE_REL = 'game-build/build/relpkg/mpr.exe'


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
    w('game-build/build/relpkg/mpr.pdb', b'PDB fake, must never ship')
    w('game-build/build/relpkg/mpr.map', b'MAP fake, must never ship')
    for n in ('astcdec-avx2-shared.dll', 'astcdec-sse2-shared.dll', 'astcdec-sse4.1-shared.dll'):
        w('game-build/build/relpkg/' + n, b'MZ fake dll')
    w('tools/setup/build/release/bin/mpr_setup.exe', b'MZ fake setup')
    w('tools/setup/build/release/bin/mpr_setup_cli.exe', b'MZ fake setup cli')
    w('game-build/build/relpkg/mpr_launcher.exe', b'MZ fake launcher')
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


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mpr-release-test-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo = make_fake_repo(os.path.join(self.tmp, 'repo'))
        self.out = os.path.join(self.tmp, 'out')
        self.prompts = make_fake_prompts(os.path.join(self.tmp, 'prompts'))

    def run_build(self, extra=(), repo=None, out=None, hook=None, real_sizes=False, prompts=None, size_rules=None):
        argv = ['--repo', repo or self.repo, '--out', out or self.out, '--version', 'test',
                '--prompts-dir', prompts or self.prompts] + list(extra)
        buf = io.StringIO()
        if size_rules is None:
            size_rules = {} if real_sizes else TINY_RULES
        rules = mock.patch.dict(mr.SIZE_RULES, size_rules)
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

    def test_local_path_in_exe_is_refused_by_default(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at C:/dev/x' + b'\x00' * 8)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('paths: mpr.exe contains', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_local_path_in_exe_only_warns_with_allow_local_paths(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at C:/dev/x' + b'\x00' * 8)
        code, log = self.run_build(['--allow-local-paths'])
        self.assertEqual(code, 0, log)
        self.assertIn('mpr.exe contains', log)
        self.assertNotIn('paths: ', log)
        self.assertNotIn('REFUSED', log)
        self.assertIn('RELEASE OK', log)

    def test_strict_paths_alias_is_accepted_and_is_the_default(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), b'MZ' + b'build at C:/dev/x' + b'\x00' * 8)
        code, log = self.run_build(['--strict-paths'])
        self.assertEqual(code, 3, log)
        self.assertIn('paths: mpr.exe contains', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_local_path_in_launcher_is_refused_by_default(self):
        write(os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'mpr_launcher.exe'),
              b'MZ' + b'launcher built at C:/dev/x' + b'\x00' * 8)
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('paths: mpr_launcher.exe contains', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_real_size_rules_refuse_small_exe(self):
        code, log = self.run_build(real_sizes=True)
        self.assertEqual(code, 3, log)
        self.assertIn('mpr.exe is', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_dll_count_other_than_three_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'astcdec-sse2-shared.dll'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_launcher_ships_beside_mpr_exe_and_in_the_zip(self):
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        src = os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'mpr_launcher.exe')
        self.assertTrue(os.path.isfile(os.path.join(self.out, ROOT, 'mpr_launcher.exe')))
        with zipfile.ZipFile(os.path.join(self.out, ROOT + '.zip')) as z:
            self.assertIn(ROOT + '/mpr_launcher.exe', z.namelist())
            self.assertEqual(z.read(ROOT + '/mpr_launcher.exe'), read_bytes(src))

    def test_missing_launcher_is_a_usage_error(self):
        os.remove(os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'mpr_launcher.exe'))
        code, log = self.run_build()
        self.assertEqual(code, 2, log)
        self.assertIn('mpr_launcher.exe not found', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_launcher_importing_vcruntime_is_refused(self):
        write(os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'mpr_launcher.exe'),
              make_pe(['kernel32.dll', 'vcruntime140.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: mpr_launcher.exe imports vcruntime140.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_launcher_below_size_minimum_is_refused(self):
        code, log = self.run_build(size_rules=dict(TINY_RULES, launcher=(100 * mr.KIB, 40 * mr.MIB)))
        self.assertEqual(code, 3, log)
        self.assertIn('size: mpr_launcher.exe is', log)
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

    def test_exe_importing_only_kernel32_passes(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['kernel32.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 0, log)
        self.assertNotIn('crt-import', log)

    def test_exe_importing_vcruntime_is_refused(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['kernel32.dll', 'vcruntime140.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: mpr.exe imports vcruntime140.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_exe_importing_ucrt_api_set_is_refused(self):
        write(os.path.join(self.repo, *EXE_REL.split('/')), make_pe(['api-ms-win-crt-runtime-l1-1-0.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: mpr.exe imports api-ms-win-crt-runtime-l1-1-0.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_dll_with_crt_delay_import_is_refused(self):
        dll = os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'astcdec-avx2-shared.dll')
        write(dll, make_pe(['kernel32.dll'], delay=['MSVCP140.dll']))
        code, log = self.run_build()
        self.assertEqual(code, 3, log)
        self.assertIn('crt-import: astcdec-avx2-shared.dll imports msvcp140.dll', log)
        self.assertEqual(self.leftovers(self.out), [])

    def test_unreadable_dll_is_a_warning_only(self):
        dll = os.path.join(self.repo, 'game-build', 'build', 'relpkg', 'astcdec-avx2-shared.dll')
        for label, data in (('not a PE file', b'plain text, not a program'),
                            ('cut short', make_pe(['vcruntime140.dll'])[:300])):
            with self.subTest(label):
                write(dll, data)
                code, log = self.run_build()
                self.assertEqual(code, 0, log)
                self.assertIn('astcdec-avx2-shared.dll is not a readable PE file', log)
                self.assertNotIn('crt-import', log)


class LauncherSizeRuleTests(unittest.TestCase):
    def test_launcher_size_limits_are_the_shipped_rule(self):
        self.assertEqual(mr.SIZE_RULES['launcher'], (100 * mr.KIB, 40 * mr.MIB))
        self.assertIn('outside the allowed', mr.size_problem('mpr_launcher.exe', 100 * mr.KIB - 1))
        self.assertIsNone(mr.size_problem('mpr_launcher.exe', 100 * mr.KIB))
        self.assertIsNone(mr.size_problem('mpr_launcher.exe', 40 * mr.MIB))
        self.assertIn('outside the allowed', mr.size_problem('mpr_launcher.exe', 40 * mr.MIB + 1))


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
        tmp = tempfile.mkdtemp(prefix='mpr-crt-zip-')
        self.addCleanup(shutil.rmtree, tmp, True)
        zpath = os.path.join(tmp, 'release.zip')
        with zipfile.ZipFile(zpath, 'w') as z:
            z.writestr(ROOT + '/mpr.exe', make_pe(['vcruntime140.dll', 'msvcp140.dll'], delay=['vcruntime140.dll']))
        with zipfile.ZipFile(zpath) as z, mock.patch.dict(mr.SIZE_RULES, TINY_RULES):
            zip_problems = []
            a = mr.audit_files(mr.zip_refs(z, ROOT, zip_problems), {'mpr.exe'}, {})
        self.assertEqual(zip_problems, [])
        self.assertEqual(a.problems, ['crt-import: mpr.exe imports vcruntime140.dll',
                                      'crt-import: mpr.exe imports msvcp140.dll'])


if __name__ == '__main__':
    unittest.main()
