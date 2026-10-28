/* SPDX-License-Identifier: GPL-3.0-only */
#include "osos.h"
#include "preferences.h"
#include "debug_log.h"

#define LINES 512

static struct {
    uint32_t magic, count;
    char lines[LINES][32];
} debug_log;

static int batching;

static const struct osos_path debug_path = {0, "iPod_Control\\Device\\CFWDebugLog"};

void cfw_debug(const char *tag, uint32_t a, uint32_t b)
{
    static const char hex[] = "0123456789abcdef";
    char *line = debug_log.lines[debug_log.count % LINES];
    int n = 0;
    for (; tag[n] && n < 13; n++)
        line[n] = tag[n];
    for (int v = 0; v < 2; v++) {
        uint32_t value = v ? b : a;
        line[n++] = ' ';
        for (int shift = 28; shift >= 0; shift -= 4)
            line[n++] = hex[(value >> shift) & 15];
    }
    while (n < 31)
        line[n++] = ' ';
    line[31] = '\n';
    debug_log.magic = 0x47474f4c;
    debug_log.count++;
    if (!batching)
        cfw_preferences_write(&debug_path, &debug_log, sizeof(debug_log));
}

void cfw_debug_dump(const char *tag, const void *object, uint32_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    const uint32_t *words = object;
    cfw_debug(tag, (uint32_t)(uintptr_t)object, bytes);
    if (!object)
        return;
    batching = 1;
    for (uint32_t offset = 0; offset < bytes; offset += 8) {
        char label[8] = {' ', '+', hex[(offset >> 8) & 15], hex[(offset >> 4) & 15],
                         hex[offset & 15], 0, 0, 0};
        cfw_debug(label, words[offset / 4], words[offset / 4 + 1]);
    }
    batching = 0;
    cfw_preferences_write(&debug_path, &debug_log, sizeof(debug_log));
}
