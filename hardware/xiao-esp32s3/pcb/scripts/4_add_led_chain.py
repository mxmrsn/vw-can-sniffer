import re, uuid, math
SCH = 'xiao-esp32s3-can-sniffer.kicad_sch'
NS = uuid.UUID('6ba7b811-9dad-11d1-80b4-00c04fd430c8')
def U(seed): return str(uuid.uuid5(NS, 'xiao-leds:' + seed))
LIB = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/'
s = open(SCH).read()

def block(text, start):
    depth, k = 0, start
    while k < len(text):
        c = text[k]
        if c == '"':
            k += 1
            while text[k] != '"' or text[k-1] == '\\': k += 1
        elif c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: return text[start:k+1]
        k += 1

def libsym(lib, name, as_name=None, base=None):
    """A library symbol, flattened if it extends another, renamed lib:name for embedding."""
    t = open(LIB + lib + '.kicad_sym').read()
    blk = block(t, t.index('(symbol "%s"' % (base or name)))
    if base:
        blk = blk.replace('(symbol "%s"' % base, '(symbol "%s"' % name, 1)
        blk = blk.replace('"%s_' % base, '"%s_' % name)
        blk = blk.replace('(property "Value" "%s"' % base, '(property "Value" "%s"' % name)
    full = (as_name or lib) + ':' + name
    blk = blk.replace('(symbol "%s"' % name, '(symbol "%s"' % full, 1)
    return full, blk

def pins_of(blk):
    out = {}
    for m in re.finditer(r'\(pin (\w+) \w+\s*\(at ([-\d.]+) ([-\d.]+) (\d+)\)[\s\S]*?\(number "([^"]+)"', blk):
        out[m.group(5)] = (float(m.group(2)), float(m.group(3)), int(m.group(4)))
    return out

SYMS = {}
for lib, name, base in (('LED', 'SK6812MINI', None), ('74xGxx', '74AHCT1G125', '74LVC1G125'),
                        ('Connector_Generic', 'Conn_01x03', None)):
    full, blk = libsym(lib, name, base=base)
    SYMS[name] = (full, pins_of(blk))
    if '(symbol "%s"' % full not in s:
        body = "\n".join(('\t' + ln) if ln.strip() else ln for ln in blk.split('\n'))
        anchor = s.index('\t\t(symbol "Device:C"')
        s = s[:anchor] + '\t' + body + '\n' + s[anchor:]
DEV_C = ('Device:C', {'1': (0, 3.81, 270), '2': (0, -3.81, 90)})
DEV_R = ('Device:R', {'1': (0, 3.81, 270), '2': (0, -3.81, 90)})

add = []
def eff(just=None, hide=False):
    o = ('\n\t\t\t(hide yes)' if hide else '') + '\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)'
    if just: o += f'\n\t\t\t\t(justify {just})'
    return o + '\n\t\t\t)'

def place(libid, ref, value, x, y, rot, footprint, extra, pins):
    props = [("Reference", ref, x + 2.54, y - 2.54, "left", False), ("Value", value, x + 2.54, y + 2.54, "left", False),
             ("Footprint", footprint, x, y, None, True), ("Datasheet", "", x, y, None, True),
             ("Description", "", x, y, None, True)] + [(k, v, x, y, None, True) for k, v in extra]
    b = ['\t(symbol', f'\t\t(lib_id "{libid}")', f'\t\t(at {x} {y} {rot})', '\t\t(unit 1)', '\t\t(body_style 1)',
         '\t\t(exclude_from_sim no)', '\t\t(in_bom yes)', '\t\t(on_board yes)', '\t\t(in_pos_files yes)',
         '\t\t(dnp no)', f'\t\t(uuid "{U(ref)}")']
    for n, v, px, py, j, h in props:
        b.append(f'\t\t(property "{n}" "{v}"\n\t\t\t(at {round(px,2)} {round(py,2)} 0)' + eff(j, h) + '\n\t\t)')
    for n in pins:
        b.append(f'\t\t(pin "{n}"\n\t\t\t(uuid "{U(ref + "." + n)}")\n\t\t)')
    b.append('\t)')
    add.append("\n".join(b))

def pin_xy(x, y, rot, pin):
    px, py, pa = pin
    r = math.radians(rot)
    rx, ry = px * math.cos(r) - py * math.sin(r), px * math.sin(r) + py * math.cos(r)
    ang = (pa + rot) % 360
    out = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[ang]   # schematic coords, y down
    return round(x + rx, 2), round(y - ry, 2), out

def wire(x1, y1, x2, y2):
    add.append(f'\t(wire\n\t\t(pts\n\t\t\t(xy {x1} {y1}) (xy {x2} {y2})\n\t\t)\n\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "{U("w:%s,%s,%s,%s" % (x1, y1, x2, y2))}")\n\t)')
def junction(x, y):
    add.append(f'\t(junction\n\t\t(at {x} {y})\n\t\t(diameter 0)\n\t\t(color 0 0 0 0)\n\t\t(uuid "{U("j:%s,%s" % (x, y))}")\n\t)')
