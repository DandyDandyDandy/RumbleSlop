"""Write port/recomp/tables/ from the outputs of closure_build.py (the version-specific tables the build reads).

    python closure_build.py        # work/unity/closure/*.tsv, closure.npz
    python make_tables.py          # tables/lift_functions.tsv.gz, boundary_direct.tsv, boundary_runtime_to_engine.tsv
    python ../port/recomp/extra_lift.py     # tables/extra_lift.tsv

Only the columns the build uses are kept: a function's start and end; a boundary target with what it is. Names of
the game's own methods do not go into the repository: `identification` keeps the P/Invoke native's managed name
(which the runtime's import policy is keyed on) and nothing else.
"""
from _env import REPO, W  # noqa: E402
import csv
import gzip
import os

CLOSURE = W + r'\work\unity\closure'
TABLES = os.path.join(REPO, 'port', 'recomp', 'tables')


def rows(name):
    with open(os.path.join(CLOSURE, name), encoding='utf-8') as f:
        yield from csv.DictReader(f, delimiter='\t')


def main():
    n = 0
    with gzip.GzipFile(os.path.join(TABLES, 'lift_functions.tsv.gz'), 'wb', mtime=0) as f:
        f.write(b'start\tend\n')
        for r in rows('lift_functions.tsv'):
            f.write(f"{int(r['start'], 16):x}\t{int(r['end'], 16):x}\n".encode())
            n += 1
    print('lift_functions.tsv.gz', n)
    for name, cols in (('boundary_direct.tsv', ('target', 'region', 'identification')),
                       ('boundary_runtime_to_engine.tsv', ('target', 'export_name', 'hint'))):
        with open(os.path.join(TABLES, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\t'.join(cols) + '\n')
            n = 0
            for r in rows(name):
                f.write('\t'.join(r[c] for c in cols) + '\n')
                n += 1
        print(name, n)


if __name__ == '__main__':
    main()
