// File storage for the logger: the microSD card on the board, or a folder on
// a computer in the simulator. Implemented by storage_pico.c and sim/storage_host.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Create the next unused log file (LOG00001.VWL, LOG00002.VWL, ...) and leave it
// open for writing. Returns false if there is no usable card.
bool storage_create_log(char *name, size_t name_cap);
bool storage_write(const uint8_t *data, size_t len);
bool storage_sync(void); // make what has been written survive a power cut
void storage_close(void);

// Read a whole small text file. Returns its length, or -1 if it is not there.
int storage_read_file(const char *name, char *buf, size_t cap);

// Every file in the card's top folder, in the card's own order. Returns how many,
// or -1 if there is no usable card.
typedef void (*storage_list_fn)(const char *name, uint32_t size, void *ctx);
int storage_list(storage_list_fn fn, void *ctx);

// Read up to `cap` bytes of a file from `offset`; *size gets the file's size.
// Returns the number of bytes read (0 past the end), or -1 if it cannot be read.
int storage_read_at(const char *name, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *size);

// Bring-up self-test: write a small file (SELFTEST.TMP), read it back, delete it. Not while a log is
// open. Returns a SELFTEST_* result (selftest.h) and says what it found in detail.
int storage_check(char *detail, size_t cap);
