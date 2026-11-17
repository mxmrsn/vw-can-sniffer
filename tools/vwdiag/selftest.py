"""Bring-up checklist: the board's self-test, with what to probe for each thing that fails.

    python3 -m vwdiag selftest                    # reads only; the LEDs light in turn
    python3 -m vwdiag --transmit selftest --active # also one CAN loopback frame and the sync output
    python3 -m vwdiag --transmit selftest --active --force   # on a bench supply, not a car

The firmware measures (hardware/pico2w/firmware/src/selftest.h); this file knows each board's
pins and parts, so a failure comes back as "check this net, then that part". Pin numbers are the
boards' pins.h files.
"""
import textwrap

BOARDS = {
    "xiao": "XIAO ESP32-S3 carrier v0.3.0",
    "dongle": "OBD dongle v0.4.0",
    "pico": "Pico 2 W board v0.2.0",
}


def board_of(items):
    detail = next((i["detail"] for i in items if i["id"] == "board"), "")
    if detail.startswith("XIAO"):
        return "xiao"
    if detail.startswith("OBD dongle") or detail.startswith("simulator"):
        return "dongle"   # the simulator stands in for the dongle
    if detail.startswith("Pico"):
        return "pico"
    return None


# (board or None for any, check id, result or None for any not-pass) -> what to do
HINTS = {
    # ---- CAN
    (None, "can", "fail"): "Controller stopped or bus off. Bus off with nothing attached: CANH and CANL shorted "
                           "together, to ground or to 12 V; measure about 60 ohm between them with two 120 ohm "
                           "terminations, 120 ohm with one.",
    (None, "can", "warn"): "High error counters: wrong bit rate on the bus (500 kbit/s here), a missing "
                           "termination, or CANH and CANL swapped.",
    ("xiao", "can_loop", "fail"): "Nothing came back through the TJA1051T/3. In order: 5 V on its VCC (pin 3) and "
                                  "3.3 V on VIO (pin 5); S (pin 8) low during the test, driven by GPIO4 / D3 "
                                  "(R2 pulls it high); TXD (pin 1) from GPIO5 / D4 and RXD (pin 4) to GPIO6 / D5, "
                                  "not swapped. With nothing on the bus, a 120 ohm across CANH-CANL helps the "
                                  "recessive edges.",
    ("dongle", "can_loop", "fail"): "Nothing came back through the TJA1043T. In order: its VCC 5 V, VIO 3.3 V and "
                                    "VBAT; STB_N (GPIO6) and EN (GPIO7) both high during the test (Normal mode); "
                                    "TXD from GPIO4 and RXD to GPIO5 through the stacking connector, not swapped; "
                                    "ERR_N (GPIO15). A 120 ohm across CANH-CANL helps with nothing attached.",
    ("dongle", "can", "warn"): "ERR_N low: the TJA1043 flags a local failure (TXD stuck dominant, over-temperature) "
                               "or a bus fault; check TXD is not held low, and the transceiver's temperature.",
    (None, "can_loop", "skip"): "Skipped for safety (it sends one frame). Unplug from the car, or on a bench "
                                "supply run with --active --force.",
    # ---- card
    ("xiao", "card", "fail"): "No card, or not FAT32 (format it FAT32; exFAT is not enabled on the ESP32). Then "
                              "3.3 V at the socket, and continuity: CS GPIO43 / D6, SCK GPIO44 / D7, MOSI GPIO7 / "
                              "D8, MISO GPIO8 / D9. GPIO43/44 carry the boot log before the firmware takes them.",
    ("dongle", "card", "fail"): "No card, or not FAT32 (format it FAT32). Then 3.3 V at the socket, the 47 k "
                                "pull-ups on CS, MOSI and MISO, and continuity: CS GPIO10, MOSI GPIO11, SCK GPIO12, "
                                "MISO GPIO13.",
    (None, "card", "fail"): "No card, or not formatted FAT32. Then 3.3 V at the socket and the SPI lines to it.",
    # ---- Bluetooth
    (None, "bluetooth", "fail"): "The radio did not start: read the boot log on USB for the Bluetooth stack's "
                                 "error. On the XIAO, the antenna switch / u.FL antenna; on the simulator, start it "
                                 "with Bluetooth (vwdiag --sim-ble).",
    # ---- supply
    ("xiao", "supply", None): "Compare with a meter at OBD pin 16 (to pin 4 or 5). GPIO9 / D10 should read "
                              "battery x 22 / 122 (about 2.2 V at 12.4 V): check the 100 k / 22 k divider and the fuse.",
    ("dongle", "supply", None): "Compare with a meter at OBD pin 16 (to pin 4 or 5). GPIO1 reads the battery behind "
                                "the input diode through 1 M / 220 k: about 2.1 V at 12.4 V; check the divider, its "
                                "capacitor and the diode.",
    # ---- GPS
    ("dongle", "gps", "skip"): "No GPS fitted is fine. If one is: its TX to GPIO17, 3.3 V and ground at the "
                               "connector, and the baud rate (vwdiag console: gps baud 38400 for many u-blox M10 "
                               "modules; the default is 9600).",
    ("dongle", "gps", "fail"): "Sentences arrive but most are garbled: the baud rate (gps baud 9600 / 38400 / "
                               "115200), or noise on the TX line to GPIO17.",
    ("dongle", "pps", "fail"): "A fix but no pulses: the PPS wire to GPIO34, and that the module has a PPS pin "
                               "(not all breakouts bring it out). Its PPS LED should blink once a second.",
    ("dongle", "pps", "skip"): "Most receivers pulse only with a fix: take it outside, give it a few minutes, run "
                               "this again.",
    (None, "pps", "warn"): "Pulses come, but no time sentence pairs with them: the module must send RMC or ZDA "
                           "within a second of each pulse.",
    ("dongle", "sync", "fail"): "The sync timer did not run: a firmware fault; report it with the boot log.",
    ("dongle", "sync", "pass"): "The timer ran; put a scope or an LED with a resistor on GPIO35 to see the pulses.",
    # ---- LEDs
    ("xiao", "leds", "look"): "Expected: all four dark, then D1 to D4 one at a time. None: 5 V at the LED chain, "
                              "the level-shift buffer, data on GPIO1 / D0. Only the first ones: the DOUT to DIN "
                              "link after the last one that lit.",
    ("dongle", "leds", "look"): "Expected: all four dark, then one at a time. None: the LEDs' supply, the buffer, "
                                "data on GPIO48. Only the first ones: the DOUT to DIN link after the last one lit.",
    (None, "leds", "look"): "Expected: all dark, then status, CAN, log and USB one at a time.",
}


def hint(board, item):
    r = item["result"]
    for key in ((board, item["id"], r), (board, item["id"], None), (None, item["id"], r), (None, item["id"], None)):
        if key in HINTS and (key[2] is not None or r != "pass"):
            return HINTS[key]
    return None


def report(items):
    """Text lines: each check, with what to do where it did not pass, and a summary."""
    board = board_of(items)
    lines = []
    width = max(len(i["id"]) for i in items)
    for i in items:
        lines.append("%-4s  %-*s  %s" % (i["result"].upper() if i["result"] == "fail" else i["result"], width,
                                        i["id"], i["detail"]))
        h = hint(board, i)
        if h and i["id"] != "board":
            indent = " " * (width + 8)
            lines += textwrap.wrap("check: " + h, 100, initial_indent=indent, subsequent_indent=indent + "       ")
    counts = {}
    for i in items:
        counts[i["result"]] = counts.get(i["result"], 0) + 1
    lines.append("")
    lines.append(", ".join("%d %s" % (counts[k], {"look": "to check by eye"}.get(k, k))
                           for k in ("pass", "warn", "fail", "skip", "look") if counts.get(k)))
    return lines
