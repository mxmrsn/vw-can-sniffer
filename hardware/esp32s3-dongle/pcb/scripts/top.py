"""Top board of the OBD dongle (v0.4.0): the ESP32-S3-MINI-1, USB-C, the 3.3 V
regulator, the microSD slot, the LED chain, the button, an optional IMU, and the
GPS / sync and expansion connectors. Architecture:
hardware/esp32s3-dongle/docs/architecture.md.

    python3 hardware/esp32s3-dongle/pcb/scripts/top.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sheetgen import Sheet  # noqa: E402

PCB = os.path.join(HERE, '..')
R0603, C0603, C0805 = 'Resistor_SMD:R_0603_1608Metric', 'Capacitor_SMD:C_0603_1608Metric', 'Capacitor_SMD:C_0805_2012Metric'

s = Sheet('VW OBD dongle - top board', '0.4.0',
          ['ESP32-S3-MINI-1, USB-C, 3.3 V LDO, microSD, addressable LED chain, button, IMU (optional), GPS / sync and expansion',
           'Not built. Parts marked to verify need a stock check'],
          seed='dongle-top')


def R(b, ref, val, a, c, mpn, dk, notes='', dnp=False):
    b.part('Device:R', ref, val, R0603, {'1': a, '2': c}, mpn=mpn, manufacturer='Vishay Dale', digikey=dk, notes=notes, dnp=dnp)


def C(b, ref, val, a, c, mpn, dk, mfr, notes='', fp=C0603, dnp=False):
    b.part('Device:C', ref, val, fp, {'1': a, '2': c}, mpn=mpn, manufacturer=mfr, digikey=dk, notes=notes, dnp=dnp)


def C100n(b, ref, a, notes='', dnp=False):
    C(b, ref, '100n', a, 'GND', 'CC0603KRX7R9BB104', '311-1344-1-ND', 'YAGEO', notes, dnp=dnp)


# ---------------------------------------------------------------- power
b = s.block('POWER',
            'USB 5 V and the bottom board 5 V are diode-ORed into V5_SYS, then a 3.3 V LDO.\n'
            'Bluetooth only, the module averages well under 100 mA: under 0.2 W in the LDO.')
usb = {'A1': 'GND', 'A12': 'GND', 'B1': 'GND', 'B12': 'GND', 'A4': 'VBUS', 'A9': 'VBUS', 'B4': 'VBUS', 'B9': 'VBUS',
       'A5': 'CC1', 'B5': 'CC2', 'A6': 'USB_DP', 'B6': 'USB_DP', 'A7': 'USB_DM', 'B7': 'USB_DM',
       'A8': None, 'B8': None, 'SH': 'GND'}
b.part('Connector:USB_C_Receptacle_USB2.0_16P', 'J1', 'USB-C', 'Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal',
       usb, mpn='USB4105-GF-A', manufacturer='GCT', notes='Console, firmware, bench power. To verify: stock')
R(b, 'R1', '5.1k', 'CC1', 'GND', 'CRCW06035K10FKEA', '', notes='Identifies a USB device to the host. To verify: stock')
R(b, 'R2', '5.1k', 'CC2', 'GND', 'CRCW06035K10FKEA', '', notes='To verify: stock')
b.part('Power_Protection:USBLC6-2SC6', 'U1', 'USBLC6-2SC6', 'Package_TO_SOT_SMD:SOT-23-6',
       {'1': 'USB_DP', '6': 'USB_DP', '3': 'USB_DM', '4': 'USB_DM', '2': 'GND', '5': 'VBUS'},
       mpn='USBLC6-2SC6', manufacturer='STMicroelectronics', notes='USB ESD protection. To verify: stock')
b.part('Device:D_Schottky', 'D1', 'MBR120VLSFT1G', 'Diode_SMD:D_SOD-123F', {'1': 'V5_SYS', '2': 'VBUS'},
       mpn='MBR120VLSFT1G', manufacturer='onsemi', digikey='MBR120VLSFT1GOSCT-ND', notes='USB into V5_SYS')
b.part('Device:D_Schottky', 'D2', 'MBR120VLSFT1G', 'Diode_SMD:D_SOD-123F', {'1': 'V5_SYS', '2': '+5V'},
       mpn='MBR120VLSFT1G', manufacturer='onsemi', digikey='MBR120VLSFT1GOSCT-ND', notes='Bottom board 5 V into V5_SYS')
b.part('Regulator_Linear:AP2112K-3.3', 'U2', 'AP2112K-3.3', 'Package_TO_SOT_SMD:SOT-23-5',
       {'1': 'V5_SYS', '2': 'GND', '3': 'V5_SYS', '4': None, '5': '+3V3'},
       mpn='AP2112K-3.3TRG1', manufacturer='Diodes Incorporated', notes='600 mA LDO. To verify: stock')
C(b, 'C1', '4.7u', 'V5_SYS', 'GND', 'CC0603KRX5R7BB475', '311-1785-1-ND', 'YAGEO')
C(b, 'C2', '4.7u', '+3V3', 'GND', 'CC0603KRX5R7BB475', '311-1785-1-ND', 'YAGEO')
C(b, 'C3', '10u', '+3V3', 'GND', 'GMC21X5R106K50NT', '4713-GMC21X5R106K50NTCT-ND', 'Cal-Chip Electronics', fp=C0805,
  notes='Bulk for the module radio')
for i, net in enumerate(('VBUS', '+5V', 'V5_SYS', 'GND')):
    b.part('power:PWR_FLAG', '#FLG0%d' % (i + 1), 'PWR_FLAG', '', {'1': net})

# ---------------------------------------------------------------- processor
b = s.block('ESP32-S3-MINI-1',
            'Quad flash, no PSRAM: GPIO 33 to 37 are free. Strapping pins 0, 3, 45, 46 carry nothing\n'
            'driven at reset (0 is the BOOT button). 19 and 20 are the chip USB. Pin map in pins.h.')
mcu = {str(n): 'GND' for n in [1, 2, 42, 43] + list(range(46, 66))}
mcu.update({'3': '+3V3', '4': 'BOOT', '5': 'OBD_SENSE', '6': 'MODE_IN', '7': None, '8': 'CAN_TXD', '9': 'CAN_RXD',
            '10': 'CAN_STB_N', '11': 'CAN_EN', '12': 'IMU_SDA', '13': 'IMU_SCL', '14': 'SD_CS', '15': 'SD_MOSI',
            '16': 'SD_SCK', '17': 'SD_MISO', '18': 'EXP_CS1', '19': 'CAN_ERR_N', '20': 'IMU_INT', '21': 'GPS_RXD',
            '22': 'GPS_TXD', '23': 'USB_DM', '24': 'USB_DP', '25': 'EXP_CS2', '26': None, '27': 'EXP_INT1',
            '28': 'EXP_INT2', '29': 'GPS_PPS', '30': 'LED_DATA', '31': 'SYNC_OUT', '32': None, '33': None,
            '34': None, '35': None, '36': None, '37': None, '38': None, '39': None, '40': None, '41': None,
            '44': None, '45': 'CHIP_EN'})
b.part('RF_Module:ESP32-S3-MINI-1', 'U3', 'ESP32-S3-MINI-1-N8', 'RF_Module:ESP32-S2-MINI-1', mcu,
       mpn='ESP32-S3-MINI-1-N8', manufacturer='Espressif', notes='Antenna end at the board edge, no copper or metal under it. To verify: stock')
R(b, 'R3', '10k', '+3V3', 'CHIP_EN', 'CRCW060310K0FKEA', '541-10.0KHCT-ND', notes='EN delay with C4')
C(b, 'C4', '1u', 'CHIP_EN', 'GND', 'CL10A105KB8NNNC', '', 'Samsung Electro-Mechanics', notes='To verify: stock')
C100n(b, 'C5', '+3V3')
b.part('Switch:SW_Push', 'S1', 'BOOT', 'Button_Switch_SMD:SW_SPST_PTS810', {'1': 'BOOT', '2': 'GND'},
       mpn='PTS810 SJM 250 SMTR LFS', manufacturer='C&K', notes='Hold while plugging in USB to force the ROM loader. To verify: stock')

# ---------------------------------------------------------------- user interface
b = s.block('BUTTON AND LEDS',
            'The button pulls WAKE_BTN low: that wakes a sleeping dongle (through the bottom board)\n'
            'and, through D3, reads as MODE_IN on a running one. D3 keeps the 12 V-level line off the GPIO.\n'
            'Four addressable LEDs on one pin, buffered to the LED supply as on v0.3.0.')
b.part('Switch:SW_Push', 'S2', 'MODE', 'Button_Switch_SMD:SW_SPST_PTS810', {'1': 'WAKE_BTN', '2': 'GND'},
       mpn='PTS810 SJM 250 SMTR LFS', manufacturer='C&K', notes='Wake and mode. To verify: stock')
b.part('Diode:1N4148W', 'D3', '1N4148W', 'Diode_SMD:D_SOD-123', {'1': 'WAKE_BTN', '2': 'MODE_IN'},
       mpn='1N4148W-7-F', manufacturer='Diodes Incorporated', notes='MODE_IN uses the GPIO internal pull-up. To verify: stock')
b.part('74xGxx:74AHCT1G125', 'U4', '74AHCT1G125', 'Package_TO_SOT_SMD:SOT-23-5',
       {'1': 'GND', '2': 'LED_DATA', '3': 'GND', '4': 'LED_BUF', '5': 'V5_SYS'},
       mpn='SN74AHCT1G125DBVR', manufacturer='Texas Instruments', notes='To verify: stock')
C100n(b, 'C6', 'V5_SYS')
R(b, 'R4', '330', 'LED_BUF', 'LED_DIN', 'CRCW0603330RFKEA', '541-330HCT-ND')
chain = ['LED_DIN', 'LED_D2', 'LED_D3', 'LED_D4', None]
role = ['status', 'CAN', 'Bluetooth', 'logging']
for i in range(4):
    b.part('LED:SK6812MINI', 'D%d' % (4 + i), 'SK6812MINI', 'LED_SMD:LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm',
           {'3': chain[i], '1': chain[i + 1], '4': 'V5_SYS', '2': 'GND'},
           manufacturer='Opsco / Worldsemi', mpn='SK6812MINI (to verify)',
           notes='LED %d of 4, %s. Right-angle part or light pipe to choose with the enclosure' % (i + 1, role[i]))
    C100n(b, 'C%d' % (7 + i), 'V5_SYS')

# ---------------------------------------------------------------- storage
b = s.block('MICROSD', 'SPI mode, shared with the expansion connector. Pull-ups keep the card quiet until it is set up.')
sd = {'1': None, '2': 'SD_CS', '3': 'SD_MOSI', '4': '+3V3', '5': 'SD_SCK', '6': 'GND', '7': 'SD_MISO', '8': None,
      '9': None, '10': None, 'SH': 'GND'}
b.part('Connector:Micro_SD_Card_Det_Hirose_DM3AT', 'J2', 'DM3AT-SF-PEJM5', 'Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5', sd,
       mpn='DM3AT-SF-PEJM5', manufacturer='Hirose', digikey='HR1964CT-ND')
for ref, net in (('R5', 'SD_CS'), ('R6', 'SD_MOSI'), ('R7', 'SD_MISO')):
    R(b, ref, '47k', '+3V3', net, 'CRCW060347K0FKEA', '', notes='To verify: stock')
C(b, 'C11', '4.7u', '+3V3', 'GND', 'CC0603KRX5R7BB475', '311-1785-1-ND', 'YAGEO')

# ---------------------------------------------------------------- IMU
b = s.block('IMU (OPTIONAL, NOT FITTED BY DEFAULT)',
            'A dongle hanging from the OBD socket is not rigidly fixed, so this is for drive replay, not\n'
            'sensor fusion. LSM6DSM is a placeholder for the chosen part. I2C address 0x6A.')
b.part('Sensor_Motion:LSM6DSM', 'U5', 'LSM6DSM', 'Package_LGA:LGA-14_3x2.5mm_P0.5mm_LayoutBorder3x4y',
       {'1': 'GND', '2': 'GND', '3': 'GND', '4': 'IMU_INT', '5': '+3V3', '6': 'GND', '7': 'GND', '8': '+3V3',
        '9': None, '10': None, '11': None, '12': '+3V3', '13': 'IMU_SCL', '14': 'IMU_SDA'},
       mpn='LSM6DSM (placeholder)', manufacturer='STMicroelectronics', notes='Part to choose', dnp=True)
C100n(b, 'C12', '+3V3', dnp=True)
C100n(b, 'C13', '+3V3', dnp=True)
R(b, 'R8', '4.7k', '+3V3', 'IMU_SDA', 'CRCW06034K70FKEA', '', notes='To verify: stock', dnp=True)
R(b, 'R9', '4.7k', '+3V3', 'IMU_SCL', 'CRCW06034K70FKEA', '', notes='To verify: stock', dnp=True)

# ---------------------------------------------------------------- connectors
b = s.block('GPS / SYNC AND EXPANSION',
            'J3: an external GPS module (UART and pulse-per-second) and a sync line for a camera / IMU rig.\n'
            'J4: a third board, e.g. extra CAN controllers for a gateway tap. SPI is shared with the card.')
b.part('Connector_Generic_MountingPin:Conn_01x06_MountingPin', 'J3', 'GPS / SYNC',
       'Connector_JST:JST_GH_SM06B-GHS-TB_1x06-1MP_P1.25mm_Horizontal',
       {'1': 'GND', '2': '+3V3', '3': 'GPS_TXD', '4': 'GPS_RXD', '5': 'GPS_PPS', '6': 'SYNC_OUT', 'MP': 'GND'},
       mpn='SM06B-GHS-TB(LF)(SN)', manufacturer='JST', notes='Pin 3 is the dongle output to the GPS receive line. To verify: stock')
b.part('Connector_Generic:Conn_02x05_Odd_Even', 'J4', 'EXPANSION', 'Connector_PinHeader_1.27mm:PinHeader_2x05_P1.27mm_Vertical_SMD',
       {'1': '+3V3', '2': 'V5_SYS', '3': 'SD_SCK', '4': 'SD_MOSI', '5': 'SD_MISO', '6': 'EXP_CS1', '7': 'EXP_CS2',
        '8': 'EXP_INT1', '9': 'EXP_INT2', '10': 'GND'},
       notes='Not fitted unless a third board is used', dnp=True)

# ---------------------------------------------------------------- to the bottom board
b = s.block('TO THE BOTTOM BOARD', 'Socket; same pin numbering as the bottom board header.')
b.part('Connector_Generic:Conn_02x07_Odd_Even', 'J5', 'TO BOTTOM', 'Connector_PinSocket_1.27mm:PinSocket_2x07_P1.27mm_Vertical_SMD',
       {'1': 'GND', '2': 'GND', '3': '+5V', '4': '+5V', '5': '+3V3', '6': 'GND', '7': 'CAN_TXD', '8': 'CAN_RXD',
        '9': 'CAN_STB_N', '10': 'CAN_EN', '11': 'CAN_ERR_N', '12': 'OBD_SENSE', '13': 'WAKE_BTN', '14': 'GND'},
       notes='2 x 7, 1.27 mm. To verify: part and stock')

os.makedirs(os.path.join(PCB, 'top'), exist_ok=True)
s.save(os.path.join(PCB, 'top', 'dongle-top.kicad_sch'))
print('wrote top/dongle-top.kicad_sch')
