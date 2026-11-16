# Board rules for this project. pcbnew rewrites the project file whenever a board is saved,
# so this runs last.
import json, copy, os
P = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'xiao-esp32s3-can-sniffer.kicad_pro')
p = json.load(open(P))
p['board']['design_settings']['rules']['min_track_width'] = 0.15   # the router necks to 0.19 mm at fine-pitch pins
ns = p['net_settings']
d = [c for c in ns['classes'] if c['name'] == 'Default'][0]
pw = copy.deepcopy(d); pw.update(name='Power', track_width=0.5, priority=0)
ns['classes'] = [d, pw]
ns['netclass_patterns'] = [{"netclass": "Power", "pattern": n} for n in ("+12V", "/VOBD", "VIN", "+5V", "VSYS", "GND", "/SW")]
json.dump(p, open(P, 'w'), indent=2)
print("project rules applied")