def noconnect(x, y):
    add.append(f'\t(no_connect\n\t\t(at {x} {y})\n\t\t(uuid "{U("nc:%s,%s" % (x, y))}")\n\t)')
def label(name, x, y, out):
    rot, just = {(-1, 0): (180, 'right'), (1, 0): (0, 'left'), (0, 1): (270, 'right'), (0, -1): (90, 'left')}[out]
    add.append(f'\t(global_label "{name}"\n\t\t(shape bidirectional)\n\t\t(at {x} {y} {rot})\n\t\t(fields_autoplaced yes)\n'
               f'\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify {just})\n\t\t)\n'
               f'\t\t(uuid "{U("l:%s:%s,%s" % (name, x, y))}")\n\t\t(property "Intersheetrefs" "${{INTERSHEET_REFS}}"\n'
               f'\t\t\t(at {x} {y} 0)\n\t\t\t(hide yes)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n'
               '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)')
PWRN = [60]
def power(kind, x, y):
    PWRN[0] += 1
    ref = '#PWR%03d' % PWRN[0]
    vy = y + 3.81 if kind == 'GND' else y - 3.81
    add.append(f'\t(symbol\n\t\t(lib_id "power:{kind}")\n\t\t(at {x} {y} 0)\n\t\t(unit 1)\n\t\t(body_style 1)\n'
               '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(in_pos_files yes)\n\t\t(dnp no)\n'
               f'\t\t(uuid "{U(ref)}")\n\t\t(property "Reference" "{ref}"\n\t\t\t(at {x} {y} 0)' + eff(None, True) + '\n\t\t)\n'
               f'\t\t(property "Value" "{kind}"\n\t\t\t(at {x} {round(vy,2)} 0)' + eff() + '\n\t\t)\n'
               f'\t\t(pin "1"\n\t\t\t(uuid "{U(ref + ":pin")}")\n\t\t)\n\t)')

def hook(x, y, rot, pin, what):
    """Short stub out of a pin, ending in a label, a power symbol or a no-connect."""
    px, py, out = pin_xy(x, y, rot, pin)
    if what == 'NC':
        noconnect(px, py); return
    ex, ey = round(px + out[0] * 2.54, 2), round(py + out[1] * 2.54, 2)
    wire(px, py, ex, ey)
    if what in ('GND', '+3V3'):
        if what == 'GND' and out != (0, 1):          # keep ground symbols pointing down
            wire(ex, ey, ex, round(ey + 2.54, 2)); ey = round(ey + 2.54, 2)
        if what == '+3V3' and out != (0, -1):
            wire(ex, ey, ex, round(ey - 2.54, 2)); ey = round(ey - 2.54, 2)
        power(what, ex, ey)
    else:
        label(what, ex, ey, out)
    return px, py

# ---- buffer: 3.3 V data in, data out at the LEDs' supply, so their input threshold is met
BUF, bp = SYMS['74AHCT1G125']
UX, UY = 45.72, 88.9
place(BUF, 'U5', '74AHCT1G125', UX, UY, 0, 'Package_TO_SOT_SMD:SOT-23-5',
      [('Manufacturer', 'Texas Instruments'), ('MPN', 'SN74AHCT1G125DBVR'), ('DigiKey', '296-1180-1-ND'),
       ('Notes', 'Lifts the 3.3 V LED data line to the LEDs supply level (their input threshold is 0.7 x VDD)')], bp)
hook(UX, UY, 0, bp['2'], 'LED_DATA')
hook(UX, UY, 0, bp['5'], 'VSYS')
hook(UX, UY, 0, bp['3'], 'GND')
hook(UX, UY, 0, bp['1'], 'GND')                    # output always enabled

place(*DEV_C[:1], 'C16', '100n', 30.48, 80.01, 0, 'Capacitor_SMD:C_0603_1608Metric',
      [('Manufacturer', 'YAGEO'), ('MPN', 'CC0603KRX7R9BB104'), ('DigiKey', '311-1344-1-ND')], DEV_C[1])
hook(30.48, 80.01, 0, DEV_C[1]['1'], 'VSYS')
hook(30.48, 80.01, 0, DEV_C[1]['2'], 'GND')

# R3 in line with the buffer's output, horizontal
yx, yy, _ = pin_xy(UX, UY, 0, bp['4'])
place(*DEV_R[:1], 'R3', '330', 68.58, UY, 90, 'Resistor_SMD:R_0603_1608Metric',
      [('Manufacturer', 'Vishay Dale'), ('MPN', 'CRCW0603330RFKEA'), ('DigiKey', '541-330HCT-ND'),
       ('Notes', 'Series resistor on the LED data line, damps ringing')], DEV_R[1])
r1x, r1y, _ = pin_xy(68.58, UY, 90, DEV_R[1]['1'])
r2x, r2y, _ = pin_xy(68.58, UY, 90, DEV_R[1]['2'])
left, right = sorted([(r1x, r1y), (r2x, r2y)])
wire(yx, yy, left[0], left[1])
rpin = '2' if (r2x, r2y) == right else '1'
hook(68.58, UY, 90, DEV_R[1][rpin], 'LED_DIN')

