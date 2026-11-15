#!/usr/bin/env python3
"""Write a STEP file made of plain boxes, for placeholder 3D models.

Usage: edit BOXES below (mm, KiCad model axes: X right, Y away from viewer's bottom, Z up) and run
    python3 tools/make_placeholder_step.py <out.step>
"""
import sys

# name, (x0, y0, z0), (x1, y1, z1)
# C&K JS102011SAQN placeholder. Plan outline is from the KiCad footprint's fab layer
# (body 9.0 x 3.6 mm, actuator 1.5 mm wide protruding 2.0 mm). Heights are ASSUMED, not from the datasheet.
BOXES = [
    ("body", (-4.5, -1.8, 0.0), (4.5, 1.8, 3.5)),
    ("actuator", (-2.0, -3.8, 1.0), (-0.5, -1.8, 2.5)),
]

class Step:
    def __init__(self):
        self.lines = []
    def add(self, text):
        self.lines.append(text)
        return len(self.lines)
    def ref(self, text):
        return "#%d" % self.add(text)

def f(v):
    return "%.6f" % v

def box(s, name, a, b):
    (x0, y0, z0), (x1, y1, z1) = a, b
    V = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    pts = [s.ref("CARTESIAN_POINT('',(%s,%s,%s))" % tuple(map(f, v))) for v in V]
    vtx = [s.ref("VERTEX_POINT('',%s)" % p) for p in pts]
    edges = {}
    def edge(i, j):
        k = (min(i, j), max(i, j))
        if k not in edges:
            p, q = V[k[0]], V[k[1]]
            d = [q[n] - p[n] for n in range(3)]
            L = sum(c * c for c in d) ** 0.5
            dr = s.ref("DIRECTION('',(%s,%s,%s))" % tuple(f(c / L) for c in d))
            vec = s.ref("VECTOR('',%s,%s)" % (dr, f(L)))
            line = s.ref("LINE('',%s,%s)" % (pts[k[0]], vec))
            edges[k] = s.ref("EDGE_CURVE('',%s,%s,%s,.T.)" % (vtx[k[0]], vtx[k[1]], line))
        return edges[k], (i < j)
    # loops are counter-clockwise seen from outside; (vertex loop, outward normal, in-plane reference direction)
    faces = [((0, 3, 2, 1), (0, 0, -1), (1, 0, 0)), ((4, 5, 6, 7), (0, 0, 1), (1, 0, 0)),
             ((0, 1, 5, 4), (0, -1, 0), (1, 0, 0)), ((2, 3, 7, 6), (0, 1, 0), (1, 0, 0)),
             ((1, 2, 6, 5), (1, 0, 0), (0, 1, 0)), ((3, 0, 4, 7), (-1, 0, 0), (0, 1, 0))]
    frefs = []
    for loop, nrm, rdir in faces:
        oes = []
        for n in range(4):
            e, fwd = edge(loop[n], loop[(n + 1) % 4])
            oes.append(s.ref("ORIENTED_EDGE('',*,*,%s,%s)" % (e, ".T." if fwd else ".F.")))
        el = s.ref("EDGE_LOOP('',(%s))" % ",".join(oes))
        fb = s.ref("FACE_OUTER_BOUND('',%s,.T.)" % el)
        ax = s.ref("AXIS2_PLACEMENT_3D('',%s,%s,%s)" % (pts[loop[0]],
                   s.ref("DIRECTION('',(%s,%s,%s))" % tuple(map(f, nrm))), s.ref("DIRECTION('',(%s,%s,%s))" % tuple(map(f, rdir)))))
        pl = s.ref("PLANE('',%s)" % ax)
        frefs.append(s.ref("ADVANCED_FACE('',(%s),%s,.T.)" % (fb, pl)))
    shell = s.ref("CLOSED_SHELL('',(%s))" % ",".join(frefs))
    return s.ref("MANIFOLD_SOLID_BREP('%s',%s)" % (name, shell))

def main(out):
    s = Step()
    app = s.ref("APPLICATION_CONTEXT('automotive design')")
    s.add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000,%s)" % app)
    pc = s.ref("PRODUCT_CONTEXT('',%s,'mechanical')" % app)
    prod = s.ref("PRODUCT('placeholder','placeholder','',(%s))" % pc)
    pdf = s.ref("PRODUCT_DEFINITION_FORMATION('','',%s)" % prod)
    pdc = s.ref("PRODUCT_DEFINITION_CONTEXT('part definition',%s,'design')" % app)
    pd = s.ref("PRODUCT_DEFINITION('design','',%s,%s)" % (pdf, pdc))
    pds = s.ref("PRODUCT_DEFINITION_SHAPE('','',%s)" % pd)
    lu = s.ref("(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.))")
    au = s.ref("(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))")
    su = s.ref("(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())")
    unc = s.ref("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-06),%s,'distance_accuracy_value','')" % lu)
    ctx = s.ref("(GEOMETRIC_REPRESENTATION_CONTEXT(3)GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((%s))GLOBAL_UNIT_ASSIGNED_CONTEXT((%s,%s,%s))REPRESENTATION_CONTEXT('',''))" % (unc, lu, au, su))
    org = s.ref("AXIS2_PLACEMENT_3D('',%s,%s,%s)" % (s.ref("CARTESIAN_POINT('',(0.,0.,0.))"), s.ref("DIRECTION('',(0.,0.,1.))"), s.ref("DIRECTION('',(1.,0.,0.))")))
    solids = [box(s, n, a, b) for n, a, b in BOXES]
    rep = s.ref("ADVANCED_BREP_SHAPE_REPRESENTATION('',(%s,%s),%s)" % (org, ",".join(solids), ctx))
    s.add("SHAPE_DEFINITION_REPRESENTATION(%s,%s)" % (pds, rep))
    body = "".join("#%d=%s;\n" % (i + 1, l) for i, l in enumerate(s.lines))
    open(out, "w").write("ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('placeholder boxes'),'2;1');\n"
                         "FILE_NAME('%s','2026-11-15T00:00:00',(''),(''),'make_placeholder_step.py','','');\n"
                         "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n%sENDSEC;\nEND-ISO-10303-21;\n" % (out.split('/')[-1], body))

if __name__ == "__main__":
    main(sys.argv[1])
