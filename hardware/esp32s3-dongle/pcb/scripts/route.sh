#!/bin/bash
# Lay out both dongle boards: schematics -> netlists -> placement -> Freerouting -> pour -> DRC.
#   hardware/esp32s3-dongle/pcb/scripts/route.sh
# Needs KiCad 10, python3.10+, and Freerouting with a recent Java (override JAVA / FREEROUTING_JAR).
cd "$(dirname "$0")/.."
KICAD=/Applications/KiCad/KiCad.app/Contents
CLI=$KICAD/MacOS/kicad-cli
KPY=$KICAD/Frameworks/Python.framework/Versions/Current/bin/python3
PY=${PY:-python3}
JAVA=${JAVA:-/opt/homebrew/opt/openjdk/bin/java}
FREEROUTING_JAR=${FREEROUTING_JAR:-$HOME/.claude/tools/freerouting/dl/freerouting-2.5.0.jar}
mkdir -p build
quiet() { grep -v "memory leak\|stdpbase\|Fontconfig\|Debug:" || true; }

$PY scripts/bottom.py && $PY scripts/top.py
for d in bottom top; do
    [ -f $d/dongle-$d.kicad_pro ] || cp ../../xiao-esp32s3/pcb/xiao-esp32s3-can-sniffer.kicad_pro $d/dongle-$d.kicad_pro
    $CLI sch export netlist $d/dongle-$d.kicad_sch -o build/$d.net 2>&1 | quiet >/dev/null
    $KPY scripts/layout.py $d build/$d.net 2>&1 | quiet
    $PY scripts/rules.py $d/dongle-$d.kicad_pro      # before the DSN export reads them
done
$PY scripts/stack_check.py
for d in bottom top; do
    B=$d/dongle-$d.kicad_pcb
    $KPY -c "import pcbnew; pcbnew.ExportSpecctraDSN(pcbnew.LoadBoard('$B'), 'build/$d.dsn')" 2>&1 | quiet
    $PY scripts/dsn_classes.py build/$d.dsn
    rm -f build/$d.ses
    # the router's outcome depends on its thread count; try a few until nothing is left unrouted
    for mt in 4 1 2 8; do
        "$JAVA" -jar "$FREEROUTING_JAR" --gui.enabled=false -de build/$d.dsn -do build/$d.ses -mp 120 -mt $mt > build/$d-freerouting.log 2>&1
        grep -q "(0 unrouted" build/$d-freerouting.log && break
    done
    echo "$d: $(grep -E 'Auto-routing stage completed' build/$d-freerouting.log | sed 's/.*final score: [0-9.]* //')"
    $KPY -c "
import pcbnew
b = pcbnew.LoadBoard('$B')
pcbnew.ImportSpecctraSES(b, 'build/$d.ses')
# the router necks tracks down at fine-pitch pins; 0.15 mm is the floor fabs handle comfortably
for t in list(b.GetTracks()):
    if t.GetClass() == 'PCB_TRACK' and t.GetWidth() < pcbnew.FromMM(0.15):
        t.SetWidth(pcbnew.FromMM(0.15))
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard('$B', b)" 2>&1 | quiet
    $PY scripts/rules.py $d/dongle-$d.kicad_pro
    drc() { $CLI pcb drc $B -o build/$d-drc.rpt --severity-error 2>&1 | quiet >/dev/null; }
    drc
    for pass in 1 2 3; do
        grep -q "starved_thermal" build/$d-drc.rpt || break
        $KPY scripts/solid_starved.py $B build/$d-drc.rpt 2>&1 | quiet
        $PY scripts/rules.py $d/dongle-$d.kicad_pro
        drc
    done
    echo "$d: $(grep 'Found' build/$d-drc.rpt | tr '\n' ' ')"
done

# STEP models for the enclosure work, and renders for a quick look
mkdir -p step build/render
for d in bottom top; do
    $CLI pcb export step --subst-models --force -o step/dongle-$d.step $d/dongle-$d.kicad_pcb 2>&1 | grep "Could not add" || true
    for s in top bottom; do
        $CLI pcb render --side $s --width 1600 --height 1300 --quality high -o build/render/$d-$s.png $d/dongle-$d.kicad_pcb >/dev/null 2>&1
    done
done