# ---- the chain: status (RGB), then CAN, Bluetooth, logging
LED, lp = SYMS['SK6812MINI']
LY, RAIL_V, RAIL_G = 106.68, 96.52, 116.84
XS = [50.8, 81.28, 111.76, 142.24]
ROLE = ['status', 'CAN', 'Bluetooth', 'logging']
taps_v, taps_g = [], []
for i, x in enumerate(XS):
    ref = 'D%d' % (i + 1)
    place(LED, ref, 'SK6812MINI', x, LY, 0, 'LED_SMD:LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm',
          [('Manufacturer', 'Opsco / Worldsemi'), ('MPN', 'SK6812MINI (to verify)'),
           ('Notes', 'Addressable RGB LED %d of 4: %s. Exact part and stock to be verified before ordering' % (i + 1, ROLE[i]))], lp)
    vx, vy, _ = pin_xy(x, LY, 0, lp['4']); wire(vx, vy, vx, RAIL_V); taps_v.append(vx)
    gx, gy, _ = pin_xy(x, LY, 0, lp['2']); wire(gx, gy, gx, RAIL_G); taps_g.append(gx)
    cx = 157.48 + 5.08 * i          # a bank at the end of the rails; on the board each sits by its LED
    cref = 'C%d' % (12 + i)
    place(*DEV_C[:1], cref, '100n', cx, LY, 0, 'Capacitor_SMD:C_0603_1608Metric',
          [('Manufacturer', 'YAGEO'), ('MPN', 'CC0603KRX7R9BB104'), ('DigiKey', '311-1344-1-ND')], DEV_C[1])
    c1 = pin_xy(cx, LY, 0, DEV_C[1]['1']); wire(c1[0], c1[1], c1[0], RAIL_V); taps_v.append(c1[0])
    c2 = pin_xy(cx, LY, 0, DEV_C[1]['2']); wire(c2[0], c2[1], c2[0], RAIL_G); taps_g.append(c2[0])
    din = pin_xy(x, LY, 0, lp['3'])
    dout = pin_xy(x, LY, 0, lp['1'])
    if i == 0:
        hook(x, LY, 0, lp['3'], 'LED_DIN')
    if i + 1 < len(XS):
        nxt = pin_xy(XS[i + 1], LY, 0, lp['3'])
        wire(dout[0], dout[1], nxt[0], nxt[1])
    else:
        noconnect(dout[0], dout[1])

# supply rails across the chain, split at every tap so each joint is an endpoint
for rail, taps, what in ((RAIL_V, taps_v, 'VSYS'), (RAIL_G, taps_g, 'GND')):
    xs = sorted(set([XS[0] - 7.62] + taps))
    for a, b2 in zip(xs, xs[1:]):
        wire(a, rail, b2, rail)
    for t in xs[1:-1]:
        junction(t, rail)
    if what == 'VSYS':
        label('VSYS', xs[0], rail, (-1, 0))
    else:
        wire(xs[0], rail, xs[0], rail + 2.54); power('GND', xs[0], rail + 2.54)

# ---- spare pad on a header: GPS pulse per second, or a camera sync line, later
CON, cp = SYMS['Conn_01x03']
JX, JY = 195.58, 107.95
place(CON, 'J3', 'SPARE', JX, JY, 180, 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical',
      [('Manufacturer', 'Sullins'), ('MPN', 'PRPC003SAAN-RC'), ('DigiKey', 'S1011EC-03-ND'),
       ('Notes', 'Spare GPIO (D1) for a GPS pulse-per-second input or a camera sync line')], cp)
# rotated, pin 1 sits at the bottom: ground there, supply at the top, so nothing meets
hook(JX, JY, 180, cp['1'], 'GND')
hook(JX, JY, 180, cp['2'], 'SPARE_IO')
hook(JX, JY, 180, cp['3'], '+3V3')

add.append('\t(text "STATUS LEDS: one addressable chain on one GPIO (D0).\\nD1 RGB status, D2 CAN, D3 Bluetooth, D4 logging.\\nU5 lifts the 3.3 V data to the LED supply level.\\nC12-C15: one beside each LED on the board."\n'
           '\t\t(exclude_from_sim no)\n\t\t(at 88.9 80.01 0)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n'
           f'\t\t\t(justify left)\n\t\t)\n\t\t(uuid "{U("txt:leds")}")\n\t)')
add.append('\t(text "SPARE I/O: D1 on a header, for a GPS pulse-per-second\\ninput or a camera trigger, timestamped like CAN frames."\n'
           '\t\t(exclude_from_sim no)\n\t\t(at 210.82 105.41 0)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n'
           f'\t\t\t(justify left)\n\t\t)\n\t\t(uuid "{U("txt:spare")}")\n\t)')

s = s[:s.rindex('\n)')] + "\n" + "\n".join(add) + s[s.rindex('\n)'):]
open(SCH, 'w').write(s)
print("added", len(add), "blocks")
