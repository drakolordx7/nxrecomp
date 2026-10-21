# SPDX-License-Identifier: GPL-3.0-or-later
"""The reference half of the end-to-end oracle: run a static AArch64 test ELF under Unicorn.

    python unicorn_side.py <prog.elf>        runs test_main, prints a JSON summary (also used by run_e2e.py)

Layout (identical on the x64 side, see x64_runner.c):
    image            the ELF's own addresses (linked at 0x7100000000)
    STACK            1 MiB at 0x7108000000, SP starts 0x100 below its top
    TLS / TLSRO      64 KiB each at 0x7109000000 / 0x7109100000, filled with a fixed pattern; TPIDR_EL0 / TPIDRRO_EL0 point
                     at their starts
    SENTINEL         0x7107ff0000: the return address (x30) of test_main; reaching it ends the run
Every register (x0..x30, SP, v0..v31, NZCV) and a SHA-256 of every writable region are reported.
"""
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
from nxrecomp.image import Image          # noqa: E402

STACK, STACK_SIZE = 0x7108000000, 0x100000
TLS, TLSRO, TLS_SIZE = 0x7109000000, 0x7109100000, 0x10000
SENTINEL = 0x7107ff0000
MAX_STEPS = int(os.environ.get('E2E_MAX_STEPS', '60000000'))
M64 = (1 << 64) - 1


def pattern(n, seed):
    out = bytearray()
    s = seed
    while len(out) < n:
        s = (s * 6364136223846793005 + 1442695040888963407) & M64
        out += struct.pack('<Q', s ^ (s >> 29))
    return bytes(out[:n])


def make_uc(model='max'):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
    cpu = {'max': A.UC_CPU_ARM64_MAX, 'a57': A.UC_CPU_ARM64_A57}[model]
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=cpu)
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)         # FP / SIMD access
    return uc, A


def rw_regions(img):
    """[(name, address, size)] of every writable region of a run."""
    out = []
    for i, (off, size, flags) in enumerate(img.segments):
        if flags & 2:
            out.append(('seg%d' % i, img.base + off, size))
    out += [('stack', STACK, STACK_SIZE), ('tls', TLS, TLS_SIZE), ('tlsro', TLSRO, TLS_SIZE)]
    return out


def initial_memory(img):
    """[(address, bytes)] everything that must be mapped and filled before the run."""
    mem = []
    mem.append((img.base, bytes(img.mem)))
    if img.bss[1]:
        mem.append((img.base + img.bss[0], bytes(img.bss[1])))
    mem.append((STACK, pattern(STACK_SIZE, 1) if False else bytes(STACK_SIZE)))
    mem.append((TLS, pattern(TLS_SIZE, 2)))
    mem.append((TLSRO, pattern(TLS_SIZE, 3)))
    return mem


def run(elf_path, model='max', entry_symbol='test_main'):
    img = Image.from_static_elf(elf_path)
    uc, A = make_uc(model)
    page = 0x1000
    end = (img.end + page - 1) // page * page
    uc.mem_map(img.base, end - img.base)
    uc.mem_write(img.base, bytes(img.mem))
    uc.mem_map(STACK, STACK_SIZE)
    uc.mem_map(TLS, TLS_SIZE)
    uc.mem_map(TLSRO, TLS_SIZE)
    uc.mem_write(TLS, pattern(TLS_SIZE, 2))
    uc.mem_write(TLSRO, pattern(TLS_SIZE, 3))
    uc.mem_map(SENTINEL, 0x1000)
    entry = None
    for name, typ, bind, shndx, value, size in img.symbols():
        if name == entry_symbol:
            entry = value
    if entry is None:
        raise SystemExit('no symbol %s' % entry_symbol)
    for i in range(31):
        uc.reg_write(getattr(A, 'UC_ARM64_REG_X%d' % i), 0)
    uc.reg_write(A.UC_ARM64_REG_SP, STACK + STACK_SIZE - 0x100)
    uc.reg_write(A.UC_ARM64_REG_X30, SENTINEL)
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, TLS)
    uc.reg_write(A.UC_ARM64_REG_TPIDRRO_EL0, TLSRO)
    steps = [0]
    uc.emu_start(entry, SENTINEL, count=MAX_STEPS)
    xr = [getattr(A, 'UC_ARM64_REG_X%d' % i) for i in range(31)]
    res = {'x': [uc.reg_read(r) for r in xr], 'sp': uc.reg_read(A.UC_ARM64_REG_SP),
           'v': ['%032x' % uc.reg_read(getattr(A, 'UC_ARM64_REG_V%d' % i)) for i in range(32)],
           'nzcv': (uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28) & 0xf, 'pc': uc.reg_read(A.UC_ARM64_REG_PC)}
    res['finished'] = res['pc'] == SENTINEL
    res['mem'] = {}
    for name, addr, size in rw_regions(img):
        res['mem'][name] = hashlib.sha256(bytes(uc.mem_read(addr, size))).hexdigest()
    # the results[] array, if the program has one, for diagnostics
    for name, typ, bind, shndx, value, size in img.symbols():
        if name == 'results' and size:
            res['results'] = [struct.unpack_from('<Q', bytes(uc.mem_read(value, size)), 8 * i)[0] for i in range(size // 8)]
    return res


if __name__ == '__main__':
    r = run(sys.argv[1])
    if 'results' in r:
        r['results'] = ['%x' % x for x in r['results'][:16]] + ['...']
    print(json.dumps(r, indent=1))
