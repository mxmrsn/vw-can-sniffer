// CAN logging to a file. Format described in hardware/pico2w/docs/logging.md.
//
//   file    = header (32 bytes) + records
//   header  = "VWLG" version(1) header_len(32) 0 0 bitrate(u32) start_ms(u32) start_epoch(u32) zeros
//   record  = type(u8) time_ms(u32) payload
//
// Numbers are little-endian; time_ms is milliseconds since power-up.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "isotp.h"

#define LOG_VERSION 1
#define LOG_HEADER_LEN 32

enum log_record {
    LOG_REC_CAN = 0x01,  // id(u32) flags(u8: bits 0-3 length, bit 6 sent by us, bit 7 29-bit id) data
    LOG_REC_LOST = 0x02, // count(u32): frames that could not be recorded since the last record
    LOG_REC_MARK = 0x03, // length(u8) text
    LOG_REC_TIME = 0x04, // epoch(u32): the wall-clock time (Unix seconds) at this record's time_ms
};

struct logger_stats {
    bool active;
    uint32_t records; // CAN frames written to the current (or last) file
    uint32_t bytes;
    uint32_t lost;    // frames not recorded: buffer full or receive queue overrun
    char name[16];
};

bool logger_start(uint32_t now_ms, uint32_t bitrate, uint32_t rx_dropped_total);
void logger_stop(void);
bool logger_active(void);
void logger_can(const struct can_frame *f, uint32_t time_ms, bool sent);
void logger_mark(const char *text, uint32_t time_ms);

// The interface has no clock of its own; a phone or computer can tell it the time.
void logger_set_time(uint32_t epoch, uint32_t now_ms);

// Call often: writes buffered records out and notices frames the CAN driver lost.
void logger_poll(uint32_t now_ms, uint32_t rx_dropped_total);
void logger_get_stats(struct logger_stats *out);

// Fetching files from the card (for copying logs off it over USB or Bluetooth).
// logger_read_file returns bytes read, or one of these:
enum { LOGGER_READ_NO_CARD = -1, LOGGER_READ_BAD_NAME = -2, LOGGER_READ_BUSY = -3 };
int logger_read_file(const char *name, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *size);
