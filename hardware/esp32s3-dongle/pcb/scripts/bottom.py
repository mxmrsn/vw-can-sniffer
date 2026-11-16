"""Bottom board of the OBD dongle (v0.4.0): the J1962 plug, protection, the CAN
transceiver, the 5 V buck that the transceiver switches, the engine-running wake
detector, and the header up to the top board. Architecture:
hardware/esp32s3-dongle/docs/architecture.md.

    python3 hardware/esp32s3-dongle/pcb/scripts/bottom.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sheetgen import Sheet  # noqa: E402

PCB = os.path.join(HERE, '..')
R0603, C0603, C0805 = 'Resistor_SMD:R_0603_1608Metric', 'Capacitor_SMD:C_0603_1608Metric', 'Capacitor_SMD:C_0805_2012Metric'

s = Sheet('VW OBD dongle - bottom board', '0.4.0',
          ['J1962 plug, protection, TJA1043T CAN with sleep/wake, INH-switched 5 V buck, wake detector',
           'Not built. Parts marked "to verify" need a stock check; the J1962 footprint waits on samples'],
          seed='dongle-bottom')
s.add_library('dongle', os.path.join(PCB, 'lib', 'dongle.kicad_sym'))


def R(b, ref, val, a, c, mpn, dk, notes='', fp=R0603):
    b.part('Device:R', ref, val, fp, {'1': a, '2': c}, mpn=mpn, manufacturer='Vishay Dale' if mpn.startswith('CRCW') else 'YAGEO', digikey=dk, notes=notes)


def C(b, ref, val, a, c, mpn, dk, mfr, notes='', fp=C0603):
    b.part('Device:C', ref, val, fp, {'1': a, '2': c}, mpn=mpn, manufacturer=mfr, digikey=dk, notes=notes)


# ---------------------------------------------------------------- OBD plug and protection
b = s.block('OBD PLUG AND PROTECTION',
            'Pin 16 is always live. Fuse, then a clamp, then a diode against reverse polarity.\n'
            'J1962 footprint left open until plug samples are measured.')
plug = {str(n): None for n in range(1, 17)}
plug.update({'4': 'GND', '5': 'GND', '6': 'CAN_H', '14': 'CAN_L', '16': 'VOBD'})
b.part('Connector_Generic:Conn_02x08_Top_Bottom', 'J1', 'J1962 male', '', plug,
       notes='OBD-II plug, PCB mount. Part and footprint chosen after measuring samples')
b.part('Device:Polyfuse', 'F1', 'PTC 0.75A 33V', 'Fuse:Fuse_1812_4532Metric', {'1': 'VOBD', '2': 'VOBD_F'},
       mpn='1812L075/33DR', manufacturer='Littelfuse', digikey='F3355CT-ND')
b.part('Device:D_Zener', 'D1', 'SMBJ18A', 'Diode_SMD:D_SMB', {'1': 'VOBD_F', '2': 'GND'},
       mpn='SMBJ18A', manufacturer='Littelfuse', digikey='SMBJ18ALFCT-ND', notes='Unidirectional TVS, clamps transients on pin 16')
b.part('Device:D_Schottky', 'D2', 'MBR140SFT1G', 'Diode_SMD:D_SOD-123F', {'1': 'VBAT', '2': 'VOBD_F'},
       mpn='MBR140SFT1G', manufacturer='onsemi', digikey='MBR140SFT1GOSCT-ND', notes='Reverse-polarity block')
C(b, 'C1', '10u', 'VBAT', 'GND', 'GMC21X5R106K50NT', '4713-GMC21X5R106K50NTCT-ND', 'Cal-Chip Electronics', fp=C0805)
C(b, 'C2', '100n', 'VBAT', 'GND', 'CL21B104KBCNNNC', '1276-1003-1-ND', 'Samsung Electro-Mechanics', fp=C0805)
b.part('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', {'1': 'VOBD'})
b.part('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', {'1': 'GND'})
b.part('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', {'1': 'VBAT'})

# ---------------------------------------------------------------- CAN transceiver
b = s.block('CAN TRANSCEIVER (TJA1043T)',
            'Modes by STB_N / EN: normal H/H, listen-only H/L, standby L/L, go-to-sleep L/H.\n'
            'Both have internal pull-downs, so it comes up in standby: it cannot drive the bus until firmware asks.\n'
            'In sleep, INH floats and R4 turns the buck off. CAN activity or a change on WAKE wakes it.')
b.part('Interface_CAN_LIN:TJA1043T', 'U1', 'TJA1043T', 'Package_SO:SOIC-14_3.9x8.7mm_P1.27mm',
       {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3', '6': 'CAN_EN', '7': 'INH',
        '8': 'CAN_ERR_N', '9': 'WAKE', '10': 'VBAT', '11': None, '12': 'CAN_L', '13': 'CAN_H', '14': 'CAN_STB_N'},
       mpn='TJA1043T/1J', manufacturer='NXP', notes='To verify: exact orderable code and stock')
C(b, 'C3', '100n', 'VBAT', 'GND', 'CL21B104KBCNNNC', '1276-1003-1-ND', 'Samsung Electro-Mechanics', fp=C0805)
C(b, 'C4', '100n', '+5V', 'GND', 'CC0603KRX7R9BB104', '311-1344-1-ND', 'YAGEO')
C(b, 'C5', '100n', '+3V3', 'GND', 'CC0603KRX7R9BB104', '311-1344-1-ND', 'YAGEO')
b.part('Power_Protection:NUP2105L', 'D3', 'NUP2105L', 'Package_TO_SOT_SMD:SOT-23', {'1': 'CAN_H', '2': 'CAN_L', '3': 'GND'},
       mpn='NUP2105LT1G', manufacturer='onsemi', notes='CAN bus ESD protection. To verify: stock')
b.part('Jumper:SolderJumper_2_Open', 'JP1', 'TERM', 'Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm',
       {'1': 'CAN_H', '2': 'TERM'}, notes='Leave open in the car (the bus is already terminated). Bench only')
R(b, 'R1', '120', 'TERM', 'CAN_L', 'RC0603FR-07120RL', '311-120HRCT-ND')
b.part('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', {'1': '+3V3'})

# ---------------------------------------------------------------- 5 V buck
b = s.block('12 V TO 5 V BUCK (AP63301, as v0.3.0)',
            'EN is driven by the transceiver INH output. EN has an internal 1.5 uA pull-up,\n'
            'so R4 holds it low (0.15 V, below the 1.03 V threshold) whenever INH floats.\n'
            'Output 0.8 V x (1 + 158k / 30.1k) = 5.0 V.')
b.part('Regulator_Switching:AP63205WU', 'U2', 'AP63301WU', 'Package_TO_SOT_SMD:TSOT-23-6',
       {'1': 'FB', '2': 'INH', '3': 'VBAT', '4': 'GND', '5': 'SW', '6': 'BST'},
       mpn='AP63301WU-7', manufacturer='Diodes Incorporated', digikey='31-AP63301WU-7CT-ND')
R(b, 'R4', '100k', 'INH', 'GND', 'CRCW0603100KFKEA', '541-100KHCT-ND', notes='Buck off when INH floats')
C(b, 'C6', '100n', 'BST', 'SW', 'CL21B104KBCNNNC', '1276-1003-1-ND', 'Samsung Electro-Mechanics', fp=C0805)
b.part('Device:L', 'L1', '6.8u', 'Inductor_SMD_Wurth:L_Wurth_WE-LQSH-4020', {'1': 'SW', '2': '+5V'},
       mpn='74404042068', manufacturer='Wurth Elektronik', digikey='732-5507-1-ND')
C(b, 'C7', '22u', '+5V', 'GND', 'CL21A226KOQNNNE', '1276-6526-1-ND', 'Samsung Electro-Mechanics', fp=C0805)
C(b, 'C8', '22u', '+5V', 'GND', 'CL21A226KOQNNNE', '1276-6526-1-ND', 'Samsung Electro-Mechanics', fp=C0805)
R(b, 'R2', '158k', '+5V', 'FB', 'CRCW0603158KFKEA', '541-158KHCT-ND')
R(b, 'R3', '30.1k', 'FB', 'GND', 'CRCW060330K1FKEA', '541-30.1KHCT-ND')
C(b, 'C9', '47p', '+5V', 'FB', 'CL10C470JB8NNNC', '1276-1037-1-ND', 'Samsung Electro-Mechanics', notes='Feed-forward')
b.part('power:PWR_FLAG', '#FLG05', 'PWR_FLAG', '', {'1': '+5V'})

# ---------------------------------------------------------------- wake detector
b = s.block('ENGINE-RUNNING WAKE',
            'U3 watches VBAT: above 13.4 V (rising; 13.2 V falling) OUTA releases, Q1 pulls WAKE_BTN low,\n'
            'and that change on WAKE wakes the transceiver. The top-board button pulls the same node low.\n'
            'Parked, nothing pulls WAKE_BTN low and R9 holds it high: about 15 uA in all.')
b.part('dongle:TPS3700', 'U3', 'TPS3700', 'Package_TO_SOT_SMD:SOT-23-6',
       {'1': 'RUN', '2': 'GND', '3': 'RUN_SENSE', '4': 'GND', '5': 'VDET', '6': None},
       mpn='TPS3700DDCR', manufacturer='Texas Instruments', notes='To verify: stock. Comparator B unused')
R(b, 'R5', '10k', 'VBAT', 'VDET', 'CRCW060310K0FKEA', '541-10.0KHCT-ND', notes='With D4, keeps U3 under its 18 V limit')
b.part('Device:D_Zener', 'D4', 'MMSZ5245B', 'Diode_SMD:D_SOD-123', {'1': 'VDET', '2': 'GND'},
       mpn='MMSZ5245BT1G', manufacturer='onsemi', notes='15 V clamp for U3 supply. To verify: stock')
C(b, 'C10', '100n', 'VDET', 'GND', 'CC0603KRX7R9BB104', '311-1344-1-ND', 'YAGEO')
R(b, 'R6', '3.24M', 'VBAT', 'RUN_SENSE', 'CRCW06033M24FKEA', '', notes='Threshold 0.4 V x (3.24M + 100k) / 100k = 13.4 V. To verify: stock')
R(b, 'R7', '100k', 'RUN_SENSE', 'GND', 'CRCW0603100KFKEA', '541-100KHCT-ND')
R(b, 'R8', '2.2M', 'VDET', 'RUN', 'CRCW06032M20FKEA', '', notes='OUTA pull-up. To verify: stock')
b.part('Transistor_FET:2N7002', 'Q1', '2N7002', 'Package_TO_SOT_SMD:SOT-23', {'1': 'RUN', '2': 'GND', '3': 'WAKE_BTN'},
       mpn='2N7002', manufacturer='onsemi', notes='To verify: stock')
R(b, 'R9', '100k', 'VBAT', 'WAKE_BTN', 'CRCW0603100KFKEA', '541-100KHCT-ND', notes='Holds WAKE high while parked')
R(b, 'R10', '10k', 'WAKE_BTN', 'WAKE', 'CRCW060310K0FKEA', '541-10.0KHCT-ND', notes='Series protection for the WAKE pin')
b.part('power:PWR_FLAG', '#FLG06', 'PWR_FLAG', '', {'1': 'VDET'})

# ---------------------------------------------------------------- OBD sense
b = s.block('OBD VOLTAGE SENSE',
            'Same 0.18 ratio as v0.3.0, ten times the resistance so it draws about 10 uA when parked.')
R(b, 'R11', '1M', 'VBAT', 'OBD_SENSE', 'CRCW06031M00FKEA', '', notes='To verify: stock')
R(b, 'R12', '220k', 'OBD_SENSE', 'GND', 'CRCW0603220KFKEA', '', notes='To verify: stock')
C(b, 'C11', '100n', 'OBD_SENSE', 'GND', 'CC0603KRX7R9BB104', '311-1344-1-ND', 'YAGEO')

# ---------------------------------------------------------------- to the top board
b = s.block('TO THE TOP BOARD', 'Pin header; the top board carries the matching socket.')
b.part('Connector_Generic:Conn_02x07_Odd_Even', 'J2', 'TO TOP', 'Connector_PinHeader_1.27mm:PinHeader_2x07_P1.27mm_Vertical_SMD',
       {'1': 'GND', '2': 'GND', '3': '+5V', '4': '+5V', '5': '+3V3', '6': 'GND', '7': 'CAN_TXD', '8': 'CAN_RXD',
        '9': 'CAN_STB_N', '10': 'CAN_EN', '11': 'CAN_ERR_N', '12': 'OBD_SENSE', '13': 'WAKE_BTN', '14': 'GND'},
       notes='2 x 7, 1.27 mm. Stack height chosen with the enclosure. To verify: part and stock')

os.makedirs(os.path.join(PCB, 'bottom'), exist_ok=True)
s.save(os.path.join(PCB, 'bottom', 'dongle-bottom.kicad_sch'))
print('wrote bottom/dongle-bottom.kicad_sch')
