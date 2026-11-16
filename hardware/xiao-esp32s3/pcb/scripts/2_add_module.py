import re, uuid

SCH = 'xiao-esp32s3-can-sniffer.kicad_sch'
NS = uuid.UUID('6ba7b811-9dad-11d1-80b4-00c04fd430c8')
def U(seed):
    return str(uuid.uuid5(NS, 'xiao-esp32s3:' + seed))

s = open(SCH).read()

# ---- embed the module's symbol definition
sym = open('lib/xiao.kicad_sym').read()
start = sym.index('(symbol "Seeed_XIAO_ESP32S3"')
depth, k = 0, start
while k < len(sym):
    c = sym[k]
    if c == '"':
        k += 1
        while sym[k] != '"' or sym[k-1] == '\\': k += 1
    elif c == '(': depth += 1
    elif c == ')':
        depth -= 1
        if depth == 0: break
    k += 1
body = sym[start:k+1].replace('(symbol "Seeed_XIAO_ESP32S3"', '(symbol "xiao:Seeed_XIAO_ESP32S3"', 1)
body = "\n".join(('\t' + ln) if ln.strip() else ln for ln in body.split('\n'))
anchor = s.index('\t\t(symbol "Device:C"')
s = s[:anchor] + '\t' + body + '\n' + s[anchor:]

# ---- the module itself
X, Y = 128.27, 60.96
props = [
    ("Reference", "U1", X + 7.62, Y - 20.32, "left"),
    ("Value", "XIAO ESP32-S3", X + 7.62, Y - 17.78, "left"),
    ("Footprint", "xiao:Seeed_XIAO_THT", X, Y, None),
    ("Datasheet", "https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/", X, Y, None),
    ("Description", "", X, Y, None),
    ("Manufacturer", "Seeed Studio", X, Y, None),
    ("MPN", "113991114", X, Y, None),
    ("Notes", "Seeed XIAO ESP32-S3 (not Sense). Socket on 2x 1x7 2.54mm headers, or solder the module flat.", X, Y, None),
]
blk = ['\t(symbol', '\t\t(lib_id "xiao:Seeed_XIAO_ESP32S3")', f'\t\t(at {X} {Y} 0)', '\t\t(unit 1)',
       '\t\t(body_style 1)', '\t\t(exclude_from_sim no)', '\t\t(in_bom yes)', '\t\t(on_board yes)',
       '\t\t(in_pos_files yes)', '\t\t(dnp no)', f'\t\t(uuid "{U("U1")}")']
for name, val, px, py, just in props:
    blk.append(f'\t\t(property "{name}" "{val}"')
    blk.append(f'\t\t\t(at {px} {py} 0)')
    if just is None:
        blk.append('\t\t\t(hide yes)')
    blk.append('\t\t\t(show_name no)')
    blk.append('\t\t\t(do_not_autoplace no)')
    blk.append('\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)' +
               (f'\n\t\t\t\t(justify {just})' if just else '') + '\n\t\t\t)')
    blk.append('\t\t)')
for n in range(1, 15):
    blk.append(f'\t\t(pin "{n}"\n\t\t\t(uuid "{U("U1.pin%d" % n)}")\n\t\t)')
blk.append('\t)')
add = ["\n".join(blk)]

# ---- labels on the pins
def label(name, x, y, rot, just):
    return (f'\t(global_label "{name}"\n\t\t(shape bidirectional)\n\t\t(at {x} {y} {rot})\n'
            '\t\t(fields_autoplaced yes)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n'
            f'\t\t\t(justify {just})\n\t\t)\n\t\t(uuid "{U("label:%s:%s,%s" % (name, x, y))}")\n'
            '\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}"\n'
            f'\t\t\t(at {x} {y} 0)\n\t\t\t(hide yes)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n'
            '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)')

LEFT = [("LED_DATA", 53.34), ("SPARE_IO", 55.88), ("MODE_SELECT", 58.42), ("CAN_SILENT", 60.96),
        ("CTX", 63.5), ("CRX", 66.04), ("SD_CS", 68.58)]
RIGHT = [("VSYS", 53.34), ("OBD_SENSE", 60.96), ("SD_MISO", 63.5), ("SD_MOSI", 66.04), ("SD_SCK", 68.58)]
for name, y in LEFT:
    add.append(label(name, 107.95, y, 180, "right"))
for name, y in RIGHT:
    add.append(label(name, 148.59, y, 0, "left"))

# ---- power pins: 3V3 up and away, GND down and away, so nothing crosses
def wire(x1, y1, x2, y2):
    return (f'\t(wire\n\t\t(pts\n\t\t\t(xy {x1} {y1}) (xy {x2} {y2})\n\t\t)\n'
            '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n'
            f'\t\t(uuid "{U("wire:%s,%s-%s,%s" % (x1, y1, x2, y2))}")\n\t)')

def power(kind, ref, x, y):
    return (f'\t(symbol\n\t\t(lib_id "power:{kind}")\n\t\t(at {x} {y} 0)\n\t\t(unit 1)\n\t\t(body_style 1)\n'
            '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(in_pos_files yes)\n\t\t(dnp no)\n'
            f'\t\t(uuid "{U("pwr:%s" % ref)}")\n'
            f'\t\t(property "Reference" "{ref}"\n\t\t\t(at {x} {y} 0)\n\t\t\t(hide yes)\n\t\t\t(show_name no)\n'
            '\t\t\t(do_not_autoplace no)\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
            f'\t\t(property "Value" "{kind}"\n\t\t\t(at {x} {y + (-3.81 if kind != "GND" else 3.81)} 0)\n'
            '\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n'
            '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
            f'\t\t(pin "1"\n\t\t\t(uuid "{U("pwr:%s:pin" % ref)}")\n\t\t)\n\t)')

add += [wire(148.59, 55.88, 161.29, 55.88), wire(161.29, 55.88, 161.29, 48.26), power("+3V3", "#PWR031", 161.29, 48.26)]
add += [wire(148.59, 58.42, 152.4, 58.42), wire(152.4, 58.42, 152.4, 71.12), power("GND", "#PWR032", 152.4, 71.12)]

s = s[:s.rindex('\n)')] + "\n" + "\n".join(add) + s[s.rindex('\n)'):]

# ---- the blue LED now means "link": Bluetooth connected or logging
s = s.replace('(title "VW CAN Sniffer - Pico 2 W")', '(title "VW CAN Sniffer - XIAO ESP32-S3")')
s = s.replace('(comment 1 "RP2350 (Pico 2 W) carrier: TJA1051T/3 CAN, OBD / 2S LiPo / USB-C power")',
              '(comment 1 "Seeed XIAO ESP32-S3 carrier: TWAI CAN via TJA1051T/3, OBD or USB power, microSD logging")')
s = s.replace('(rev "0.2.0")', '(rev "0.3.0")')
open(SCH, 'w').write(s)
print("added", len(add), "blocks")
