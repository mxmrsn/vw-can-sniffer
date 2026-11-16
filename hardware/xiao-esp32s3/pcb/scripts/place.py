import pcbnew, os, re, sys
from pcbnew import FromMM as MM, ToMM as TM, VECTOR2I
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRC = '../../pico2w/pcb/pico2w-can-sniffer.kicad_pcb'
DST = 'xiao-esp32s3-can-sniffer.kicad_pcb'
KI = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/'
b = pcbnew.LoadBoard(SRC)
TRACKS = list(b.GetTracks()); FPS = list(b.GetFootprints()); ZONES = list(b.Zones()); DRAW = list(b.GetDrawings())

# ---- what the schematic wants
t = open(os.environ.get('NETLIST', 'build/netlist.net')).read()
comps = {}
for m in re.finditer(r"\(comp\s*\(ref \"([^\"]+)\"\)\s*\(value \"([^\"]*)\"\)\s*\(footprint \"([^\"]*)\"\)", t):
    comps[m.group(1)] = (m.group(2), m.group(3))
padnet = {}
for name, body in re.findall(r'\(net\s*\n?\s*\(code "\d+"\)\s*\n?\s*\(name "([^"]+)"\)(.*?)(?=\(net\s|\Z)', t, re.S):
    for ref, pin in re.findall(r'\(ref "([^"]+)"\)\s*\n?\s*\(pin "([^"]+)"\)', body):
        padnet[(ref, pin)] = name

for x in TRACKS: b.Remove(x)
for z in ZONES: b.Remove(z)
for d in DRAW:
    if d.GetLayer() in (pcbnew.Edge_Cuts, pcbnew.F_SilkS, pcbnew.B_SilkS, pcbnew.Dwgs_User):
        b.Remove(d)
keep = {}
for f in FPS:
    r = f.GetReference()
    if r in comps and comps[r][1] == f.GetFPIDAsString():
        keep[r] = f
    else:
        b.Remove(f)

def load(fpid, ref):
    lib, name = fpid.split(':')
    path = os.path.abspath('lib/xiao.pretty') if lib == 'xiao' else KI + lib + '.pretty'
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, fpid
    fp.SetReference(ref)
    b.Add(fp)
    return fp
for r, (val, fpid) in comps.items():
    if r not in keep:
        keep[r] = load(fpid, r)
    keep[r].SetValue(val)

# ---- placement: (x, y, rotation); board 100..168 x 100..138
X = 3.0   # everything right of the module shifts by this to make room for a mounting hole
PLACE = {
 'U1': (111.0, 110.6, 90),
 'J2': (133.5+X, 108.6, 180),      # card slot on the top edge
 'J3': (133.5+X, 121.2, 90),
 # indicators along the bottom edge, buffer and button above them
 'U5': (104.5, 124.0, 0), 'C16': (101.6, 124.0, 90), 'R3': (108.5, 124.0, 0),
 'D1': (104.0, 133.2, 0), 'D2': (112.0, 133.2, 0), 'D3': (120.0, 133.2, 0), 'D4': (128.0, 133.2, 0),
 'C12': (107.8, 133.2, 90), 'C13': (115.8, 133.2, 90), 'C14': (123.8, 133.2, 90), 'C15': (131.8, 133.2, 90),
 'S1': (118.5, 125.5, 0), 'R1': (125.3, 125.5, 90),
 # CAN
 'U4': (137.5+X, 128.0, 90), 'C1': (132.5+X, 126.0, 90), 'C2': (132.5+X, 130.0, 90), 'C8': (129.8+X, 128.0, 90),
 'C7': (143.0+X, 127.3, 0), 'JP1': (143.0+X, 130.5, 0), 'R7': (143.0+X, 133.5, 0), 'R2': (137.5+X, 133.8, 0),
 # power
 'S3': (159.5+X, 101.92, 180),   # actuator out through the top edge
 'R9': (153.0+X, 104.6, 90),
 'U3': (150.5+X, 113.0, 0), 'C6': (150.5+X, 110.0, 0), 'C3': (154.5+X, 113.0, 90),
 'L1': (159.5+X, 113.5, 0), 'C4': (157.5+X, 118.2, 90), 'C5': (160.5+X, 118.2, 90),
 'R15': (152.5+X, 117.5, 0), 'R16': (152.5+X, 119.5, 0), 'C11': (148.8+X, 117.5, 0),
 'F1': (152.0+X, 124.5, 0), 'D6': (160.5+X, 124.8, 0), 'D7': (152.0+X, 129.0, 0), 'D5': (152.0+X, 133.5, 0),
 'R11': (146.5+X, 121.5, 0), 'R12': (146.5+X, 123.5, 0), 'C9': (146.5+X, 125.5, 0),
 'J1': (160.5+X, 132.0, 0), 'C10': (129.5, 119.6, 0),
 'H1': (124.8, 104.0, 0), 'H3': (136.0, 134.6, 0),   # diagonal
}
EXTRA = set(PLACE) - set(keep)
assert not EXTRA, EXTRA
for r, f in keep.items():
    if r not in PLACE:
        print("NOT PLACED", r); continue
    x, y, rot = PLACE[r]
    f.SetPosition(VECTOR2I(MM(x), MM(y)))
    f.SetOrientationDegrees(rot)

