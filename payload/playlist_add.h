/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CFW_PLAYLIST_ADD_H
#define CFW_PLAYLIST_ADD_H
#include "osos.h"

/* Song hold menus: add the Add to Playlist item. */
void cfw_playlist_add_menu(struct osos_context_menu *menu);
/* Song chosen from a hold menu (NULL clears it). */
void cfw_playlist_add_select(struct osos_media_item *item);
/* Playlist chooser: returns 1 when action was handled or support-queried. */
int cfw_playlist_add_action(void *controller, const char *action, uint32_t argument);

#endif
