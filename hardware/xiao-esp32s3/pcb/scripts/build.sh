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
    if f.GetReference() in ('C12', 'C13', 'C14', 'C15', 'C16', 'U5'):
        for p in f.Pads():
            if p.GetNetname() == 'GND':
                p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard('xiao-esp32s3-can-sniffer.kicad_pcb', b)
PY
$PY scripts/rules.py     # last: saving a board rewrites the project file
$CLI pcb drc xiao-esp32s3-can-sniffer.kicad_pcb -o build/drc.rpt --severity-error 2>&1 | quiet >/dev/null
grep "Found" build/drc.rpt
