#!/usr/bin/env python3
"""Placeholder STEP boxes for parts KiCad's library has no model for.

    python3 make_models.py          (writes the .step files next to this script)

Plan outlines are from the KiCad footprints' fab layers; heights are datasheet nominals.
Model axes: X right, Y up the page (KiCad's Y flipped), Z up from the board.
The SK6812MINI box is copied from the XIAO board's 3dmodels folder.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XIAO = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'xiao-esp32s3', 'pcb', '3dmodels'))
sys.path.insert(0, XIAO)
import make_placeholder_step as gen  # noqa: E402

MODELS = {
    # ESP32-S3-MINI-1: 15.4 x 20.5 x 2.4 mm, antenna end at footprint Y -9.75
    'ESP32-S3-MINI-1_placeholder.step': [("module", (-7.7, -10.25, 0.0), (7.7, 9.75, 2.4))],
    # 1812 PTC fuse: 4.5 x 3.2 mm body, 1.8 mm tall (tallest common 1812 resettable)
    'Fuse_1812_placeholder.step': [("fuse", (-2.25, -1.6, 0.0), (2.25, 1.6, 1.8))],
}

for name, boxes in MODELS.items():
    gen.BOXES = boxes
    gen.main(os.path.join(HERE, name))
shutil.copy(os.path.join(XIAO, 'SK6812MINI_placeholder.step'), HERE)
print('wrote', ', '.join(list(MODELS) + ['SK6812MINI_placeholder.step']))
