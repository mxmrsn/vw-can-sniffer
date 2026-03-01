#!/usr/bin/env python3
"""
Read VW CAN sniffer UART/USB stream and print CSV to stdout.

Usage:
  python3 tools/usb_dump.py /dev/ttyACM0
"""

import sys
import struct
import serial

SOF = 0xA5
TYPE_CAN = 0x01
TYPE_STAT = 0x02


def xor_crc(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
    return c


def main():
    if len(sys.argv) < 2:
        print("Usage: usb_dump.py <serial_port>", file=sys.stderr)
        sys.exit(1)

    port = sys.argv[1]
    ser = serial.Serial(port, 115200, timeout=0.1)

    print("ts_us,id,dlc,data_hex")

    state = 0
    length = 0
    msg_type = 0
    payload = bytearray()
    crc = 0

    while True:
        b = ser.read(1)
        if not b:
            continue
        b = b[0]

        if state == 0:  # wait SOF
            if b == SOF:
                state = 1
        elif state == 1:  # LEN
            length = b
            crc = b
            payload.clear()
            state = 2
        elif state == 2:  # TYPE
            msg_type = b
            crc ^= b
            if length == 0:
                state = 4
            else:
                state = 3
        elif state == 3:  # PAYLOAD
            payload.append(b)
            crc ^= b
            if len(payload) >= length:
                state = 4
        elif state == 4:  # CRC
            if crc == b:
                if msg_type == TYPE_CAN and len(payload) >= 9:
                    ts_us, can_id = struct.unpack_from('<II', payload, 0)
                    dlc = payload[8]
                    data = payload[9:9+dlc]
                    data_hex = ' '.join(f"{x:02X}" for x in data)
                    print(f"{ts_us},{can_id:08X},{dlc},{data_hex}")
                elif msg_type == TYPE_STAT and len(payload) >= 8:
                    rx_ok, rx_drop = struct.unpack_from('<II', payload, 0)
                    print(f"# stat rx_ok={rx_ok} rx_drop={rx_drop}")
            state = 0


if __name__ == '__main__':
    main()
