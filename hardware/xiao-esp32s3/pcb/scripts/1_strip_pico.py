import re, sys

SCH='xiao-esp32s3-can-sniffer.kicad_sch'
s=open(SCH).read()

def split_top(text):
    start = text.index('\n\t(')
    blocks, i = [], start
    while True:
        j = text.find('\n\t(', i)
        if j < 0: break
        k, depth = j+1, 0
        while k < len(text):
            c = text[k]
            if c == '"':
                k += 1
                while text[k] != '"' or text[k-1] == '\\': k += 1
            elif c == '(': depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0: break
            k += 1
        blocks.append(text[j+1:k+1]); i = k+1
    return text[:start+1], blocks, text[i:]

pre, blocks, suf = split_top(s)

def kind(b):
    m = re.match(r'\s*\((\w+)', b)
    if not m:
        raise SystemExit("unparsed block: %r" % b[:80])
    return m.group(1)

def ref(b):
    m = re.search(r'\(property "Reference" "([^"]+)"', b)
    return m.group(1) if m else None

def pts(b):
    return [(float(x), float(y)) for x, y in re.findall(r'\(xy ([-\d.]+) ([-\d.]+)\)', b)]

def at(b):
    m = re.search(r'\(at ([-\d.]+) ([-\d.]+)', b)
    return (float(m.group(1)), float(m.group(2))) if m else None

# Areas whose contents go away with the parts they belong to.
U1    = (100, 10, 180, 105)    # the Pico and the labels on its pins
S2BOX = (24, 50, 62, 66)       # second button and its pull-down
D3BOX = (43, 80, 62, 105)      # yellow LED
D4BOX = (52, 92, 62, 118)      # red LED
LEDBOX = (20, 74, 62, 103)      # the two LEDs that become an addressable chain
BOXES = [S2BOX, D3BOX, D4BOX, LEDBOX]
def inside(p, box):
    return box[0] <= p[0] <= box[2] and box[1] <= p[1] <= box[3]
def in_any(p):
    return inside(p, U1) or any(inside(p, b) for b in BOXES)

DROP_REFS = {'U1', 'D3', 'R5', 'D4', 'R6', 'S2', 'R2', 'D1', 'D2', 'R3', 'R4', '#PWR015', '#PWR016', 'H2', 'H4',
             '#PWR031', '#PWR032', '#PWR08', '#PWR09', '#PWR017', '#PWR018'}

kept, dropped = [], []
for b in blocks:
    k = kind(b)
    gone = False
    if k == 'symbol':
        gone = ref(b) in DROP_REFS
    elif k == 'wire':
        p = pts(b)
        gone = len(p) == 2 and all(in_any(q) for q in p)
    elif k in ('junction', 'no_connect'):
        gone = in_any(at(b))
    elif k in ('global_label', 'label'):
        p = at(b)
        gone = inside(p, U1) or (p in [(60.96, 52.07), (60.96, 82.55), (60.96, 95.25), (60.96, 77.47), (60.96, 80.01)])
    (dropped if gone else kept).append(b)

print("dropped %d blocks:" % len(dropped))
for b in dropped:
    name = ref(b)
    if not name:
        m = re.match(r'\s*\(\w+ "([^"]*)"', b)
        name = m.group(1)[:28] if m else ""
    print("   ", kind(b), name, at(b) or pts(b)[:2])

if '--write' in sys.argv:
    open(SCH,'w').write(pre + "\n".join(kept) + suf)
    print("written")
