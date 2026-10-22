"""Reference hashes for the C++ TXTR reader (tests/test_pak.cpp, env MPRS_TXTR_REF).

Takes the rows of work/survey/textures/txtr_all.tsv sorted by asset id, every 40th row, decodes each extracted TXTR with
tools/survey/txtr_decode.py load() and writes one line per row: `<asset id><TAB><sha256 hex of the tiled GPU buffer>`.
usage: python txtr_ref_hashes.py <out.tsv>
"""
import csv, hashlib, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))  # tools/setup/scripts -> project root
sys.path.insert(0, os.path.join(ROOT, 'tools', 'survey'))
from txtr_decode import load  # noqa: E402


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: txtr_ref_hashes.py <out.tsv>')
    tsv = os.path.join(ROOT, 'work', 'survey', 'textures', 'txtr_all.tsv')
    with open(tsv, newline='') as f:
        rows = list(csv.DictReader(f, delimiter='\t'))
    rows.sort(key=lambda r: r['id'])
    picks = rows[::40]
    lines = []
    for r in picks:
        head, tiled = load(os.path.join(ROOT, *r['path'].split('/')))
        if len(tiled) != int(r['gpu_size']):
            sys.exit(f"{r['path']}: decoded {len(tiled)} bytes, table says {r['gpu_size']}")
        lines.append(f"{r['id']}\t{hashlib.sha256(tiled).hexdigest()}\n")
    with open(sys.argv[1], 'w', newline='') as f:
        f.writelines(lines)
    print(f'{len(rows)} table rows, sampled {len(picks)} (every 40th by id) -> {sys.argv[1]}')


if __name__ == '__main__':
    main()
