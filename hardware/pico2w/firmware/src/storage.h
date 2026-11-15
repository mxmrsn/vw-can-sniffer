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
