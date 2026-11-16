# Prepare Freerouting's input: ground is routed like any other net (then poured
# over), and the power nets get 0.5 mm tracks.
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'\n\s*\(plane GND \(polygon [^\n]*\n[^\n]*\)\)', '', s)
i = s.index("(class kicad_default"); j = s.index("(circuit", i)
names = s[i + len("(class kicad_default"):j].split()
power = ["+12V", "/VOBD", "VIN", "+5V", "VSYS", "/SW", "+3V3", "GND"]
sig = [n for n in names if n not in power]
end = s.index("\n    )\n", j) + len("\n    )\n")
def cls(name, nets, w):
    return ("(class %s %s\n      (circuit\n        (use_via \"Via[0-1]_800:400_um\")\n      )\n"
            "      (rule\n        (width %d)\n        (clearance 200)\n      )\n    )\n    " % (name, " ".join(nets), w))
s = s[:i] + cls("kicad_default", sig, 250) + cls("power", power, 500).rstrip() + "\n" + s[end:]
open(p, 'w').write(s)
