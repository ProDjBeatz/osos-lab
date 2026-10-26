/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CFW_PLAYLIST_RENAME_H
#define CFW_PLAYLIST_RENAME_H
#include <stdint.h>

/* Returns 1 when action was a rename action (handled or support-queried). */
int cfw_playlist_rename_action(void *controller, const char *action,
                               uint32_t argument);

/* While the rename screen is open, draw grey or translucent text (the typed name
   in the light edit box) solid black. Returns color itself, or out filled in;
   callers must not recolour a returned out (Dark Mode included). */
const unsigned char *cfw_playlist_rename_text_color(const unsigned char *color,
                                                    unsigned char *out);

#endif
