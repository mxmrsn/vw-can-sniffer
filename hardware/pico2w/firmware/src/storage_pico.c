// microSD card storage for the logger: FatFs over SPI1, using
// carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (Apache-2.0). FAT32 and exFAT cards.
//
// Not yet run on hardware.
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "../pins.h"
#include "selftest.h"
#include "ff.h"
#include "hw_config.h"

// ---- what the library needs to know about the board
static spi_t spi = {
    .hw_inst = SD_SPI_PORT,
    .sck_gpio = SD_SCK_PIN,
    .mosi_gpio = SD_MOSI_PIN,
    .miso_gpio = SD_MISO_PIN,
    .baud_rate = 12500 * 1000, // conservative; the library drops to 400 kHz to initialise the card
};
static sd_spi_if_t spi_if = { .spi = &spi, .ss_gpio = SD_CS_PIN };
static sd_card_t sd_card = { .type = SD_IF_SPI, .spi_if_p = &spi_if };

size_t sd_get_num(void)
{
    return 1;
}

sd_card_t *sd_get_by_num(size_t num)
{
    return num == 0 ? &sd_card : NULL;
}

// ---- storage.h
static FATFS fs;
static FIL log_file;
static bool mounted, file_open;

static bool mount(void)
{
    if (!mounted)
        mounted = f_mount(&fs, "", 1) == FR_OK;
    return mounted;
}

// After any error assume the card was pulled: forget it, so the next attempt starts clean.
static void drop_card(void)
{
    if (file_open)
        f_close(&log_file);
    file_open = false;
    f_unmount("");
    mounted = false;
}

bool storage_create_log(char *name, size_t name_cap)
{
    if (file_open)
        storage_close();
    if (!mount())
        return false;

    unsigned highest = 0, n;
    DIR dir;
    FILINFO info;
    FRESULT r = f_findfirst(&dir, &info, "", "LOG?????.VWL");
    while (r == FR_OK && info.fname[0]) {
        if (sscanf(info.fname + 3, "%5u", &n) == 1 && n > highest)
            highest = n;
        r = f_findnext(&dir, &info);
    }
    f_closedir(&dir);
    if (r != FR_OK || highest >= 99999) {
        drop_card();
        return false;
    }

    snprintf(name, name_cap, "LOG%05u.VWL", highest + 1);
    if (f_open(&log_file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        drop_card();
        return false;
    }
    file_open = true;
    return true;
}

bool storage_write(const uint8_t *data, size_t len)
{
    UINT written = 0;
    if (!file_open || f_write(&log_file, data, (UINT)len, &written) != FR_OK || written != len) {
        drop_card();
        return false;
    }
    return true;
}

bool storage_sync(void)
{
    if (!file_open || f_sync(&log_file) != FR_OK) {
        drop_card();
        return false;
    }
    return true;
}

void storage_close(void)
{
    if (file_open)
        f_close(&log_file);
    file_open = false;
}

int storage_read_file(const char *name, char *buf, size_t cap)
{
    FIL f;
    UINT n = 0;
    if (file_open || !mount())
        return -1;
    if (f_open(&f, name, FA_READ) != FR_OK)
        return -1;
    FRESULT r = f_read(&f, buf, (UINT)cap, &n);
    f_close(&f);
    return r == FR_OK ? (int)n : -1;
}

int storage_list(storage_list_fn fn, void *ctx)
{
    if (!mount())
        return -1;
    DIR dir;
    FILINFO info;
    int count = 0;
    FRESULT r = f_findfirst(&dir, &info, "", "*");
    while (r == FR_OK && info.fname[0]) {
        if (!(info.fattrib & (AM_DIR | AM_HID | AM_SYS))) {
            fn(info.fname, (uint32_t)info.fsize, ctx);
            count++;
        }
        r = f_findnext(&dir, &info);
    }
    f_closedir(&dir);
    if (r != FR_OK) {
        drop_card();
        return -1;
    }
    return count;
}

int storage_read_at(const char *name, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *size)
{
    FIL f;
    UINT n = 0;
    if (!mount() || f_open(&f, name, FA_READ) != FR_OK)
        return -1;
    *size = (uint32_t)f_size(&f);
    FRESULT r = FR_OK;
    if (offset < *size) {
        r = f_lseek(&f, offset);
        if (r == FR_OK)
            r = f_read(&f, buf, (UINT)cap, &n);
    }
    f_close(&f);
    return r == FR_OK ? (int)n : -1;
}

int storage_check(char *detail, size_t cap)
{
    static uint8_t out[2048], back[2048];
    FIL f;
    UINT n;
    if (file_open) {
        snprintf(detail, cap, "a log is open");
        return SELFTEST_SKIP;
    }
    if (!mount()) {
        snprintf(detail, cap, "no card, or it cannot be read (FAT32 or exFAT needed)");
        return SELFTEST_FAIL;
    }
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = (uint8_t)(i * 7 + 3);
    bool ok = f_open(&f, "SELFTEST.TMP", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if (ok) {
        ok = f_write(&f, out, sizeof(out), &n) == FR_OK && n == sizeof(out);
        ok = f_close(&f) == FR_OK && ok;
    }
    if (ok && f_open(&f, "SELFTEST.TMP", FA_READ) == FR_OK) {
        ok = f_read(&f, back, sizeof(back), &n) == FR_OK && n == sizeof(back);
        f_close(&f);
    } else {
        ok = false;
    }
    f_unlink("SELFTEST.TMP");
    if (!ok) {
        drop_card();
        snprintf(detail, cap, "the card mounted but a test file could not be written and read");
        return SELFTEST_FAIL;
    }
    if (memcmp(out, back, sizeof(out))) {
        snprintf(detail, cap, "test file read back differs from what was written");
        return SELFTEST_FAIL;
    }
    DWORD free_clusters;
    FATFS *pfs;
    unsigned long mb = 0;
    if (f_getfree("", &free_clusters, &pfs) == FR_OK)
        mb = (unsigned long)((uint64_t)(pfs->n_fatent - 2) * pfs->csize / 2048);
    snprintf(detail, cap, "%lu MB card, test file written, read back and deleted", mb);
    return SELFTEST_PASS;
}
