// Simulator stand-in for the microSD card: a folder named by the VWDIAG_SD
// environment variable. With the variable unset there is "no card".
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/storage.h"

static FILE *log_file;

static const char *card(void)
{
    const char *dir = getenv("VWDIAG_SD");
    return dir && *dir ? dir : NULL;
}

bool storage_create_log(char *name, size_t name_cap)
{
    const char *dir = card();
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d)
        return false;
    unsigned highest = 0, n;
    for (struct dirent *e; (e = readdir(d));) {
        char tail[8];
        if (sscanf(e->d_name, "LOG%5u.%3s", &n, tail) == 2 && !strcmp(tail, "VWL") && n > highest)
            highest = n;
    }
    closedir(d);
    if (highest >= 99999)
        return false;
    char path[1024];
    snprintf(name, name_cap, "LOG%05u.VWL", highest + 1);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    log_file = fopen(path, "wb");
    return log_file != NULL;
}

bool storage_write(const uint8_t *data, size_t len)
{
    return log_file && fwrite(data, 1, len, log_file) == len;
}

bool storage_sync(void)
{
    return log_file && fflush(log_file) == 0 && fsync(fileno(log_file)) == 0;
}

void storage_close(void)
{
    if (log_file)
        fclose(log_file);
    log_file = NULL;
}

int storage_read_file(const char *name, char *buf, size_t cap)
{
    const char *dir = card();
    if (!dir)
        return -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}

int storage_list(storage_list_fn fn, void *ctx)
{
    const char *dir = card();
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d)
        return -1;
    int count = 0;
    for (struct dirent *e; (e = readdir(d));) {
        char path[1024];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_name[0] == '.' || stat(path, &st) || !S_ISREG(st.st_mode))
            continue;
        fn(e->d_name, (uint32_t)st.st_size, ctx);
        count++;
    }
    closedir(d);
    return count;
}

int storage_read_at(const char *name, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *size)
{
    const char *dir = card();
    if (!dir)
        return -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    struct stat st;
    if (fstat(fileno(f), &st)) {
        fclose(f);
        return -1;
    }
    *size = (uint32_t)st.st_size;
    size_t n = 0;
    if (offset < *size && fseek(f, (long)offset, SEEK_SET) == 0)
        n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}
