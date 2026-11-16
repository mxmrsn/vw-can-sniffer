# Prepare Freerouting's input: ground is routed like any other net (then poured
# over), the power nets get 0.5 mm tracks, and tracks keep 0.16 mm from copper
# (0.01 mm over the board rule, so rounding never trips DRC).
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'\n\s*\(plane GND \(polygon [^\n]*\n[^\n]*\)\)', '', s)
i = s.index("(class kicad_default"); j = s.index("(circuit", i)
names = s[i + len("(class kicad_default"):j].split()
power = [n for n in names if n.strip('"').lstrip("/") in ("VOBD", "VOBD_F", "VBAT", "+5V", "SW", "GND", "+3V3", "VBUS", "V5_SYS")]
sig = [n for n in names if n not in power]
end = s.index("\n    )\n", j) + len("\n    )\n")
def cls(name, nets, w):
    return ("(class %s %s\n      (circuit\n        (use_via \"Via[0-1]_800:400_um\")\n      )\n"
            "      (rule\n        (width %d)\n        (clearance 160)\n      )\n    )\n    " % (name, " ".join(nets), w))
s = s[:i] + cls("kicad_default", sig, 250) + cls("power", power, 500).rstrip() + "\n" + s[end:]
open(p, 'w').write(s)
