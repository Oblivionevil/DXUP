"""Compares two shader_bench outputs: native.csv dxup.csv [top].

Prints totals and the shaders DXUP runs slowest relative to native.
"""
import csv
import sys


def load(path):
    with open(path) as f:
        return {row['shader']: float(row['us']) for row in csv.DictReader(f)}


native, dxup = load(sys.argv[1]), load(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
both = [(name, native[name], dxup[name]) for name in native
        if name in dxup and native[name] > 0.5 and dxup[name] > 0.0]
total_n = sum(n for _, n, _ in both)
total_d = sum(d for _, _, d in both)
print('%d shaders, native %.1f us, dxup %.1f us, ratio %.3f' % (len(both), total_n, total_d, total_d / total_n))
print('slower (by extra us):')
for name, n, d in sorted(both, key=lambda x: x[1] - x[2])[:0] or sorted(both, key=lambda x: x[2] - x[1], reverse=True)[:top]:
    print('  %-28s native %8.2f  dxup %8.2f  x%.2f' % (name, n, d, d / n))
print('faster:')
for name, n, d in sorted(both, key=lambda x: x[2] - x[1])[:8]:
    print('  %-28s native %8.2f  dxup %8.2f  x%.2f' % (name, n, d, d / n))
