#include "selftest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void selftest_add(struct selftest_report *r, const char *id, enum selftest_result result, const char *fmt, ...)
{
    if (r->count >= SELFTEST_MAX)
        return;
    struct selftest_item *it = &r->items[r->count++];
    it->result = (uint8_t)result;
    snprintf(it->id, sizeof(it->id), "%s", id);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->detail, sizeof(it->detail), fmt, ap);
    va_end(ap);
}

const char *selftest_result_name(enum selftest_result result)
{
    switch (result) {
    case SELFTEST_PASS:
        return "pass";
    case SELFTEST_WARN:
        return "warn";
    case SELFTEST_FAIL:
        return "FAIL";
    case SELFTEST_SKIP:
        return "skip";
    case SELFTEST_LOOK:
        return "look";
    }
    return "?";
}
