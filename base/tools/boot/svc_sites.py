r"""List the SVC numbers used by each module of a game (svc #n sites) and which of them the runtime has no handler for.
  base\tools\.venv\Scripts\python base\tools\boot\svc_sites.py --game <games\<id>>
Reads the module files from the game's game.json (<game>\game\exefs\<file>) and the registered numbers from runtime\src."""
import argparse, collections, glob, json, os, re, struct, sys

BASE = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(BASE, 'scripts'))
sys.path.insert(0, os.path.join(BASE, 'recomp'))
import nxpaths
from nxrecomp import nso

ap = argparse.ArgumentParser()
ap.add_argument('--game', required=True, help='a game folder (with game.json) or a game id')
game = nxpaths.game_dir(ap.parse_args().game)
man = json.load(open(os.path.join(game, 'game.json'), encoding='utf-8'))

reg = set()
for f in glob.glob(os.path.join(BASE, 'runtime', 'src', 'kernel', 'svc_*.cpp')) + glob.glob(os.path.join(BASE, 'runtime', 'src', 'services', '*.cpp')):
    for m in re.finditer(r'svc_register\(\s*(0x[0-9A-Fa-f]+)', open(f, errors='replace').read()): reg.add(int(m.group(1), 16))
names = {}
inc = os.path.join(BASE, 'runtime', 'src', 'kernel', 'svc_names.inc')
for m in re.finditer(r'NXRT_SVC\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*"(\w+)"', open(inc).read()): names[int(m.group(1), 0)] = m.group(2)
for mod in man['modules']:
    name = mod['name']
    d = open(os.path.join(game, 'game', 'exefs', mod.get('file', name)), 'rb').read()
    t = nso.nso_info(d)['segments'][0]['data']
    c = collections.Counter()
    for i in range(0, len(t) - 3, 4):
        w = struct.unpack_from('<I', t, i)[0]
        if (w & 0xffe0001f) == 0xd4000001: c[(w >> 5) & 0xffff] += 1
    print('%s: %d sites, %d distinct' % (name, sum(c.values()), len(c)))
    for n in sorted(c):
        print('   0x%02x %-34s x%-3d %s' % (n, names.get(n, '?'), c[n], '' if n in reg else 'NO HANDLER'))