# ---- nets
def net(name):
    n = b.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(b, name); b.Add(n)
    return n
for r, f in keep.items():
    for p in f.Pads():
        key = (r, p.GetNumber())
        p.SetNet(net(padnet[key]) if key in padnet else b.FindNet(''))

# ---- outline: rounded corners, kinder to hands and to an enclosure
W0, H0, W1, H1 = 100.0, 100.0, 168.0, 138.0
R = 3.0
def edge(shape):
    shape.SetLayer(pcbnew.Edge_Cuts); shape.SetWidth(MM(0.1)); b.Add(shape)
for (x1, y1, x2, y2) in ((W0 + R, H0, W1 - R, H0), (W1, H0 + R, W1, H1 - R),
                         (W1 - R, H1, W0 + R, H1), (W0, H1 - R, W0, H0 + R)):
    s = pcbnew.PCB_SHAPE(b); s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(VECTOR2I(MM(x1), MM(y1))); s.SetEnd(VECTOR2I(MM(x2), MM(y2)))
    edge(s)
k = R * (1 - 0.5 ** 0.5)   # how far the arc's midpoint sits in from the corner
for (sx, sy), (mx, my), (ex, ey) in (
        ((W0, H0 + R), (W0 + k, H0 + k), (W0 + R, H0)),
        ((W1 - R, H0), (W1 - k, H0 + k), (W1, H0 + R)),
        ((W1, H1 - R), (W1 - k, H1 - k), (W1 - R, H1)),
        ((W0 + R, H1), (W0 + k, H1 - k), (W0, H1 - R))):
    a = pcbnew.PCB_SHAPE(b); a.SetShape(pcbnew.SHAPE_T_ARC)
    a.SetArcGeometry(VECTOR2I(MM(sx), MM(sy)), VECTOR2I(MM(mx), MM(my)), VECTOR2I(MM(ex), MM(ey)))
    edge(a)

# ---- ground on both layers
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(b)
    z.SetLayer(layer); z.SetNet(net('GND'))
    o = z.Outline(); o.NewOutline()
    for px, py in ((W0, H0), (W1, H0), (W1, H1), (W0, H1)):
        o.Append(MM(px), MM(py))
    z.SetLocalClearance(MM(0.3)); z.SetMinThickness(MM(0.25))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    b.Add(z)

pcbnew.SaveBoard(DST, b)
print("placed", len(keep), "footprints")

# ---- 3D models for the enclosure work: the module and the LEDs have none in KiCad's library
b = pcbnew.LoadBoard(DST)
for f in list(b.GetFootprints()):
    r = f.GetReference()
    path = None
    if r == 'U1':
        path = '${KIPRJMOD}/3dmodels/Seeed_XIAO_socketed.step'
    elif r in ('D1', 'D2', 'D3', 'D4'):
        path = '${KIPRJMOD}/3dmodels/SK6812MINI_placeholder.step'
    if path:
        f.Models().clear()
        m = pcbnew.FP_3DMODEL(); m.m_Filename = path
        f.Models().push_back(m)
pcbnew.SaveBoard(DST, b)
print("3D models assigned")
