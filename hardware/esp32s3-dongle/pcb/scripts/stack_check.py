# The stacking connector: each bottom-board pin must meet the top-board pin carrying the same net.
# Pads are compared by row (y) and side (x left or right of the connector's centre).
import os, sys
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build')
def load(name):
    rows = [line.split() for line in open(os.path.join(here, name + '-stack.txt'))]
    cx = sum(float(r[1]) for r in rows) / len(rows)
    return {(round(float(r[2]), 1), float(r[1]) > cx): (r[0], r[3].lstrip('/')) for r in rows}
bottom, top = load('bottom'), load('top')
bad = [(k, bottom[k], top.get(k)) for k in bottom if top.get(k, (None, None))[1] != bottom[k][1]]
if bad:
    sys.exit('stacking connector mismatch: %s' % bad)
print('stacking connector: all 14 pins meet the same net on both boards')
