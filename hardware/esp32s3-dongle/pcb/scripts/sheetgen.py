"""Build a KiCad 10 schematic from a parts list.

Every pin gets a short wire stub ending in a net label, a power symbol or a
no-connect flag, so connectivity is exactly the parts list and ERC can check it.
Parts are laid out in titled blocks, left to right, wrapping as needed. The
result is a readable reference drawing, not a hand-crafted one.

    sheet = Sheet("Title", "rev", ["comment"])
    blk = sheet.block("POWER INPUT", "what this block does")
    blk.part("Device:R", "R1", "100k", "Resistor_SMD:R_0603_1608Metric",
             {"1": "VBAT", "2": "GND"}, mpn=..., manufacturer=..., digikey=..., notes=...)
    sheet.save("board.kicad_sch")
"""
import math
import re
import uuid

LIB = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/'
NS = uuid.UUID('6ba7b811-9dad-11d1-80b4-00c04fd430c8')
POWER = {'GND', '+3V3', '+5V'}          # drawn as power symbols; everything else gets a label
GRID = 1.27
STUB = 2.54


def snap(v):
    return round(round(v / GRID) * GRID, 2)


def _block(text, start):
    depth, k = 0, start
    while k < len(text):
        c = text[k]
        if c == '"':
            k += 1
            while text[k] != '"' or text[k - 1] == '\\':
                k += 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[start:k + 1]
        k += 1
    raise ValueError('unbalanced')


class Symbol:
    """A library symbol, flattened if it extends another, with its pin geometry."""

    def __init__(self, lib_id, text=None):
        self.lib_id = lib_id
        lib, name = lib_id.split(':')
        t = text if text is not None else open(LIB + lib + '.kicad_sym').read()
        blk = _block(t, t.index('(symbol "%s"' % name))
        base = re.search(r'\(extends "([^"]+)"\)', blk[:800])
        if base:
            parent = _block(t, t.index('(symbol "%s"' % base.group(1)))
            # keep the derived symbol's own properties, take the parent's drawing and pins
            props = re.findall(r'\n\t\t\(property "[^"]+"[\s\S]*?\n\t\t\)', blk)
            body = parent.replace('(symbol "%s"' % base.group(1), '(symbol "%s"' % name, 1)
            body = body.replace('"%s_' % base.group(1), '"%s_' % name)
            for p in props:
                key = re.match(r'\n\t\t\(property "([^"]+)"', p).group(1)
                body = re.sub(r'\n\t\t\(property "%s"[\s\S]*?\n\t\t\)' % re.escape(key), lambda m: p, body, count=1)
            blk = body
        self.body = blk.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
        self.pins = {}
        for m in re.finditer(r'\(pin (\w+) \w+\s*\(at ([-\d.]+) ([-\d.]+) (\d+)\)\s*\(length ([\d.]+)\)[\s\S]*?\(name "([^"]*)"[\s\S]*?\(number "([^"]+)"', self.body):
            num = m.group(7)
            if num not in self.pins:          # stacked pins share a number; one stub is enough
                self.pins[num] = (float(m.group(2)), float(m.group(3)), int(m.group(4)), m.group(6), m.group(1))
        self.power = '(power)' in self.body[:400] or self.lib_id.startswith('power:')
        xs = [p[0] for p in self.pins.values()] or [0]
        ys = [p[1] for p in self.pins.values()] or [0]
        for m in re.finditer(r'\(rectangle\s*\(start ([-\d.]+) ([-\d.]+)\)\s*\(end ([-\d.]+) ([-\d.]+)\)', self.body):
            xs += [float(m.group(1)), float(m.group(3))]
            ys += [float(m.group(2)), float(m.group(4))]
        self.bbox = (min(xs), min(ys), max(xs), max(ys))


