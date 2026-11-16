#!/bin/bash
# Rebuild the v0.3.0 board from scratch: schematic from the Pico board's, then
# placement, autorouting (Freerouting), ground pour and DRC.
#
#   hardware/xiao-esp32s3/pcb/scripts/build.sh            # everything
#   hardware/xiao-esp32s3/pcb/scripts/build.sh --board    # keep the schematic, redo the board
#
# Needs KiCad 10, python3 (3.10+ for the schematic steps), and Freerouting with a
# Java runtime new enough for it. Override JAVA / FREEROUTING_JAR / PY if needed.
cd "$(dirname "$0")/.."
KICAD=/Applications/KiCad/KiCad.app/Contents
CLI=$KICAD/MacOS/kicad-cli
KPY=$KICAD/Frameworks/Python.framework/Versions/Current/bin/python3   # KiCad's own Python, for pcbnew
PY=${PY:-python3}
JAVA=${JAVA:-/opt/homebrew/opt/openjdk/bin/java}
FREEROUTING_JAR=${FREEROUTING_JAR:-$HOME/.claude/tools/freerouting/dl/freerouting-2.5.0.jar}
mkdir -p build
quiet() { grep -v "memory leak\|stdpbase\|Fontconfig" || true; }

if [ "$1" != "--board" ]; then
    cp ../../pico2w/pcb/pico2w-can-sniffer.kicad_sch xiao-esp32s3-can-sniffer.kicad_sch
    $PY scripts/1_strip_pico.py --write >/dev/null
    $PY scripts/2_add_module.py >/dev/null
    $PY - <<'PY'
import re
for p in ('lib/xiao.kicad_sym', 'xiao-esp32s3-can-sniffer.kicad_sch'):
    s = open(p).read()
    # the module drives ground, as KiCad's own module symbols do
    s = re.sub(r'\(pin power_in line\n(\t+)\(at 20\.32 2\.54 180\)', r'(pin power_out line\n\1(at 20.32 2.54 180)', s)
    open(p, 'w').write(s)
PY
    $PY scripts/3_add_silent_pullup.py >/dev/null
    $PY scripts/4_add_led_chain.py
    $PY scripts/5_notes.py
    $CLI sch erc xiao-esp32s3-can-sniffer.kicad_sch -o build/erc.rpt --severity-error 2>&1 | quiet | grep -i violation
fi

$CLI sch export netlist xiao-esp32s3-can-sniffer.kicad_sch -o build/netlist.net 2>&1 | quiet >/dev/null
NETLIST=build/netlist.net $KPY scripts/place.py 2>&1 | quiet
$KPY - <<'PY' 2>&1 | quiet
import pcbnew
b = pcbnew.LoadBoard('xiao-esp32s3-can-sniffer.kicad_pcb')
pcbnew.ExportSpecctraDSN(b, 'build/board.dsn')
PY
$PY scripts/dsn_classes.py build/board.dsn
rm -f build/board.ses
"$JAVA" -jar "$FREEROUTING_JAR" --gui.enabled=false -de build/board.dsn -do build/board.ses -mp 120 -mt 4 > build/freerouting.log 2>&1
grep -E "Auto-routing stage completed" build/freerouting.log | sed 's/.*INFO *\[[^]]*\] //'
$KPY - <<'PY' 2>&1 | quiet
import pcbnew
b = pcbnew.LoadBoard('xiao-esp32s3-can-sniffer.kicad_pcb')
pcbnew.ImportSpecctraSES(b, 'build/board.ses')
# the router leaves room for only one thermal spoke at these; they are also tied in by a trace
for f in list(b.GetFootprints()):
    if f.GetReference() in ('C12', 'C13', 'C14', 'C15', 'C16', 'U5', 'J3'):
        for p in f.Pads():
            if p.GetNetname() == 'GND':
                p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard('xiao-esp32s3-can-sniffer.kicad_pcb', b)
PY
$PY scripts/rules.py     # last: saving a board rewrites the project file
drc() { $CLI pcb drc xiao-esp32s3-can-sniffer.kicad_pcb -o build/drc.rpt --severity-error 2>&1 | quiet >/dev/null; }
drc
for pass in 1 2 3; do
    grep -q "starved_thermal" build/drc.rpt || break
    $KPY scripts/solid_starved.py 2>&1 | quiet
    $PY scripts/rules.py >/dev/null
    drc
done
grep "Found" build/drc.rpt

# fabrication outputs: gerbers and drill (zipped for upload), placement file, STEP for the enclosure
B=xiao-esp32s3-can-sniffer
rm -rf fab/gerbers && mkdir -p fab/gerbers
$CLI pcb export gerbers -l F.Cu,B.Cu,F.Mask,B.Mask,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,Edge.Cuts -o fab/gerbers/ $B.kicad_pcb 2>&1 | quiet >/dev/null
$CLI pcb export drill --excellon-separate-th -o fab/gerbers/ $B.kicad_pcb 2>&1 | quiet >/dev/null
rm -f fab/$B-v0.3.0-gerbers.zip && (cd fab/gerbers && zip -q ../$B-v0.3.0-gerbers.zip *)
$CLI pcb export pos --format csv --units mm --side both -o fab/$B-pos.csv $B.kicad_pcb 2>&1 | quiet >/dev/null
$CLI pcb export step --subst-models --force -o fab/$B.step $B.kicad_pcb 2>&1 | grep "Could not add" || true
ls fab/gerbers | wc -l | xargs echo "fab files:"
