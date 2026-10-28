/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CFW_DEBUG_LOG_H
#define CFW_DEBUG_LOG_H
#include <stdint.h>

/* Temporary diagnostics: one line (tag, two words) appended to
   iPod_Control\Device\CFWDebugLog, the last 512 lines kept. */
void cfw_debug(const char *tag, uint32_t a, uint32_t b);
/* Dump words of an object, two per line, tagged with the byte offset. */
void cfw_debug_dump(const char *tag, const void *object, uint32_t bytes);

#endif
