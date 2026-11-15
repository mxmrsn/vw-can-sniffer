// microSD card storage for the logger: ESP-IDF's FATFS over the sdmmc SPI
// driver. Same numbered-file behaviour as the Pico board: LOG00001.VWL upward,
// never overwriting anything already on the card.
//
// Short (8.3) names only, which is what the log format and AUTORUN.TXT use, so
// long-filename support stays off.
//
// Not yet run on hardware.
#include "storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "pins.h"

#define MOUNT_POINT "/sd"
#define SD_FREQ_KHZ 12500 // conservative; the card is initialised at 400 kHz first

static sdmmc_card_t *card;
static FILE *log_file;
static bool bus_ready;

static bool mount(void)
{
    if (card)
        return true;

    if (!bus_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = SD_MOSI_PIN,
            .miso_io_num = SD_MISO_PIN,
            .sclk_io_num = SD_SCK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        if (spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA) != ESP_OK)
            return false;
        bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_PIN;
    slot.host_id = SD_SPI_HOST;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false, // a card the board cannot read is the owner's to deal with
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &cfg, &card) == ESP_OK;
}

// After any error assume the card was pulled: forget it, so the next attempt starts clean.
static void drop_card(void)
{
    if (log_file)
        fclose(log_file);
    log_file = NULL;
    if (card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        card = NULL;
    }
}

bool storage_create_log(char *name, size_t name_cap)
{
    if (log_file)
        storage_close();
    if (!mount())
        return false;

    unsigned highest = 0, n;
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        drop_card();
        return false;
    }
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strlen(entry->d_name) != 12 || strncasecmp(entry->d_name, "LOG", 3)
            || strcasecmp(entry->d_name + 8, ".VWL"))
            continue;
        if (sscanf(entry->d_name + 3, "%5u", &n) == 1 && n > highest)
            highest = n;
    }
    closedir(dir);
    if (highest >= 99999) {
        drop_card();
        return false;
    }

    char path[sizeof(MOUNT_POINT) + 16];
    snprintf(name, name_cap, "LOG%05u.VWL", highest + 1);
    snprintf(path, sizeof(path), MOUNT_POINT "/LOG%05u.VWL", highest + 1);
    // O_EXCL rather than fopen("wb"): if the directory scan missed a file, fail
    // rather than write over somebody's recording.
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd >= 0)
        log_file = fdopen(fd, "wb");
    if (!log_file) {
        if (fd >= 0)
            close(fd);
        drop_card();
        return false;
    }
    return true;
}

bool storage_write(const uint8_t *data, size_t len)
{
    if (!log_file || fwrite(data, 1, len, log_file) != len) {
        drop_card();
        return false;
    }
    return true;
}

bool storage_sync(void)
{
    if (!log_file || fflush(log_file) != 0 || fsync(fileno(log_file)) != 0) {
        drop_card();
        return false;
    }
    return true;
}

void storage_close(void)
{
    if (log_file)
        fclose(log_file);
    log_file = NULL;
}

int storage_read_file(const char *name, char *buf, size_t cap)
{
    char path[sizeof(MOUNT_POINT) + 16];
    if (log_file || !mount())
        return -1;
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap, f);
    int err = ferror(f);
    fclose(f);
    return err ? -1 : (int)n;
}
