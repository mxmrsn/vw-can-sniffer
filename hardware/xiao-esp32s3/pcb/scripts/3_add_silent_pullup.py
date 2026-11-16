import re, uuid
SCH='xiao-esp32s3-can-sniffer.kicad_sch'
NS=uuid.UUID('6ba7b811-9dad-11d1-80b4-00c04fd430c8')
def U(seed): return str(uuid.uuid5(NS,'xiao-esp32s3:'+seed))
s=open(SCH).read()

X, Y = 60.96, 200.66          # clear space below the transceiver block
def eff(just=None, hide=False):
    out = ''
    if hide: out += '\n\t\t\t(hide yes)'
    out += '\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)'
    if just: out += f'\n\t\t\t\t(justify {just})'
    return out + '\n\t\t\t)'

props = [("Reference","R2",X+3.81,Y-1.27,"left",False), ("Value","100k",X+3.81,Y+1.27,"left",False),
         ("Footprint","Resistor_SMD:R_0603_1608Metric",X,Y,None,True), ("Datasheet","",X,Y,None,True),
         ("Description","",X,Y,None,True), ("Manufacturer","Vishay Dale",X,Y,None,True),
         ("MPN","CRCW0603100KFKEA",X,Y,None,True), ("DigiKey","541-100KHCT-ND",X,Y,None,True),
         ("Notes","Holds the transceiver in listen-only until firmware drives it low",X,Y,None,True)]
blk=['\t(symbol','\t\t(lib_id "Device:R")',f'\t\t(at {X} {Y} 0)','\t\t(unit 1)','\t\t(body_style 1)',
     '\t\t(exclude_from_sim no)','\t\t(in_bom yes)','\t\t(on_board yes)','\t\t(in_pos_files yes)','\t\t(dnp no)',
     f'\t\t(uuid "{U("R2")}")']
for n,v,px,py,j,h in props:
    blk.append(f'\t\t(property "{n}" "{v}"\n\t\t\t(at {px} {py} 0)' + eff(j,h) + '\n\t\t)')
for n in (1,2):
    blk.append(f'\t\t(pin "{n}"\n\t\t\t(uuid "{U("R2.pin%d"%n)}")\n\t\t)')
blk.append('\t)')
add=["\n".join(blk)]

def wire(x1,y1,x2,y2):
    return (f'\t(wire\n\t\t(pts\n\t\t\t(xy {x1} {y1}) (xy {x2} {y2})\n\t\t)\n'
            '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n'
            f'\t\t(uuid "{U("w2:%s,%s-%s,%s"%(x1,y1,x2,y2))}")\n\t)')
def label(name,x,y,rot,just):
    return (f'\t(global_label "{name}"\n\t\t(shape bidirectional)\n\t\t(at {x} {y} {rot})\n'
            '\t\t(fields_autoplaced yes)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n'
            f'\t\t\t(justify {just})\n\t\t)\n\t\t(uuid "{U("lbl2:%s:%s,%s"%(name,x,y))}")\n'
            '\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}"\n'
            f'\t\t\t(at {x} {y} 0)\n\t\t\t(hide yes)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n'
            '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)')
def power(kind, ref, x, y):
    return (f'\t(symbol\n\t\t(lib_id "power:{kind}")\n\t\t(at {x} {y} 0)\n\t\t(unit 1)\n\t\t(body_style 1)\n'
            '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(in_pos_files yes)\n\t\t(dnp no)\n'
            f'\t\t(uuid "{U("pwr2:%s"%ref)}")\n'
            f'\t\t(property "Reference" "{ref}"\n\t\t\t(at {x} {y} 0)' + eff(None, True) + '\n\t\t)\n'
            f'\t\t(property "Value" "{kind}"\n\t\t\t(at {x} {y-3.81} 0)' + eff() + '\n\t\t)\n'
            f'\t\t(pin "1"\n\t\t\t(uuid "{U("pwr2:%s:pin"%ref)}")\n\t\t)\n\t)')

add += [wire(X, Y-3.81, X, Y-7.62), power("+3V3", "#PWR048", X, Y-7.62)]
add += [wire(X, Y+3.81, X, Y+7.62), label("CAN_SILENT", X, Y+7.62, 270, "right")]
add.append(f'\t(text "R2 holds the transceiver in listen-only (S high) from power-up\\nuntil firmware drives CAN_SILENT low. The ESP32-S3 pins\\nfloat during reset, so without it the bus could be driven."\n\t\t(exclude_from_sim no)\n'
           f'\t\t(at {X+5.08} {Y+13.97} 0)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n'
           f'\t\t\t(justify left)\n\t\t)\n\t\t(uuid "{U("txt:r2")}")\n\t)')

s = s[:s.rindex('\n)')] + "\n" + "\n".join(add) + s[s.rindex('\n)'):]
open(SCH,'w').write(s)
print("added", len(add))