class Sheet:
    def __init__(self, title, rev, comments, date='2026-11-15', seed=None):
        clean = lambda t: t.replace('"', "'")
        self.title, self.rev, self.comments, self.date = clean(title), rev, [clean(c) for c in comments], date
        self.seed = seed or title
        self.symbols = {}
        self.items = []
        self.geo = []        # (kind, net, x1, y1, x2, y2): wires, pin ends, label and symbol anchors
        self.blocks = []
        self.pwr = 0
        self.extra_libs = {}

    def U(self, s):
        return str(uuid.uuid5(NS, self.seed + ':' + s))

    def symbol(self, lib_id):
        if lib_id not in self.symbols:
            text = self.extra_libs.get(lib_id.split(':')[0])
            self.symbols[lib_id] = Symbol(lib_id, open(text).read() if text else None)
        return self.symbols[lib_id]

    def add_library(self, nickname, path):
        """A project symbol library, e.g. for parts KiCad does not ship."""
        self.extra_libs[nickname] = path

    def block(self, title, note=''):
        b = Block(self, title, note)
        self.blocks.append(b)
        return b

    # ---- low-level items
    @staticmethod
    def _eff(just=None, hide=False):
        o = ('\n\t\t\t(hide yes)' if hide else '') + '\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)'
        if just:
            o += '\n\t\t\t\t(justify %s)' % just
        return o + '\n\t\t\t)'

    def wire(self, x1, y1, x2, y2):
        self.items.append('\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "%s")\n\t)'
                          % (x1, y1, x2, y2, self.U('w:%s,%s,%s,%s' % (x1, y1, x2, y2))))

    def label(self, name, x, y, out):
        rot, just = {(-1, 0): (180, 'right bottom'), (1, 0): (0, 'left bottom'),
                     (0, 1): (270, 'right bottom'), (0, -1): (90, 'left bottom')}[out]
        self.items.append('\t(label "%s"\n\t\t(at %s %s %d)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify %s)\n\t\t)\n\t\t(uuid "%s")\n\t)'
                          % (name, x, y, rot, just, self.U('l:%s:%s,%s' % (name, x, y))))

    def noconnect(self, x, y):
        self.items.append('\t(no_connect\n\t\t(at %s %s)\n\t\t(uuid "%s")\n\t)' % (x, y, self.U('nc:%s,%s' % (x, y))))

    def text(self, s, x, y, size=1.27):
        s = s.replace('"', "'").replace('\n', '\\n')
        self.items.append('\t(text "%s"\n\t\t(exclude_from_sim no)\n\t\t(at %s %s 0)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size %s %s)\n\t\t\t)\n\t\t\t(justify left)\n\t\t)\n\t\t(uuid "%s")\n\t)'
                          % (s, x, y, size, size, self.U('t:%s,%s' % (x, y))))

    def power(self, kind, x, y, out=None):
        """A power symbol whose pin sits at (x, y), its body pointing away along `out`."""
        self.pwr += 1
        ref = '#PWR%03d' % self.pwr
        sym = self.symbol('power:' + kind)
        out = out or ((0, 1) if kind == 'GND' else (0, -1))
        if kind == 'GND':
            rot = {(0, 1): 0, (0, -1): 180, (-1, 0): 270, (1, 0): 90}[out]
        else:
            rot = {(0, -1): 0, (0, 1): 180, (-1, 0): 90, (1, 0): 270}[out]
        vx, vy = round(x + out[0] * 5.08, 2), round(y + out[1] * 5.08, 2)
        self.items.append(
            '\t(symbol\n\t\t(lib_id "power:%s")\n\t\t(at %s %s %d)\n\t\t(unit 1)\n\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(in_pos_files yes)\n\t\t(dnp no)\n\t\t(uuid "%s")\n'
            '\t\t(property "Reference" "%s"\n\t\t\t(at %s %s 0)%s\n\t\t)\n\t\t(property "Value" "%s"\n\t\t\t(at %s %s 0)%s\n\t\t)\n\t\t(pin "1"\n\t\t\t(uuid "%s")\n\t\t)\n\t)'
            % (kind, x, y, rot, self.U(ref), ref, x, y, self._eff(None, True), kind, vx, vy, self._eff(), self.U(ref + ':1')))
        return sym

    def place(self, lib_id, ref, value, x, y, rot, footprint, fields, dnp=False):
        sym = self.symbol(lib_id)
        b = ['\t(symbol', '\t\t(lib_id "%s")' % lib_id, '\t\t(at %s %s %d)' % (x, y, rot), '\t\t(unit 1)', '\t\t(body_style 1)',
             '\t\t(exclude_from_sim no)', '\t\t(in_bom yes)', '\t\t(on_board yes)', '\t\t(in_pos_files yes)',
             '\t\t(dnp %s)' % ('yes' if dnp else 'no'), '\t\t(uuid "%s")' % self.U(ref)]
        x0, y0, x1, y1 = sym.bbox
        props = [('Reference', ref, x + x1 + 1.27, y - y1 - 1.27, 'left', False),
                 ('Value', value, x + x1 + 1.27, y - y1 + 1.27, 'left', False),
                 ('Footprint', footprint, x, y, None, True), ('Datasheet', '', x, y, None, True),
                 ('Description', '', x, y, None, True)] + [(k, v, x, y, None, True) for k, v in fields if v]
        for n, v, px, py, j, h in props:
            b.append('\t\t(property "%s" "%s"\n\t\t\t(at %s %s 0)%s\n\t\t)' % (n, v.replace('"', "'"), round(px, 2), round(py, 2), self._eff(j, h)))
        for n in sym.pins:
            b.append('\t\t(pin "%s"\n\t\t\t(uuid "%s")\n\t\t)' % (n, self.U(ref + '.' + n)))
        b.append('\t)')
        self.items.append('\n'.join(b))
        return sym

    def pin_xy(self, x, y, rot, pin):
        px, py, pa = pin[0], pin[1], pin[2]
        r = math.radians(rot)
        rx, ry = px * math.cos(r) - py * math.sin(r), px * math.sin(r) + py * math.cos(r)
        ang = (pa + rot) % 360
        out = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[ang]
        return round(x + rx, 2), round(y - ry, 2), out

    def hook(self, x, y, rot, pin, net, stub=STUB):
        px, py, out = self.pin_xy(x, y, rot, pin)
        tag = net if net is not None else 'NC@%s,%s' % (px, py)
        self.geo.append(('pin', tag, px, py, px, py))
        if net is None:
            self.noconnect(px, py)
            return
        if net not in POWER:
            stub = STUB
        ex, ey = round(px + out[0] * stub, 2), round(py + out[1] * stub, 2)
        self.wire(px, py, ex, ey)
        self.geo.append(('wire', net, px, py, ex, ey))
        if net in POWER:
            self.power(net, ex, ey, out)         # turned to point away from the part, so it never meets a neighbour
        else:
            self.label(net, ex, ey, out)

    def check(self):
        """Fail if geometry of two different nets touches: KiCad would join them."""
        def on(seg, x, y):
            _, _, x1, y1, x2, y2 = seg
            if x1 == x2 == x and min(y1, y2) <= y <= max(y1, y2):
                return True
            return y1 == y2 == y and min(x1, x2) <= x <= max(x1, x2)
        points = [(g[1], g[2], g[3]) for g in self.geo] + [(g[1], g[4], g[5]) for g in self.geo]
        wires = [g for g in self.geo if g[0] == 'wire']
        bad = set()
        for net, x, y in points:
            for w in wires:
                if w[1] != net and on(w, x, y):
                    bad.add((min(net, w[1]), max(net, w[1]), x, y))
        if bad:
            raise SystemExit('nets touch: ' + '; '.join('%s/%s at %s,%s' % b for b in sorted(bad)[:12]))

    # ---- output
    def save(self, path, paper=('A3', 420, 297)):
        self.geo = []
        x, y, row_h = 20.32, 30.48, 0
        width = paper[1] - 20
        for b in self.blocks:
            w, h = b.size()
            if x + w > width and x > 20.32:
                x, y, row_h = 20.32, y + row_h + 12.7, 0
            b.draw(snap(x), snap(y))
            x += w + 12.7
            row_h = max(row_h, h)
        if y + row_h > paper[2] - 20:
            paper = ('A2', 594, 420) if y + row_h < 400 else ('A1', 841, 594)
        self.check()
        libs = '\n'.join('\t' + '\n\t'.join(s.body.split('\n')) for s in self.symbols.values())
        head = ('(kicad_sch\n\t(version 20260306)\n\t(generator "eeschema")\n\t(generator_version "10.0")\n\t(uuid "%s")\n\t(paper "%s")\n'
                '\t(title_block\n\t\t(title "%s")\n\t\t(date "%s")\n\t\t(rev "%s")\n%s\t)\n\t(lib_symbols\n%s\n\t)\n'
                % (self.U('sheet'), paper[0], self.title, self.date, self.rev,
                   ''.join('\t\t(comment %d "%s")\n' % (i + 1, c) for i, c in enumerate(self.comments)), libs))
        open(path, 'w').write(head + '\n'.join(self.items) + '\n)\n')


