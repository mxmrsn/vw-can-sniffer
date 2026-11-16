"""Place one dongle board from its netlist, on the temporary outline.

    KiCad's python:  layout.py bottom|top <netlist>

Both boards share one outline (56 x 44 mm, 3 mm corners) and the same mounting
holes. The bottom board's header J2 and the top board's socket J5 sit at the same
XY; the socket is on the top board's underside. Everything here is provisional
until the enclosure gives the real outline.
"""
import os
import re
import sys

import pcbnew
from pcbnew import FromMM as MM, ToMM as TM, VECTOR2I

BOARD, NETLIST = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.normpath(os.path.join(HERE, '..'))
TEMPLATE = os.path.normpath(os.path.join(PCB, '..', '..', 'xiao-esp32s3', 'pcb', 'xiao-esp32s3-can-sniffer.kicad_pcb'))
DST = os.path.join(PCB, BOARD, 'dongle-%s.kicad_pcb' % BOARD)
KI = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/'

W0, H0, W1, H1, R = 100.0, 100.0, 156.0, 144.0, 3.0
STACK = (128.0, 128.0)                   # board-to-board connector, both boards
HOLES = {'H1': (150.5, 133.5), 'H2': (138.5, 103.0)}

# placeholder STEP models (3dmodels/make_models.py) for footprints KiCad has none for
MODELS = {'ESP32-S2-MINI-1': 'ESP32-S3-MINI-1_placeholder.step',
          'Fuse_1812_4532Metric': 'Fuse_1812_placeholder.step',
          'LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm': 'SK6812MINI_placeholder.step'}

# (x, y, rotation[, 'back'])
PLACE = {
    'top': {
        'U3': (111.5, 109.8, 0),           # antenna along the top edge; a channel on its left for traces
        'J3': (148.0, 103.1, 180),         # GPS / sync, cable out of the top edge
        'J2': (147.3, 121.0, 90),          # microSD, card in from the right edge
        'J1': (103.2, 133.0, 270),         # USB-C, out of the left edge
        'J4': (128.0, 111.0, 0),
        'J5': STACK + (0, 'back'),
        'S2': (106.0, 141.5, 0), 'S1': (136.0, 134.0, 90),
        'R1': (113.0, 123.5, 0), 'R2': (116.5, 123.5, 0), 'U1': (112.5, 128.0, 0),
        'D1': (112.5, 133.5, 90), 'D2': (116.0, 133.5, 90), 'U2': (120.5, 129.5, 0),
        'C1': (120.5, 134.0, 0), 'C2': (120.5, 125.8, 0), 'C3': (112.5, 138.0, 0),
        'R3': (121.2, 107.0, 90), 'C4': (121.2, 110.5, 90), 'C5': (121.2, 114.0, 90),
        'U4': (122.0, 119.0, 0), 'C6': (127.0, 119.0, 0), 'R4': (131.0, 119.0, 0),
        'D3': (106.5, 124.0, 0),
        'D4': (118.0, 141.4, 0), 'D5': (126.0, 141.4, 0), 'D6': (134.0, 141.4, 0), 'D7': (142.0, 141.4, 0),
        'C7': (121.8, 141.4, 90), 'C8': (129.8, 141.4, 90), 'C9': (137.8, 141.4, 90), 'C10': (145.8, 141.4, 90),
        'C11': (135.0, 117.0, 0), 'R5': (135.0, 120.0, 0), 'R6': (135.0, 122.5, 0), 'R7': (135.0, 125.0, 0),
        'U5': (138.0, 108.0, 0), 'C12': (125.0, 105.4, 0), 'C13': (128.5, 105.4, 0), 'R8': (132.0, 105.4, 0),
        'R9': (141.5, 111.6, 0),
    },
    'bottom': {
        'J1': (112.0, 141.5, 0),           # OBD wires in from the bottom edge
        'J2': STACK + (0,),
        'F1': (108.5, 135.5, 0), 'D1': (116.5, 135.7, 0), 'D2': (124.5, 136.5, 0),
        'C2': (130.5, 136.5, 0),
        'U2': (106.5, 125.0, 0), 'C1': (102.8, 125.0, 90), 'C6': (110.5, 125.0, 90), 'L1': (106.5, 119.0, 0),
        'C7': (111.5, 119.0, 90), 'C8': (114.0, 119.0, 90), 'R2': (112.0, 128.5, 0), 'R3': (112.0, 130.3, 0),
        'C9': (116.0, 128.5, 0), 'R4': (106.5, 129.0, 0),
        'U1': (122.0, 113.5, 0), 'C3': (117.0, 110.5, 90), 'C4': (127.7, 110.5, 90), 'C5': (127.7, 113.7, 90),
        'D3': (117.5, 122.0, 0), 'JP1': (136.0, 115.0, 0), 'R1': (136.0, 118.0, 0),
        'U3': (143.0, 113.0, 0), 'R5': (148.5, 111.0, 0), 'D4': (148.5, 114.0, 0), 'C10': (148.5, 117.0, 0),
        'R6': (143.0, 118.0, 0), 'R7': (143.0, 120.5, 0), 'R8': (143.0, 108.8, 0), 'Q1': (147.5, 121.0, 0),
        'R9': (148.5, 125.0, 0), 'R10': (143.0, 125.0, 0),
        'R11': (137.0, 125.0, 0), 'R12': (137.0, 127.5, 0), 'C11': (137.0, 130.0, 0),
    },
}[BOARD]
for ref, xy in HOLES.items():
    PLACE[ref] = xy + (0,)

