# The Pico board's notes on the schematic, rewritten for this board.
p = 'xiao-esp32s3-can-sniffer.kicad_sch'
s = open(p).read()
N = "\\n"
rep = {"Raspberry Pi Pico 2 W  (RP2350A + CYW43439 Wi-Fi / BLE)" + N + "CAN via PIO (can2040)":
       "Seeed XIAO ESP32-S3  (ESP32-S3 + Wi-Fi / BLE)" + N + "CAN via the chip's own TWAI controller, no software bit-banging" + N + "D2 is a strapping pin: R1 holds it low at reset",
       "VSYS POWER OR-ING" + N + "Pico micro-USB VBUS (via the Pico on-board Schottky) or 12V buck (via D5)":
       "VSYS POWER OR-ING" + N + "XIAO USB-C VBUS (via the module's own diode) or 12V buck (via D5)",
       "microSD LOGGING (SPI1: GP8 MISO, GP10 SCK, GP11 MOSI, GP12 CS)":
       "microSD LOGGING (D9 MISO, D7 SCK, D8 MOSI, D6 CS)"}
for a, b in rep.items():
    assert a in s, a[:40]
    s = s.replace(a, b)
first = [t for t in s.split('(text "') if t.startswith("D5 blocks back-feed")][0].split('"')[0]
s = s.replace(first, "D5 blocks back-feed into the buck when running from USB only, and is the series" + N
              + "diode Seeed requires when feeding the XIAO's 5V pad. TJA1051 VCC sits on VSYS" + N
              + "so the transceiver also works on the bench from USB.")
open(p, 'w').write(s)
