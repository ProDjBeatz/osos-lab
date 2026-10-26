/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CFW_PLAYLIST_RENAME_H
#define CFW_PLAYLIST_RENAME_H
#include <stdint.h>

/* Returns 1 when action was a rename action (handled or support-queried). */
int cfw_playlist_rename_action(void *controller, const char *action,
                               uint32_t argument);

/* TextView draw colour (via cfw_text_draw_color_stub): the typed name on the rename
   screen is drawn black on its light edit box, Dark Mode or not; all other text
   goes to cfw_dark_mode_text_color. */
void cfw_text_draw_color(void *context, const unsigned char *color, const char *text);

#endif
