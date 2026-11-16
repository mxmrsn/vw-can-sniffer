# Board rules for a dongle board. pcbnew rewrites the project file whenever a board is
# saved, so this runs after every save.   python3 rules.py <project.kicad_pro>
import copy, json, sys
P = sys.argv[1]
p = json.load(open(P))
r = p['board']['design_settings']['rules']
r['min_track_width'] = 0.15     # the router necks to 0.19 mm at fine-pitch pins
r['min_hole_clearance'] = 0.15  # USB-C NPTH pegs sit 0.19 mm from the shell pads
ns = p['net_settings']
d = [c for c in ns['classes'] if c['name'] == 'Default'][0]
d['clearance'] = 0.15           # LGA-14 at 0.5 mm pitch leaves 0.15 mm between pads
pw = copy.deepcopy(d); pw.update(name='Power', track_width=0.5, priority=0)
ns['classes'] = [d, pw]
ns['netclass_patterns'] = [{"netclass": "Power", "pattern": n} for n in
                           ("/VOBD", "/VOBD_F", "/VBAT", "+5V", "/SW", "GND", "+3V3", "/VBUS", "/V5_SYS")]
json.dump(p, open(P, 'w'), indent=2)