class Block:
    def __init__(self, sheet, title, note):
        self.sheet, self.title, self.note = sheet, title, note
        self.parts = []

    def part(self, lib_id, ref, value, footprint, nets, rot=0, mpn='', manufacturer='', digikey='', notes='', dnp=False):
        sym = self.sheet.symbol(lib_id)
        missing = set(sym.pins) - set(nets)
        unknown = set(nets) - set(sym.pins)
        if missing or unknown:
            raise ValueError('%s: pins without a net %s, nets for unknown pins %s' % (ref, sorted(missing), sorted(unknown)))
        self.parts.append((lib_id, ref, value, footprint, nets, rot,
                           [('Manufacturer', manufacturer), ('MPN', mpn), ('DigiKey', digikey), ('Notes', notes)], dnp))

    def _extent(self, sym, rot):
        """Width and height a part needs, including its stubs and labels."""
        x0, y0, x1, y1 = sym.bbox
        if rot in (90, 270):
            x0, y0, x1, y1 = -y1, x0, -y0, x1
        return x0, y0, x1, y1

    def size(self):
        w, h = 0, 0
        for lib_id, ref, value, fp, nets, rot, fields, dnp in self.parts:
            x0, y0, x1, y1 = self._extent(self.sheet.symbol(lib_id), rot)
            label = max([len(n or '') for n in nets.values()] + [4]) * 1.0 + 5
            w += (x1 - x0) + 2 * label + 5.08
            h = max(h, (y1 - y0) + 2 * label)
        lines = 1 + self.note.count('\n') + (1 if self.note else 0)
        return w, h + 6 + lines * 2.54

    def draw(self, x, y):
        s = self.sheet
        lines = self.note.split('\n') if self.note else []
        s.text(self.title, x, y, 1.8)
        for i, ln in enumerate(lines):
            s.text(ln, x, round(y + 3.81 + i * 2.54, 2))
        top = y + 6 + (len(lines) + 1) * 2.54
        cx = x
        for lib_id, ref, value, fp, nets, rot, fields, dnp in self.parts:
            sym = s.symbol(lib_id)
            x0, y0, x1, y1 = self._extent(sym, rot)
            label = max([len(n or '') for n in nets.values()] + [4]) * 1.0 + 5
            px = snap(cx + label - x0)
            py = snap(top + label + y1)
            s.place(lib_id, ref, value, px, py, rot, fp, fields, dnp)
            # supply symbols on neighbouring pins of one side alternate between a short and a long
            # stub, so their names do not print over each other
            sides = {}
            for num, pin in sym.pins.items():
                if nets[num] in POWER:
                    _, _, out = s.pin_xy(px, py, rot, pin)
                    sides.setdefault(out, []).append(num)
            stagger = {}
            for out, nums in sides.items():
                key = (lambda n: s.pin_xy(px, py, rot, sym.pins[n])[0]) if out[1] else (lambda n: s.pin_xy(px, py, rot, sym.pins[n])[1])
                for i, n in enumerate(sorted(nums, key=key)):
                    stagger[n] = STUB + (7.62 if i % 2 else 0)
            for num, pin in sym.pins.items():
                s.hook(px, py, rot, pin, nets[num], stagger.get(num, STUB))
            cx = px + x1 + label + 5.08