t = open(NETLIST).read()
comps = {m.group(1): (m.group(2), m.group(3)) for m in
         re.finditer(r'\(comp\s*\(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*\(footprint "([^"]*)"\)', t)}
padnet = {}
for name, body in re.findall(r'\(net\s*\n?\s*\(code "\d+"\)\s*\n?\s*\(name "([^"]+)"\)(.*?)(?=\(net\s|\Z)', t, re.S):
    for ref, pin in re.findall(r'\(ref "([^"]+)"\)\s*\n?\s*\(pin "([^"]+)"\)', body):
        padnet[(ref, pin)] = name
missing = set(comps) - set(PLACE)
assert not missing, 'not placed: %s' % sorted(missing)

# Load every footprint before touching the template: removing items first upsets the
# plugin layer of KiCad's Python bindings.
loaded = {}
for ref, (val, fpid) in sorted(comps.items()):
    lib, name = fpid.split(':')
    loaded[ref] = pcbnew.FootprintLoad(KI + lib + '.pretty', name)
    assert loaded[ref], fpid

b = pcbnew.LoadBoard(TEMPLATE)
TRACKS, ZONES, FPS, DRAW = list(b.GetTracks()), list(b.Zones()), list(b.GetFootprints()), list(b.GetDrawings())
for group in (TRACKS, ZONES, FPS, DRAW):
    for item in group:
        b.Remove(item)


def net(name):
    n = b.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(b, name)
        b.Add(n)
    return n


for ref, (val, fpid) in sorted(comps.items()):
    fp = loaded[ref]
    fp.SetReference(ref)
    fp.SetValue(val)
    b.Add(fp)
    x, y, rot = PLACE[ref][:3]
    fp.SetPosition(VECTOR2I(MM(x), MM(y)))
    fp.SetOrientationDegrees(rot)
    if len(PLACE[ref]) > 3 and PLACE[ref][3] == 'back':
        fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
    model = MODELS.get(fpid.split(':')[1])
    if model:
        fp.Models().clear()
        m = pcbnew.FP_3DMODEL()
        m.m_Filename = '${KIPRJMOD}/../3dmodels/' + model
        fp.Models().push_back(m)
    for p in fp.Pads():
        key = (ref, p.GetNumber())
        if key in padnet:
            p.SetNet(net(padnet[key]))


def edge(shape):
    shape.SetLayer(pcbnew.Edge_Cuts)
    shape.SetWidth(MM(0.1))
    b.Add(shape)


for (x1, y1, x2, y2) in ((W0 + R, H0, W1 - R, H0), (W1, H0 + R, W1, H1 - R),
                         (W1 - R, H1, W0 + R, H1), (W0, H1 - R, W0, H0 + R)):
    s = pcbnew.PCB_SHAPE(b)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(VECTOR2I(MM(x1), MM(y1)))
    s.SetEnd(VECTOR2I(MM(x2), MM(y2)))
    edge(s)
k = R * (1 - 0.5 ** 0.5)
for (sx, sy), (mx, my), (ex, ey) in (((W0, H0 + R), (W0 + k, H0 + k), (W0 + R, H0)),
                                     ((W1 - R, H0), (W1 - k, H0 + k), (W1, H0 + R)),
                                     ((W1, H1 - R), (W1 - k, H1 - k), (W1 - R, H1)),
                                     ((W0 + R, H1), (W0 + k, H1 - k), (W0, H1 - R))):
    a = pcbnew.PCB_SHAPE(b)
    a.SetShape(pcbnew.SHAPE_T_ARC)
    a.SetArcGeometry(VECTOR2I(MM(sx), MM(sy)), VECTOR2I(MM(mx), MM(my)), VECTOR2I(MM(ex), MM(ey)))
    edge(a)


def rect_zone(layer_set, x0, y0, x1, y1, netname=None, rule=False):
    z = pcbnew.ZONE(b)
    z.SetLayerSet(layer_set)
    o = z.Outline()
    o.NewOutline()
    for px, py in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        o.Append(MM(px), MM(py))
    if rule:
        z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(True)
        z.SetDoNotAllowVias(True)
        z.SetDoNotAllowPads(True)
        z.SetDoNotAllowZoneFills(True)
        z.SetDoNotAllowFootprints(False)
    else:
        z.SetNet(net(netname))
        z.SetLocalClearance(MM(0.3))
        z.SetMinThickness(MM(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)   # no floating copper
    b.Add(z)


both = pcbnew.LSET()
both.AddLayer(pcbnew.F_Cu)
both.AddLayer(pcbnew.B_Cu)
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    ls = pcbnew.LSET()
    ls.AddLayer(layer)
    rect_zone(ls, W0, H0, W1, H1, 'GND')
if BOARD == 'bottom':
    # nothing conductive under the top board's antenna, a few millimetres above
    rect_zone(both, W0, H0, 135.5, 108.0, rule=True)

pcbnew.SaveBoard(DST, b)
print('placed %d footprints on %s' % (len(comps), os.path.basename(DST)))

# where each pin of the stacking connector lands, for the cross-check between boards
for f in b.GetFootprints():
    if f.GetReference() in ('J2', 'J5') and 'x07' in f.GetFPIDAsString():
        with open(os.path.join(PCB, 'build', '%s-stack.txt' % BOARD), 'w') as out:
            for p in f.Pads():
                out.write('%s %.2f %.2f %s\n' % (p.GetNumber(), TM(p.GetPosition().x), TM(p.GetPosition().y), p.GetNetname()))
