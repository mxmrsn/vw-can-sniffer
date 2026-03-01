# UART CAN Stream Protocol v1

Goal: simple, fixed framing for CAN sniff data over UART.

## Frame Structure
All multi-byte fields are little-endian.

```
[SOF][LEN][TYPE][PAYLOAD...][CRC]

SOF   : 0xA5 (1 byte)
LEN   : payload length in bytes (1 byte, 0-255)
TYPE  : 0x01 = CAN frame (1 byte)
CRC   : 8-bit XOR of [LEN, TYPE, PAYLOAD]
```

### TYPE 0x01 Payload (CAN frame)
```
[timestamp_us(4)][can_id(4)][dlc(1)][data(0-8)]

- timestamp_us: micros() at capture time (wraps ~71 min)
- can_id: 11-bit or 29-bit raw ID (use bit 31 to mark extended)
- dlc: 0-8
```

### CAN ID Encoding
- Standard ID: 0x000007FF max
- Extended ID: set bit31 to 1 and store 29-bit ID in bits 0..28

### TYPE 0x02 Payload (Status)
```
[rx_ok(4)][rx_drop(4)]
```

- `rx_ok`: number of CAN frames received
- `rx_drop`: number of frames dropped due to ring buffer full

### CRC
```
crc = LEN ^ TYPE ^ payload[0] ^ payload[1] ^ ...
```

## Notes
- Stream is byte-oriented; use SOF to resync.
- CRC is simple XOR to keep Teensy/ESP32 code light.
- You can add new message types later (e.g., 0x02 = status).
