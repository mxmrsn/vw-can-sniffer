# A ground pad whose thermal spokes land on a small island of pour gets a solid
# connection instead. Every such pad is also tied in by a trace (DRC checks that),
# so this only tidies the pour; where the islands fall changes with each routing run.
# Run with KiCad's Python after a DRC has written build/drc.rpt.
import re
import pcbnew

report = open('build/drc.rpt').read()
starved = set()
for block in re.split(r'\n(?=\[)', report):
    if block.startswith('[starved_thermal]'):
        m = re.search(r'(?:PTH )?[Pp]ad (\S+) \[GND\] of (\S+)', block)
        if m:
            starved.add((m.group(2), m.group(1)))

b = pcbnew.LoadBoard('xiao-esp32s3-can-sniffer.kicad_pcb')
for f in list(b.GetFootprints()):
    for p in f.Pads():
        if (f.GetReference(), p.GetNumber()) in starved:
            p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
            print("solid ground connection:", f.GetReference(), "pad", p.GetNumber())
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard('xiao-esp32s3-can-sniffer.kicad_pcb', b)
