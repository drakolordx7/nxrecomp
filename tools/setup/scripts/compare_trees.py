"""Compare two folders byte for byte (file set, sizes, SHA-256). Prints a summary and exits 1 on any difference.
usage: python compare_trees.py <folder A> <folder B> [--jobs N]
Used to check that a setup run produced exactly the files an independent extraction (hactool) produced."""
import argparse, concurrent.futures as cf, hashlib, os, sys, time


def walk(root):
    out = {}
    for dp, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(dp, f)
            out[os.path.relpath(p, root).replace('\\', '/')] = p
    return out


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb', buffering=0) as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    t0 = time.time()
    A, B = walk(a.a), walk(a.b)
    only_a = sorted(set(A) - set(B))
    only_b = sorted(set(B) - set(A))
    common = sorted(set(A) & set(B))
    bad = []
    size_bad = [r for r in common if os.path.getsize(A[r]) != os.path.getsize(B[r])]
    todo = [r for r in common if r not in size_bad]
    total = sum(os.path.getsize(A[r]) for r in todo)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for r, (ha, hb) in zip(todo, ex.map(lambda r: (sha(A[r]), sha(B[r])), todo)):
            if ha != hb:
                bad.append(r)
    print(f'A: {len(A)} files   B: {len(B)} files   common: {len(common)}   hashed {total / 1e9:.2f} GB each in {time.time() - t0:.0f} s')
    for r in only_a[:20]:
        print('only in A:', r)
    for r in only_b[:20]:
        print('only in B:', r)
    for r in size_bad[:20]:
        print('size differs:', r)
    for r in bad[:20]:
        print('content differs:', r)
    ok = not (only_a or only_b or size_bad or bad)
    print('IDENTICAL' if ok else f'DIFFERENT (only A {len(only_a)}, only B {len(only_b)}, size {len(size_bad)}, content {len(bad)})')
    sys.exit(0 if ok else 1)


main()
